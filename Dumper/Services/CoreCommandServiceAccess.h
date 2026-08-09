#pragma once

#include "CoreCommandService.h"

#include <atomic>

namespace UExplorer::Services
{

inline std::atomic<CoreCommandService*> g_CoreCommandService{nullptr};

inline void SetCoreCommandService(CoreCommandService* service) noexcept
{
	g_CoreCommandService.store(service, std::memory_order_release);
}

inline CoreCommandService* GetCoreCommandService() noexcept
{
	return g_CoreCommandService.load(std::memory_order_acquire);
}

} // namespace UExplorer::Services
