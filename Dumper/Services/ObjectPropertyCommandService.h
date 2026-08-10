#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/PropertyCodec.h"
#include "Utils/Json/json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct ObjectPropertyCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

enum class ObjectPropertyExecutionError : std::uint8_t
{
	None,
	DependencyChanged,
	ExecutionThreadInvalid,
	ObjectHandleStale,
	AddressOverflow
};

class ObjectPropertyReadWork final : public Runtime::IGameThreadWork
{
public:
	bool Execute() override;
	ObjectPropertyExecutionError ExecutionError() const noexcept { return m_Error; }
	Runtime::HandleError HandleError() const noexcept { return m_HandleError; }
	const Runtime::PropertyValue& Value() const noexcept { return m_Value; }
	const Runtime::ObjectHandle& Object() const noexcept { return m_Object; }
	std::uint64_t TypeSnapshotGeneration() const noexcept;
	std::uint64_t ObjectSnapshotGeneration() const noexcept;
	const std::string& DeclaringTypePath() const noexcept { return m_DeclaringTypePath; }
	const std::string& PropertyName() const noexcept { return m_PropertyName; }
	std::uint32_t ArrayIndex() const noexcept { return m_ArrayIndex; }
	std::uint32_t PropertyOffset() const noexcept;
	std::uint32_t PropertySize() const noexcept;

private:
	friend class ObjectPropertyCommandService;
	ObjectPropertyReadWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		std::shared_ptr<const Runtime::EngineSnapshot> objects,
		std::shared_ptr<const Runtime::TypeSnapshot> types,
		std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
		Runtime::ObjectHandle object,
		const Runtime::ReflectedProperty& property,
		std::string declaringTypePath,
		std::uint32_t arrayIndex);

	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	std::shared_ptr<const Runtime::EngineSnapshot> m_Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> m_Types;
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> m_Reflection;
	Runtime::ObjectHandle m_Object;
	const Runtime::ReflectedProperty* m_Property = nullptr;
	std::string m_DeclaringTypePath;
	std::string m_PropertyName;
	std::uint32_t m_ArrayIndex = 0;
	ObjectPropertyExecutionError m_Error = ObjectPropertyExecutionError::None;
	Runtime::HandleError m_HandleError = Runtime::HandleError::None;
	Runtime::PropertyValue m_Value;
};

struct ObjectPropertyReadPreparation
{
	std::shared_ptr<ObjectPropertyReadWork> Work;
	std::optional<ObjectPropertyCommandError> Error;

	bool Ok() const noexcept { return static_cast<bool>(Work) && !Error; }
};

struct ObjectPropertyCommandResult
{
	json Data = nullptr;
	std::optional<ObjectPropertyCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

class ObjectPropertyCommandService final
{
public:
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;

	static ObjectPropertyReadPreparation PrepareRead(
		const json& data,
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine) noexcept;
	static ObjectPropertyCommandResult CompleteRead(
		const ObjectPropertyReadWork& work) noexcept;
};

} // namespace UExplorer::Services
