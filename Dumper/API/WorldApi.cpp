#include "WorldApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealContainers.h"
#include "OffsetFinder/Offsets.h"

#include <format>

namespace UExplorer::API
{

// Helper: get GWorld pointer
static void* GetGWorld()
{
	if (Off::InSDK::World::GWorld == 0) return nullptr;
	uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
	void** worldPtr = reinterpret_cast<void**>(base + Off::InSDK::World::GWorld);
	return *worldPtr;
}

void RegisterWorldRoutes(HttpServer& server)
{
	// GET /api/v1/world — current world info
	server.Get("/api/v1/world", [](const HttpRequest&) -> HttpResponse {
		try {
			void* world = GetGWorld();
			if (!world)
				return { 500, "application/json", MakeError("GWorld not available") };

			UEObject worldObj(world);
			json data;
			data["name"] = worldObj ? worldObj.GetName() : "Unknown";
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(world));

			// Count actors via GObjects (find all Actor instances)
			int actorCount = 0;
			int32 total = ObjectArray::Num();
			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;
				if (obj.IsA(EClassCastFlags::Actor)) actorCount++;
			}
			data["actor_count"] = actorCount;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read world") };
		}
	});

	// GET /api/v1/world/actors — paginated actor list
	server.Get("/api/v1/world/actors", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			int offset = 0, limit = 50;
			if (params.count("offset")) offset = std::stoi(params["offset"]);
			if (params.count("limit")) limit = std::stoi(params["limit"]);
			if (limit > 500) limit = 500;

			std::string filter = params.count("q") ? params["q"] : "";
			std::string classFilter = params.count("class") ? params["class"] : "";

			json items = json::array();
			int32 total = ObjectArray::Num();
			int count = 0, skipped = 0, matched = 0;

			for (int32 i = 0; i < total && count < limit; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;
				if (!obj.IsA(EClassCastFlags::Actor)) continue;

				std::string name;
				try { name = obj.GetName(); } catch (...) { continue; }

				if (!filter.empty() && name.find(filter) == std::string::npos)
					continue;

				std::string className;
				try {
					UEObject cls = obj.GetClass();
					className = cls ? cls.GetName() : "Unknown";
				} catch (...) { className = "Unknown"; }

				if (!classFilter.empty() && className.find(classFilter) == std::string::npos)
					continue;

				matched++;
				if (skipped < offset) { skipped++; continue; }

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
		catch (...) {
			return { 500, "application/json", MakeError("Failed to enumerate actors") };
		}
	});

	// GET /api/v1/world/shortcuts — quick access objects
	server.Get("/api/v1/world/shortcuts", [](const HttpRequest&) -> HttpResponse {
		try {
			json data;
			auto findFirst = [](const char* className) -> json {
				int32 total = ObjectArray::Num();
				for (int32 i = 0; i < total; i++)
				{
					UEObject obj = ObjectArray::GetByIndex(i);
					if (!obj) continue;
					try {
						UEObject cls = obj.GetClass();
						if (!cls) continue;
						std::string cn = cls.GetName();
						if (cn.find(className) != std::string::npos
							&& obj.GetName().find("Default__") == std::string::npos)
						{
							json r;
							r["index"] = i;
							r["name"] = obj.GetName();
							r["class"] = cn;
							r["address"] = std::format("0x{:X}",
								reinterpret_cast<uintptr_t>(obj.GetAddress()));
							return r;
						}
					} catch (...) {}
				}
				return nullptr;
			};

			data["game_mode"] = findFirst("GameMode");
			data["game_state"] = findFirst("GameState");
			data["player_controller"] = findFirst("PlayerController");
			data["pawn"] = findFirst("Pawn");

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to find shortcuts") };
		}
	});
}

} // namespace UExplorer::API
