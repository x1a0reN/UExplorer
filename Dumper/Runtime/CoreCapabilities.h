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
	std::shared_ptr<const WorldSnapshot> World;
	bool WorldInspectServiceEnabled = false;
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
		false,
		"MEMORY_READ_COMMAND_NOT_IMPLEMENTED",
		"The bounded raw-memory read command is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.raw_write",
		false,
		"MEMORY_WRITE_COMMAND_NOT_IMPLEMENTED",
		"The bounded raw-memory write command is not registered",
		{"memory.safe"});
	builder.Define(
		"memory.typed",
		false,
		"MEMORY_TYPED_COMMAND_NOT_IMPLEMENTED",
		"Validated typed-memory commands are not registered",
		{"memory.safe"});
	builder.Define(
		"memory.pointer_chain",
		false,
		"POINTER_CHAIN_COMMAND_NOT_IMPLEMENTED",
		"The bounded pointer-chain command is not registered",
		{"memory.safe"});
	builder.Define(
		"call.invoke",
		probes.FunctionCallServiceEnabled,
		"FUNCTION_CALL_SERVICE_NOT_READY",
		"No validated function-call domain command is registered",
		{"engine.property_codec", "engine.type_snapshot", "objects.snapshot",
			"game_thread.executor", "functions.handles"});
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
		false,
		"WORLD_DETAILS_NOT_IMPLEMENTED",
		"Actor details, components, and current-world shortcuts are not implemented",
		{"world.inspect"});
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
	builder.Define(
		"dump.dumpspace",
		false,
		"TARGET_FIXTURE_REQUIRED",
		"A target-generated Dumpspace artifact has not passed semantic validation",
		{"engine.core"});
	builder.Define(
		"dump.ida",
		false,
		"TARGET_FIXTURE_REQUIRED",
		"A target-generated IDA mapping has not passed semantic validation",
		{"engine.core"});

	return builder.Build(context.Generation());
}

inline std::vector<std::string> RequiredReadyCapabilities()
{
	return {"engine.core", "game_thread.executor", "transport.named_pipe"};
}

} // namespace UExplorer::Runtime
