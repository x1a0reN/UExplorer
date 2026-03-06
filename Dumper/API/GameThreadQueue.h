#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

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
inline std::atomic<bool> g_Processing{false};

inline void ProcessQueue()
{
	if (!g_Enabled || !g_OrigProcessEvent) return;

	if (g_Processing.load(std::memory_order_relaxed)) return;

	void* callObj = nullptr;
	void* callFunc = nullptr;
	void* callParams = nullptr;
	bool hasWork = false;

	{
		std::lock_guard<std::mutex> lk(g_QueueMutex);
		if (g_Pending.Submitted && !g_Pending.Done.load())
		{
			hasWork = true;
			callObj = g_Pending.Object;
			callFunc = g_Pending.Function;
			callParams = g_Pending.Params;
		}
	}

	if (hasWork)
	{
		g_Processing.store(true, std::memory_order_relaxed);
		g_OrigProcessEvent(callObj, callFunc, callParams);
		g_Processing.store(false, std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> lk(g_QueueMutex);
			g_Pending.Done.store(true);
		}
		g_QueueCV.notify_all();
	}
}

inline bool Submit(void* obj, void* func, void* params, int timeoutMs = 5000)
{
	if (!g_Enabled || !g_OrigProcessEvent) return false;

	std::unique_lock<std::mutex> lk(g_QueueMutex);

	if (!g_QueueCV.wait_for(lk, std::chrono::milliseconds(timeoutMs),
		[] { return !g_Pending.Submitted || g_Pending.Done.load(); }))
	{
		return false;
	}

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
