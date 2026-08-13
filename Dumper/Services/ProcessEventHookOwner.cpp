#include "ProcessEventHookOwner.h"

#include "HookCommandService.h"
#include "Runtime/CallbackBarrier.h"
#include "Runtime/EngineContext.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/EngineSnapshot.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/HookEventCollector.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/TypeSnapshot.h"
#include "Runtime/VTableHook.h"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

extern "C" __declspec(dllimport) SIZE_T __stdcall VirtualQuery(
	LPCVOID lpAddress,
	PMEMORY_BASIC_INFORMATION lpBuffer,
	SIZE_T dwLength);

namespace UExplorer::Services
{
namespace
{
	constexpr std::int64_t kMaxVTableIndex = 512;
	constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;

	bool ValidLimits(const ProcessEventHookLimits& limits) noexcept
	{
		return limits.MaxClassCandidates > 0
			&& limits.MaxClassCandidates <= 1'000'000
			&& limits.MaxPatchedVTables > 0
			&& limits.MaxPatchedVTables <= 65'536
			&& limits.MaxPatchedVTables <= limits.MaxClassCandidates;
	}

	bool IsExecutableAddress(const std::uintptr_t address) noexcept
	{
		if (address == 0)
			return false;
		MEMORY_BASIC_INFORMATION information{};
		if (VirtualQuery(
			reinterpret_cast<const void*>(address),
			&information,
			sizeof(information)) != sizeof(information)
			|| information.State != MEM_COMMIT
			|| (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return false;
		}
		const DWORD protection = information.Protect & 0xFF;
		return protection == PAGE_EXECUTE
			|| protection == PAGE_EXECUTE_READ
			|| protection == PAGE_EXECUTE_READWRITE
			|| protection == PAGE_EXECUTE_WRITECOPY;
	}

	bool TryReadObjectVTable(void* const object, void**& vtable) noexcept
	{
		vtable = nullptr;
		if (!object)
			return false;
		__try
		{
			vtable = *reinterpret_cast<void***>(object);
			return vtable != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
			return false;
		}
	}

	std::size_t HashAddress(std::uintptr_t value) noexcept
	{
		std::uint64_t mixed = static_cast<std::uint64_t>(value);
		mixed ^= mixed >> 33;
		mixed *= 0xff51afd7ed558ccdULL;
		mixed ^= mixed >> 33;
		mixed *= 0xc4ceb9fe1a85ec53ULL;
		mixed ^= mixed >> 33;
		return static_cast<std::size_t>(mixed);
	}

	ProcessEventHookError MapSubmitError(
		const Runtime::GameThreadSubmitResult result) noexcept
	{
		switch (result)
		{
		case Runtime::GameThreadSubmitResult::Completed:
			return ProcessEventHookError::None;
		case Runtime::GameThreadSubmitResult::Disabled:
			return ProcessEventHookError::GameThreadDisabled;
		case Runtime::GameThreadSubmitResult::Cancelled:
			return ProcessEventHookError::Stopping;
		case Runtime::GameThreadSubmitResult::PumpThreadWaitDenied:
			return ProcessEventHookError::GameThreadWaitDenied;
		case Runtime::GameThreadSubmitResult::QueueBusy:
			return ProcessEventHookError::GameThreadQueueBusy;
		case Runtime::GameThreadSubmitResult::TimedOutBeforeStart:
			return ProcessEventHookError::GameThreadTimedOutBeforeStart;
		case Runtime::GameThreadSubmitResult::TimedOutWhileRunning:
			return ProcessEventHookError::GameThreadTimedOutWhileRunning;
		case Runtime::GameThreadSubmitResult::ExecutionFailed:
			return ProcessEventHookError::GameThreadExecutionFailed;
		}
		return ProcessEventHookError::GameThreadExecutionFailed;
	}
}

const char* ToString(const ProcessEventHookError error) noexcept
{
	switch (error)
	{
	case ProcessEventHookError::None: return "NONE";
	case ProcessEventHookError::InvalidConfiguration: return "PROCESS_EVENT_HOOK_INVALID_CONFIGURATION";
	case ProcessEventHookError::Stopping: return "PROCESS_EVENT_HOOK_STOPPING";
	case ProcessEventHookError::SnapshotUnavailable: return "PROCESS_EVENT_HOOK_SNAPSHOT_UNAVAILABLE";
	case ProcessEventHookError::SnapshotGenerationMismatch: return "PROCESS_EVENT_HOOK_SNAPSHOT_GENERATION_MISMATCH";
	case ProcessEventHookError::ClassEvidenceUnavailable: return "PROCESS_EVENT_HOOK_CLASS_EVIDENCE_UNAVAILABLE";
	case ProcessEventHookError::ClassCandidateLimitExceeded: return "PROCESS_EVENT_HOOK_CLASS_CANDIDATE_LIMIT_EXCEEDED";
	case ProcessEventHookError::PatchCapacityExceeded: return "PROCESS_EVENT_HOOK_PATCH_CAPACITY_EXCEEDED";
	case ProcessEventHookError::NoPatchCandidates: return "PROCESS_EVENT_HOOK_NO_PATCH_CANDIDATES";
	case ProcessEventHookError::CurrentExecutionThreadInvalid: return "PROCESS_EVENT_HOOK_EXECUTION_THREAD_INVALID";
	case ProcessEventHookError::ClassHandleStale: return "PROCESS_EVENT_HOOK_CLASS_HANDLE_STALE";
	case ProcessEventHookError::DefaultObjectHandleStale: return "PROCESS_EVENT_HOOK_CDO_HANDLE_STALE";
	case ProcessEventHookError::VTableReadFailed: return "PROCESS_EVENT_HOOK_VTABLE_READ_FAILED";
	case ProcessEventHookError::ProcessEventPointerInvalid: return "PROCESS_EVENT_HOOK_TARGET_INVALID";
	case ProcessEventHookError::CanonicalProcessEventMissing: return "PROCESS_EVENT_HOOK_CANONICAL_TARGET_MISSING";
	case ProcessEventHookError::ActiveOwnerConflict: return "PROCESS_EVENT_HOOK_OWNER_CONFLICT";
	case ProcessEventHookError::CollectorConfigurationFailed: return "PROCESS_EVENT_HOOK_COLLECTOR_CONFIGURATION_FAILED";
	case ProcessEventHookError::PatchFailed: return "PROCESS_EVENT_HOOK_PATCH_FAILED";
	case ProcessEventHookError::RestoreFailed: return "PROCESS_EVENT_HOOK_RESTORE_FAILED";
	case ProcessEventHookError::CallbackDrainTimedOut: return "PROCESS_EVENT_HOOK_CALLBACK_DRAIN_TIMEOUT";
	case ProcessEventHookError::MaintenanceBusy: return "PROCESS_EVENT_HOOK_MAINTENANCE_BUSY";
	case ProcessEventHookError::MaintenanceDrainTimedOut: return "PROCESS_EVENT_HOOK_MAINTENANCE_DRAIN_TIMEOUT";
	case ProcessEventHookError::GameThreadDisabled: return "PROCESS_EVENT_HOOK_GAME_THREAD_DISABLED";
	case ProcessEventHookError::GameThreadQueueBusy: return "PROCESS_EVENT_HOOK_GAME_THREAD_BUSY";
	case ProcessEventHookError::GameThreadWaitDenied: return "PROCESS_EVENT_HOOK_GAME_THREAD_WAIT_DENIED";
	case ProcessEventHookError::GameThreadTimedOutBeforeStart: return "PROCESS_EVENT_HOOK_GAME_THREAD_TIMEOUT_BEFORE_START";
	case ProcessEventHookError::GameThreadTimedOutWhileRunning: return "PROCESS_EVENT_HOOK_GAME_THREAD_TIMEOUT_WHILE_RUNNING";
	case ProcessEventHookError::GameThreadExecutionFailed: return "PROCESS_EVENT_HOOK_GAME_THREAD_EXECUTION_FAILED";
	case ProcessEventHookError::AllocationFailed: return "PROCESS_EVENT_HOOK_ALLOCATION_FAILED";
	}
	return "PROCESS_EVENT_HOOK_UNKNOWN_ERROR";
}

struct ProcessEventHookOwner::State final : std::enable_shared_from_this<State>
{
	struct ClassCandidate
	{
		Runtime::ObjectHandle Class;
		Runtime::ObjectHandle DefaultObject;
	};

	struct DispatchEntry
	{
		std::uintptr_t Slot = 0;
		Runtime::ProcessEventFn Original = nullptr;
	};

	struct DispatchSnapshot final
	{
		std::vector<DispatchEntry> Entries;
		std::vector<std::uint32_t> Buckets;

		const DispatchEntry* Find(const std::uintptr_t slot) const noexcept
		{
			if (slot == 0 || Buckets.empty())
				return nullptr;
			const std::size_t mask = Buckets.size() - 1;
			std::size_t bucket = HashAddress(slot) & mask;
			for (std::size_t probe = 0; probe < Buckets.size(); ++probe)
			{
				const std::uint32_t stored = Buckets[bucket];
				if (stored == 0)
					return nullptr;
				const DispatchEntry& entry = Entries[stored - 1];
				if (entry.Slot == slot)
					return &entry;
				bucket = (bucket + 1) & mask;
			}
			return nullptr;
		}
	};

	struct PatchRecord
	{
		std::uintptr_t Slot = 0;
		std::unique_ptr<Runtime::VTableHookToken> Token;
	};

	struct Operation final
	{
		std::shared_ptr<const Runtime::EngineSnapshot> Objects;
		std::shared_ptr<const Runtime::TypeSnapshot> Types;
		std::vector<ClassCandidate> Candidates;
		ProcessEventHookResult Result;
	};

	struct ReconcileWork final : Runtime::IGameThreadWork
	{
		ReconcileWork(std::shared_ptr<State> owner, std::shared_ptr<Operation> operation)
			: Owner(std::move(owner)), OperationState(std::move(operation))
		{
		}

		~ReconcileWork() override
		{
			if (Owner)
				Owner->EndMaintenance();
		}

		bool Execute() override
		{
			Owner->RunReconcile(*OperationState);
			return true;
		}

		std::shared_ptr<State> Owner;
		std::shared_ptr<Operation> OperationState;
	};

	State(
		std::shared_ptr<const Runtime::EngineContext> context,
		Runtime::EngineFacade& engine,
		Runtime::GameThreadExecutor& executor,
		Runtime::HookEventCollector& collector,
		HookCommandService& commandService,
		const ProcessEventHookLimits limits) noexcept
		: Context(std::move(context)),
		  Engine(&engine),
		  Executor(&executor),
		  Collector(&collector),
		  CommandService(&commandService),
		  Limits(limits)
	{
		if (!Context || !Engine->IsConfigured() || !Collector->IsConfigured()
			|| !CommandService->IsConfigured() || !ValidLimits(Limits)
			|| Engine->ContextGeneration() != Context->Generation())
		{
			LastError.store(ProcessEventHookError::InvalidConfiguration, std::memory_order_release);
			return;
		}
		const Runtime::OffsetReport* index = Context->FindOffset("process_event.index");
		const Runtime::OffsetReport* offset = Context->FindOffset("process_event.offset");
		if (!index || !offset || !index->IsValidated() || !offset->IsValidated()
			|| index->Value <= 0 || index->Value >= kMaxVTableIndex
			|| offset->Value <= 0
			|| static_cast<std::uint64_t>(offset->Value)
				> (std::numeric_limits<std::uintptr_t>::max)() - Context->ModuleBase())
		{
			LastError.store(ProcessEventHookError::InvalidConfiguration, std::memory_order_release);
			return;
		}
		ProcessEventIndex = static_cast<std::int32_t>(index->Value);
		CanonicalProcessEvent = Context->ModuleBase()
			+ static_cast<std::uintptr_t>(offset->Value);
		if (!IsExecutableAddress(CanonicalProcessEvent))
		{
			LastError.store(ProcessEventHookError::ProcessEventPointerInvalid, std::memory_order_release);
			return;
		}
		Configured.store(true, std::memory_order_release);
		LastError.store(ProcessEventHookError::None, std::memory_order_release);
	}

	bool TryBeginMaintenance() noexcept
	{
		std::uint32_t expected = 0;
		return !Stopping.load(std::memory_order_acquire)
			&& MaintenanceInFlight.compare_exchange_strong(
				expected,
				1,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
	}

	void EndMaintenance() noexcept
	{
		MaintenanceInFlight.store(0, std::memory_order_release);
		MaintenanceCondition.notify_all();
	}

	void SetFailure(const ProcessEventHookError error) noexcept
	{
		CoverageComplete.store(false, std::memory_order_release);
		LastError.store(error, std::memory_order_release);
	}

	std::shared_ptr<const DispatchSnapshot> BuildDispatch(
		std::vector<DispatchEntry> entries) const
	{
		auto snapshot = std::make_shared<DispatchSnapshot>();
		snapshot->Entries = std::move(entries);
		if (snapshot->Entries.empty())
			return snapshot;
		std::size_t bucketCount = 2;
		while (bucketCount < snapshot->Entries.size() * 2)
			bucketCount *= 2;
		snapshot->Buckets.assign(bucketCount, 0);
		const std::size_t mask = bucketCount - 1;
		for (std::size_t index = 0; index < snapshot->Entries.size(); ++index)
		{
			std::size_t bucket = HashAddress(snapshot->Entries[index].Slot) & mask;
			while (snapshot->Buckets[bucket] != 0)
				bucket = (bucket + 1) & mask;
			snapshot->Buckets[bucket] = static_cast<std::uint32_t>(index + 1);
		}
		return snapshot;
	}

	bool PublishCollectorConfiguration(const bool enabled) noexcept
	{
		const Runtime::HookCollectorSnapshot current = Collector->Snapshot();
		if (!current.Configured || !current.ConfigurationSnapshotStable)
			return false;
		Runtime::HookCollectorConfig configuration = current.Configuration;
		configuration.Enabled = enabled;
		configuration.KindMask = Runtime::HookEventKindBit(
			Runtime::HookEventKind::ProcessEventEnter)
			| Runtime::HookEventKindBit(Runtime::HookEventKind::ProcessEventExit);
		configuration.CoalesceOnOverflow = false;
		return Collector->PublishConfiguration(configuration).Ok();
	}

	bool RollbackAddedPatches(
		const std::size_t oldPatchCount,
		std::shared_ptr<const DispatchSnapshot> oldDispatch,
		const std::chrono::milliseconds timeout) noexcept
	{
		const bool firstInstall = oldPatchCount == 0;
		if (firstInstall)
			Callbacks.BeginStopping();

		bool restored = true;
		for (std::size_t index = Patches.size(); index > oldPatchCount; --index)
		{
			PatchRecord& patch = Patches[index - 1];
			if (patch.Token && patch.Token->IsActive() && !patch.Token->Disable())
				restored = false;
		}
		if (!restored)
		{
			SetFailure(ProcessEventHookError::RestoreFailed);
			Installed.store(!Patches.empty(), std::memory_order_release);
			PatchCount.store(Patches.size(), std::memory_order_release);
			return false;
		}

		Patches.resize(oldPatchCount);
		Dispatch.store(std::move(oldDispatch), std::memory_order_release);
		PatchCount.store(Patches.size(), std::memory_order_release);
		Installed.store(!Patches.empty(), std::memory_order_release);
		if (!firstInstall)
			return true;

		if (!Callbacks.WaitForDrain(timeout))
		{
			SetFailure(ProcessEventHookError::CallbackDrainTimedOut);
			return false;
		}
		std::shared_ptr<State> expected = shared_from_this();
		if (!ProcessEventHookOwner::s_Active.compare_exchange_strong(
			expected,
			{},
			std::memory_order_acq_rel,
			std::memory_order_acquire)
			&& expected)
		{
			SetFailure(ProcessEventHookError::ActiveOwnerConflict);
			return false;
		}
		if (!PublishCollectorConfiguration(false))
		{
			SetFailure(ProcessEventHookError::CollectorConfigurationFailed);
			return false;
		}
		if (!Callbacks.Reset())
		{
			SetFailure(ProcessEventHookError::CallbackDrainTimedOut);
			return false;
		}
		return true;
	}

	void RunReconcile(Operation& operation) noexcept
	{
		ProcessEventHookResult result{
			.ObjectSnapshotGeneration = operation.Objects
				? operation.Objects->Generation : 0,
			.TypeSnapshotGeneration = operation.Types
				? operation.Types->Generation() : 0,
			.ClassCandidateCount = operation.Candidates.size()
		};
		try
		{
			if (Stopping.load(std::memory_order_acquire))
			{
				result.Error = ProcessEventHookError::Stopping;
				operation.Result = result;
				return;
			}
			if (!Engine->IsCurrentExecutionThreadValid())
			{
				result.Error = ProcessEventHookError::CurrentExecutionThreadInvalid;
				SetFailure(result.Error);
				operation.Result = result;
				return;
			}
			if (!operation.Objects || !operation.Types
				|| Engine->Snapshots().Current() != operation.Objects
				|| Engine->Types().Current() != operation.Types)
			{
				result.Error = ProcessEventHookError::SnapshotGenerationMismatch;
				SetFailure(result.Error);
				operation.Result = result;
				return;
			}

			const auto oldDispatch = Dispatch.load(std::memory_order_acquire);
			std::vector<DispatchEntry> allEntries = oldDispatch
				? oldDispatch->Entries : std::vector<DispatchEntry>{};
			std::vector<DispatchEntry> observed;
			observed.reserve(operation.Candidates.size());
			for (const ClassCandidate& candidate : operation.Candidates)
			{
				if (!Engine->ValidateObjectHandle(candidate.Class).Ok())
				{
					result.Error = ProcessEventHookError::ClassHandleStale;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				if (!Engine->ValidateObjectHandle(candidate.DefaultObject).Ok())
				{
					result.Error = ProcessEventHookError::DefaultObjectHandleStale;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				void** vtable = nullptr;
				if (!Runtime::ReadValue(candidate.DefaultObject.Address, vtable).Ok()
					|| !vtable)
				{
					result.Error = ProcessEventHookError::VTableReadFailed;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				const std::uintptr_t vtableAddress = reinterpret_cast<std::uintptr_t>(vtable);
				const std::uintptr_t slotOffset = static_cast<std::uintptr_t>(ProcessEventIndex)
					* sizeof(void*);
				if (vtableAddress > (std::numeric_limits<std::uintptr_t>::max)() - slotOffset)
				{
					result.Error = ProcessEventHookError::VTableReadFailed;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				const std::uintptr_t slot = vtableAddress + slotOffset;
				void* current = nullptr;
				if (!Runtime::ReadValue(slot, current).Ok() || !current)
				{
					result.Error = ProcessEventHookError::VTableReadFailed;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				const DispatchEntry* existing = oldDispatch ? oldDispatch->Find(slot) : nullptr;
				if (existing)
				{
					if (current != reinterpret_cast<void*>(&ProcessEventHookOwner::HookedProcessEvent))
					{
						result.Error = ProcessEventHookError::ActiveOwnerConflict;
						SetFailure(result.Error);
						operation.Result = result;
						return;
					}
					observed.push_back(*existing);
					continue;
				}
				if (current == reinterpret_cast<void*>(&ProcessEventHookOwner::HookedProcessEvent))
				{
					result.Error = ProcessEventHookError::ActiveOwnerConflict;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				const std::uintptr_t originalAddress = reinterpret_cast<std::uintptr_t>(current);
				if (!IsExecutableAddress(originalAddress))
				{
					result.Error = ProcessEventHookError::ProcessEventPointerInvalid;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				observed.push_back({
					.Slot = slot,
					.Original = reinterpret_cast<Runtime::ProcessEventFn>(current)
				});
			}

			std::ranges::sort(observed, {}, &DispatchEntry::Slot);
			observed.erase(std::unique(
				observed.begin(),
				observed.end(),
				[](const DispatchEntry& left, const DispatchEntry& right) {
					return left.Slot == right.Slot;
				}), observed.end());
			result.UniqueVTableCount = observed.size();
			bool canonicalPresent = false;
			std::vector<DispatchEntry> additions;
			additions.reserve(observed.size());
			for (const DispatchEntry& entry : observed)
			{
				canonicalPresent = canonicalPresent
					|| reinterpret_cast<std::uintptr_t>(entry.Original) == CanonicalProcessEvent;
				if (!oldDispatch || !oldDispatch->Find(entry.Slot))
					additions.push_back(entry);
			}
			if (!canonicalPresent)
			{
				result.Error = ProcessEventHookError::CanonicalProcessEventMissing;
				SetFailure(result.Error);
				operation.Result = result;
				return;
			}
			if (observed.empty())
			{
				result.Error = ProcessEventHookError::NoPatchCandidates;
				SetFailure(result.Error);
				operation.Result = result;
				return;
			}
			if (additions.size() > Limits.MaxPatchedVTables
				|| Patches.size() > Limits.MaxPatchedVTables - additions.size())
			{
				result.Error = ProcessEventHookError::PatchCapacityExceeded;
				SetFailure(result.Error);
				operation.Result = result;
				return;
			}

			allEntries.insert(allEntries.end(), additions.begin(), additions.end());
			const auto nextDispatch = BuildDispatch(std::move(allEntries));
			Patches.reserve(Patches.size() + additions.size());
			const std::size_t oldPatchCount = Patches.size();
			const bool firstInstall = oldPatchCount == 0;
			if (firstInstall)
			{
				if (!Callbacks.Reset())
				{
					result.Error = ProcessEventHookError::CollectorConfigurationFailed;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				std::shared_ptr<State> expected;
				if (!ProcessEventHookOwner::s_Active.compare_exchange_strong(
					expected,
					shared_from_this(),
					std::memory_order_acq_rel,
					std::memory_order_acquire))
				{
					result.Error = ProcessEventHookError::ActiveOwnerConflict;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
				ProcessEventHookOwner::s_FallbackOriginal.store(
					reinterpret_cast<void*>(CanonicalProcessEvent),
					std::memory_order_release);
				if (!PublishCollectorConfiguration(true))
				{
					std::shared_ptr<State> active = shared_from_this();
					ProcessEventHookOwner::s_Active.compare_exchange_strong(
						active,
						{},
						std::memory_order_acq_rel,
						std::memory_order_acquire);
					result.Error = ProcessEventHookError::CollectorConfigurationFailed;
					SetFailure(result.Error);
					operation.Result = result;
					return;
				}
			}
			Dispatch.store(nextDispatch, std::memory_order_release);

			for (const DispatchEntry& addition : additions)
			{
				Runtime::VTableHookInstallResult installed = Runtime::VTableHookToken::Install(
					reinterpret_cast<void**>(addition.Slot),
					reinterpret_cast<void*>(&ProcessEventHookOwner::HookedProcessEvent),
					reinterpret_cast<void*>(addition.Original));
				if (!installed.Ok())
				{
					result.Error = ProcessEventHookError::PatchFailed;
					if (!RollbackAddedPatches(
						oldPatchCount,
						oldDispatch,
						std::chrono::milliseconds(5000)))
					{
						result.Error = LastError.load(std::memory_order_acquire);
					}
					else
						SetFailure(result.Error);
					result.TotalPatchCount = Patches.size();
					operation.Result = result;
					return;
				}
				Patches.push_back({
					.Slot = addition.Slot,
					.Token = std::move(installed.Token)
				});
			}

			if (Engine->Snapshots().Current() != operation.Objects
				|| Engine->Types().Current() != operation.Types)
			{
				result.Error = ProcessEventHookError::SnapshotGenerationMismatch;
				if (!RollbackAddedPatches(
					oldPatchCount,
					oldDispatch,
					std::chrono::milliseconds(5000)))
				{
					result.Error = LastError.load(std::memory_order_acquire);
				}
				else
					SetFailure(result.Error);
				result.TotalPatchCount = Patches.size();
				operation.Result = result;
				return;
			}

			const std::uint64_t previousTypeGeneration =
				CoveredTypeGeneration.load(std::memory_order_acquire);
			Installed.store(true, std::memory_order_release);
			CoverageComplete.store(true, std::memory_order_release);
			CoveredObjectGeneration.store(operation.Objects->Generation, std::memory_order_release);
			CoveredTypeGeneration.store(operation.Types->Generation(), std::memory_order_release);
			ClassCandidateCount.store(operation.Candidates.size(), std::memory_order_release);
			PatchCount.store(Patches.size(), std::memory_order_release);
			LastError.store(ProcessEventHookError::None, std::memory_order_release);
			result.AddedPatchCount = additions.size();
			result.TotalPatchCount = Patches.size();
			result.Changed = !additions.empty()
				|| previousTypeGeneration != operation.Types->Generation();
			operation.Result = result;
		}
		catch (const std::bad_alloc&)
		{
			result.Error = ProcessEventHookError::AllocationFailed;
			SetFailure(result.Error);
			operation.Result = result;
		}
		catch (...)
		{
			result.Error = ProcessEventHookError::GameThreadExecutionFailed;
			SetFailure(result.Error);
			operation.Result = result;
		}
	}

	std::shared_ptr<const Runtime::EngineContext> Context;
	Runtime::EngineFacade* Engine = nullptr;
	Runtime::GameThreadExecutor* Executor = nullptr;
	Runtime::HookEventCollector* Collector = nullptr;
	HookCommandService* CommandService = nullptr;
	ProcessEventHookLimits Limits;
	std::int32_t ProcessEventIndex = -1;
	std::uintptr_t CanonicalProcessEvent = 0;
	std::mutex ControlMutex;
	std::mutex MaintenanceMutex;
	std::condition_variable MaintenanceCondition;
	Runtime::CallbackBarrier Callbacks;
	std::vector<PatchRecord> Patches;
	std::atomic<std::shared_ptr<const DispatchSnapshot>> Dispatch;
	std::atomic<bool> Configured{false};
	std::atomic<bool> Installed{false};
	std::atomic<bool> CoverageComplete{false};
	std::atomic<bool> Stopping{false};
	std::atomic<std::uint32_t> MaintenanceInFlight{0};
	std::atomic<std::uint64_t> CoveredObjectGeneration{0};
	std::atomic<std::uint64_t> CoveredTypeGeneration{0};
	std::atomic<std::size_t> ClassCandidateCount{0};
	std::atomic<std::size_t> PatchCount{0};
	std::atomic<std::uint64_t> NextCorrelation{1};
	std::atomic<std::uint64_t> EnterPublished{0};
	std::atomic<std::uint64_t> ExitPublished{0};
	std::atomic<std::uint64_t> UnmatchedFunction{0};
	std::atomic<std::uint64_t> DispatchMiss{0};
	std::atomic<std::uint64_t> CorrelationExhausted{0};
	std::atomic<ProcessEventHookError> LastError{ProcessEventHookError::InvalidConfiguration};
};

ProcessEventHookOwner::ProcessEventHookOwner(
	std::shared_ptr<const Runtime::EngineContext> context,
	Runtime::EngineFacade& engine,
	Runtime::GameThreadExecutor& executor,
	Runtime::HookEventCollector& collector,
	HookCommandService& commandService,
	const ProcessEventHookLimits limits) noexcept
{
	try
	{
		m_State = std::make_shared<State>(
			std::move(context),
			engine,
			executor,
			collector,
			commandService,
			limits);
	}
	catch (...)
	{
		m_State.reset();
	}
}

ProcessEventHookOwner::~ProcessEventHookOwner()
{
	if (m_State && !Diagnostics().SafeToUnload)
		StopAndDrain(std::chrono::milliseconds(5000));
}

bool ProcessEventHookOwner::IsConfigured() const noexcept
{
	return m_State && m_State->Configured.load(std::memory_order_acquire);
}

ProcessEventHookResult ProcessEventHookOwner::Reconcile(
	const std::chrono::milliseconds timeout) noexcept
{
	ProcessEventHookResult result;
	if (!m_State || !IsConfigured() || timeout.count() <= 0
		|| timeout.count() > Runtime::GameThreadExecutor::kMaxTimeoutMs)
	{
		result.Error = ProcessEventHookError::InvalidConfiguration;
		return result;
	}

	std::unique_lock<std::mutex> owner(m_State->ControlMutex);
	if (m_State->Stopping.load(std::memory_order_acquire))
	{
		result.Error = ProcessEventHookError::Stopping;
		return result;
	}
	try
	{
		const std::shared_ptr<const Runtime::EngineSnapshot> objects =
			m_State->Engine->Snapshots().Current();
		const std::shared_ptr<const Runtime::TypeSnapshot> types =
			m_State->Engine->Types().Current();
		if (!objects || !types)
		{
			result.Error = ProcessEventHookError::SnapshotUnavailable;
			m_State->LastError.store(result.Error, std::memory_order_release);
			return result;
		}
		result.ObjectSnapshotGeneration = objects->Generation;
		result.TypeSnapshotGeneration = types->Generation();
		if (objects->SessionId != m_State->Engine->SessionId()
			|| types->SessionId() != m_State->Engine->SessionId()
			|| objects->ContextGeneration != m_State->Context->Generation()
			|| types->ContextGeneration() != m_State->Context->Generation()
			|| types->ObjectSnapshotGeneration() != objects->Generation
			|| !types->IsConfigured(m_State->Context->Generation()))
		{
			result.Error = ProcessEventHookError::SnapshotGenerationMismatch;
			m_State->SetFailure(result.Error);
			return result;
		}
		if (m_State->Installed.load(std::memory_order_acquire)
			&& m_State->CoverageComplete.load(std::memory_order_acquire)
			&& m_State->CoveredObjectGeneration.load(std::memory_order_acquire)
				== objects->Generation
			&& m_State->CoveredTypeGeneration.load(std::memory_order_acquire)
				== types->Generation())
		{
			result.ClassCandidateCount =
				m_State->ClassCandidateCount.load(std::memory_order_acquire);
			result.UniqueVTableCount =
				m_State->PatchCount.load(std::memory_order_acquire);
			result.TotalPatchCount = result.UniqueVTableCount;
			return result;
		}

		auto operation = std::make_shared<State::Operation>();
		operation->Objects = objects;
		operation->Types = types;
		operation->Candidates.reserve((std::min)(
			types->Types().size(),
			m_State->Limits.MaxClassCandidates));
		for (const Runtime::ReflectedType& type : types->Types())
		{
			if (type.Kind != Runtime::ReflectedTypeKind::Class)
				continue;
			if (type.DefaultObjectState == Runtime::ClassDefaultObjectState::NotConstructed)
				continue;
			if (type.DefaultObjectState != Runtime::ClassDefaultObjectState::Present
				|| !type.DefaultObject)
			{
				result.Error = ProcessEventHookError::ClassEvidenceUnavailable;
				m_State->SetFailure(result.Error);
				return result;
			}
			if (operation->Candidates.size() >= m_State->Limits.MaxClassCandidates)
			{
				result.Error = ProcessEventHookError::ClassCandidateLimitExceeded;
				m_State->SetFailure(result.Error);
				return result;
			}
			operation->Candidates.push_back({
				.Class = type.Handle,
				.DefaultObject = *type.DefaultObject
			});
		}
		result.ClassCandidateCount = operation->Candidates.size();
		if (operation->Candidates.empty())
		{
			result.Error = ProcessEventHookError::NoPatchCandidates;
			m_State->SetFailure(result.Error);
			return result;
		}
		if (!m_State->TryBeginMaintenance())
		{
			result.Error = ProcessEventHookError::MaintenanceBusy;
			return result;
		}

		std::shared_ptr<State::ReconcileWork> work;
		try
		{
			work = std::make_shared<State::ReconcileWork>(m_State, operation);
		}
		catch (...)
		{
			m_State->EndMaintenance();
			throw;
		}
		const Runtime::GameThreadSubmitResult submitted =
			m_State->Executor->SubmitOwned(work, static_cast<int>(timeout.count()));
		work.reset();
		if (submitted == Runtime::GameThreadSubmitResult::Completed)
			return operation->Result;
		result.Error = MapSubmitError(submitted);
		m_State->LastError.store(result.Error, std::memory_order_release);
		return result;
	}
	catch (const std::bad_alloc&)
	{
		result.Error = ProcessEventHookError::AllocationFailed;
		m_State->SetFailure(result.Error);
		return result;
	}
	catch (...)
	{
		result.Error = ProcessEventHookError::GameThreadExecutionFailed;
		m_State->SetFailure(result.Error);
		return result;
	}
}

ProcessEventHookResult ProcessEventHookOwner::StopAndDrain(
	const std::chrono::milliseconds timeout) noexcept
{
	ProcessEventHookResult result;
	if (!m_State || timeout.count() <= 0)
	{
		result.Error = ProcessEventHookError::InvalidConfiguration;
		return result;
	}
	std::unique_lock<std::mutex> owner(m_State->ControlMutex);
	m_State->Stopping.store(true, std::memory_order_release);
	m_State->CoverageComplete.store(false, std::memory_order_release);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	{
		std::unique_lock<std::mutex> maintenance(m_State->MaintenanceMutex);
		if (!m_State->MaintenanceCondition.wait_until(maintenance, deadline, [state = m_State] {
			return state->MaintenanceInFlight.load(std::memory_order_acquire) == 0;
		}))
		{
			result.Error = ProcessEventHookError::MaintenanceDrainTimedOut;
			m_State->LastError.store(result.Error, std::memory_order_release);
			return result;
		}
	}

	m_State->Callbacks.BeginStopping();
	bool restored = true;
	for (auto patch = m_State->Patches.rbegin(); patch != m_State->Patches.rend(); ++patch)
	{
		if (patch->Token && patch->Token->IsActive() && !patch->Token->Disable())
			restored = false;
	}
	if (!restored)
	{
		result.Error = ProcessEventHookError::RestoreFailed;
		m_State->LastError.store(result.Error, std::memory_order_release);
		result.TotalPatchCount = m_State->Patches.size();
		return result;
	}

	const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
		deadline - std::chrono::steady_clock::now());
	if (remaining.count() <= 0 || !m_State->Callbacks.WaitForDrain(remaining))
	{
		result.Error = ProcessEventHookError::CallbackDrainTimedOut;
		m_State->LastError.store(result.Error, std::memory_order_release);
		return result;
	}
	std::shared_ptr<State> expected = m_State;
	if (!s_Active.compare_exchange_strong(
		expected,
		{},
		std::memory_order_acq_rel,
		std::memory_order_acquire)
		&& expected)
	{
		result.Error = ProcessEventHookError::ActiveOwnerConflict;
		m_State->LastError.store(result.Error, std::memory_order_release);
		return result;
	}
	if (!m_State->PublishCollectorConfiguration(false))
	{
		result.Error = ProcessEventHookError::CollectorConfigurationFailed;
		m_State->LastError.store(result.Error, std::memory_order_release);
		return result;
	}
	m_State->Patches.clear();
	m_State->Dispatch.store(nullptr, std::memory_order_release);
	m_State->Installed.store(false, std::memory_order_release);
	m_State->PatchCount.store(0, std::memory_order_release);
	m_State->LastError.store(ProcessEventHookError::None, std::memory_order_release);
	return result;
}

ProcessEventHookDiagnostics ProcessEventHookOwner::Diagnostics() const noexcept
{
	if (!m_State)
		return {};
	const bool installed = m_State->Installed.load(std::memory_order_acquire);
	const std::uint32_t callbackInFlight = m_State->Callbacks.InFlight();
	return {
		.Configured = m_State->Configured.load(std::memory_order_acquire),
		.Installed = installed,
		.CoverageComplete = m_State->CoverageComplete.load(std::memory_order_acquire),
		.Stopping = m_State->Stopping.load(std::memory_order_acquire),
		.SafeToUnload = !installed
			&& callbackInFlight == 0
			&& s_Active.load(std::memory_order_acquire).get() != m_State.get(),
		.ContextGeneration = m_State->Context ? m_State->Context->Generation() : 0,
		.CoveredObjectSnapshotGeneration =
			m_State->CoveredObjectGeneration.load(std::memory_order_acquire),
		.CoveredTypeSnapshotGeneration =
			m_State->CoveredTypeGeneration.load(std::memory_order_acquire),
		.ClassCandidateCount = m_State->ClassCandidateCount.load(std::memory_order_acquire),
		.PatchedVTableCount = m_State->PatchCount.load(std::memory_order_acquire),
		.CallbackInFlight = callbackInFlight,
		.MaintenanceInFlight = m_State->MaintenanceInFlight.load(std::memory_order_acquire),
		.EnterPublished = m_State->EnterPublished.load(std::memory_order_acquire),
		.ExitPublished = m_State->ExitPublished.load(std::memory_order_acquire),
		.UnmatchedFunction = m_State->UnmatchedFunction.load(std::memory_order_acquire),
		.DispatchMiss = m_State->DispatchMiss.load(std::memory_order_acquire),
		.CorrelationExhausted = m_State->CorrelationExhausted.load(std::memory_order_acquire),
		.LastError = m_State->LastError.load(std::memory_order_acquire)
	};
}

void ProcessEventHookOwner::HookedProcessEvent(
	void* const object,
	void* const function,
	void* const params)
{
	const std::shared_ptr<State> state = s_Active.load(std::memory_order_acquire);
	if (!state)
	{
		const auto fallback = reinterpret_cast<Runtime::ProcessEventFn>(
			s_FallbackOriginal.load(std::memory_order_acquire));
		if (fallback)
			fallback(object, function, params);
		return;
	}

	auto callback = state->Callbacks.Enter();
	Runtime::ProcessEventFn original = nullptr;
	void** vtable = nullptr;
	if (TryReadObjectVTable(object, vtable))
	{
		const std::uintptr_t vtableAddress = reinterpret_cast<std::uintptr_t>(vtable);
		const std::uintptr_t slotOffset = static_cast<std::uintptr_t>(state->ProcessEventIndex)
			* sizeof(void*);
		if (vtableAddress <= (std::numeric_limits<std::uintptr_t>::max)() - slotOffset)
		{
			const auto dispatch = state->Dispatch.load(std::memory_order_acquire);
			const State::DispatchEntry* entry = dispatch
				? dispatch->Find(vtableAddress + slotOffset) : nullptr;
			if (entry)
				original = entry->Original;
		}
	}
	if (!original)
	{
		state->DispatchMiss.fetch_add(1, std::memory_order_relaxed);
		original = reinterpret_cast<Runtime::ProcessEventFn>(state->CanonicalProcessEvent);
	}

	std::shared_ptr<const HookEnabledSnapshot> enabled;
	const HookProducerSubscription* subscription = nullptr;
	std::uint64_t correlation = 0;
	bool enterPublished = false;
	if (callback.OwnedWorkAllowed()
		&& !state->Stopping.load(std::memory_order_acquire)
		&& function)
	{
		enabled = state->CommandService->AcquireEnabledSnapshot();
		subscription = enabled
			? enabled->FindByFunctionAddress(reinterpret_cast<std::uintptr_t>(function))
			: nullptr;
		if (subscription
			&& subscription->Spec.Capture.Mode == HookCaptureMode::FixedMetadata)
		{
			correlation = state->NextCorrelation.fetch_add(1, std::memory_order_relaxed);
			if (correlation == 0 || correlation > kMaxProtocolInteger)
			{
				state->CorrelationExhausted.fetch_add(1, std::memory_order_relaxed);
				correlation = 0;
			}
			else
			{
				const Runtime::HookPublishResult published = state->Collector->TryPublish({
					.Kind = Runtime::HookEventKind::ProcessEventEnter,
					.Source = reinterpret_cast<std::uintptr_t>(function),
					.Subject = subscription->Id,
					.Correlation = correlation,
					.Payload = {},
					.Coalescible = false
				});
				if (published.Published())
				{
					state->EnterPublished.fetch_add(1, std::memory_order_relaxed);
					enterPublished = true;
				}
			}
		}
		else if (!subscription && enabled && enabled->Size() != 0)
			state->UnmatchedFunction.fetch_add(1, std::memory_order_relaxed);
	}

	if (original)
		original(object, function, params);

	if (subscription && correlation != 0 && enterPublished
		&& callback.OwnedWorkAllowed()
		&& !state->Stopping.load(std::memory_order_acquire))
	{
		const Runtime::HookPublishResult published = state->Collector->TryPublish({
			.Kind = Runtime::HookEventKind::ProcessEventExit,
			.Source = reinterpret_cast<std::uintptr_t>(function),
			.Subject = subscription->Id,
			.Correlation = correlation,
			.Payload = {},
			.Coalescible = false
		});
		if (published.Published())
			state->ExitPublished.fetch_add(1, std::memory_order_relaxed);
	}
}

} // namespace UExplorer::Services
