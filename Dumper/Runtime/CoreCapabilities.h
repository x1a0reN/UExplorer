#pragma once

#include "CapabilityRegistry.h"
#include "EngineContext.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

struct RuntimeProbes
{
	bool GameThreadExecutorEnabled = false;
	bool GameThreadPumpObserved = false;
	bool GameThreadPumpThreadStable = false;
	bool GameThreadPumpActive = false;
	bool SafeMemoryEnabled = false;
	bool ObjectIdentitySourceEnabled = false;
	bool ObjectHandleValidationEnabled = false;
	bool FunctionCallServiceEnabled = false;
	bool NamedPipeListening = false;
	bool LegacyHttpListening = false;
};

inline std::shared_ptr<const CapabilitySnapshot> BuildCoreCapabilities(
	const EngineContext& context,
	const RuntimeProbes& probes)
{
	CapabilityRegistryBuilder builder;
	builder.Define("engine.core", true);
	builder.Define(
		"engine.process_event",
		context.HasValidatedOffset("process_event.index")
			&& context.HasValidatedOffset("process_event.offset"),
		"PROCESS_EVENT_NOT_VALIDATED",
		"ProcessEvent index and module offset must both be validated",
		{"engine.core"});
	builder.Define(
		"engine.world_global",
		context.HasValidatedOffset("gworld"),
		"GWORLD_NOT_VALIDATED",
		"No unambiguous GWorld offset is available",
		{"engine.core"});
	const bool gameThreadAvailable = probes.GameThreadExecutorEnabled
		&& probes.GameThreadPumpObserved
		&& probes.GameThreadPumpThreadStable
		&& probes.GameThreadPumpActive;
	std::string gameThreadReasonCode;
	std::string gameThreadReason;
	if (!probes.GameThreadExecutorEnabled)
	{
		gameThreadReasonCode = "GAME_THREAD_EXECUTOR_DISABLED";
		gameThreadReason = "The game-thread executor is not enabled";
	}
	else if (!probes.GameThreadPumpObserved)
	{
		gameThreadReasonCode = "GAME_THREAD_PUMP_NOT_OBSERVED";
		gameThreadReason = "PostRender has not executed since the executor was enabled";
	}
	else if (!probes.GameThreadPumpThreadStable)
	{
		gameThreadReasonCode = "GAME_THREAD_PUMP_THREAD_MISMATCH";
		gameThreadReason = "PostRender callbacks were observed on multiple threads";
	}
	else if (!probes.GameThreadPumpActive)
	{
		gameThreadReasonCode = "GAME_THREAD_PUMP_STALLED";
		gameThreadReason = "The last PostRender callback is outside the liveness window";
	}
	builder.Define(
		"game_thread.executor",
		gameThreadAvailable,
		std::move(gameThreadReasonCode),
		std::move(gameThreadReason),
		{"engine.process_event"});
	builder.Define(
		"memory.safe",
		probes.SafeMemoryEnabled,
		"SAFE_MEMORY_NOT_READY",
		"Centralized checked memory access is not active",
		{"engine.core"});
	builder.Define(
		"objects.identity_source",
		probes.ObjectIdentitySourceEnabled,
		"OBJECT_IDENTITY_SOURCE_NOT_READY",
		"No validated FUObjectItem serial identity source is active",
		{"engine.core"});
	builder.Define(
		"objects.handles",
		probes.ObjectHandleValidationEnabled,
		"OBJECT_HANDLE_VALIDATION_NOT_READY",
		"Index and serial validation is not active",
		{"objects.identity_source", "game_thread.executor"});
	builder.Define(
		"transport.named_pipe",
		probes.NamedPipeListening,
		"PIPE_LISTENER_NOT_READY",
		"The v1 named-pipe listener is not accepting sessions");
	builder.Define(
		"transport.legacy_http",
		probes.LegacyHttpListening,
		"LEGACY_HTTP_NOT_LISTENING",
		"The temporary legacy HTTP transport is not listening");

	builder.Define("status.inspect", true, {}, {}, {"engine.core"});
	builder.Define("memory.raw_read", true, {}, {}, {"memory.safe"});
	builder.Define("memory.raw_write", true, {}, {}, {"memory.safe"});
	builder.Define(
		"objects.snapshot",
		false,
		"OBJECT_SNAPSHOT_NOT_IMPLEMENTED",
		"Immutable generation snapshots are not implemented",
		{"objects.handles"});
	builder.Define(
		"call.invoke",
		probes.FunctionCallServiceEnabled,
		"FUNCTION_CALL_SERVICE_NOT_READY",
		"No validated function-call domain command is registered",
		{"game_thread.executor", "objects.handles"});
	builder.Define("world.inspect", true, {}, {}, {"engine.world_global", "objects.handles"});
	builder.Define(
		"world.mutate",
		false,
		"WORLD_MUTATION_DISABLED",
		"Validated reflected world mutation is not implemented",
		{"game_thread.executor", "objects.handles"});
	builder.Define(
		"watch.properties",
		false,
		"WATCH_SCHEDULER_NOT_IMPLEMENTED",
		"The bounded game-thread watch scheduler is not implemented",
		{"objects.handles"});
	builder.Define(
		"hook.monitor",
		false,
		"HOOK_COLLECTOR_NOT_IMPLEMENTED",
		"The allocation-free bounded hook collector is not implemented",
		{"game_thread.executor"});
	builder.Define(
		"blueprint.decompile",
		false,
		"BLUEPRINT_READER_NOT_VERIFIED",
		"Bytecode bounds and version codecs are not verified",
		{"engine.core"});
	builder.Define(
		"dump.cpp",
		false,
		"TARGET_FIXTURE_REQUIRED",
		"Generated SDK artifacts have not passed target fixtures",
		{"engine.core"});
	builder.Define(
		"dump.usmap",
		false,
		"TARGET_FIXTURE_REQUIRED",
		"A complete target-generated mapping has not passed semantic validation",
		{"engine.core"});

	return builder.Build(context.Generation());
}

inline std::vector<std::string> RequiredReadyCapabilities()
{
	return {"engine.core", "game_thread.executor", "transport.named_pipe"};
}

} // namespace UExplorer::Runtime
