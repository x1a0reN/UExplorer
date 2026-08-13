#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace UExplorer::Runtime
{
class EngineContext;
class EngineFacade;
class GameThreadExecutor;
class HookEventCollector;
}

namespace UExplorer::Services
{

class HookCommandService;

enum class ProcessEventHookError : std::uint8_t
{
	None,
	InvalidConfiguration,
	Stopping,
	SnapshotUnavailable,
	SnapshotGenerationMismatch,
	ClassEvidenceUnavailable,
	ClassCandidateLimitExceeded,
	PatchCapacityExceeded,
	NoPatchCandidates,
	CurrentExecutionThreadInvalid,
	ClassHandleStale,
	DefaultObjectHandleStale,
	VTableReadFailed,
	ProcessEventPointerInvalid,
	CanonicalProcessEventMissing,
	ActiveOwnerConflict,
	CollectorConfigurationFailed,
	PatchFailed,
	RestoreFailed,
	CallbackDrainTimedOut,
	MaintenanceBusy,
	MaintenanceDrainTimedOut,
	GameThreadDisabled,
	GameThreadQueueBusy,
	GameThreadWaitDenied,
	GameThreadTimedOutBeforeStart,
	GameThreadTimedOutWhileRunning,
	GameThreadExecutionFailed,
	AllocationFailed
};

const char* ToString(ProcessEventHookError error) noexcept;

struct ProcessEventHookLimits
{
	std::size_t MaxClassCandidates = 65'536;
	std::size_t MaxPatchedVTables = 16'384;
};

struct ProcessEventHookResult
{
	ProcessEventHookError Error = ProcessEventHookError::None;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::size_t ClassCandidateCount = 0;
	std::size_t UniqueVTableCount = 0;
	std::size_t AddedPatchCount = 0;
	std::size_t TotalPatchCount = 0;
	bool Changed = false;

	bool Ok() const noexcept { return Error == ProcessEventHookError::None; }
};

struct ProcessEventHookDiagnostics
{
	bool Configured = false;
	bool Installed = false;
	bool CoverageComplete = false;
	bool Stopping = false;
	bool SafeToUnload = true;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t CoveredObjectSnapshotGeneration = 0;
	std::uint64_t CoveredTypeSnapshotGeneration = 0;
	std::size_t ClassCandidateCount = 0;
	std::size_t PatchedVTableCount = 0;
	std::uint32_t CallbackInFlight = 0;
	std::uint32_t MaintenanceInFlight = 0;
	std::uint64_t EnterPublished = 0;
	std::uint64_t ExitPublished = 0;
	std::uint64_t ParameterCaptureSucceeded = 0;
	std::uint64_t ParameterFrameUnavailable = 0;
	std::uint64_t ParameterReadFailed = 0;
	std::uint64_t ParameterEncodeFailed = 0;
	std::uint64_t UnmatchedFunction = 0;
	std::uint64_t DispatchMiss = 0;
	std::uint64_t CorrelationExhausted = 0;
	ProcessEventHookError LastError = ProcessEventHookError::None;
};

// Owns every ProcessEvent vtable patch used by the release runtime. Class CDO
// evidence is consumed only from the current immutable TypeSnapshot, all live
// handle/vtable reads run on the witnessed game-thread executor, and callback
// work is limited to immutable lookup, optional plan-bounded scalar copies, and
// bounded collector publication. Parameter decoding never runs in the callback.
class ProcessEventHookOwner final
{
public:
	ProcessEventHookOwner(
		std::shared_ptr<const Runtime::EngineContext> context,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& executor,
		Runtime::HookEventCollector& collector,
		HookCommandService& commandService,
		ProcessEventHookLimits limits = {}) noexcept;
	~ProcessEventHookOwner();

	ProcessEventHookOwner(const ProcessEventHookOwner&) = delete;
	ProcessEventHookOwner& operator=(const ProcessEventHookOwner&) = delete;

	bool IsConfigured() const noexcept;
	ProcessEventHookResult Reconcile(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;
	ProcessEventHookResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;
	ProcessEventHookDiagnostics Diagnostics() const noexcept;

private:
	struct State;

	static void HookedProcessEvent(void* object, void* function, void* params);

	inline static std::atomic<std::shared_ptr<State>> s_Active;
	inline static std::atomic<void*> s_FallbackOriginal{nullptr};
	std::shared_ptr<State> m_State;
};

} // namespace UExplorer::Services
