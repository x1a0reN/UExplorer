#include "WatchApi.h"
#include "ApiCommon.h"
#include "EventsApi.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/Enums.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/UnrealContainers.h"

#include <format>
#include <mutex>
#include <map>
#include <atomic>
#include <vector>
#include <chrono>

namespace UExplorer::API
{

struct WatchEntry
{
	int Id;
	int32 ObjectIndex;
	std::string PropertyName;
	json LastValue;
	int64_t LastChangeTime;
	int64_t CreatedTime;
};

static std::mutex g_WatchMutex;
static std::map<int, WatchEntry> g_Watches;
static std::atomic<int> g_WatchCounter{0};

static int64_t WatchNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// Helper: read a property value by name (reuses ObjectsApi pattern)
static json ReadPropByName(int32 objIdx, const std::string& propName)
{
	UEObject obj = ObjectArray::GetByIndex(objIdx);
	if (!obj) return nullptr;
	UEClass cls = obj.GetClass();
	if (!cls) return nullptr;
	uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());

	for (const auto& prop : cls.GetProperties())
	{
		if (prop.GetName() != propName) continue;
		uint8* addr = objAddr + prop.GetOffset();
		EClassCastFlags type = prop.GetCastFlags();

		if (type & EClassCastFlags::IntProperty) return *reinterpret_cast<int32*>(addr);
		if (type & EClassCastFlags::FloatProperty) return *reinterpret_cast<float*>(addr);
		if (type & EClassCastFlags::DoubleProperty) return *reinterpret_cast<double*>(addr);
		if (type & EClassCastFlags::BoolProperty) {
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			return (*(addr + bp.GetByteOffset()) & bp.GetFieldMask()) != 0;
		}
		if (type & EClassCastFlags::ByteProperty) return (int)*addr;
		if (type & EClassCastFlags::NameProperty) return reinterpret_cast<FName*>(addr)->ToString();
		return std::format("({})", prop.GetCppType());
	}
	return nullptr;
}

void RegisterWatchRoutes(HttpServer& server)
{
	// POST /api/v1/watch/add — add a property watch
	server.Post("/api/v1/watch/add", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			int32 objIdx = body.value("object_index", -1);
			std::string propName = body.value("property", "");

			if (objIdx < 0 || objIdx >= ObjectArray::Num())
				return { 400, "application/json", MakeError("Invalid object_index") };
			if (propName.empty())
				return { 400, "application/json", MakeError("Missing 'property'") };

			json curVal = ReadPropByName(objIdx, propName);
			int64_t now = WatchNowMs();
			int id = ++g_WatchCounter;

			{
				std::lock_guard<std::mutex> lk(g_WatchMutex);
				g_Watches[id] = { id, objIdx, propName, curVal, now, now };
			}

			json data;
			data["id"] = id;
			data["object_index"] = objIdx;
			data["property"] = propName;
			data["current_value"] = curVal;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to add watch") };
		}
	});

	// DELETE /api/v1/watch/:id — remove a watch
	server.Delete("/api/v1/watch/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			int id = std::stoi(GetPathSegment(req.Path, 3));
			std::lock_guard<std::mutex> lk(g_WatchMutex);
			if (g_Watches.erase(id) == 0)
				return { 404, "application/json", MakeError("Watch not found") };

			json data;
			data["removed"] = id;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid watch ID") };
		}
	});

	// GET /api/v1/watch/list — list all watches with current values
	server.Get("/api/v1/watch/list", [](const HttpRequest&) -> HttpResponse {
		try {
			std::lock_guard<std::mutex> lk(g_WatchMutex);
			json items = json::array();
			for (auto& [id, w] : g_Watches)
			{
				json cur = ReadPropByName(w.ObjectIndex, w.PropertyName);
				bool changed = false;
				if (cur != w.LastValue) {
					w.LastValue = cur;
					w.LastChangeTime = WatchNowMs();
					changed = true;

					// Broadcast SSE event for value change
					try {
						json evt;
						evt["id"] = w.Id;
						evt["property"] = w.PropertyName;
						evt["value"] = cur;
						evt["timestamp"] = w.LastChangeTime;
						BroadcastWatchEvent(w.Id, evt.dump());
					} catch (...) {}
				}
				json item;
				item["id"] = w.Id;
				item["object_index"] = w.ObjectIndex;
				item["property"] = w.PropertyName;
				item["value"] = cur;
				item["changed"] = changed;
				item["last_change"] = w.LastChangeTime;
				item["created"] = w.CreatedTime;
				items.push_back(std::move(item));
			}
			json data;
			data["watches"] = items;
			data["count"] = (int)g_Watches.size();
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to list watches") };
		}
	});
}

} // namespace UExplorer::API