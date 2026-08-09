#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace UExplorer::GameThread
{

using ProcessEventFn = void(*)(void*, void*, void*);

enum class TaskState : std::uint8_t
{
	Queued,
	Running,
	Completed,
	Failed,
	Cancelled,
	Expired
};

enum class SubmitResult : std::uint8_t
{
	Completed,
	Disabled,
	QueueBusy,
	TimedOutBeforeStart,
	TimedOutWhileRunning,
	ExecutionFailed
};

struct CallTask
{
	void* Object = nullptr;
	void* Function = nullptr;
	std::vector<std::uint8_t> Params;
	TaskState State = TaskState::Queued;
	std::chrono::steady_clock::time_point Deadline;
};

inline constexpr std::size_t kQueueCapacity = 64;
inline std::mutex g_QueueMutex;
inline std::condition_variable g_QueueCV;
inline std::deque<std::shared_ptr<CallTask>> g_Queue;
inline std::atomic<bool> g_Enabled{ false };
inline ProcessEventFn g_OrigProcessEvent = nullptr;
inline std::atomic<bool> g_Processing{false};

inline bool InvokeProcessEvent(
	ProcessEventFn processEvent,
	void* object,
	void* function,
	void* params)
{
#if defined(_MSC_VER)
	__try
	{
		processEvent(object, function, params);
		return true;
	}
	__except (1)
	{
		return false;
	}
#else
	try
	{
		processEvent(object, function, params);
		return true;
	}
	catch (...)
	{
		return false;
	}
#endif
}

inline void ProcessQueue()
{
	if (!g_Enabled.load(std::memory_order_acquire))
		return;
	if (g_Processing.exchange(true, std::memory_order_acq_rel))
		return;

	std::shared_ptr<CallTask> task;
	ProcessEventFn processEvent = nullptr;
	{
		std::lock_guard<std::mutex> lk(g_QueueMutex);
		while (!g_Queue.empty() && g_Queue.front()->State != TaskState::Queued)
			g_Queue.pop_front();
		if (g_Enabled.load(std::memory_order_relaxed) && g_OrigProcessEvent && !g_Queue.empty())
		{
			task = g_Queue.front();
			g_Queue.pop_front();
			if (std::chrono::steady_clock::now() >= task->Deadline)
			{
				task->State = TaskState::Expired;
			}
			else
			{
				task->State = TaskState::Running;
				processEvent = g_OrigProcessEvent;
			}
		}
	}

	if (!task || !processEvent)
	{
		g_Processing.store(false, std::memory_order_release);
		g_QueueCV.notify_all();
		return;
	}

	const TaskState finalState = InvokeProcessEvent(
		processEvent,
		task->Object,
		task->Function,
		task->Params.empty() ? nullptr : task->Params.data())
		? TaskState::Completed
		: TaskState::Failed;

	{
		std::lock_guard<std::mutex> lk(g_QueueMutex);
		task->State = finalState;
		g_Processing.store(false, std::memory_order_release);
	}
	g_QueueCV.notify_all();
}

inline SubmitResult Submit(
	void* obj,
	void* func,
	std::vector<std::uint8_t>& params,
	int timeoutMs = 5000)
{
	if (!obj || !func || timeoutMs <= 0)
		return SubmitResult::ExecutionFailed;

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeoutMs);
	std::unique_lock<std::mutex> lk(g_QueueMutex);
	if (!g_Enabled.load(std::memory_order_relaxed) || !g_OrigProcessEvent)
		return SubmitResult::Disabled;

	if (!g_QueueCV.wait_until(lk, deadline, [] {
		return !g_Enabled.load(std::memory_order_relaxed) || g_Queue.size() < kQueueCapacity;
	}))
	{
		return SubmitResult::QueueBusy;
	}
	if (!g_Enabled.load(std::memory_order_relaxed) || !g_OrigProcessEvent)
		return SubmitResult::Disabled;

	auto task = std::make_shared<CallTask>();
	task->Object = obj;
	task->Function = func;
	task->Params = params;
	task->Deadline = deadline;
	g_Queue.push_back(task);
	g_QueueCV.notify_all();

	const bool finished = g_QueueCV.wait_until(lk, deadline, [&task] {
		return task->State == TaskState::Completed
			|| task->State == TaskState::Failed
			|| task->State == TaskState::Cancelled
			|| task->State == TaskState::Expired;
	});
	if (!finished)
	{
		if (task->State == TaskState::Queued)
		{
			task->State = TaskState::Expired;
			for (auto it = g_Queue.begin(); it != g_Queue.end(); ++it)
			{
				if (*it == task)
				{
					g_Queue.erase(it);
					break;
				}
			}
			lk.unlock();
			g_QueueCV.notify_all();
			return SubmitResult::TimedOutBeforeStart;
		}
		return SubmitResult::TimedOutWhileRunning;
	}

	if (task->State == TaskState::Completed)
	{
		params = task->Params;
		return SubmitResult::Completed;
	}
	if (task->State == TaskState::Cancelled)
		return SubmitResult::Disabled;
	if (task->State == TaskState::Expired)
		return SubmitResult::TimedOutBeforeStart;
	return SubmitResult::ExecutionFailed;
}

inline bool Enable(ProcessEventFn origPE)
{
	if (!origPE)
		return false;
	std::lock_guard<std::mutex> lk(g_QueueMutex);
	if (g_Processing.load(std::memory_order_acquire) || !g_Queue.empty())
		return false;
	g_OrigProcessEvent = origPE;
	g_Enabled.store(true, std::memory_order_release);
	g_QueueCV.notify_all();
	return true;
}

inline bool DisableAndDrain(int timeoutMs = 5000)
{
	std::unique_lock<std::mutex> lk(g_QueueMutex);
	g_Enabled.store(false, std::memory_order_release);
	for (const auto& task : g_Queue)
	{
		if (task->State == TaskState::Queued)
			task->State = TaskState::Cancelled;
	}
	g_Queue.clear();
	g_QueueCV.notify_all();

	const bool drained = g_QueueCV.wait_for(
		lk,
		std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0),
		[] { return !g_Processing.load(std::memory_order_acquire) && g_Queue.empty(); });
	if (drained)
		g_OrigProcessEvent = nullptr;
	return drained;
}

inline bool IsEnabled()
{
	return g_Enabled.load(std::memory_order_acquire);
}

inline bool HasPending()
{
	std::lock_guard<std::mutex> lk(g_QueueMutex);
	return g_Processing.load(std::memory_order_acquire) || !g_Queue.empty();
}

inline std::size_t QueueDepth()
{
	std::lock_guard<std::mutex> lk(g_QueueMutex);
	return g_Queue.size();
}

} // namespace UExplorer::GameThread
