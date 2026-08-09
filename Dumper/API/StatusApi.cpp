#include "StatusApi.h"
#include "ApiCommon.h"

#include "Runtime/CoreRuntimeAccess.h"
#include "Runtime/GameThreadExecutor.h"

#include <windows.h>
#include <format>

// Script offset externs and BuildScriptOffsetDiagnosticsJson now in ApiCommon.h

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

static json SerializeCapabilities(const Runtime::CoreRuntimeSnapshot& snapshot)
{
	json capabilities = json::object();
	if (!snapshot.Capabilities)
		return capabilities;

	for (const auto& [name, capability] : snapshot.Capabilities->All())
	{
		capabilities[name] = {
			{"available", capability.Available},
			{"reason_code", capability.ReasonCode.empty() ? json(nullptr) : json(capability.ReasonCode)},
			{"reason", capability.Reason.empty() ? json(nullptr) : json(capability.Reason)},
			{"dependencies", capability.Dependencies}
		};
	}
	return capabilities;
}

static json SerializeRuntime(const Runtime::CoreRuntimeSnapshot& snapshot)
{
	json runtime;
	runtime["state"] = Runtime::ToString(snapshot.State);
	runtime["liveness"] = snapshot.IsLive();
	runtime["readiness"] = snapshot.IsReady();
	runtime["active_requests"] = snapshot.ActiveRequests;
	runtime["readiness_blockers"] = snapshot.ReadinessBlockers;
	runtime["failure_code"] = snapshot.FailureCode.empty() ? json(nullptr) : json(snapshot.FailureCode);
	runtime["failure_message"] = snapshot.FailureMessage.empty() ? json(nullptr) : json(snapshot.FailureMessage);
	runtime["context_generation"] = snapshot.Context
		? json(snapshot.Context->Generation())
		: json(nullptr);
	return runtime;
}

static json SerializeGameThreadDiagnostics()
{
	const Runtime::GameThreadDiagnostics diagnostics =
		Runtime::GetGameThreadExecutor().GetDiagnostics();
	return {
		{"enabled", diagnostics.Enabled},
		{"processing", diagnostics.Processing},
		{"pump_observed", diagnostics.PumpObserved},
		{"pump_thread_stable", diagnostics.PumpThreadStable},
		{"pump_thread_id", diagnostics.PumpThreadId},
		{"last_tick_monotonic_us", diagnostics.LastPumpTickMonotonicUs},
		{"tick_count", diagnostics.PumpTickCount},
		{"last_task_duration_us", diagnostics.LastTaskDurationUs},
		{"queue_depth", diagnostics.QueueDepth},
		{"queue_capacity", diagnostics.Capacity},
		{"pump_backend", Runtime::GetPostRenderPumpBackend().BackendName()}
	};
}

static json OffsetValue(const Runtime::EngineContext& context, const std::string& name)
{
	const Runtime::OffsetReport* report = context.FindOffset(name);
	return report ? json(report->Value) : json(nullptr);
}

static json SerializeOffsetReports(const Runtime::EngineContext& context)
{
	json reports = json::object();
	for (const auto& [name, report] : context.Offsets())
	{
		reports[name] = {
			{"value", report.Value},
			{"required", report.Required},
			{"state", Runtime::ToString(report.State)},
			{"source", report.Source},
			{"checks", report.Checks},
			{"reason_code", report.ReasonCode.empty() ? json(nullptr) : json(report.ReasonCode)},
			{"reason", report.Reason.empty() ? json(nullptr) : json(report.Reason)}
		};
	}
	return reports;
}

static bool TryGetRuntimeSnapshot(Runtime::CoreRuntimeSnapshot& outSnapshot)
{
	Runtime::CoreRuntime* runtime = Runtime::GetCoreRuntime();
	if (!runtime)
		return false;
	outSnapshot = runtime->Snapshot();
	return true;
}

void RegisterStatusRoutes(HttpServer& server)
{
	// GET /api/v1/status/engine — detailed engine internals
	server.Get("/api/v1/status/engine", [](const HttpRequest&) -> HttpResponse {
		Runtime::CoreRuntimeSnapshot snapshot;
		if (!TryGetRuntimeSnapshot(snapshot) || !snapshot.Context)
			return { 503, "application/json", MakeError("CORE_CONTEXT_UNAVAILABLE") };
		const Runtime::EngineContext& context = *snapshot.Context;
		const std::uintptr_t moduleBase = context.ModuleBase();

		json offsets;
		offsets["gobjects"] = OffsetValue(context, "gobjects");
		offsets["gnames"] = OffsetValue(context, "gnames");
		offsets["gworld"] = OffsetValue(context, "gworld");
		offsets["gengine"] = OffsetValue(context, "gengine");
		offsets["process_event_index"] = OffsetValue(context, "process_event.index");
		offsets["process_event_offset"] = OffsetValue(context, "process_event.offset");
		offsets["fuobjectitem_serial"] = OffsetValue(context, "fuobjectitem.serial_number");
		offsets["ulevel_actors"] = OffsetValue(context, "ulevel.actors");
		offsets["ufunction_script"] = OffsetValue(context, "ufunction.script");

		json addresses;
		addresses["module_base"] = std::format("0x{:X}", moduleBase);
		addresses["gobjects"] = std::format("0x{:X}", context.ObjectArrayAddress());
		const Runtime::OffsetReport* gnames = context.FindOffset("gnames");
		const Runtime::OffsetReport* gworld = context.FindOffset("gworld");
		const Runtime::OffsetReport* gengine = context.FindOffset("gengine");
		addresses["gnames"] = gnames && gnames->IsValidated()
			? json(std::format("0x{:X}", moduleBase + static_cast<std::uintptr_t>(gnames->Value)))
			: json(nullptr);
		addresses["gworld_ptr"] = gworld && gworld->IsValidated()
			? json(std::format("0x{:X}", moduleBase + static_cast<std::uintptr_t>(gworld->Value)))
			: json(nullptr);
		addresses["gengine_ptr"] = gengine && gengine->IsValidated()
			? json(std::format("0x{:X}", moduleBase + static_cast<std::uintptr_t>(gengine->Value)))
			: json(nullptr);

		json internals;
		internals["use_fproperty"] = context.Profile().UsesFProperty;
		internals["use_namepool"] = context.Profile().UsesNamePool;
		internals["use_large_world_coordinates"] = context.Profile().UsesLargeWorldCoordinates;
		internals["use_case_preserving_name"] = context.Profile().UsesCasePreservingName;
		internals["is_enum_name_only"] = context.Profile().EnumNameOnly;
		internals["is_small_enum_value"] = context.Profile().SmallEnumValue;

		json data;
		data["game_name"] = context.GameName();
		data["game_version"] = context.GameVersion();
		data["architecture"] = GetProcessArchitecture();
		data["pid"] = context.ProcessId();
		data["object_count"] = context.ObjectCount();
		data["offsets"] = std::move(offsets);
		data["offset_reports"] = SerializeOffsetReports(context);
		data["addresses"] = std::move(addresses);
		data["internals"] = std::move(internals);
		data["script_offset_diagnostics"] = BuildScriptOffsetDiagnosticsJson();
		data["runtime"] = SerializeRuntime(snapshot);
		data["capabilities"] = SerializeCapabilities(snapshot);
		data["game_thread"] = SerializeGameThreadDiagnostics();

		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/status/reconnect — re-scan engine globals and offsets
	server.Post("/api/v1/status/reconnect", [](const HttpRequest&) -> HttpResponse {
		return { 409, "application/json",
			MakeError("RECONNECT_DISABLED: engine globals cannot be mutated without an exclusive CoreRuntime transition") };
	});

	// GET /api/v1/status/health — heartbeat
	server.Get("/api/v1/status/health", [](const HttpRequest&) -> HttpResponse {
		Runtime::CoreRuntimeSnapshot snapshot;
		if (!TryGetRuntimeSnapshot(snapshot))
			return { 503, "application/json", MakeError("CORE_RUNTIME_UNAVAILABLE") };
		json data = SerializeRuntime(snapshot);
		data["alive"] = snapshot.IsLive();
		data["game_thread"] = SerializeGameThreadDiagnostics();
		return { snapshot.IsLive() ? 200 : 503, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/status — full status info
	server.Get("/api/v1/status", [](const HttpRequest&) -> HttpResponse {
		Runtime::CoreRuntimeSnapshot snapshot;
		if (!TryGetRuntimeSnapshot(snapshot) || !snapshot.Context)
			return { 503, "application/json", MakeError("CORE_CONTEXT_UNAVAILABLE") };
		const Runtime::EngineContext& context = *snapshot.Context;
		json data;
		data["game_name"] = context.GameName();
		data["game_version"] = context.GameVersion();
		data["object_count"] = context.ObjectCount();
		data["gobjects_address"] = std::format("0x{:X}", context.ObjectArrayAddress());
		data["pid"] = context.ProcessId();
		data["architecture"] = GetProcessArchitecture();
		data["script_offset_diagnostics"] = BuildScriptOffsetDiagnosticsJson();
		data["runtime"] = SerializeRuntime(snapshot);
		data["capabilities"] = SerializeCapabilities(snapshot);
		data["game_thread"] = SerializeGameThreadDiagnostics();
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API
