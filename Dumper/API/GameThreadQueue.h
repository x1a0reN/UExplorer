#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

// Single-slot game-thread dispatch queue.
// HTTP thread submits a ProcessEvent call and blocks until the game thread executes it.
// Game thread polls from HookedProcessEvent (called thousands of times/sec).

namespace UExplorer::GameThread
{

struct PendingCall
{
	void* Object;
	void* Function;
	void* Params;
	std::atomic<bool> Done{false};
	bool Submitted{false};
};

inline std::mutex g_QueueMutex;
inline std::condition_variable g_QueueCV;
inline PendingCall g_Pending;
inline bool g_Enabled = false;
inline void (*g_OrigProcessEvent)(void*, void*, void*) = nullptr;
inline std::atomic<bool> g_Processing{false};  // reentrancy guard

// Called from game thread (inside HookedProcessEvent) — non-blocking
inline void ProcessQueue()
{
	if (!g_Enabled || !g_OrigProcessEvent) return;

	// Reentrancy guard: ProcessEvent can recurse, don't re-enter
	if (g_Processing.load(std::memory_order_relaxed)) return;

	bool hasWork = false;
	{
		std::lock_guard<std::mutex> lk(g_QueueMutex);
		hasWork = g_Pending.Submitted && !g_Pending.Done.load();
	}

	if (hasWork)
	{
		g_Processing.store(true, std::memory_order_relaxed);
		g_OrigProcessEvent(g_Pending.Object, g_Pending.Function, g_Pending.Params);
		g_Processing.store(false, std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> lk(g_QueueMutex);
			g_Pending.Done.store(true);
		}
		g_QueueCV.notify_all();
	}
}

// Called from HTTP thread — blocks until game thread executes (or timeout)
inline bool Submit(void* obj, void* func, void* params, int timeoutMs = 5000)
{
	if (!g_Enabled || !g_OrigProcessEvent) return false;

	std::unique_lock<std::mutex> lk(g_QueueMutex);

	g_Pending.Object = obj;
	g_Pending.Function = func;
	g_Pending.Params = params;
	g_Pending.Done.store(false);
	g_Pending.Submitted = true;

	bool ok = g_QueueCV.wait_for(lk, std::chrono::milliseconds(timeoutMs),
		[] { return g_Pending.Done.load(); });

	g_Pending.Submitted = false;
	return ok;
}

inline void Enable(void (*origPE)(void*, void*, void*))
{
	g_OrigProcessEvent = origPE;
	g_Enabled = true;
}

inline void Disable()
{
	g_Enabled = false;
	g_OrigProcessEvent = nullptr;
}

} // namespace UExplorer::GameThread
