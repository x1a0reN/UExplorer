#pragma once

#include "CapabilityRegistry.h"
#include "EngineContext.h"
#include "EngineNameCodec.h"
#include "ReflectionLayout.h"
#include "TypeSnapshot.h"
#include "WorldSnapshot.h"

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
	bool FunctionHandleValidationEnabled = false;
	bool ObjectSnapshotPublished = false;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::shared_ptr<const ReflectionRuntimeSnapshot> Reflection;
	std::shared_ptr<const TypeSnapshot> Types;
	bool ObjectPropertyServiceEnabled = false;
	bool FunctionCallServiceEnabled = false;
	bool FunctionCallBatchCommandServiceEnabled = false;
	bool MemoryReadCommandServiceEnabled = false;
	bool MemoryWriteCommandServiceEnabled = false;
	bool WatchCommandServiceEnabled = false;
	bool DomainEventPumpEnabled = false;
	bool BlueprintBytecodeCaptureEnabled = false;
	bool BlueprintBytecodeProfileEnabled = false;
	bool HookCommandServiceEnabled = false;
	bool HookProducerInstalled = false;
	bool DumpCommandServiceEnabled = false;
	bool DumpWorkerEnabled = false;
	std::shared_ptr<const WorldSnapshot> World;
	bool WorldInspectServiceEnabled = false;
	bool WorldMutationServiceEnabled = false;
	bool NamedPipeListening = false;
};

inline std::shared_ptr<const CapabilitySnapshot> BuildCoreCapabilities(
	const EngineContext& context,
	const RuntimeProbes& probes)
{
	CapabilityRegistryBuilder builder;
	builder.Define("engine.core", true);
	const EngineNameProfile& nameProfile = context.NameProfile();
	const bool namesAvailable = nameProfile.Validated
		&& IsEngineNameProfileLayoutValid(nameProfile);
	builder.Define(
		"engine.names",
		namesAvailable,
		nameProfile.ReasonCode.empty() ? "NAME_STORAGE_LAYOUT_NOT_VALIDATED" : nameProfile.ReasonCode,
		nameProfile.Reason.empty()
			? "No complete immutable FName storage layout is available"
			: nameProfile.Reason,
		{"engine.core", "memory.safe"});
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
	const bool reflectionPublished = static_cast<bool>(probes.Reflection);
	const bool reflectionReady = reflectionPublished
		&& probes.Reflection->IsLayoutConfigured(context.Generation());
	const bool propertyCodecReady = reflectionPublished
		&& probes.Reflection->IsPropertyCodecConfigured(context.Generation());
	builder.Define(
		"engine.reflection",
		reflectionReady,
		reflectionPublished
			? "REFLECTION_RUNTIME_INVALID"
			: "REFLECTION_RUNTIME_NOT_PUBLISHED",
		reflectionPublished
			? "The reflection snapshot does not match this context generation or layout fingerprint"
			: "No immutable reflection layout has passed semantic witnesses",
		{"engine.names", "memory.safe"});
	builder.Define(
		"engine.property_codec",
		propertyCodecReady,
		"PROPERTY_CODEC_NOT_CONFIGURED",
		"No immutable property codec profile matching the published reflection layout has passed its available layout witnesses",
		{"engine.reflection"});
	const bool typeSnapshotPublished = static_cast<bool>(probes.Types);
	const bool typeSnapshotReady = typeSnapshotPublished
		&& probes.ObjectSnapshotGeneration != 0
		&& probes.Types->ObjectSnapshotGeneration() == probes.ObjectSnapshotGeneration
		&& probes.Types->IsConfigured(context.Generation());
	builder.Define(
		"engine.type_snapshot",
		typeSnapshotReady,
		typeSnapshotPublished
			? "TYPE_SNAPSHOT_INVALID"
			: "TYPE_SNAPSHOT_NOT_PUBLISHED",
		typeSnapshotPublished
			? "The type snapshot does not match the active context, object generation, or reflection layout"
			: "No complete immutable type snapshot has been published",
		{"engine.reflection", "objects.snapshot"});
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
		"functions.handles",
		probes.FunctionHandleValidationEnabled,
		"FUNCTION_HANDLE_VALIDATION_NOT_READY",
		"Function owner, path, and signature validation is not active",
		{"objects.handles"});
	builder.Define(
		"transport.named_pipe",
		probes.NamedPipeListening,
		"PIPE_LISTENER_NOT_READY",
		"The v1 named-pipe listener is not accepting sessions");
	builder.Define("status.inspect", true, {}, {}, {"engine.core"});
	builder.Define(
		"objects.snapshot",
		probes.ObjectSnapshotPublished,
		"OBJECT_SNAPSHOT_UNAVAILABLE",
		"No complete immutable object snapshot has been published",
		{"engine.names", "objects.handles"});
	builder.Define(
		"objects.properties",
		probes.ObjectPropertyServiceEnabled,
		"OBJECT_PROPERTY_SERVICE_NOT_READY",
		"The stable-handle reflected property read service is not registered",
		{"engine.property_codec", "engine.type_snapshot", "objects.handles", "game_thread.executor"});
	builder.Define(
		"types.inspect",
		true,
		{},
		{},
		{"engine.type_snapshot"});
	builder.Define(
		"memory.raw_read",
		probes.MemoryReadCommandServiceEnabled,
		"MEMORY_COMMAND_SERVICE_NOT_READY",
		"The bounded raw-memory command service is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.raw_write",
		probes.MemoryWriteCommandServiceEnabled,
		"MEMORY_COMMAND_SERVICE_NOT_READY",
		"The bounded raw-memory command service is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.typed_read",
		probes.MemoryReadCommandServiceEnabled,
		"MEMORY_COMMAND_SERVICE_NOT_READY",
		"The validated scalar typed-memory read command is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.typed_write",
		probes.MemoryWriteCommandServiceEnabled,
		"MEMORY_COMMAND_SERVICE_NOT_READY",
		"The validated scalar typed-memory write command is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.pointer_chain",
		probes.MemoryReadCommandServiceEnabled,
		"MEMORY_COMMAND_SERVICE_NOT_READY",
		"The bounded pointer-chain command is not registered",
		{"memory.safe"});
	builder.Define(
		"watch.properties",
		probes.WatchCommandServiceEnabled,
		"WATCH_COMMAND_SERVICE_NOT_READY",
		"The bounded property-watch scheduler is not registered",
		{"objects.properties", "objects.snapshot", "engine.type_snapshot", "game_thread.executor"});
	builder.Define(
		"watch.events.push",
		probes.WatchCommandServiceEnabled && probes.DomainEventPumpEnabled,
		!probes.WatchCommandServiceEnabled
			? "WATCH_COMMAND_SERVICE_NOT_READY"
			: (!probes.NamedPipeListening
				? "PIPE_LISTENER_NOT_READY"
				: "DOMAIN_EVENT_PUMP_NOT_READY"),
		!probes.WatchCommandServiceEnabled
			? "The bounded property-watch scheduler is not registered"
			: (!probes.NamedPipeListening
				? "The PID-scoped Named Pipe listener is not active"
				: "The owned Watch event publisher is not running"),
		{"watch.properties", "transport.named_pipe"});
	builder.Define(
		"call.invoke",
		probes.FunctionCallServiceEnabled,
		"FUNCTION_CALL_SERVICE_NOT_READY",
		"No validated function-call domain command is registered",
		{"engine.property_codec", "engine.type_snapshot", "objects.snapshot",
			"game_thread.executor", "functions.handles"});
	builder.Define(
		"call.batch.jobs",
		probes.FunctionCallBatchCommandServiceEnabled,
		"CALL_BATCH_ADAPTER_NOT_READY",
		"No bounded batch coordinator with an owned exact-call adapter is registered",
		{"engine.core"});
	builder.Define(
		"call.batch",
		probes.FunctionCallBatchCommandServiceEnabled,
		"CALL_BATCH_ADAPTER_NOT_READY",
		"The bounded batch coordinator has no owned adapter to the exact single-call path",
		{"call.batch.jobs", "call.invoke"});
	builder.Define(
		"call.static",
		false,
		"CALL_STATIC_OPERATION_RETIRED",
		"Static calls use call.invoke with an explicit CDO target; the legacy operation is not registered",
		{"call.invoke"});
	const bool worldSnapshotReady = probes.World
		&& probes.World->SessionId.size() > 0
		&& probes.World->ContextGeneration == context.Generation()
		&& probes.World->Generation > 0
		&& probes.Types
		&& probes.World->ObjectSnapshotGeneration == probes.ObjectSnapshotGeneration
		&& probes.World->TypeSnapshotGeneration == probes.Types->Generation()
		&& probes.Types->ObjectSnapshotGeneration()
			== probes.World->ObjectSnapshotGeneration;
	builder.Define(
		"engine.world_snapshot",
		worldSnapshotReady,
		probes.World ? "WORLD_SNAPSHOT_INVALID" : "WORLD_SNAPSHOT_NOT_PUBLISHED",
		probes.World
			? "The published current-world snapshot does not match the active context"
			: "No immutable current-world snapshot has been published",
		{"engine.world_global", "objects.snapshot", "engine.type_snapshot"});
	builder.Define(
		"world.inspect",
		probes.WorldInspectServiceEnabled,
		"WORLD_COMMAND_NOT_READY",
		"Immutable current-world commands are not registered",
		{"engine.world_snapshot", "objects.handles"});
	builder.Define(
		"world.details",
		probes.WorldInspectServiceEnabled,
		"WORLD_DETAILS_COMMAND_NOT_READY",
		"Immutable actor detail, component, and shortcut commands are not registered",
		{"world.inspect"});
	builder.Define(
		"world.mutate",
		probes.WorldMutationServiceEnabled,
		"WORLD_MUTATION_SERVICE_NOT_READY",
		"The strict single-field reflected world mutation service is not registered",
		{"world.details", "engine.process_event", "engine.property_codec",
			"functions.handles", "game_thread.executor"});
	builder.Define(
		"hook.monitor",
		probes.HookCommandServiceEnabled && probes.HookProducerInstalled,
		!probes.HookCommandServiceEnabled
			? "HOOK_COMMAND_SERVICE_NOT_READY"
			: "HOOK_PRODUCER_NOT_READY",
		!probes.HookCommandServiceEnabled
			? "The bounded hook registry and collector are not registered"
			: "The ProcessEvent producer lacks complete current type-generation vtable coverage",
		{"engine.process_event", "engine.type_snapshot", "functions.handles"});
	builder.Define(
		"hook.events.push",
		probes.HookCommandServiceEnabled
			&& probes.HookProducerInstalled
			&& probes.DomainEventPumpEnabled,
		!probes.HookCommandServiceEnabled || !probes.HookProducerInstalled
			? "HOOK_PRODUCER_NOT_READY"
			: (!probes.NamedPipeListening
				? "PIPE_LISTENER_NOT_READY"
				: "DOMAIN_EVENT_PUMP_NOT_READY"),
		!probes.HookCommandServiceEnabled || !probes.HookProducerInstalled
			? "The ProcessEvent producer lacks complete current type-generation coverage"
			: (!probes.NamedPipeListening
				? "The PID-scoped Named Pipe listener is not active"
				: "The owned Hook event publisher is not running"),
		{"hook.monitor", "transport.named_pipe"});
	builder.Define(
		"blueprint.bytecode",
		probes.BlueprintBytecodeCaptureEnabled,
		"BYTECODE_CAPTURE_UNAVAILABLE",
		"No exact generation-bound bounded Script capture source is published",
		{"engine.type_snapshot", "functions.handles", "game_thread.executor"});
	builder.Define(
		"blueprint.decompile",
		probes.BlueprintBytecodeCaptureEnabled
			&& probes.BlueprintBytecodeProfileEnabled,
		probes.BlueprintBytecodeCaptureEnabled
			? "BYTECODE_PROFILE_UNAVAILABLE"
			: "BYTECODE_CAPTURE_UNAVAILABLE",
		probes.BlueprintBytecodeCaptureEnabled
			? "No source-catalog bytecode profile passed the runtime marker, name, property, and canonical math dependency gates"
			: "No exact generation-bound bounded Script capture source is published",
		{"blueprint.bytecode"});
	builder.Define(
		"dump.jobs",
		probes.DumpCommandServiceEnabled && probes.DumpWorkerEnabled,
		probes.DumpCommandServiceEnabled
			? "DUMP_WORKER_NOT_INJECTED"
			: "DUMP_COMMAND_SERVICE_NOT_READY",
		probes.DumpCommandServiceEnabled
			? "No owned dump coordinator/worker is injected; an empty job store is not fabricated"
			: "The generation-bound dump job query boundary is not registered",
		{});
	const auto dumpReasonCode = [&probes] {
		if (!probes.DumpCommandServiceEnabled)
			return std::string("DUMP_COMMAND_SERVICE_NOT_READY");
		if (!probes.DumpWorkerEnabled)
			return std::string("DUMP_WORKER_NOT_INJECTED");
		return std::string();
	};
	const auto dumpReason = [&probes] {
		if (!probes.DumpCommandServiceEnabled)
			return std::string("The owned dump command boundary is not registered");
		if (!probes.DumpWorkerEnabled)
			return std::string("No owned generator worker is injected; legacy generators are not a fallback");
		return std::string();
	};
	builder.Define(
		"dump.cpp",
		probes.DumpCommandServiceEnabled
			&& probes.DumpWorkerEnabled,
		dumpReasonCode(),
		dumpReason(),
		{"dump.jobs", "objects.snapshot", "engine.type_snapshot"});
	builder.Define(
		"dump.usmap",
		probes.DumpCommandServiceEnabled
			&& probes.DumpWorkerEnabled,
		dumpReasonCode(),
		dumpReason(),
		{"dump.jobs", "objects.snapshot", "engine.type_snapshot"});
	builder.Define(
		"dump.dumpspace",
		probes.DumpCommandServiceEnabled
			&& probes.DumpWorkerEnabled,
		dumpReasonCode(),
		dumpReason(),
		{"dump.jobs", "objects.snapshot", "engine.type_snapshot"});
	builder.Define(
		"dump.ida",
		probes.DumpCommandServiceEnabled
			&& probes.DumpWorkerEnabled,
		dumpReasonCode(),
		dumpReason(),
		{"dump.jobs", "objects.snapshot", "engine.type_snapshot"});

	return builder.Build(context.Generation());
}

inline std::vector<std::string> RequiredReadyCapabilities()
{
	return {"engine.core", "game_thread.executor", "transport.named_pipe"};
}

} // namespace UExplorer::Runtime
