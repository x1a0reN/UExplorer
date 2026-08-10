#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/ParamFrame.h"
#include "Utils/Json/json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct FunctionCallCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

enum class FunctionCallExecutionError : std::uint8_t
{
	None,
	DependencyChanged,
	ExecutionThreadInvalid,
	TargetHandleStale,
	FunctionHandleStale,
	ArgumentHandleStale,
	ProcessEventUnavailable
};

struct FunctionCallInput
{
	const Runtime::ReflectedParameter* Parameter = nullptr;
	Runtime::PropertyScalar Value;
};

struct FunctionCallOutput
{
	std::string Name;
	Runtime::ReflectedParameterDirection Direction =
		Runtime::ReflectedParameterDirection::Output;
	Runtime::PropertyValue Value;
};

class FunctionCallWork final : public Runtime::IGameThreadWork
{
public:
	bool Execute() override;
	FunctionCallExecutionError ExecutionError() const noexcept { return m_Error; }
	Runtime::HandleError HandleError() const noexcept { return m_HandleError; }
	const std::string& FailedArgument() const noexcept { return m_FailedArgument; }
	const Runtime::ObjectHandle& Target() const noexcept { return m_Target; }
	const Runtime::FunctionHandle& FunctionHandle() const noexcept;
	const Runtime::ReflectedFunction& Function() const noexcept { return *m_Function; }
	const std::vector<FunctionCallOutput>& Outputs() const noexcept { return m_Outputs; }
	std::uint64_t TypeSnapshotGeneration() const noexcept;
	std::uint64_t ObjectSnapshotGeneration() const noexcept;
	bool Invoked() const noexcept { return m_Invoked; }

private:
	friend class FunctionCallCommandService;
	FunctionCallWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& gameThread,
		std::shared_ptr<const Runtime::EngineSnapshot> objects,
		std::shared_ptr<const Runtime::TypeSnapshot> types,
		std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
		Runtime::ObjectHandle target,
		const Runtime::ReflectedFunction& function,
		std::vector<FunctionCallInput> inputs,
		Runtime::ParamFrame frame,
		std::size_t outputCount);

	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	Runtime::GameThreadExecutor& m_GameThread;
	std::shared_ptr<const Runtime::EngineSnapshot> m_Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> m_Types;
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> m_Reflection;
	Runtime::ObjectHandle m_Target;
	const Runtime::ReflectedFunction* m_Function = nullptr;
	std::vector<FunctionCallInput> m_Inputs;
	Runtime::ParamFrame m_Frame;
	std::vector<FunctionCallOutput> m_Outputs;
	FunctionCallExecutionError m_Error = FunctionCallExecutionError::None;
	Runtime::HandleError m_HandleError = Runtime::HandleError::None;
	std::string m_FailedArgument;
	bool m_Invoked = false;
};

struct FunctionCallPreparation
{
	std::shared_ptr<FunctionCallWork> Work;
	std::optional<FunctionCallCommandError> Error;

	bool Ok() const noexcept { return static_cast<bool>(Work) && !Error; }
};

struct FunctionCallCommandResult
{
	json Data = nullptr;
	std::optional<FunctionCallCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

class FunctionCallCommandService final
{
public:
	static constexpr std::size_t kMaxParameters = 128;
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;

	static FunctionCallPreparation PrepareInvoke(
		const json& data,
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& gameThread) noexcept;
	static FunctionCallCommandResult CompleteInvoke(
		const FunctionCallWork& work) noexcept;
};

} // namespace UExplorer::Services
