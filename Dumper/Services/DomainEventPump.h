#pragma once

#include "IPC/NamedPipeRpcServer.h"
#include "Runtime/WatchScheduler.h"
#include "Services/HookCommandService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace UExplorer::Services
{

enum class DomainEventPumpError : std::uint8_t
{
	None,
	InvalidConfiguration,
	AlreadyStarted,
	WorkerStartFailed,
	InvalidDeadline,
	WorkerThreadDrainDenied,
	DrainTimedOut,
	WatchDrainFailed,
	HookDrainFailed,
	PublishFailed,
	WorkerExitedUnexpectedly
};

const char* ToString(DomainEventPumpError error) noexcept;

struct DomainEventPumpLimits
{
	std::size_t MaxWatchEventsPerBatch = 64;
	std::size_t MaxHookEventsPerBatch = 128;
	std::size_t MaxSerializedDataBytes = 48 * 1024;
	std::size_t ShutdownQuietPasses = 16;
	std::chrono::milliseconds PollInterval{10};
	std::chrono::milliseconds ShutdownQuietInterval{1};
};

struct DomainEventPumpDiagnostics
{
	bool Configured = false;
	bool Started = false;
	bool Running = false;
	bool StopRequested = false;
	bool Stopped = false;
	std::uint64_t PublishedWatchEvents = 0;
	std::uint64_t PublishedHookEvents = 0;
	std::uint64_t PublishDroppedEvents = 0;
	std::uint64_t TransportQueueEvictions = 0;
	std::uint64_t PayloadOmissions = 0;
	std::uint64_t WatchSourceDroppedEvents = 0;
	std::uint64_t WatchSourceCoalescedEvents = 0;
	std::uint64_t HookSourceDroppedEvents = 0;
	DomainEventPumpError LastError = DomainEventPumpError::None;
};

struct DomainEventPumpStopResult
{
	DomainEventPumpError Error = DomainEventPumpError::None;
	bool Ok() const noexcept { return Error == DomainEventPumpError::None; }
};

// Drains transport-specific copies only. Watch pull history/events and Hook
// retained logs remain independently queryable through their command services.
class DomainEventPump final
{
public:
	DomainEventPump(
		Runtime::WatchScheduler& watches,
		HookCommandService& hooks,
		IPC::NamedPipeRpcServer& pipe,
		DomainEventPumpLimits limits = {}) noexcept;
	~DomainEventPump();

	DomainEventPump(const DomainEventPump&) = delete;
	DomainEventPump& operator=(const DomainEventPump&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	bool Start() noexcept;
	DomainEventPumpStopResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;
	DomainEventPumpDiagnostics Diagnostics() const noexcept;

private:
	bool DrainWatchBatch() noexcept;
	bool DrainHookBatch() noexcept;
	bool Publish(
		const char* kind,
		std::uint64_t timestampUs,
		nlohmann::json data,
		bool watchEvent) noexcept;
	void WorkerLoop() noexcept;
	void JoinWorkerNoexcept() noexcept;
	void SetError(DomainEventPumpError error) noexcept;

	Runtime::WatchScheduler& m_Watches;
	HookCommandService& m_Hooks;
	IPC::NamedPipeRpcServer& m_Pipe;
	DomainEventPumpLimits m_Limits;
	bool m_Configured = false;
	std::thread m_Worker;
	std::thread::id m_WorkerId;
	mutable std::mutex m_LifecycleMutex;
	std::condition_variable m_LifecycleCondition;
	std::atomic<bool> m_Started{false};
	std::atomic<bool> m_Running{false};
	std::atomic<bool> m_StopRequested{false};
	std::atomic<bool> m_Exited{false};
	std::atomic<bool> m_Stopped{false};
	std::atomic<std::uint64_t> m_PublishedWatchEvents{0};
	std::atomic<std::uint64_t> m_PublishedHookEvents{0};
	std::atomic<std::uint64_t> m_PublishDroppedEvents{0};
	std::atomic<std::uint64_t> m_TransportQueueEvictions{0};
	std::atomic<std::uint64_t> m_PayloadOmissions{0};
	std::atomic<std::uint64_t> m_WatchSourceDroppedEvents{0};
	std::atomic<std::uint64_t> m_WatchSourceCoalescedEvents{0};
	std::atomic<std::uint64_t> m_HookSourceDroppedEvents{0};
	std::atomic<DomainEventPumpError> m_LastError{DomainEventPumpError::None};
};

} // namespace UExplorer::Services
