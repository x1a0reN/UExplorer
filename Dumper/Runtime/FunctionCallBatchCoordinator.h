#pragma once

#include "ObjectHandle.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace UExplorer::Runtime
{

using FunctionCallBatchId = std::uint64_t;

enum class FunctionCallBatchError : std::uint8_t
{
	None,
	InvalidConfiguration,
	WorkerStartFailed,
	Stopped,
	InvalidRequest,
	SessionMismatch,
	ContextGenerationMismatch,
	SnapshotGenerationInvalid,
	DeadlineInvalid,
	ItemLimitExceeded,
	RequestBytesLimitExceeded,
	Busy,
	IdExhausted,
	NotFound,
	Terminal,
	AllocationFailed,
	DrainTimedOut,
	WorkerThreadDrainDenied
};

const char* ToString(FunctionCallBatchError error) noexcept;

enum class FunctionCallBatchPolicy : std::uint8_t
{
	ContinueOnError,
	StopOnFirstFailure
};

const char* ToString(FunctionCallBatchPolicy policy) noexcept;

enum class FunctionCallBatchState : std::uint8_t
{
	Queued,
	Running,
	Succeeded,
	Failed,
	Cancelled,
	DeadlineExceeded
};

const char* ToString(FunctionCallBatchState state) noexcept;

enum class FunctionCallBatchItemState : std::uint8_t
{
	Pending,
	Running,
	Succeeded,
	Failed,
	Cancelled,
	DeadlineExceeded,
	SkippedFailFast
};

const char* ToString(FunctionCallBatchItemState state) noexcept;

struct FunctionCallBatchBinding
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
};

// ExpectedFunctionPath must exactly equal Function.FullPath. SerializedRequest
// is an opaque, already serialized request body; the coordinator never parses
// it and takes ownership of all bytes before Submit returns.
struct FunctionCallBatchItem
{
	ObjectHandle Target;
	FunctionHandle Function;
	std::string ExpectedFunctionPath;
	std::vector<std::byte> SerializedRequest;
};

struct FunctionCallBatchSpec
{
	FunctionCallBatchBinding Binding;
	FunctionCallBatchPolicy Policy = FunctionCallBatchPolicy::StopOnFirstFailure;
	std::vector<FunctionCallBatchItem> Items;
};

// This request and every nested byte/string buffer are coordinator-owned and
// immutable for the complete worker callback lifetime.
struct FunctionCallBatchRequest
{
	FunctionCallBatchId Id = 0;
	FunctionCallBatchSpec Spec;
	std::chrono::steady_clock::time_point SubmittedAt;
	std::chrono::steady_clock::time_point Deadline;
};

enum class FunctionCallBatchWorkerStatus : std::uint8_t
{
	Succeeded,
	Failed,
	Cancelled
};

struct FunctionCallBatchWorkerResult
{
	FunctionCallBatchWorkerStatus Status = FunctionCallBatchWorkerStatus::Failed;
	std::vector<std::byte> SerializedResponse;
	std::string DiagnosticCode;
	std::string DiagnosticMessage;
};

class IFunctionCallBatchExecutionContext
{
public:
	virtual ~IFunctionCallBatchExecutionContext() = default;

	// Cancellation and deadline enforcement are cooperative while Execute is
	// running. The worker must not retain this context reference after return.
	virtual bool IsCancellationRequested() noexcept = 0;
	virtual bool IsDeadlineExceeded() noexcept = 0;
	virtual std::size_t MaxResultBytesPerItem() const noexcept = 0;
};

class IFunctionCallBatchWorker
{
public:
	virtual ~IFunctionCallBatchWorker() = default;

	// Calls are serialized in item order and occur without the coordinator
	// mutex. request is immutable and owns the item at itemIndex. The injected
	// worker itself is retained by shared_ptr until the coordinator is drained.
	virtual FunctionCallBatchWorkerResult Execute(
		const std::shared_ptr<const FunctionCallBatchRequest>& request,
		std::size_t itemIndex,
		IFunctionCallBatchExecutionContext& context) = 0;
};

struct FunctionCallBatchCoordinatorLimits
{
	std::size_t MaxItems = 64;
	std::size_t MaxRequestBytesPerItem = 256 * 1024;
	std::size_t MaxTotalRequestBytes = 4 * 1024 * 1024;
	std::size_t MaxResultBytesPerItem = 256 * 1024;
	std::size_t MaxTotalResultBytes = 4 * 1024 * 1024;
	std::size_t MaxDiagnosticCodeBytes = 256;
	std::size_t MaxDiagnosticMessageBytes = 4096;
	std::size_t MaxRetainedBatches = 8;
};

struct FunctionCallBatchItemResult
{
	std::size_t Index = 0;
	FunctionCallBatchItemState State = FunctionCallBatchItemState::Pending;
	std::uint64_t StartedAtMonotonicUs = 0;
	std::uint64_t FinishedAtMonotonicUs = 0;
	std::vector<std::byte> SerializedResponse;
	std::string DiagnosticCode;
	std::string DiagnosticMessage;
};

struct FunctionCallBatchSummary
{
	std::shared_ptr<const FunctionCallBatchRequest> Request;
	FunctionCallBatchState State = FunctionCallBatchState::Queued;
	std::uint64_t StartedAtMonotonicUs = 0;
	std::uint64_t FinishedAtMonotonicUs = 0;
	bool CancellationRequested = false;
	bool DeadlineWasExceeded = false;
	bool ShutdownCancellationRequested = false;
	std::size_t RetainedResultBytes = 0;
	std::vector<FunctionCallBatchItemResult> Items;
};

struct FunctionCallBatchSubmitResult
{
	FunctionCallBatchError Error = FunctionCallBatchError::None;
	FunctionCallBatchId Id = 0;

	bool Ok() const noexcept { return Error == FunctionCallBatchError::None; }
};

enum class FunctionCallBatchCancelDisposition : std::uint8_t
{
	None,
	CancelledBeforeStart,
	CancellationRequested
};

struct FunctionCallBatchCancelResult
{
	FunctionCallBatchError Error = FunctionCallBatchError::None;
	FunctionCallBatchCancelDisposition Disposition = FunctionCallBatchCancelDisposition::None;

	bool Ok() const noexcept { return Error == FunctionCallBatchError::None; }
};

struct FunctionCallBatchSnapshotResult
{
	FunctionCallBatchError Error = FunctionCallBatchError::None;
	FunctionCallBatchSummary Summary;

	bool Ok() const noexcept { return Error == FunctionCallBatchError::None; }
};

struct FunctionCallBatchCoordinatorSnapshot
{
	bool Configured = false;
	bool Stopping = false;
	bool Stopped = false;
	bool WorkerRunning = false;
	FunctionCallBatchId ActiveBatchId = 0;
	FunctionCallBatchId LastIssuedBatchId = 0;
	std::size_t RetainedBatchCount = 0;
	std::uint64_t SubmittedBatchCount = 0;
	std::uint64_t CompletedBatchCount = 0;
	std::uint64_t RejectedBusyCount = 0;
};

struct FunctionCallBatchStopResult
{
	FunctionCallBatchError Error = FunctionCallBatchError::None;

	bool Ok() const noexcept { return Error == FunctionCallBatchError::None; }
};

class FunctionCallBatchCoordinator final
{
public:
	static constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
	static constexpr std::size_t kHardMaxItems = 64;
	static constexpr std::size_t kHardMaxRequestBytesPerItem = 1024 * 1024;
	static constexpr std::size_t kHardMaxTotalRequestBytes = 16 * 1024 * 1024;
	static constexpr std::size_t kHardMaxResultBytesPerItem = 1024 * 1024;
	static constexpr std::size_t kHardMaxTotalResultBytes = 16 * 1024 * 1024;
	static constexpr std::size_t kHardMaxRetainedBatches = 64;
	static constexpr std::int64_t kMaxDeadlineMs = 86'400'000;

	FunctionCallBatchCoordinator(
		std::string sessionId,
		std::uint64_t contextGeneration,
		std::shared_ptr<IFunctionCallBatchWorker> worker,
		FunctionCallBatchCoordinatorLimits limits = {});
	~FunctionCallBatchCoordinator();

	FunctionCallBatchCoordinator(const FunctionCallBatchCoordinator&) = delete;
	FunctionCallBatchCoordinator& operator=(const FunctionCallBatchCoordinator&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	FunctionCallBatchSubmitResult Submit(
		FunctionCallBatchSpec spec,
		std::chrono::milliseconds deadlineFromNow) noexcept;
	FunctionCallBatchCancelResult Cancel(FunctionCallBatchId id) noexcept;
	FunctionCallBatchSnapshotResult Snapshot(FunctionCallBatchId id) const noexcept;
	FunctionCallBatchCoordinatorSnapshot Snapshot() const noexcept;
	FunctionCallBatchStopResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;

private:
	struct BatchRecord;
	class ExecutionContext;

	FunctionCallBatchError ValidateSpec(
		const FunctionCallBatchSpec& spec,
		std::chrono::milliseconds deadlineFromNow) const noexcept;
	std::shared_ptr<BatchRecord> FindLocked(FunctionCallBatchId id) const noexcept;
	FunctionCallBatchSummary CopySummaryLocked(const BatchRecord& record) const;
	void FinalizePendingLocked(
		BatchRecord& record,
		FunctionCallBatchItemState state,
		const char* code,
		const char* message) noexcept;
	bool StoreWorkerResultLocked(
		BatchRecord& record,
		std::size_t itemIndex,
		FunctionCallBatchWorkerResult result,
		bool workerThrew) noexcept;
	void CompleteLocked(
		const std::shared_ptr<BatchRecord>& record,
		FunctionCallBatchState state) noexcept;
	void RequestStopLocked() noexcept;
	void WorkerLoop() noexcept;
	void JoinWorkerNoexcept() noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	std::shared_ptr<IFunctionCallBatchWorker> m_Worker;
	FunctionCallBatchCoordinatorLimits m_Limits;
	bool m_Configured = false;
	FunctionCallBatchError m_StartupError = FunctionCallBatchError::InvalidConfiguration;
	mutable std::mutex m_Mutex;
	std::mutex m_JoinMutex;
	std::condition_variable m_WorkAvailable;
	std::condition_variable m_Drained;
	std::deque<std::shared_ptr<BatchRecord>> m_Records;
	std::shared_ptr<BatchRecord> m_ActiveBatch;
	std::thread m_WorkerThread;
	std::thread::id m_WorkerThreadId;
	bool m_StopRequested = false;
	bool m_Stopped = false;
	bool m_WorkerExited = true;
	FunctionCallBatchId m_NextBatchId = 1;
	FunctionCallBatchId m_LastIssuedBatchId = 0;
	std::uint64_t m_SubmittedBatchCount = 0;
	std::uint64_t m_CompletedBatchCount = 0;
	std::uint64_t m_RejectedBusyCount = 0;
};

} // namespace UExplorer::Runtime
