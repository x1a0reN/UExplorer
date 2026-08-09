#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

using ProcessEventFn = void(*)(void*, void*, void*);

enum class GameThreadTaskState : std::uint8_t
{
	Queued,
	Running,
	Completed,
	Failed,
	Cancelled,
	Expired
};

enum class GameThreadSubmitResult : std::uint8_t
{
	Completed,
	Disabled,
	Cancelled,
	PumpThreadWaitDenied,
	QueueBusy,
	TimedOutBeforeStart,
	TimedOutWhileRunning,
	ExecutionFailed
};

enum class GameThreadQueueResult : std::uint8_t
{
	Accepted,
	Disabled,
	QueueBusy,
	Invalid
};

enum class GameThreadCancelResult : std::uint8_t
{
	Cancelled,
	Running,
	Terminal,
	Invalid
};

class IGameThreadWork
{
public:
	virtual ~IGameThreadWork() = default;
	virtual bool Execute() = 0;
};

struct GameThreadDiagnostics
{
	bool Enabled = false;
	bool Processing = false;
	bool PumpObserved = false;
	bool PumpThreadStable = false;
	std::uint32_t PumpThreadId = 0;
	std::uint64_t LastPumpTickMonotonicUs = 0;
	std::uint64_t PumpTickCount = 0;
	std::uint64_t LastTaskDurationUs = 0;
	std::size_t QueueDepth = 0;
	std::size_t Capacity = 0;
};

struct GameThreadTaskTiming
{
	std::uint64_t QueuedUs = 0;
	std::uint64_t ExecuteUs = 0;
};

struct GameThreadTaskControl;

class GameThreadTicket final
{
public:
	GameThreadTicket() = default;
	explicit operator bool() const noexcept { return static_cast<bool>(m_Control); }

private:
	friend class GameThreadExecutor;
	explicit GameThreadTicket(std::shared_ptr<GameThreadTaskControl> control)
		: m_Control(std::move(control))
	{
	}

	std::shared_ptr<GameThreadTaskControl> m_Control;
};

class GameThreadExecutor final
{
public:
	static constexpr std::size_t kCapacity = 128;
	static constexpr int kMaxTimeoutMs = 120000;

	GameThreadExecutor() = default;
	GameThreadExecutor(const GameThreadExecutor&) = delete;
	GameThreadExecutor& operator=(const GameThreadExecutor&) = delete;

	bool Enable(ProcessEventFn processEvent);
	bool DisableAndDrain(int timeoutMs = 5000);

	GameThreadQueueResult Enqueue(
		std::shared_ptr<IGameThreadWork> work,
		std::chrono::steady_clock::time_point deadline,
		GameThreadTicket& ticket);
	GameThreadSubmitResult Wait(const GameThreadTicket& ticket);
	GameThreadCancelResult Cancel(const GameThreadTicket& ticket);
	bool TryGetTiming(const GameThreadTicket& ticket, GameThreadTaskTiming& timing) const;

	GameThreadSubmitResult SubmitOwned(
		std::shared_ptr<IGameThreadWork> work,
		int timeoutMs = 5000);
	GameThreadSubmitResult SubmitProcessEvent(
		void* object,
		void* function,
		std::vector<std::uint8_t>& params,
		int timeoutMs = 5000);

	void Pump() noexcept;
	bool IsEnabled() const noexcept;
	bool HasPending() const;
	std::size_t QueueDepth() const;
	GameThreadDiagnostics GetDiagnostics() const;
	bool IsCurrentPumpThread() const noexcept;

private:
	GameThreadSubmitResult SubmitPrepared(
		std::shared_ptr<IGameThreadWork> work,
		int timeoutMs);
	GameThreadSubmitResult ResultForTerminalState(GameThreadTaskState state) const noexcept;
	bool InvokeGuarded(IGameThreadWork* work) noexcept;
	void RemoveQueuedLocked(const std::shared_ptr<GameThreadTaskControl>& task);

	mutable std::mutex m_Mutex;
	std::condition_variable m_Condition;
	std::deque<std::shared_ptr<GameThreadTaskControl>> m_Queue;
	std::atomic<bool> m_Enabled{false};
	ProcessEventFn m_ProcessEvent = nullptr;
	std::atomic<bool> m_Processing{false};
	std::atomic<std::uint32_t> m_PumpThreadId{0};
	std::atomic<bool> m_PumpThreadMismatch{false};
	std::atomic<std::uint64_t> m_LastPumpTickMonotonicUs{0};
	std::atomic<std::uint64_t> m_PumpTickCount{0};
	std::atomic<std::uint64_t> m_LastTaskDurationUs{0};
};

class IGameThreadPump
{
public:
	virtual ~IGameThreadPump() = default;
	virtual void Tick() noexcept = 0;
	virtual const char* BackendName() const noexcept = 0;
};

class PostRenderPumpBackend final : public IGameThreadPump
{
public:
	explicit PostRenderPumpBackend(GameThreadExecutor& executor) noexcept
		: m_Executor(executor)
	{
	}

	void Tick() noexcept override;
	const char* BackendName() const noexcept override { return "post_render_vtable"; }

private:
	GameThreadExecutor& m_Executor;
};

GameThreadExecutor& GetGameThreadExecutor();
PostRenderPumpBackend& GetPostRenderPumpBackend();

} // namespace UExplorer::Runtime
