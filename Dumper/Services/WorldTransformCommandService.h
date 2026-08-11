#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/PropertyCodec.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/WorldSnapshot.h"
#include "Utils/Json/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct WorldTransformCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

enum class WorldTransformExecutionError : std::uint8_t
{
	None,
	DependencyChanged,
	ExecutionThreadInvalid,
	ActorHandleStale,
	RootComponentHandleStale,
	RootComponentChanged,
	AddressOverflow,
	MemoryReadFailed,
	ValueChangedDuringRead,
	ValueDecodeFailed,
	ValueShapeInvalid
};

enum class WorldTransformPrecision : std::uint8_t
{
	Float32,
	Float64
};

struct WorldTransformVector
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
};

struct WorldTransformRotator
{
	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
};

struct WorldTransformValue
{
	WorldTransformPrecision Precision = WorldTransformPrecision::Float32;
	WorldTransformVector Location;
	WorldTransformRotator Rotation;
	WorldTransformVector Scale;
	bool AbsoluteLocation = false;
	bool AbsoluteRotation = false;
	bool AbsoluteScale = false;
};

class WorldTransformReadWork final : public Runtime::IGameThreadWork
{
public:
	bool Execute() override;
	WorldTransformExecutionError ExecutionError() const noexcept { return m_Error; }
	Runtime::HandleError HandleError() const noexcept { return m_HandleError; }
	Runtime::MemoryError MemoryError() const noexcept { return m_MemoryError; }
	const std::string& DecodeErrorCode() const noexcept { return m_DecodeErrorCode; }
	const std::string& DecodeError() const noexcept { return m_DecodeError; }
	const Runtime::WorldSnapshotObject& Actor() const noexcept { return m_Actor; }
	const Runtime::WorldSnapshotObject& RootComponent() const noexcept { return m_RootComponent; }
	const WorldTransformValue& Value() const noexcept { return m_Value; }
	std::uint64_t WorldSnapshotGeneration() const noexcept;
	std::uint64_t ObjectSnapshotGeneration() const noexcept;
	std::uint64_t TypeSnapshotGeneration() const noexcept;

private:
	friend class WorldTransformCommandService;
	WorldTransformReadWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		std::shared_ptr<const Runtime::EngineSnapshot> objects,
		std::shared_ptr<const Runtime::TypeSnapshot> types,
		std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
		std::shared_ptr<const Runtime::WorldSnapshot> world,
		Runtime::WorldSnapshotObject actor,
		Runtime::WorldSnapshotObject rootComponent,
		const Runtime::ReflectedProperty& actorRootProperty,
		std::array<const Runtime::ReflectedProperty*, 3> transformProperties,
		std::array<const Runtime::ReflectedProperty*, 3> absoluteProperties,
		WorldTransformPrecision precision,
		std::uint32_t spanOffset,
		std::uint32_t spanSize);

	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	std::shared_ptr<const Runtime::EngineSnapshot> m_Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> m_Types;
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> m_Reflection;
	std::shared_ptr<const Runtime::WorldSnapshot> m_World;
	Runtime::WorldSnapshotObject m_Actor;
	Runtime::WorldSnapshotObject m_RootComponent;
	const Runtime::ReflectedProperty* m_ActorRootProperty = nullptr;
	std::array<const Runtime::ReflectedProperty*, 3> m_TransformProperties{};
	std::array<const Runtime::ReflectedProperty*, 3> m_AbsoluteProperties{};
	std::uint32_t m_SpanOffset = 0;
	std::vector<std::byte> m_FirstBytes;
	std::vector<std::byte> m_StableBytes;
	std::vector<std::byte> m_FinalBytes;
	WorldTransformExecutionError m_Error = WorldTransformExecutionError::None;
	Runtime::HandleError m_HandleError = Runtime::HandleError::None;
	Runtime::MemoryError m_MemoryError = Runtime::MemoryError::None;
	std::string m_DecodeErrorCode;
	std::string m_DecodeError;
	WorldTransformValue m_Value;
};

struct WorldTransformReadPreparation
{
	std::shared_ptr<WorldTransformReadWork> Work;
	std::optional<WorldTransformCommandError> Error;

	bool Ok() const noexcept { return static_cast<bool>(Work) && !Error; }
};

struct WorldTransformCommandResult
{
	json Data = nullptr;
	std::optional<WorldTransformCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

class WorldTransformCommandService final
{
public:
	static constexpr std::size_t kMaxAggregateBytes = 64 * 1024;

	static WorldTransformReadPreparation PrepareRead(
		const json& data,
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine) noexcept;
	static WorldTransformCommandResult CompleteRead(
		const WorldTransformReadWork& work) noexcept;
};

} // namespace UExplorer::Services
