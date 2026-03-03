#include "StatusApi.h"
#include "ApiCommon.h"

#include "Generators/Generator.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "OffsetFinder/Offsets.h"
#include "Settings.h"

#include <windows.h>
#include <filesystem>
#include <mutex>

extern "C" const char* UExplorer_GetScriptOffsetConfidence();
extern "C" const char* UExplorer_GetScriptOffsetAnomalyTags();
extern "C" int32 UExplorer_GetScriptOffsetSelectedOffset();
extern "C" int32 UExplorer_GetScriptOffsetSelectedScore();
extern "C" int32 UExplorer_GetScriptOffsetScoreGapTop2();
extern "C" int32 UExplorer_GetScriptOffsetBpEndHits();
extern "C" int32 UExplorer_GetScriptOffsetWeightedBpEndHits();
extern "C" int32 UExplorer_GetScriptOffsetGenericScriptHits();
extern "C" int32 UExplorer_GetScriptOffsetVerifyProbed();
extern "C" int32 UExplorer_GetScriptOffsetVerifyHeaderValid();
extern "C" int32 UExplorer_GetScriptOffsetVerifyEndHits();
extern "C" int32 UExplorer_GetScriptOffsetVerifyFirstOpcodeValid();
extern "C" int32 UExplorer_GetScriptOffsetVerifySizeSane();
extern "C" int32 UExplorer_GetScriptOffsetVerifyEndRate();
extern "C" int32 UExplorer_GetScriptOffsetVerifyOpcodeRate();

namespace UExplorer::API
{

static json BuildScriptOffsetDiagnosticsJson()
{
	json diag;
	diag["selected_offset"] = UExplorer_GetScriptOffsetSelectedOffset();
	diag["selected_score"] = UExplorer_GetScriptOffsetSelectedScore();
	diag["score_gap_top2"] = UExplorer_GetScriptOffsetScoreGapTop2();
	diag["bp_end_hits"] = UExplorer_GetScriptOffsetBpEndHits();
	diag["weighted_bp_end_hits"] = UExplorer_GetScriptOffsetWeightedBpEndHits();
	diag["generic_script_hits"] = UExplorer_GetScriptOffsetGenericScriptHits();
	diag["verify_probed"] = UExplorer_GetScriptOffsetVerifyProbed();
	diag["verify_header_valid"] = UExplorer_GetScriptOffsetVerifyHeaderValid();
	diag["verify_end_hits"] = UExplorer_GetScriptOffsetVerifyEndHits();
	diag["verify_first_opcode_valid"] = UExplorer_GetScriptOffsetVerifyFirstOpcodeValid();
	diag["verify_size_sane"] = UExplorer_GetScriptOffsetVerifySizeSane();
	diag["verify_end_rate"] = UExplorer_GetScriptOffsetVerifyEndRate();
	diag["verify_opcode_rate"] = UExplorer_GetScriptOffsetVerifyOpcodeRate();
	diag["confidence"] = UExplorer_GetScriptOffsetConfidence();
	diag["anomaly_tags"] = UExplorer_GetScriptOffsetAnomalyTags();
	return diag;
}

static std::mutex g_ReconnectMutex;

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
	// GET /api/v1/status/engine — detailed engine internals
	server.Get("/api/v1/status/engine", [](const HttpRequest&) -> HttpResponse {
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));

		json offsets;
		offsets["gobjects"] = Off::InSDK::ObjArray::GObjects;
		offsets["gnames"] = Off::InSDK::NameArray::GNames;
		offsets["gworld"] = Off::InSDK::World::GWorld;
		offsets["gengine"] = Off::InSDK::Engine::GEngine;
		offsets["process_event_index"] = Off::InSDK::ProcessEvent::PEIndex;
		offsets["process_event_offset"] = Off::InSDK::ProcessEvent::PEOffset;
		offsets["ulevel_actors"] = Off::InSDK::ULevel::Actors;
		offsets["ufunction_script"] = Off::UFunction::Script;

		json addresses;
		addresses["module_base"] = std::format("0x{:X}", moduleBase);
		addresses["gobjects"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(ObjectArray::DEBUGGetGObjects()));
		addresses["gnames"] = Off::InSDK::NameArray::GNames > 0
			? json(std::format("0x{:X}", moduleBase + static_cast<uintptr_t>(Off::InSDK::NameArray::GNames)))
			: json(nullptr);
		addresses["gworld_ptr"] = Off::InSDK::World::GWorld > 0
			? json(std::format("0x{:X}", moduleBase + static_cast<uintptr_t>(Off::InSDK::World::GWorld)))
			: json(nullptr);
		addresses["gengine_ptr"] = Off::InSDK::Engine::GEngine > 0
			? json(std::format("0x{:X}", moduleBase + static_cast<uintptr_t>(Off::InSDK::Engine::GEngine)))
			: json(nullptr);

		json internals;
		internals["use_fproperty"] = Settings::Internal::bUseFProperty;
		internals["use_namepool"] = Settings::Internal::bUseNamePool;
		internals["use_large_world_coordinates"] = Settings::Internal::bUseLargeWorldCoordinates;
		internals["use_case_preserving_name"] = Settings::Internal::bUseCasePreservingName;
		internals["is_enum_name_only"] = Settings::Internal::bIsEnumNameOnly;
		internals["is_small_enum_value"] = Settings::Internal::bIsSmallEnumValue;

		json data;
		data["game_name"] = Settings::Generator::GameName;
		data["game_version"] = Settings::Generator::GameVersion;
		data["architecture"] = GetProcessArchitecture();
		data["pid"] = GetCurrentProcessId();
		data["object_count"] = ObjectArray::Num();
		data["offsets"] = std::move(offsets);
		data["addresses"] = std::move(addresses);
		data["internals"] = std::move(internals);
		data["script_offset_diagnostics"] = BuildScriptOffsetDiagnosticsJson();

		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/status/reconnect — re-scan engine globals and offsets
	server.Post("/api/v1/status/reconnect", [](const HttpRequest&) -> HttpResponse {
		std::lock_guard<std::mutex> lk(g_ReconnectMutex);

		try {
			Generator::InitEngineCore();
			Generator::InitInternal();

			json data;
			data["reconnected"] = true;
			data["object_count"] = ObjectArray::Num();
			data["game_name"] = Settings::Generator::GameName;
			data["game_version"] = Settings::Generator::GameVersion;
			data["gobjects_address"] = std::format("0x{:X}",
				reinterpret_cast<uintptr_t>(ObjectArray::DEBUGGetGObjects()));
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const std::exception& e) {
			return { 500, "application/json",
				MakeError(std::string("Reconnect failed: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Reconnect failed") };
		}
	});

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
		data["script_offset_diagnostics"] = BuildScriptOffsetDiagnosticsJson();
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API
