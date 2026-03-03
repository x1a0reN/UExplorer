#include "CallApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"
#include "GameThreadQueue.h"

#include <format>
#include <vector>

namespace UExplorer::API
{

// Helper: fill a single param in the buffer from JSON
static bool FillParam(uint8* buf, const UEProperty& prop, const json& val)
{
	uint8* addr = buf + prop.GetOffset();
	EClassCastFlags type = prop.GetCastFlags();

	try {
		if (type & EClassCastFlags::BoolProperty) {
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			uint8* byteAddr = addr + bp.GetByteOffset();
			if (val.get<bool>()) *byteAddr |= bp.GetFieldMask();
			else *byteAddr &= ~bp.GetFieldMask();
		}
		else if (type & EClassCastFlags::ByteProperty)
			*reinterpret_cast<uint8*>(addr) = static_cast<uint8>(val.get<int>());
		else if (type & EClassCastFlags::IntProperty)
			*reinterpret_cast<int32*>(addr) = val.get<int32>();
		else if (type & EClassCastFlags::Int64Property)
			*reinterpret_cast<int64*>(addr) = val.get<int64>();
		else if (type & EClassCastFlags::FloatProperty)
			*reinterpret_cast<float*>(addr) = val.get<float>();
		else if (type & EClassCastFlags::DoubleProperty)
			*reinterpret_cast<double*>(addr) = val.get<double>();
		else
			return false;
		return true;
	}
	catch (...) {
		return false;
	}
}

// Helper: read a param value back as JSON
static json ReadParam(uint8* buf, const UEProperty& prop)
{
	uint8* addr = buf + prop.GetOffset();
	EClassCastFlags type = prop.GetCastFlags();

	if (type & EClassCastFlags::BoolProperty) {
		UEBoolProperty bp = prop.Cast<UEBoolProperty>();
		return (*(addr + bp.GetByteOffset()) & bp.GetFieldMask()) != 0;
	}
	if (type & EClassCastFlags::ByteProperty) return static_cast<int>(*addr);
	if (type & EClassCastFlags::IntProperty) return *reinterpret_cast<int32*>(addr);
	if (type & EClassCastFlags::Int64Property) return *reinterpret_cast<int64*>(addr);
	if (type & EClassCastFlags::FloatProperty) return *reinterpret_cast<float*>(addr);
	if (type & EClassCastFlags::DoubleProperty) return *reinterpret_cast<double*>(addr);
	if (type & EClassCastFlags::NameProperty) return reinterpret_cast<FName*>(addr)->ToString();
	return std::format("({})", prop.GetCppType());
}

static UEFunction FindFunctionRecursive(UEClass cls, const std::string& funcName)
{
	for (UEStruct s = cls; s; s = s.GetSuper())
	{
		for (const auto& f : s.GetFunctions())
		{
			if (f.GetName() == funcName)
				return f;
		}
	}
	return nullptr;
}

static bool IsStaticFunction(const UEFunction& func)
{
	const uint64 flags = static_cast<uint64>(func.GetFunctionFlags());
	return (flags & static_cast<uint64>(EFunctionFlags::Static)) != 0;
}

static bool ExecuteFunctionCall(
	UEObject targetObject,
	UEClass ownerClass,
	const UEFunction& func,
	const json& params,
	bool useGameThread,
	bool forceStatic,
	json& outResult,
	std::string& outError)
{
	if (!func)
	{
		outError = "Function is null";
		return false;
	}

	const bool isStatic = forceStatic || IsStaticFunction(func);
	const int32 paramSize = func.GetStructSize();
	std::vector<uint8> paramBuf(paramSize > 0 ? paramSize : 256, 0);

	for (const auto& prop : func.GetProperties())
	{
		const uint64 flags = static_cast<uint64>(prop.GetPropertyFlags());
		const bool isOut = (flags & static_cast<uint64>(EPropertyFlags::OutParm)) != 0;
		const bool isReturn = (flags & static_cast<uint64>(EPropertyFlags::ReturnParm)) != 0;
		if (isOut || isReturn)
			continue;

		const std::string pName = prop.GetName();
		if (!params.contains(pName))
			continue;

		if (!FillParam(paramBuf.data(), prop, params[pName]))
		{
			outError = "Unsupported parameter type: " + pName;
			return false;
		}
	}

	void* objAddr = nullptr;
	if (isStatic)
	{
		if (!ownerClass)
		{
			outError = "Missing owner class for static call";
			return false;
		}
		UEObject cdo = ownerClass.GetDefaultObject();
		objAddr = cdo.GetAddress();
	}
	else
	{
		objAddr = targetObject.GetAddress();
	}

	if (!objAddr)
	{
		outError = isStatic ? "Static call CDO is null" : "Target object is null";
		return false;
	}

	const int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
	if (peIdx <= 0 || peIdx >= 200)
	{
		outError = "Invalid ProcessEvent index";
		return false;
	}

	void** vft = *reinterpret_cast<void***>(objAddr);
	if (!vft)
	{
		outError = "Target VTable is null";
		return false;
	}

	void* procEvent = vft[peIdx];
	if (!procEvent)
	{
		outError = "ProcessEvent not found";
		return false;
	}
	void* funcAddr = const_cast<void*>(func.GetAddress());

	if (useGameThread && GameThread::g_Enabled)
	{
		if (!GameThread::Submit(objAddr, funcAddr, paramBuf.data(), 10000))
		{
			outError = "Game thread call timed out";
			return false;
		}
	}
	else
	{
		auto PE = reinterpret_cast<void(*)(void*, void*, void*)>(procEvent);
		PE(objAddr, funcAddr, paramBuf.data());
	}

	json results;
	for (const auto& prop : func.GetProperties())
	{
		const uint64 flags = static_cast<uint64>(prop.GetPropertyFlags());
		const bool isOut = (flags & static_cast<uint64>(EPropertyFlags::OutParm)) != 0;
		const bool isReturn = (flags & static_cast<uint64>(EPropertyFlags::ReturnParm)) != 0;
		if (!isOut && !isReturn)
			continue;

		try {
			const std::string key = isReturn ? "return" : prop.GetName();
			results[key] = ReadParam(paramBuf.data(), prop);
		}
		catch (...) {}
	}

	outResult = std::move(results);
	return true;
}

static bool ResolveCallClass(const json& body, UEClass& outClass, std::string& outError)
{
	if (body.contains("class_index"))
	{
		int32 classIndex = body.value("class_index", -1);
		if (classIndex < 0 || classIndex >= ObjectArray::Num())
		{
			outError = "Invalid class_index";
			return false;
		}

		UEObject classObj = ObjectArray::GetByIndex(classIndex);
		if (!classObj || !classObj.IsA(EClassCastFlags::Class))
		{
			outError = "class_index does not point to UClass";
			return false;
		}

		outClass = classObj.Cast<UEClass>();
		return true;
	}

	if (body.contains("class_name"))
	{
		std::string className = body.value("class_name", "");
		if (className.empty())
		{
			outError = "Missing class_name";
			return false;
		}

		outClass = ObjectArray::FindClassFast(className);
		if (!outClass)
		{
			outError = "Class not found: " + className;
			return false;
		}
		return true;
	}

	if (body.contains("object_index"))
	{
		int32 objectIndex = body.value("object_index", -1);
		if (objectIndex < 0 || objectIndex >= ObjectArray::Num())
		{
			outError = "Invalid object_index";
			return false;
		}

		UEObject obj = ObjectArray::GetByIndex(objectIndex);
		if (!obj)
		{
			outError = "Object is null";
			return false;
		}

		outClass = obj.IsA(EClassCastFlags::Class) ? obj.Cast<UEClass>() : obj.GetClass();
		if (!outClass)
		{
			outError = "Unable to resolve class from object";
			return false;
		}
		return true;
	}

	outError = "Missing class_index/class_name/object_index";
	return false;
}

void RegisterCallRoutes(HttpServer& server)
{
	// POST /api/v1/call/function - call UFunction via ProcessEvent
	server.Post("/api/v1/call/function", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			int32 objIdx = body.value("object_index", -1);
			std::string funcName = body.value("function_name", "");
			json params = body.value("params", json::object());
			bool useGameThread = body.value("use_game_thread", false);

			if (objIdx < 0 || objIdx >= ObjectArray::Num())
				return { 400, "application/json", MakeError("Invalid object_index") };
			if (funcName.empty())
				return { 400, "application/json", MakeError("Missing function_name") };

			UEObject obj = ObjectArray::GetByIndex(objIdx);
			if (!obj)
				return { 404, "application/json", MakeError("Object is null") };

			UEClass cls = obj.IsA(EClassCastFlags::Class) ? obj.Cast<UEClass>() : obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			UEFunction func = FindFunctionRecursive(cls, funcName);
			if (!func)
				return { 404, "application/json", MakeError("Function not found: " + funcName) };

			if (obj.IsA(EClassCastFlags::Class) && !IsStaticFunction(func))
				return { 400, "application/json", MakeError("Non-static function requires an instance object") };

			json result;
			std::string err;
			if (!ExecuteFunctionCall(obj, cls, func, params, useGameThread, false, result, err))
				return { 500, "application/json", MakeError(err) };

			json data;
			data["function"] = funcName;
			data["called"] = true;
			data["object_index"] = objIdx;
			data["is_static"] = IsStaticFunction(func);
			data["result"] = std::move(result);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Function call failed") };
		}
	});

	// POST /api/v1/call/static - call static function via class CDO
	server.Post("/api/v1/call/static", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string funcName = body.value("function_name", "");
			json params = body.value("params", json::object());
			bool useGameThread = body.value("use_game_thread", false);

			if (funcName.empty())
				return { 400, "application/json", MakeError("Missing function_name") };

			UEClass cls;
			std::string resolveErr;
			if (!ResolveCallClass(body, cls, resolveErr))
				return { 400, "application/json", MakeError(resolveErr) };

			UEFunction func = FindFunctionRecursive(cls, funcName);
			if (!func)
				return { 404, "application/json", MakeError("Function not found: " + funcName) };
			if (!IsStaticFunction(func))
				return { 400, "application/json", MakeError("Function is not static: " + funcName) };

			UEObject classObj(cls.GetAddress());
			json result;
			std::string err;
			if (!ExecuteFunctionCall(classObj, cls, func, params, useGameThread, true, result, err))
				return { 500, "application/json", MakeError(err) };

			json data;
			data["function"] = funcName;
			data["called"] = true;
			data["class"] = cls.GetName();
			data["is_static"] = true;
			data["result"] = std::move(result);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Static function call failed") };
		}
	});

	// POST /api/v1/call/batch - call one function on multiple objects
	server.Post("/api/v1/call/batch", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::vector<int32> objectIndices = body.value("object_indices", std::vector<int32>{});
			std::string funcName = body.value("function_name", "");
			json params = body.value("params", json::object());
			bool useGameThread = body.value("use_game_thread", false);

			if (objectIndices.empty())
				return { 400, "application/json", MakeError("Missing object_indices") };
			if (funcName.empty())
				return { 400, "application/json", MakeError("Missing function_name") };

			json items = json::array();
			int successCount = 0;
			int failedCount = 0;

			for (int32 objIdx : objectIndices)
			{
				json item;
				item["object_index"] = objIdx;

				if (objIdx < 0 || objIdx >= ObjectArray::Num())
				{
					item["called"] = false;
					item["error"] = "Invalid object_index";
					items.push_back(std::move(item));
					failedCount++;
					continue;
				}

				UEObject obj = ObjectArray::GetByIndex(objIdx);
				if (!obj)
				{
					item["called"] = false;
					item["error"] = "Object is null";
					items.push_back(std::move(item));
					failedCount++;
					continue;
				}

				UEClass cls = obj.GetClass();
				if (!cls)
				{
					item["called"] = false;
					item["error"] = "Object has no class";
					items.push_back(std::move(item));
					failedCount++;
					continue;
				}

				UEFunction func = FindFunctionRecursive(cls, funcName);
				if (!func)
				{
					item["called"] = false;
					item["error"] = "Function not found";
					items.push_back(std::move(item));
					failedCount++;
					continue;
				}

				json result;
				std::string err;
				if (!ExecuteFunctionCall(obj, cls, func, params, useGameThread, false, result, err))
				{
					item["called"] = false;
					item["error"] = err;
					items.push_back(std::move(item));
					failedCount++;
					continue;
				}

				item["called"] = true;
				item["result"] = std::move(result);
				items.push_back(std::move(item));
				successCount++;
			}

			json data;
			data["function"] = funcName;
			data["requested"] = static_cast<int>(objectIndices.size());
			data["success"] = successCount;
			data["failed"] = failedCount;
			data["items"] = std::move(items);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Batch call failed") };
		}
	});
}

} // namespace UExplorer::API
