#pragma once

#include "CoreStatusDiagnostics.h"
#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Utils/Json/json.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace UExplorer::Runtime
{
class ObjectSnapshotReflectionCandidateSource;
class ObjectSnapshotTypeCandidateSource;
class WatchScheduler;
class IBlueprintBytecodeCaptureSource;
class IBlueprintBytecodeProfileSource;
}

namespace UExplorer::Services
{

using json = nlohmann::json;

class DumpCommandService;
class FunctionCallBatchCommandService;
class HookCommandService;

struct CoreCommandRequest
{
	std::uint64_t RequestId = 0;
	std::string Operation;
	std::string SessionId;
	int TimeoutMs = 0;
	json Data = json::object();
};

struct CoreCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct CoreCommandTiming
{
	std::uint64_t QueuedUs = 0;
	std::uint64_t ExecuteUs = 0;
};

struct CoreCommandResponse
{
	bool Ok = false;
	std::uint64_t RequestId = 0;
	std::string SessionId;
	json Data = nullptr;
	std::optional<CoreCommandError> Error;
	CoreCommandTiming Timing;
};

using GameThreadQueuedCallback = std::function<void(const Runtime::GameThreadTicket&)>;

class CoreCommandService final
{
public:
	CoreCommandService(
		Runtime::CoreRuntime& runtime,
		Runtime::GameThreadExecutor& gameThread,
		Runtime::EngineFacade& engine,
		ICoreStatusDiagnosticsSource& statusDiagnostics,
		const Runtime::ObjectSnapshotReflectionCandidateSource* reflectionSource = nullptr,
		const Runtime::ObjectSnapshotTypeCandidateSource* typeSource = nullptr,
		Runtime::WatchScheduler* watchScheduler = nullptr,
		Runtime::IBlueprintBytecodeCaptureSource* blueprintCaptureSource = nullptr,
		const Runtime::IBlueprintBytecodeProfileSource* blueprintProfileSource = nullptr,
		HookCommandService* hookCommandService = nullptr,
		DumpCommandService* dumpCommandService = nullptr,
		FunctionCallBatchCommandService* functionCallBatchCommandService = nullptr);

	CoreCommandService(const CoreCommandService&) = delete;
	CoreCommandService& operator=(const CoreCommandService&) = delete;

	bool IsConfigured() const noexcept;
	const std::string& SessionId() const noexcept { return m_SessionId; }
	CoreCommandResponse Execute(
		const CoreCommandRequest& request,
		const GameThreadQueuedCallback& onGameThreadQueued = {}) noexcept;

private:
	CoreCommandResponse ExecuteStatus(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteSnapshotPage(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteTypeCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteObjectPropertyRead(
		const CoreCommandRequest& request,
		const GameThreadQueuedCallback& onGameThreadQueued);
	CoreCommandResponse ExecuteFunctionCall(
		const CoreCommandRequest& request,
		const GameThreadQueuedCallback& onGameThreadQueued);
	CoreCommandResponse ExecuteFunctionCallBatch(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteWorldTransformRead(
		const CoreCommandRequest& request,
		const GameThreadQueuedCallback& onGameThreadQueued);
	CoreCommandResponse ExecuteWorldTransformUpdate(
		const CoreCommandRequest& request,
		const GameThreadQueuedCallback& onGameThreadQueued);
	CoreCommandResponse ExecuteMemoryCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteWatchCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteBlueprintCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteHookCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteDumpCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteWorldCommand(const CoreCommandRequest& request);
	CoreCommandResponse ExecuteHandleIssue(
		const CoreCommandRequest& request,
		bool functionHandle,
		const GameThreadQueuedCallback& onGameThreadQueued);
	CoreCommandResponse Success(
		const CoreCommandRequest& request,
		json data,
		CoreCommandTiming timing = {}) const;
	CoreCommandResponse Failure(
		const CoreCommandRequest& request,
		std::string code,
		std::string message,
		json details = json::object(),
		CoreCommandTiming timing = {}) const;

	Runtime::CoreRuntime& m_Runtime;
	Runtime::GameThreadExecutor& m_GameThread;
	Runtime::EngineFacade& m_Engine;
	ICoreStatusDiagnosticsSource& m_StatusDiagnostics;
	const Runtime::ObjectSnapshotReflectionCandidateSource* m_ReflectionSource = nullptr;
	const Runtime::ObjectSnapshotTypeCandidateSource* m_TypeSource = nullptr;
	Runtime::WatchScheduler* m_WatchScheduler = nullptr;
	Runtime::IBlueprintBytecodeCaptureSource* m_BlueprintCaptureSource = nullptr;
	const Runtime::IBlueprintBytecodeProfileSource* m_BlueprintProfileSource = nullptr;
	HookCommandService* m_HookCommandService = nullptr;
	DumpCommandService* m_DumpCommandService = nullptr;
	FunctionCallBatchCommandService* m_FunctionCallBatchCommandService = nullptr;
	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
};

} // namespace UExplorer::Services
