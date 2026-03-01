#include "ClassesApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/Enums.h"

namespace UExplorer::API
{

// Helper: serialize a single property to JSON
static json SerializeProperty(const UEProperty& prop)
{
	json j;
	j["name"] = prop.GetName();
	j["type"] = prop.GetCppType();
	j["offset"] = prop.GetOffset();
	j["size"] = prop.GetSize();
	j["array_dim"] = prop.GetArrayDim();
	j["flags"] = prop.StringifyFlags();
	return j;
}

// Helper: serialize a single function to JSON
static json SerializeFunction(const UEFunction& func)
{
	json j;
	j["name"] = func.GetName();
	j["full_name"] = func.GetFullName();
	j["flags"] = func.StringifyFlags();
	j["param_size"] = func.GetStructSize();
	j["has_script"] = func.HasScript();
	j["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(func.GetExecFunction()));

	json params = json::array();
	for (const auto& prop : func.GetProperties())
	{
		json p;
		p["name"] = prop.GetName();
		p["type"] = prop.GetCppType();
		p["offset"] = prop.GetOffset();
		p["size"] = prop.GetSize();
		p["flags"] = prop.StringifyFlags();
		params.push_back(std::move(p));
	}
	j["params"] = params;
	return j;
}

void RegisterClassesRoutes(HttpServer& server)
{
	// GET /api/v1/classes — paginated class list
	server.Get("/api/v1/classes", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Class)) continue;

			try {
				std::string name = obj.GetName();
				if (!filter.empty() && name.find(filter) == std::string::npos) continue;
				matched++;

				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue; // keep counting matched

				UEStruct cls = obj.Cast<UEStruct>();
				json item;
				item["index"] = i;
				item["name"] = name;
				item["full_name"] = obj.GetFullName();
				item["size"] = cls.GetStructSize();
				item["super"] = cls.GetSuper() ? cls.GetSuper().GetName() : "";
				item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				items.push_back(std::move(item));
				count++;
			} catch (...) { continue; }
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/classes/:name — class full info
	server.Get("/api/v1/classes/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		if (name.empty())
			return { 400, "application/json", MakeError("Missing class name") };

		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json data;
			data["name"] = cls.GetName();
			data["full_name"] = cls.GetFullName();
			data["cpp_name"] = cls.GetCppName();
			data["size"] = cls.GetStructSize();
			data["alignment"] = cls.GetMinAlignment();
			data["index"] = cls.GetIndex();
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(cls.GetAddress()));
			data["super"] = cls.GetSuper() ? cls.GetSuper().GetName() : "";

			// Fields
			json fields = json::array();
			for (const auto& prop : cls.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			data["fields"] = fields;

			// Functions
			json funcs = json::array();
			for (const auto& func : cls.GetFunctions())
			{
				try { funcs.push_back(SerializeFunction(func)); }
				catch (...) {}
			}
			data["functions"] = funcs;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to serialize class") };
		}
	});

	// GET /api/v1/classes/:name/fields
	server.Get("/api/v1/classes/:name/fields", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json fields = json::array();
			for (const auto& prop : cls.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			return { 200, "application/json", MakeResponse(fields) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get fields") };
		}
	});

	// GET /api/v1/classes/:name/functions
	server.Get("/api/v1/classes/:name/functions", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json funcs = json::array();
			for (const auto& func : cls.GetFunctions())
			{
				try { funcs.push_back(SerializeFunction(func)); }
				catch (...) {}
			}
			return { 200, "application/json", MakeResponse(funcs) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get functions") };
		}
	});

	// GET /api/v1/classes/:name/hierarchy — inheritance chain
	server.Get("/api/v1/classes/:name/hierarchy", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			// Walk up the inheritance chain
			json parents = json::array();
			UEStruct cur = cls.GetSuper();
			while (cur)
			{
				parents.push_back(cur.GetName());
				cur = cur.GetSuper();
			}

			// Find direct subclasses
			json children = json::array();
			int32 total = ObjectArray::Num();
			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj || !obj.IsA(EClassCastFlags::Class)) continue;
				try {
					UEStruct s = obj.Cast<UEStruct>();
					if (s.GetSuper() && s.GetSuper().GetName() == name)
						children.push_back(obj.GetName());
				} catch (...) {}
			}

			json data;
			data["name"] = name;
			data["parents"] = parents;
			data["children"] = children;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get hierarchy") };
		}
	});

	// GET /api/v1/structs — paginated struct list (non-class, non-function)
	server.Get("/api/v1/structs", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			if (!obj.IsA(EClassCastFlags::Struct)) continue;
			if (obj.IsA(EClassCastFlags::Class) || obj.IsA(EClassCastFlags::Function)) continue;

			try {
				std::string name = obj.GetName();
				if (!filter.empty() && name.find(filter) == std::string::npos) continue;
				matched++;
				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue;

				UEStruct s = obj.Cast<UEStruct>();
				json item;
				item["index"] = i;
				item["name"] = name;
				item["size"] = s.GetStructSize();
				item["super"] = s.GetSuper() ? s.GetSuper().GetName() : "";
				items.push_back(std::move(item));
				count++;
			} catch (...) { continue; }
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/structs/:name — struct detail
	server.Get("/api/v1/structs/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEStruct s = ObjectArray::FindStructFast(name);
		if (!s)
			return { 404, "application/json", MakeError("Struct not found: " + name) };

		try {
			json data;
			data["name"] = s.GetName();
			data["full_name"] = s.GetFullName();
			data["size"] = s.GetStructSize();
			data["alignment"] = s.GetMinAlignment();
			data["super"] = s.GetSuper() ? s.GetSuper().GetName() : "";

			json fields = json::array();
			for (const auto& prop : s.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			data["fields"] = fields;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to serialize struct") };
		}
	});
}

} // namespace UExplorer::API