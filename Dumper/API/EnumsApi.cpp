#include "EnumsApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/Enums.h"

namespace UExplorer::API
{

void RegisterEnumsRoutes(HttpServer& server)
{
	// GET /api/v1/enums — paginated enum list
	server.Get("/api/v1/enums", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 5000) limit = 5000;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Enum)) continue;

			try {
				std::string name = obj.GetName();
				if (!filter.empty() && name.find(filter) == std::string::npos) continue;
				matched++;
				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue;

				json item;
				item["index"] = i;
				item["name"] = name;
				item["full_name"] = obj.GetFullName();
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

	// GET /api/v1/enums/:name — enum values
	server.Get("/api/v1/enums/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		if (name.empty())
			return { 400, "application/json", MakeError("Missing enum name") };

		// Find enum by iterating (no FindEnumFast in Dumper7)
		UEEnum found;
		int32 total = ObjectArray::Num();
		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Enum)) continue;
			try {
				if (obj.GetName() == name) { found = obj.Cast<UEEnum>(); break; }
			} catch (...) {}
		}

		if (!found)
			return { 404, "application/json", MakeError("Enum not found: " + name) };

		try {
			json data;
			data["name"] = found.GetName();
			data["full_name"] = found.GetFullName();
			data["underlying_type"] = found.GetEnumTypeAsStr();

			json values = json::array();
			for (const auto& [fname, val] : found.GetNameValuePairs())
			{
				json v;
				v["name"] = fname.ToString();
				v["value"] = val;
				values.push_back(std::move(v));
			}
			data["values"] = values;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to serialize enum") };
		}
	});
}

} // namespace UExplorer::API