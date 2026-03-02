#include "StatusApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Settings.h"

#include <windows.h>
#include <filesystem>

namespace UExplorer::API
{

static std::string GetProcessArchitecture()
{
#ifdef _WIN64
	BOOL bIsWow64 = FALSE;
	IsWow64Process(GetCurrentProcess(), &bIsWow64);
	if (bIsWow64)
		return "x86 (WOW64)";
	return "x64";
#else
	return "x86";
#endif
}

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
		data["pid"] = GetCurrentProcessId();
		data["architecture"] = GetProcessArchitecture();
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API
