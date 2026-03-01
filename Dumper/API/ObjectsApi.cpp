#include "WinMemApi.h"

#include "ObjectsApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/UnrealContainers.h"
#include "Unreal/Enums.h"

namespace UExplorer::API
{

// Helper: read a single property value from a live object as JSON
static json ReadPropertyValue(uint8* objAddr, const UEProperty& prop)
{
	uint8* addr = objAddr + prop.GetOffset();
	EClassCastFlags type = prop.GetCastFlags();

	try {
		if (type & EClassCastFlags::BoolProperty)
		{
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			bool val = (*(addr + bp.GetByteOffset()) & bp.GetFieldMask()) != 0;
			return val;
		}
		if (type & EClassCastFlags::ByteProperty)
			return (int)*reinterpret_cast<uint8*>(addr);
		if (type & EClassCastFlags::IntProperty)
			return *reinterpret_cast<int32*>(addr);
		if (type & EClassCastFlags::Int64Property)
			return *reinterpret_cast<int64*>(addr);
		if (type & EClassCastFlags::UInt64Property)
			return *reinterpret_cast<uint64*>(addr);
		if (type & EClassCastFlags::FloatProperty)
			return *reinterpret_cast<float*>(addr);
		if (type & EClassCastFlags::DoubleProperty)
			return *reinterpret_cast<double*>(addr);
		if (type & EClassCastFlags::NameProperty)
		{
			FName* fn = reinterpret_cast<FName*>(addr);
			return fn->ToString();
		}
		if (type & EClassCastFlags::StrProperty)
		{
			UC::FString* fs = reinterpret_cast<UC::FString*>(addr);
			if (fs->IsValid())
				return fs->ToString();
			return "";
		}
		if (type & EClassCastFlags::TextProperty)
			return "(FText)";
		if (type & EClassCastFlags::ObjectProperty)
		{
			void* ptr = *reinterpret_cast<void**>(addr);
			if (!ptr) return nullptr;
			UEObject ref(ptr);
			if (ref) return ref.GetName();
			return std::format("0x{:X}", reinterpret_cast<uintptr_t>(ptr));
		}
		if (type & EClassCastFlags::ArrayProperty)
		{
			// Read TArray header: Data ptr + NumElements
			int32 num = *reinterpret_cast<int32*>(addr + sizeof(void*));
			return std::format("TArray[{}]", num);
		}
	}
	catch (...) {}

	return std::format("({})", prop.GetCppType());
}

// Helper: write a property value from JSON
static bool WritePropertyValue(uint8* objAddr, const UEProperty& prop, const json& value)
{
	uint8* addr = objAddr + prop.GetOffset();
	EClassCastFlags type = prop.GetCastFlags();

	DWORD oldProtect;
	VirtualProtect(addr, prop.GetSize(), PAGE_EXECUTE_READWRITE, &oldProtect);

	bool ok = true;
	try {
		if (type & EClassCastFlags::BoolProperty)
		{
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			uint8* byteAddr = addr + bp.GetByteOffset();
			if (value.get<bool>())
				*byteAddr |= bp.GetFieldMask();
			else
				*byteAddr &= ~bp.GetFieldMask();
		}
		else if (type & EClassCastFlags::ByteProperty)
			*reinterpret_cast<uint8*>(addr) = static_cast<uint8>(value.get<int>());
		else if (type & EClassCastFlags::IntProperty)
			*reinterpret_cast<int32*>(addr) = value.get<int32>();
		else if (type & EClassCastFlags::Int64Property)
			*reinterpret_cast<int64*>(addr) = value.get<int64>();
		else if (type & EClassCastFlags::FloatProperty)
			*reinterpret_cast<float*>(addr) = value.get<float>();
		else if (type & EClassCastFlags::DoubleProperty)
			*reinterpret_cast<double*>(addr) = value.get<double>();
		else
			ok = false;
	}
	catch (...) { ok = false; }

	VirtualProtect(addr, prop.GetSize(), oldProtect, &oldProtect);
	return ok;
}

void RegisterObjectsRoutes(HttpServer& server)
{
	// GET /api/v1/objects/count
	server.Get("/api/v1/objects/count", [](const HttpRequest&) -> HttpResponse {
		int32 total = ObjectArray::Num();
		int classes = 0, structs = 0, enums = 0, functions = 0, packages = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;

			if (obj.IsA(EClassCastFlags::Class)) classes++;
			else if (obj.IsA(EClassCastFlags::Function)) functions++;
			else if (obj.IsA(EClassCastFlags::Struct)) structs++;
			else if (obj.IsA(EClassCastFlags::Enum)) enums++;

			if (obj.IsA(EClassCastFlags::Package)) packages++;
		}

		json data;
		data["total"] = total;
		data["classes"] = classes;
		data["structs"] = structs;
		data["enums"] = enums;
		data["functions"] = functions;
		data["packages"] = packages;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects — paginated object list
	server.Get("/api/v1/objects", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);

		int offset = 0, limit = 50;
		if (params.count("offset")) offset = std::stoi(params["offset"]);
		if (params.count("limit")) limit = std::stoi(params["limit"]);
		if (limit > 500) limit = 500;

		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int count = 0, skipped = 0;

		for (int32 i = 0; i < total && count < limit; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;

			std::string name;
			try { name = obj.GetName(); }
			catch (...) { continue; }

			if (!filter.empty() && name.find(filter) == std::string::npos)
				continue;

			if (skipped < offset) { skipped++; continue; }

			json item;
			item["index"] = i;
			item["name"] = name;

			try {
				UEObject cls = obj.GetClass();
				item["class"] = cls ? cls.GetName() : "Unknown";
			}
			catch (...) { item["class"] = "Unknown"; }

			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
			count++;
		}

		json data;
		data["items"] = items;
		data["total"] = total;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects/:index — single object detail
	server.Get("/api/v1/objects/:index", [](const HttpRequest& req) -> HttpResponse {
		std::string idxStr = GetPathSegment(req.Path, 3);
		if (idxStr.empty())
			return { 400, "application/json", MakeError("Missing object index") };

		int32 idx = std::stoi(idxStr);
		if (idx < 0 || idx >= ObjectArray::Num())
			return { 404, "application/json", MakeError("Index out of range") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null at index " + idxStr) };

		try {
			json data;
			data["index"] = idx;
			data["name"] = obj.GetName();
			data["full_name"] = obj.GetFullName();
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));

			UEObject cls = obj.GetClass();
			data["class"] = cls ? cls.GetName() : "Unknown";

			// Outer chain
			json outers = json::array();
			UEObject outer = obj.GetOuter();
			while (outer)
			{
				outers.push_back(outer.GetName());
				outer = outer.GetOuter();
			}
			data["outer_chain"] = outers;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read object") };
		}
	});

	// GET /api/v1/objects/:index/properties — read live property values
	server.Get("/api/v1/objects/:index/properties", [](const HttpRequest& req) -> HttpResponse {
		std::string idxStr = GetPathSegment(req.Path, 3);
		if (idxStr.empty())
			return { 400, "application/json", MakeError("Missing object index") };

		int32 idx = std::stoi(idxStr);
		if (idx < 0 || idx >= ObjectArray::Num())
			return { 404, "application/json", MakeError("Index out of range") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			json props = json::array();

			for (const auto& prop : cls.GetProperties())
			{
				try {
					json p;
					p["name"] = prop.GetName();
					p["type"] = prop.GetCppType();
					p["offset"] = prop.GetOffset();
					p["size"] = prop.GetSize();
					p["value"] = ReadPropertyValue(objAddr, prop);
					props.push_back(std::move(p));
				}
				catch (...) {}
			}

			return { 200, "application/json", MakeResponse(props) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read properties") };
		}
	});

	// POST /api/v1/objects/:index/property/:name — write a single property value
	server.Post("/api/v1/objects/:index/property/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string idxStr = GetPathSegment(req.Path, 3);
		std::string propName = GetPathSegment(req.Path, 5);
		if (idxStr.empty() || propName.empty())
			return { 400, "application/json", MakeError("Missing index or property name") };

		int32 idx = std::stoi(idxStr);
		if (idx < 0 || idx >= ObjectArray::Num())
			return { 404, "application/json", MakeError("Index out of range") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			json body = json::parse(req.Body);
			if (!body.contains("value"))
				return { 400, "application/json", MakeError("Missing 'value' field") };

			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());

			for (const auto& prop : cls.GetProperties())
			{
				if (prop.GetName() == propName)
				{
					if (WritePropertyValue(objAddr, prop, body["value"]))
					{
						json data;
						data["property"] = propName;
						data["written"] = true;
						data["new_value"] = ReadPropertyValue(objAddr, prop);
						return { 200, "application/json", MakeResponse(data) };
					}
					return { 400, "application/json", MakeError("Unsupported property type for writing") };
				}
			}
			return { 404, "application/json", MakeError("Property not found: " + propName) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Property write failed") };
		}
	});
}

} // namespace UExplorer::API