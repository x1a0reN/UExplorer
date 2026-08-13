#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/ParamFrame.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/WorldSnapshot.h"
#include "Utils/Json/json.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct WorldMutationCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

enum class WorldMutationField : std::uint8_t
{
	Location,
	Rotation,
	Scale
};

enum class WorldMutationSpace : std::uint8_t
{
	World,
	Relative
};

enum class WorldMutationExecutionPhase : std::uint8_t
{
	BeforeInvoke,
	Invoke,
	AfterInvoke
};

enum class WorldMutationExecutionError : std::uint8_t
{
	None,
	DependencyChanged,
	ExecutionThreadInvalid,
	ActorHandleStale,
	RootComponentHandleStale,
	RootComponentChanged,
	FunctionHandleStale,
	AddressOverflow,
	MemoryReadFailed,
	ProcessEventUnavailable,
	ReturnValueInvalid
};

struct WorldMutationValue
{
	std::array<double, 3> Components{};
	bool TeleportPhysics = false;
};

class WorldMutationUpdateWork final : public Runtime::IGameThreadWork
{
public:
	bool Execute() override;

	WorldMutationExecutionError ExecutionError() const noexcept { return m_Error; }
	WorldMutationExecutionPhase ExecutionPhase() const noexcept { return m_Phase; }
	Runtime::HandleError HandleError() const noexcept { return m_HandleError; }
	Runtime::MemoryError MemoryError() const noexcept { return m_MemoryError; }
	WorldMutationField Field() const noexcept { return m_Field; }
	WorldMutationSpace Space() const noexcept { return m_Space; }
	const WorldMutationValue& Value() const noexcept { return m_Value; }
	const Runtime::WorldSnapshotObject& Actor() const noexcept { return m_Actor; }
	const Runtime::WorldSnapshotObject& RootComponent() const noexcept
	{
		return m_RootComponent;
	}
	const Runtime::ObjectHandle& Target() const noexcept { return m_Target; }
	const Runtime::FunctionHandle& FunctionHandle() const noexcept;
	const std::string& FunctionPath() const noexcept;
	std::uint64_t ContextGeneration() const noexcept;
	std::uint64_t ObjectSnapshotGeneration() const noexcept;
	std::uint64_t TypeSnapshotGeneration() const noexcept;
	std::uint64_t WorldSnapshotGeneration() const noexcept;
	bool Invoked() const noexcept { return m_Invoked; }
	bool HasSetterResult() const noexcept { return m_SetterResult.has_value(); }
	bool SetterResult() const noexcept { return m_SetterResult.value_or(false); }

private:
	friend class WorldMutationCommandService;

	WorldMutationUpdateWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& gameThread,
		std::shared_ptr<const Runtime::EngineSnapshot> objects,
		std::shared_ptr<const Runtime::TypeSnapshot> types,
		std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
		std::shared_ptr<const Runtime::WorldSnapshot> world,
		Runtime::WorldSnapshotObject actor,
		Runtime::WorldSnapshotObject rootComponent,
		Runtime::ObjectHandle target,
		const Runtime::ReflectedProperty& actorRootProperty,
		const Runtime::ReflectedFunction& function,
		const Runtime::ReflectedProperty* returnProperty,
		WorldMutationField field,
		WorldMutationSpace space,
		WorldMutationValue value,
		Runtime::ParamFrame frame);

	bool ValidateLiveBindings(WorldMutationExecutionPhase phase) noexcept;
	bool DecodeSetterResult() noexcept;

	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	Runtime::GameThreadExecutor& m_GameThread;
	std::shared_ptr<const Runtime::EngineSnapshot> m_Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> m_Types;
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> m_Reflection;
	std::shared_ptr<const Runtime::WorldSnapshot> m_World;
	Runtime::WorldSnapshotObject m_Actor;
	Runtime::WorldSnapshotObject m_RootComponent;
	Runtime::ObjectHandle m_Target;
	const Runtime::ReflectedProperty* m_ActorRootProperty = nullptr;
	const Runtime::ReflectedFunction* m_Function = nullptr;
	const Runtime::ReflectedProperty* m_ReturnProperty = nullptr;
	WorldMutationField m_Field = WorldMutationField::Scale;
	WorldMutationSpace m_Space = WorldMutationSpace::World;
	WorldMutationValue m_Value;
	Runtime::ParamFrame m_Frame;
	WorldMutationExecutionError m_Error = WorldMutationExecutionError::None;
	WorldMutationExecutionPhase m_Phase = WorldMutationExecutionPhase::BeforeInvoke;
	Runtime::HandleError m_HandleError = Runtime::HandleError::None;
	Runtime::MemoryError m_MemoryError = Runtime::MemoryError::None;
	std::optional<bool> m_SetterResult;
	bool m_Invoked = false;
};

struct WorldMutationUpdatePreparation
{
	std::shared_ptr<WorldMutationUpdateWork> Work;
	std::optional<WorldMutationCommandError> Error;

	bool Ok() const noexcept { return static_cast<bool>(Work) && !Error; }
};

struct WorldMutationCommandResult
{
	json Data = nullptr;
	std::optional<WorldMutationCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

class WorldMutationCommandService final
{
public:
	static constexpr std::size_t kMaxParameterBytes = 64 * 1024;

	// One request admits exactly one field. This avoids presenting sequential
	// ProcessEvent calls as an atomic transform transaction.
	static WorldMutationUpdatePreparation PrepareUpdate(
		const json& data,
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& gameThread) noexcept;
	static WorldMutationCommandResult CompleteUpdate(
		const WorldMutationUpdateWork& work) noexcept;
};

} // namespace UExplorer::Services
