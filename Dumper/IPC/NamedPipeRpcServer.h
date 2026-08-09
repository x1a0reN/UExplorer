#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/GameThreadExecutor.h"
#include "Services/CoreCommandService.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace UExplorer::IPC
{

struct NamedPipeServerDiagnostics
{
	bool Started = false;
	bool Listening = false;
	bool AdmissionsOpen = false;
	bool StopRequested = false;
	std::uint32_t TargetProcessId = 0;
	std::uint32_t ConnectedClientProcessId = 0;
	std::size_t PendingRequests = 0;
	std::size_t QueuedRequests = 0;
	std::size_t WorkerCount = 0;
	std::uint64_t AcceptedConnections = 0;
	std::uint64_t RejectedConnections = 0;
	std::uint64_t CompletedRequests = 0;
	std::uint64_t CancelledRequests = 0;
	std::size_t QueuedEvents = 0;
	bool EventWriterRunning = false;
	std::uint64_t EnqueuedEvents = 0;
	std::uint64_t SentEvents = 0;
	std::uint64_t DroppedEvents = 0;
	std::uint32_t LastNativeError = 0;
	std::string LastErrorCode;
	std::string LastErrorMessage;
};

enum class EventPublishResult : std::uint8_t
{
	Accepted,
	DroppedOldest,
	Unavailable,
	InvalidKind,
	InvalidTimestamp,
	PayloadTooLarge,
	SequenceExhausted,
	PublishFailed
};

class NamedPipeRpcServer final
{
public:
	using ShutdownCallback = std::function<void()>;

	static constexpr std::size_t kWorkerCount = 4;
	static constexpr std::size_t kPendingRequestLimit = 256;
	static constexpr std::size_t kIoChunkBytes = 64 * 1024;
	static constexpr std::size_t kEventQueueCapacity = 1024;
	static constexpr std::size_t kMaxEventPayloadBytes = 64 * 1024;

	NamedPipeRpcServer(
		Runtime::CoreRuntime& runtime,
		Services::CoreCommandService& commandService,
		Runtime::GameThreadExecutor& gameThread,
		ShutdownCallback shutdownCallback);
	~NamedPipeRpcServer();

	NamedPipeRpcServer(const NamedPipeRpcServer&) = delete;
	NamedPipeRpcServer& operator=(const NamedPipeRpcServer&) = delete;

	bool Start();
	bool OpenAdmissions();
	EventPublishResult PublishEvent(
		std::string kind,
		std::uint64_t timestampUs,
		nlohmann::json data) noexcept;
	bool Stop(std::chrono::milliseconds timeout);
	bool IsListening() const noexcept;
	std::wstring PipeName() const;
	NamedPipeServerDiagnostics Diagnostics() const;

private:
	class Impl;
	std::unique_ptr<Impl> m_Impl;
};

} // namespace UExplorer::IPC
