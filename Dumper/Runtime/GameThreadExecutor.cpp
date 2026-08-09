#include "GameThreadExecutor.h"

#include <algorithm>
#include <utility>

namespace UExplorer::Runtime
{

struct GameThreadTaskControl
{
	std::shared_ptr<IGameThreadWork> Work;
	GameThreadTaskState State = GameThreadTaskState::Queued;
	std::chrono::steady_clock::time_point Deadline;
	std::chrono::steady_clock::time_point EnqueuedAt;
	std::chrono::steady_clock::time_point StartedAt;
	std::chrono::steady_clock::time_point FinishedAt;
};

namespace
{

std::uint64_t MonotonicMicroseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

class ProcessEventWork final : public IGameThreadWork
{
public:
	ProcessEventWork(
		const ProcessEventFn processEvent,
		void* object,
		void* function,
		std::vector<std::uint8_t> params)
		: m_ProcessEvent(processEvent),
		  m_Object(object),
		  m_Function(function),
		  m_Params(std::move(params))
	{
	}

	bool Execute() override
	{
		m_ProcessEvent(
			m_Object,
			m_Function,
			m_Params.empty() ? nullptr : m_Params.data());
		return true;
	}

	const std::vector<std::uint8_t>& Params() const noexcept { return m_Params; }

private:
	ProcessEventFn m_ProcessEvent;
	void* m_Object;
	void* m_Function;
	std::vector<std::uint8_t> m_Params;
};

bool InvokeCppExceptionGuard(IGameThreadWork* work) noexcept
{
	try
	{
		return work && work->Execute();
	}
	catch (...)
	{
		return false;
	}
}

} // namespace

bool GameThreadExecutor::Enable(const ProcessEventFn processEvent)
{
	if (!processEvent)
		return false;

	std::lock_guard<std::mutex> lock(m_Mutex);
	if (m_Enabled.load(std::memory_order_acquire)
		|| m_Processing.load(std::memory_order_acquire)
		|| !m_Queue.empty())
	{
		return false;
	}

	m_ProcessEvent = processEvent;
	m_PumpThreadId.store(0, std::memory_order_release);
	m_PumpThreadMismatch.store(false, std::memory_order_release);
	m_LastPumpTickMonotonicUs.store(0, std::memory_order_release);
	m_PumpTickCount.store(0, std::memory_order_release);
	m_LastTaskDurationUs.store(0, std::memory_order_release);
	m_Enabled.store(true, std::memory_order_release);
	m_Condition.notify_all();
	return true;
}

bool GameThreadExecutor::DisableAndDrain(const int timeoutMs)
{
	std::vector<std::shared_ptr<IGameThreadWork>> releasedWork;
	std::unique_lock<std::mutex> lock(m_Mutex);
	m_Enabled.store(false, std::memory_order_release);
	releasedWork.reserve(m_Queue.size());
	for (const auto& task : m_Queue)
	{
		if (task->State == GameThreadTaskState::Queued)
		{
			task->State = GameThreadTaskState::Cancelled;
			task->FinishedAt = std::chrono::steady_clock::now();
			releasedWork.push_back(std::move(task->Work));
		}
	}
	m_Queue.clear();
	m_Condition.notify_all();

	const bool drained = m_Condition.wait_for(
		lock,
		std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0),
		[this] {
			return !m_Processing.load(std::memory_order_acquire) && m_Queue.empty();
		});
	if (drained)
		m_ProcessEvent = nullptr;
	return drained;
}

GameThreadQueueResult GameThreadExecutor::Enqueue(
	std::shared_ptr<IGameThreadWork> work,
	const std::chrono::steady_clock::time_point deadline,
	GameThreadTicket& ticket)
{
	ticket = {};
	const auto now = std::chrono::steady_clock::now();
	if (!work
		|| deadline <= now
		|| deadline - now > std::chrono::milliseconds(kMaxTimeoutMs))
		return GameThreadQueueResult::Invalid;

	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!m_Enabled.load(std::memory_order_acquire) || !m_ProcessEvent)
		return GameThreadQueueResult::Disabled;
	const std::size_t pending = m_Queue.size()
		+ (m_Processing.load(std::memory_order_acquire) ? 1U : 0U);
	if (pending >= kCapacity)
		return GameThreadQueueResult::QueueBusy;

	auto control = std::make_shared<GameThreadTaskControl>();
	control->Work = std::move(work);
	control->Deadline = deadline;
	control->EnqueuedAt = now;
	m_Queue.push_back(control);
	ticket = GameThreadTicket(control);
	m_Condition.notify_all();
	return GameThreadQueueResult::Accepted;
}

GameThreadSubmitResult GameThreadExecutor::Wait(const GameThreadTicket& ticket)
{
	if (!ticket.m_Control)
		return GameThreadSubmitResult::ExecutionFailed;

	const std::shared_ptr<GameThreadTaskControl> task = ticket.m_Control;
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (IsCurrentPumpThread()
		&& task->State != GameThreadTaskState::Completed
		&& task->State != GameThreadTaskState::Failed
		&& task->State != GameThreadTaskState::Cancelled
		&& task->State != GameThreadTaskState::Expired)
	{
		return GameThreadSubmitResult::PumpThreadWaitDenied;
	}
	const bool terminal = m_Condition.wait_until(lock, task->Deadline, [&task] {
		return task->State == GameThreadTaskState::Completed
			|| task->State == GameThreadTaskState::Failed
			|| task->State == GameThreadTaskState::Cancelled
			|| task->State == GameThreadTaskState::Expired;
	});
	if (terminal)
		return ResultForTerminalState(task->State);

	if (task->State == GameThreadTaskState::Queued)
	{
		task->State = GameThreadTaskState::Expired;
		task->FinishedAt = std::chrono::steady_clock::now();
		std::shared_ptr<IGameThreadWork> releasedWork = std::move(task->Work);
		RemoveQueuedLocked(task);
		lock.unlock();
		m_Condition.notify_all();
		releasedWork.reset();
		return GameThreadSubmitResult::TimedOutBeforeStart;
	}
	if (task->State == GameThreadTaskState::Running)
		return GameThreadSubmitResult::TimedOutWhileRunning;
	return ResultForTerminalState(task->State);
}

GameThreadCancelResult GameThreadExecutor::Cancel(const GameThreadTicket& ticket)
{
	if (!ticket.m_Control)
		return GameThreadCancelResult::Invalid;

	const std::shared_ptr<GameThreadTaskControl> task = ticket.m_Control;
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (task->State == GameThreadTaskState::Queued)
	{
		task->State = GameThreadTaskState::Cancelled;
		task->FinishedAt = std::chrono::steady_clock::now();
		std::shared_ptr<IGameThreadWork> releasedWork = std::move(task->Work);
		RemoveQueuedLocked(task);
		lock.unlock();
		m_Condition.notify_all();
		releasedWork.reset();
		return GameThreadCancelResult::Cancelled;
	}
	if (task->State == GameThreadTaskState::Running)
		return GameThreadCancelResult::Running;
	return GameThreadCancelResult::Terminal;
}

bool GameThreadExecutor::TryGetTiming(
	const GameThreadTicket& ticket,
	GameThreadTaskTiming& timing) const
{
	timing = {};
	if (!ticket.m_Control)
		return false;

	std::lock_guard<std::mutex> lock(m_Mutex);
	const GameThreadTaskControl& task = *ticket.m_Control;
	if (task.EnqueuedAt.time_since_epoch().count() == 0)
		return false;
	const auto now = std::chrono::steady_clock::now();
	const auto queueEnd = task.StartedAt.time_since_epoch().count() != 0
		? task.StartedAt
		: (task.FinishedAt.time_since_epoch().count() != 0 ? task.FinishedAt : now);
	const auto executionEnd = task.FinishedAt.time_since_epoch().count() != 0
		? task.FinishedAt
		: now;
	timing.QueuedUs = queueEnd >= task.EnqueuedAt
		? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			queueEnd - task.EnqueuedAt).count())
		: 0;
	timing.ExecuteUs = task.StartedAt.time_since_epoch().count() != 0
		&& executionEnd >= task.StartedAt
		? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			executionEnd - task.StartedAt).count())
		: 0;
	return true;
}

GameThreadSubmitResult GameThreadExecutor::SubmitOwned(
	std::shared_ptr<IGameThreadWork> work,
	const int timeoutMs)
{
	return SubmitPrepared(std::move(work), timeoutMs);
}

GameThreadSubmitResult GameThreadExecutor::SubmitProcessEvent(
	void* object,
	void* function,
	std::vector<std::uint8_t>& params,
	const int timeoutMs)
{
	if (!object || !function || timeoutMs <= 0 || timeoutMs > kMaxTimeoutMs)
		return GameThreadSubmitResult::ExecutionFailed;
	if (IsCurrentPumpThread())
		return GameThreadSubmitResult::PumpThreadWaitDenied;

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeoutMs);
	std::shared_ptr<ProcessEventWork> work;
	GameThreadTicket ticket;
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Enabled.load(std::memory_order_acquire) || !m_ProcessEvent)
			return GameThreadSubmitResult::Disabled;
		const std::size_t pending = m_Queue.size()
			+ (m_Processing.load(std::memory_order_acquire) ? 1U : 0U);
		if (pending >= kCapacity)
			return GameThreadSubmitResult::QueueBusy;

		work = std::make_shared<ProcessEventWork>(
			m_ProcessEvent,
			object,
			function,
			params);
		auto control = std::make_shared<GameThreadTaskControl>();
		control->Work = work;
		control->Deadline = deadline;
		control->EnqueuedAt = std::chrono::steady_clock::now();
		m_Queue.push_back(control);
		ticket = GameThreadTicket(control);
	}
	m_Condition.notify_all();
	const GameThreadSubmitResult result = Wait(ticket);
	if (result == GameThreadSubmitResult::Completed)
		params = work->Params();
	return result;
}

GameThreadSubmitResult GameThreadExecutor::SubmitPrepared(
	std::shared_ptr<IGameThreadWork> work,
	const int timeoutMs)
{
	if (!work || timeoutMs <= 0 || timeoutMs > kMaxTimeoutMs)
		return GameThreadSubmitResult::ExecutionFailed;
	if (IsCurrentPumpThread())
		return GameThreadSubmitResult::PumpThreadWaitDenied;

	GameThreadTicket ticket;
	const GameThreadQueueResult queued = Enqueue(
		std::move(work),
		std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs),
		ticket);
	switch (queued)
	{
	case GameThreadQueueResult::Accepted:
		return Wait(ticket);
	case GameThreadQueueResult::Disabled:
		return GameThreadSubmitResult::Disabled;
	case GameThreadQueueResult::QueueBusy:
		return GameThreadSubmitResult::QueueBusy;
	case GameThreadQueueResult::Invalid:
		return GameThreadSubmitResult::ExecutionFailed;
	}
	return GameThreadSubmitResult::ExecutionFailed;
}

void GameThreadExecutor::Pump() noexcept
{
	if (!m_Enabled.load(std::memory_order_acquire))
		return;

	const std::uint32_t currentThreadId = GetCurrentThreadId();
	std::uint32_t expectedThreadId = 0;
	if (!m_PumpThreadId.compare_exchange_strong(
		expectedThreadId,
		currentThreadId,
		std::memory_order_acq_rel,
		std::memory_order_acquire)
		&& expectedThreadId != currentThreadId)
	{
		m_PumpThreadMismatch.store(true, std::memory_order_release);
	}
	m_LastPumpTickMonotonicUs.store(MonotonicMicroseconds(), std::memory_order_release);
	m_PumpTickCount.fetch_add(1, std::memory_order_acq_rel);
	if (m_PumpThreadMismatch.load(std::memory_order_acquire))
		return;

	if (m_Processing.exchange(true, std::memory_order_acq_rel))
		return;

	std::shared_ptr<GameThreadTaskControl> task;
	std::vector<std::shared_ptr<IGameThreadWork>> discardedWork;
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		while (!m_Queue.empty())
		{
			task = m_Queue.front();
			m_Queue.pop_front();
			if (task->State != GameThreadTaskState::Queued)
			{
				task.reset();
				continue;
			}
			if (std::chrono::steady_clock::now() >= task->Deadline)
			{
				task->State = GameThreadTaskState::Expired;
				task->FinishedAt = std::chrono::steady_clock::now();
				discardedWork.push_back(std::move(task->Work));
				task.reset();
				m_Condition.notify_all();
				continue;
			}
			if (!m_Enabled.load(std::memory_order_acquire))
			{
				task->State = GameThreadTaskState::Cancelled;
				task->FinishedAt = std::chrono::steady_clock::now();
				discardedWork.push_back(std::move(task->Work));
				task.reset();
				m_Condition.notify_all();
				continue;
			}
			task->State = GameThreadTaskState::Running;
			task->StartedAt = std::chrono::steady_clock::now();
			break;
		}
	}

	if (!task)
	{
		m_Processing.store(false, std::memory_order_release);
		m_Condition.notify_all();
		return;
	}

	const std::uint64_t startedUs = MonotonicMicroseconds();
	const bool succeeded = InvokeGuarded(task->Work.get());
	const std::uint64_t finishedUs = MonotonicMicroseconds();
	m_LastTaskDurationUs.store(
		finishedUs >= startedUs ? finishedUs - startedUs : 0,
		std::memory_order_release);

	std::shared_ptr<IGameThreadWork> releasedWork;
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		task->State = succeeded
			? GameThreadTaskState::Completed
			: GameThreadTaskState::Failed;
		task->FinishedAt = std::chrono::steady_clock::now();
		releasedWork = std::move(task->Work);
		m_Processing.store(false, std::memory_order_release);
	}
	m_Condition.notify_all();
	releasedWork.reset();
}

bool GameThreadExecutor::IsEnabled() const noexcept
{
	return m_Enabled.load(std::memory_order_acquire);
}

bool GameThreadExecutor::HasPending() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Processing.load(std::memory_order_acquire) || !m_Queue.empty();
}

std::size_t GameThreadExecutor::QueueDepth() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Queue.size();
}

GameThreadDiagnostics GameThreadExecutor::GetDiagnostics() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	const std::uint64_t tickCount = m_PumpTickCount.load(std::memory_order_acquire);
	const bool threadMismatch = m_PumpThreadMismatch.load(std::memory_order_acquire);
	return {
		.Enabled = m_Enabled.load(std::memory_order_acquire),
		.Processing = m_Processing.load(std::memory_order_acquire),
		.PumpObserved = tickCount > 0,
		.PumpThreadStable = tickCount > 0 && !threadMismatch,
		.PumpThreadId = m_PumpThreadId.load(std::memory_order_acquire),
		.LastPumpTickMonotonicUs = m_LastPumpTickMonotonicUs.load(std::memory_order_acquire),
		.PumpTickCount = tickCount,
		.LastTaskDurationUs = m_LastTaskDurationUs.load(std::memory_order_acquire),
		.QueueDepth = m_Queue.size(),
		.Capacity = kCapacity
	};
}

bool GameThreadExecutor::IsCurrentPumpThread() const noexcept
{
	const std::uint32_t pumpThreadId = m_PumpThreadId.load(std::memory_order_acquire);
	return m_Enabled.load(std::memory_order_acquire)
		&& pumpThreadId != 0
		&& !m_PumpThreadMismatch.load(std::memory_order_acquire)
		&& pumpThreadId == GetCurrentThreadId();
}

GameThreadSubmitResult GameThreadExecutor::ResultForTerminalState(
	const GameThreadTaskState state) const noexcept
{
	switch (state)
	{
	case GameThreadTaskState::Completed:
		return GameThreadSubmitResult::Completed;
	case GameThreadTaskState::Cancelled:
		return GameThreadSubmitResult::Cancelled;
	case GameThreadTaskState::Expired:
		return GameThreadSubmitResult::TimedOutBeforeStart;
	case GameThreadTaskState::Failed:
		return GameThreadSubmitResult::ExecutionFailed;
	case GameThreadTaskState::Queued:
	case GameThreadTaskState::Running:
		return GameThreadSubmitResult::ExecutionFailed;
	}
	return GameThreadSubmitResult::ExecutionFailed;
}

bool GameThreadExecutor::InvokeGuarded(IGameThreadWork* work) noexcept
{
#if defined(_MSC_VER)
	__try
	{
		return InvokeCppExceptionGuard(work);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	return InvokeCppExceptionGuard(work);
#endif
}

void GameThreadExecutor::RemoveQueuedLocked(
	const std::shared_ptr<GameThreadTaskControl>& task)
{
	const auto it = std::find(m_Queue.begin(), m_Queue.end(), task);
	if (it != m_Queue.end())
		m_Queue.erase(it);
}

void PostRenderPumpBackend::Tick() noexcept
{
	m_Executor.Pump();
	if (!m_Executor.IsCurrentPumpThread())
		return;

	IGameThreadFrameClient* client = m_FrameClient.load(std::memory_order_acquire);
	if (!client)
		return;

	auto frameLease = m_FrameClientBarrier.Enter();
	if (!frameLease.OwnedWorkAllowed()
		|| m_FrameClient.load(std::memory_order_acquire) != client)
	{
		return;
	}
	(void)client->PumpFrame(kFrameWorkBudget);
}

bool PostRenderPumpBackend::AttachFrameClient(IGameThreadFrameClient& client) noexcept
{
	std::lock_guard<std::mutex> lock(m_FrameClientMutex);
	if (m_FrameClient.load(std::memory_order_acquire) || m_DrainingClient)
		return false;
	if (!m_FrameClientBarrier.Reset())
		return false;
	m_FrameClient.store(&client, std::memory_order_release);
	return true;
}

bool PostRenderPumpBackend::DetachFrameClient(
	IGameThreadFrameClient& client,
	const std::chrono::milliseconds timeout)
{
	std::lock_guard<std::mutex> lock(m_FrameClientMutex);
	IGameThreadFrameClient* current = m_FrameClient.load(std::memory_order_acquire);
	if (current && current != &client)
		return false;
	if (m_DrainingClient && m_DrainingClient != &client)
		return false;
	if (current == &client)
	{
		m_FrameClient.store(nullptr, std::memory_order_release);
		m_FrameClientBarrier.BeginStopping();
		m_DrainingClient = &client;
	}
	else if (!m_DrainingClient)
	{
		return true;
	}

	if (!m_FrameClientBarrier.WaitForDrain(timeout))
		return false;
	m_DrainingClient = nullptr;
	return true;
}

bool PostRenderPumpBackend::HasFrameClient() const noexcept
{
	return m_FrameClient.load(std::memory_order_acquire) != nullptr;
}

std::uint32_t PostRenderPumpBackend::FrameClientInFlight() const noexcept
{
	return m_FrameClientBarrier.InFlight();
}

GameThreadExecutor& GetGameThreadExecutor()
{
	static GameThreadExecutor executor;
	return executor;
}

PostRenderPumpBackend& GetPostRenderPumpBackend()
{
	static PostRenderPumpBackend backend(GetGameThreadExecutor());
	return backend;
}

} // namespace UExplorer::Runtime
