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
#include <iostream>

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
	catch (...) { return false; }
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
	if (type & EClassCastFlags::ByteProperty) return (int)*addr;
	if (type & EClassCastFlags::IntProperty) return *reinterpret_cast<int32*>(addr);
	if (type & EClassCastFlags::Int64Property) return *reinterpret_cast<int64*>(addr);
	if (type & EClassCastFlags::FloatProperty) return *reinterpret_cast<float*>(addr);
	if (type & EClassCastFlags::DoubleProperty) return *reinterpret_cast<double*>(addr);
	if (type & EClassCastFlags::NameProperty) return reinterpret_cast<FName*>(addr)->ToString();
	return std::format("({})", prop.GetCppType());
}

void RegisterCallRoutes(HttpServer& server)
{
	// POST /api/v1/call/function — call UFunction via ProcessEvent
	server.Post("/api/v1/call/function", [](const HttpRequest& req) -> HttpResponse {
		try {
			std::cerr << "[CallApi] Request received" << std::endl;
			json body = json::parse(req.Body);
			int32 objIdx = body.value("object_index", -1);
			std::string funcName = body.value("function_name", "");
			json params = body.value("params", json::object());
			std::cerr << "[CallApi] objIdx=" << objIdx << " func=" << funcName << std::endl;

			if (objIdx < 0 || objIdx >= ObjectArray::Num())
				return { 400, "application/json", MakeError("Invalid object_index") };
			if (funcName.empty())
				return { 400, "application/json", MakeError("Missing function_name") };

			UEObject obj = ObjectArray::GetByIndex(objIdx);
			if (!obj)
				return { 404, "application/json", MakeError("Object is null") };
			std::cerr << "[CallApi] Object found: " << obj.GetName() << std::endl;

			UEClass cls;
			// Check if this object is already a class - use it directly for static function lookup
			if (obj.IsA(EClassCastFlags::Class)) {
				cls = obj.Cast<UEClass>();
				std::cerr << "[CallApi] Object is a class: " << cls.GetName() << std::endl;
			} else {
				cls = obj.GetClass();
			}
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };
			std::cerr << "[CallApi] Using class: " << cls.GetName() << std::endl;

			// Find the function
			UEFunction func;
			for (const auto& f : cls.GetFunctions())
			{
				if (f.GetName() == funcName) { func = f; break; }
			}
			if (!func)
				return { 404, "application/json", MakeError("Function not found: " + funcName) };
			std::cerr << "[CallApi] Function found, paramSize=" << func.GetStructSize() << std::endl;

			// Allocate param buffer
			int32 paramSize = func.GetStructSize();
			std::vector<uint8> paramBuf(paramSize > 0 ? paramSize : 256, 0);

			// Fill in params from JSON
			for (const auto& prop : func.GetProperties())
			{
				uint64 flags = static_cast<uint64>(prop.GetPropertyFlags());
				bool isOut = (flags & static_cast<uint64>(EPropertyFlags::OutParm)) != 0;
				bool isReturn = (flags & static_cast<uint64>(EPropertyFlags::ReturnParm)) != 0;
				if (isReturn || isOut) continue;

				std::string pName = prop.GetName();
				if (params.contains(pName))
					FillParam(paramBuf.data(), prop, params[pName]);
			}

			// Call the function
			bool useGameThread = body.value("use_game_thread", false);
			void* funcAddr = func.GetAddress();

			// Check if this is a static function
			uint64 funcFlags = static_cast<uint64>(func.GetFunctionFlags());
			bool isStatic = (funcFlags & static_cast<uint64>(EFunctionFlags::Static)) != 0;

			void* objAddr;
			void* procEvent = nullptr;
			int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;

			if (isStatic) {
				// For static functions, use the class's default object (CDO) and its VTable ProcessEvent
				// SDK: GetDefaultObj()->ProcessEvent(Func, &Parms)
				UEClass cls = obj.Cast<UEClass>();
				UEObject cdo = cls.GetDefaultObject();
				objAddr = cdo.GetAddress();
				if (objAddr && peIdx > 0 && peIdx < 200) {
					void** objVft = *reinterpret_cast<void***>(objAddr);
					procEvent = objVft[peIdx];
				}
				std::cerr << "[CallApi] Static function, using CDO: " << std::hex << objAddr << " PE=" << procEvent << std::dec << std::endl;
			} else {
				// For non-static functions, use the object's own VTable ProcessEvent
				// SDK: UObject::ProcessEvent(Func, &Parms) - uses 'this' to get VTable
				objAddr = obj.GetAddress();
				if (objAddr && peIdx > 0 && peIdx < 200) {
					void** objVft = *reinterpret_cast<void***>(objAddr);
					procEvent = objVft[peIdx];
				}
				std::cerr << "[CallApi] Non-static function, using object VTable: " << std::hex << objAddr << " PE=" << procEvent << std::dec << std::endl;
			}

			if (!procEvent) {
				return { 500, "application/json", MakeError("ProcessEvent not found") };
			}

			// Call via game thread if requested and hooks are enabled
			if (useGameThread && GameThread::g_Enabled) {
				std::cerr << "[CallApi] Calling via GameThreadQueue..." << std::endl;
				bool ok = GameThread::Submit(objAddr, funcAddr, paramBuf.data(), 10000);
				std::cerr << "[CallApi] GameThreadQueue result: " << ok << std::endl;
				if (!ok) {
					return { 500, "application/json", MakeError("Game thread call timed out") };
				}
			} else {
				// Direct call via ProcessEvent
				auto PE = reinterpret_cast<void(*)(void*, void*, void*)>(procEvent);
				std::cerr << "[CallApi] Calling ProcessEvent..." << std::endl;
				PE(objAddr, funcAddr, paramBuf.data());
				std::cerr << "[CallApi] ProcessEvent returned" << std::endl;
			}

			// Read return/out values
			json result;
			for (const auto& prop : func.GetProperties())
			{
				uint64 flags = static_cast<uint64>(prop.GetPropertyFlags());
				bool isOut = (flags & static_cast<uint64>(EPropertyFlags::OutParm)) != 0;
				bool isReturn = (flags & static_cast<uint64>(EPropertyFlags::ReturnParm)) != 0;
				if (!isReturn && !isOut) continue;

				try {
					std::string key = isReturn ? "return" : prop.GetName();
					result[key] = ReadParam(paramBuf.data(), prop);
				} catch (...) {}
			}

			json data;
			data["function"] = funcName;
			data["called"] = true;
			data["result"] = result;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Function call failed") };
		}
	});
}

} // namespace UExplorer::API
