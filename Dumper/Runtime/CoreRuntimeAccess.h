#pragma once

#include "CoreRuntime.h"

#include <atomic>

namespace UExplorer::Runtime
{

inline std::atomic<CoreRuntime*> g_CoreRuntime{nullptr};

inline void SetCoreRuntime(CoreRuntime* runtime)
{
	g_CoreRuntime.store(runtime, std::memory_order_release);
}

inline CoreRuntime* GetCoreRuntime()
{
	return g_CoreRuntime.load(std::memory_order_acquire);
}

} // namespace UExplorer::Runtime
