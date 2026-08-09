#pragma once

#include "Runtime/GameThreadExecutor.h"

// Transitional source-compatibility adapter for legacy API modules. New domain
// services use Runtime::GameThreadExecutor directly.
namespace UExplorer::GameThread
{

using ProcessEventFn = Runtime::ProcessEventFn;
using TaskState = Runtime::GameThreadTaskState;
using SubmitResult = Runtime::GameThreadSubmitResult;
using Diagnostics = Runtime::GameThreadDiagnostics;

inline constexpr std::size_t kQueueCapacity = Runtime::GameThreadExecutor::kCapacity;

inline SubmitResult Submit(
	void* object,
	void* function,
	std::vector<std::uint8_t>& params,
	const int timeoutMs = 5000)
{
	return Runtime::GetGameThreadExecutor().SubmitProcessEvent(
		object,
		function,
		params,
		timeoutMs);
}

inline bool Enable(const ProcessEventFn processEvent)
{
	return Runtime::GetGameThreadExecutor().Enable(processEvent);
}

inline bool DisableAndDrain(const int timeoutMs = 5000)
{
	return Runtime::GetGameThreadExecutor().DisableAndDrain(timeoutMs);
}

inline void ProcessQueue() noexcept
{
	Runtime::GetPostRenderPumpBackend().Tick();
}

inline bool IsEnabled() noexcept
{
	return Runtime::GetGameThreadExecutor().IsEnabled();
}

inline bool HasPending()
{
	return Runtime::GetGameThreadExecutor().HasPending();
}

inline std::size_t QueueDepth()
{
	return Runtime::GetGameThreadExecutor().QueueDepth();
}

inline Diagnostics GetDiagnostics()
{
	return Runtime::GetGameThreadExecutor().GetDiagnostics();
}

} // namespace UExplorer::GameThread
