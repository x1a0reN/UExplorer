#include "WinMemApi.h"

#include "ObjectsApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/UnrealContainers.h"
#include "Unreal/Enums.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>

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
			return static_cast<int>(*reinterpret_cast<uint8*>(addr));
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
			return reinterpret_cast<FName*>(addr)->ToString();
		if (type & EClassCastFlags::StrProperty)
		{
			UC::FString* fs = reinterpret_cast<UC::FString*>(addr);
			return fs->IsValid() ? json(fs->ToString()) : json("");
		}
		if (type & EClassCastFlags::TextProperty)
			return "(FText)";
		if (type & EClassCastFlags::ObjectProperty)
		{
			void* ptr = *reinterpret_cast<void**>(addr);
			if (!ptr) return nullptr;
			UEObject ref(ptr);
			if (!ref) return std::format("0x{:X}", reinterpret_cast<uintptr_t>(ptr));

			json out;
			out["name"] = ref.GetName();
			out["class"] = ref.GetClass() ? ref.GetClass().GetName() : "Unknown";
			out["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(ptr));
			out["index"] = ref.GetIndex();
			return out;
		}
		if (type & EClassCastFlags::ArrayProperty)
		{
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

	DWORD oldProtect = 0;
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
	catch (...) {
		ok = false;
	}

	VirtualProtect(addr, prop.GetSize(), oldProtect, &oldProtect);
	return ok;
}

static bool TryParseObjectIndex(const std::string& idxStr, int32& outIdx)
{
	if (idxStr.empty())
		return false;
	if (!std::all_of(idxStr.begin(), idxStr.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
		return false;

	try {
		outIdx = std::stoi(idxStr);
	}
	catch (...) {
		return false;
	}

	return outIdx >= 0 && outIdx < ObjectArray::Num();
}

static bool TryFindProperty(const UEClass& cls, const std::string& propName, UEProperty& outProp)
{
	for (const auto& prop : cls.GetProperties())
	{
		if (prop.GetName() == propName)
		{
			outProp = prop;
			return true;
		}
	}
	return false;
}

static json BuildOuterChain(UEObject obj)
{
	json outers = json::array();
	for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
	{
		json item;
		item["name"] = outer.GetName();
		item["full_name"] = outer.GetFullName();
		item["index"] = outer.GetIndex();
		item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(outer.GetAddress()));
		outers.push_back(std::move(item));
	}
	return outers;
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
		int offset = 0;
		int limit = 50;
		if (params.count("offset")) offset = std::stoi(params["offset"]);
		if (params.count("limit")) limit = std::stoi(params["limit"]);
		if (limit > 500) limit = 500;
		if (offset < 0) offset = 0;

		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int count = 0;
		int skipped = 0;

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
			catch (...) {
				item["class"] = "Unknown";
			}
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

	// GET /api/v1/objects/search — multi-condition search
	server.Get("/api/v1/objects/search", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			std::string nameFilter = params.count("q") ? params["q"] : "";
			std::string classFilter = params.count("class") ? params["class"] : "";
			std::string packageFilter = params.count("package") ? params["package"] : "";
			int offset = 0;
			int limit = 50;
			if (params.count("offset")) offset = std::stoi(params["offset"]);
			if (params.count("limit")) limit = std::stoi(params["limit"]);
			if (limit > 500) limit = 500;
			if (offset < 0) offset = 0;

			json items = json::array();
			int32 total = ObjectArray::Num();
			int matched = 0;
			int count = 0;
			int skipped = 0;

			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;

				std::string name;
				try { name = obj.GetName(); }
				catch (...) { continue; }

				if (!nameFilter.empty() && name.find(nameFilter) == std::string::npos)
					continue;

				std::string className;
				try {
					UEObject cls = obj.GetClass();
					className = cls ? cls.GetName() : "Unknown";
				}
				catch (...) {
					className = "Unknown";
				}

				if (!classFilter.empty() && className.find(classFilter) == std::string::npos)
					continue;

				if (!packageFilter.empty())
				{
					bool found = false;
					for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
					{
						std::string outerName;
						try { outerName = outer.GetName(); }
						catch (...) { break; }
						if (outerName == packageFilter || outerName.find(packageFilter) != std::string::npos)
						{
							found = true;
							break;
						}
					}
					if (!found && name != packageFilter)
						continue;
				}

				matched++;
				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue;

				json item;
				item["index"] = i;
				item["name"] = name;
				item["class"] = className;
				item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				items.push_back(std::move(item));
				count++;
			}

			json data;
			data["items"] = items;
			data["matched"] = matched;
			data["offset"] = offset;
			data["limit"] = limit;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const std::exception& e) {
			return { 500, "application/json", MakeError(std::string("Search error: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Search error: unknown") };
		}
	});

	// GET /api/v1/objects/by-address/:addr — find object by address
	server.Get("/api/v1/objects/by-address/:addr", [](const HttpRequest& req) -> HttpResponse {
		std::string addrStr = GetPathSegment(req.Path, 4);
		if (addrStr.empty())
			return { 400, "application/json", MakeError("Missing address") };

		uintptr_t targetAddr = 0;
		try {
			targetAddr = std::stoull(addrStr, nullptr, 16);
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid address") };
		}
		if (!targetAddr)
			return { 400, "application/json", MakeError("Invalid address") };

		int32 total = ObjectArray::Num();
		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			if (reinterpret_cast<uintptr_t>(obj.GetAddress()) != targetAddr) continue;

			json data;
			data["index"] = i;
			data["name"] = obj.GetName();
			data["full_name"] = obj.GetFullName();
			data["address"] = std::format("0x{:X}", targetAddr);
			data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			return { 200, "application/json", MakeResponse(data) };
		}

		return { 404, "application/json", MakeError("No object found at address " + addrStr) };
	});

	// GET /api/v1/objects/by-path/:path — find object by full path
	server.Get("/api/v1/objects/by-path/:path", [](const HttpRequest& req) -> HttpResponse {
		std::string targetPath = GetPathSegment(req.Path, 4);
		if (targetPath.empty())
			return { 400, "application/json", MakeError("Missing path") };

		int32 total = ObjectArray::Num();
		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			try {
				if (obj.GetFullName() != targetPath && obj.GetName() != targetPath)
					continue;

				json data;
				data["index"] = i;
				data["name"] = obj.GetName();
				data["full_name"] = obj.GetFullName();
				data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
				return { 200, "application/json", MakeResponse(data) };
			}
			catch (...) {}
		}

		return { 404, "application/json", MakeError("No object found with path " + targetPath) };
	});

	// GET /api/v1/objects/:index/outer-chain — full outer chain
	server.Get("/api/v1/objects/:index/outer-chain", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		json data;
		data["index"] = idx;
		data["name"] = obj.GetName();
		data["outer_chain"] = BuildOuterChain(obj);
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects/:index/properties — read live property values
	server.Get("/api/v1/objects/:index/properties", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

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

	// GET /api/v1/objects/:index/property/:name — read a single property value
	server.Get("/api/v1/objects/:index/property/:name", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		std::string propName = GetPathSegment(req.Path, 5);
		if (propName.empty())
			return { 400, "application/json", MakeError("Missing property name") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			UEProperty prop;
			if (!TryFindProperty(cls, propName, prop))
				return { 404, "application/json", MakeError("Property not found: " + propName) };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			json data;
			data["object_index"] = idx;
			data["property"] = prop.GetName();
			data["type"] = prop.GetCppType();
			data["offset"] = prop.GetOffset();
			data["size"] = prop.GetSize();
			data["value"] = ReadPropertyValue(objAddr, prop);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read property") };
		}
	});

	// POST /api/v1/objects/:index/property/:name — write a single property value
	server.Post("/api/v1/objects/:index/property/:name", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		std::string propName = GetPathSegment(req.Path, 5);
		if (propName.empty())
			return { 400, "application/json", MakeError("Missing property name") };

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

			UEProperty prop;
			if (!TryFindProperty(cls, propName, prop))
				return { 404, "application/json", MakeError("Property not found: " + propName) };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			if (!WritePropertyValue(objAddr, prop, body["value"]))
				return { 400, "application/json", MakeError("Unsupported property type for writing") };

			json data;
			data["property"] = propName;
			data["written"] = true;
			data["new_value"] = ReadPropertyValue(objAddr, prop);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Property write failed") };
		}
	});

	// GET /api/v1/objects/:index — single object detail
	server.Get("/api/v1/objects/:index", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 404, "application/json", MakeError("Not found") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			json data;
			data["index"] = idx;
			data["name"] = obj.GetName();
			data["full_name"] = obj.GetFullName();
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			data["outer_chain"] = BuildOuterChain(obj);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read object") };
		}
	});

	// GET /api/v1/packages — list all packages
	server.Get("/api/v1/packages", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		if (offset < 0) offset = 0;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0;
		int count = 0;
		int skipped = 0;
		std::set<std::string> seen;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Package)) continue;

			std::string name;
			try { name = obj.GetName(); }
			catch (...) { continue; }

			if (!filter.empty() && name.find(filter) == std::string::npos)
				continue;
			if (seen.count(name))
				continue;
			seen.insert(name);

			matched++;
			if (skipped < offset) { skipped++; continue; }
			if (count >= limit) continue;

			json item;
			item["name"] = name;
			item["index"] = i;
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
			count++;
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/packages/:name/contents — list all types in a package
	server.Get("/api/v1/packages/:name/contents", [](const HttpRequest& req) -> HttpResponse {
		std::string packageName = GetPathSegment(req.Path, 3);
		if (packageName.empty())
			return { 400, "application/json", MakeError("Missing package name") };

		json items = json::array();
		int32 total = ObjectArray::Num();

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			std::string objName;
			try { objName = obj.GetName(); }
			catch (...) { continue; }

			bool belongs = false;
			for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
			{
				std::string outerName;
				try { outerName = outer.GetName(); }
				catch (...) { break; }
				if (outerName == packageName)
				{
					belongs = true;
					break;
				}
			}

			if (!belongs && objName != packageName)
				continue;
			if (obj.IsA(EClassCastFlags::Package))
				continue;

			json item;
			item["index"] = i;
			item["name"] = objName;
			item["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
		}

		json data;
		data["package"] = packageName;
		data["items"] = items;
		data["count"] = items.size();
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API
