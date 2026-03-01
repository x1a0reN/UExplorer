#include "StatusApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Settings.h"

namespace UExplorer::API
{

void RegisterStatusRoutes(HttpServer& server)
{
	// GET /api/v1/status/health — heartbeat
	server.Get("/api/v1/status/health", [](const HttpRequest&) -> HttpResponse {
		return { 200, "application/json", MakeResponse({{"alive", true}}) };
	});

	// GET /api/v1/status — full status info
	server.Get("/api/v1/status", [](const HttpRequest&) -> HttpResponse {
		json data;
		data["game_name"] = Settings::Generator::GameName;
		data["game_version"] = Settings::Generator::GameVersion;
		data["object_count"] = ObjectArray::Num();
		data["gobjects_address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(ObjectArray::DEBUGGetGObjects()));
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API