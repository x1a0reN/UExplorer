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
	std::uint32_t LastNativeError = 0;
	std::string LastErrorCode;
	std::string LastErrorMessage;
};

class NamedPipeRpcServer final
{
public:
	using ShutdownCallback = std::function<void()>;

	static constexpr std::size_t kWorkerCount = 4;
	static constexpr std::size_t kPendingRequestLimit = 256;
	static constexpr std::size_t kIoChunkBytes = 64 * 1024;

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
	bool Stop(std::chrono::milliseconds timeout);
	bool IsListening() const noexcept;
	std::wstring PipeName() const;
	NamedPipeServerDiagnostics Diagnostics() const;

private:
	class Impl;
	std::unique_ptr<Impl> m_Impl;
};

} // namespace UExplorer::IPC
