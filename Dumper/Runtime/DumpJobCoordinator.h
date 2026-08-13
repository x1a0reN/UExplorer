#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace UExplorer::Runtime
{

using DumpJobId = std::uint64_t;

enum class DumpJobError : std::uint8_t
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
	Busy,
	IdExhausted,
	NotFound,
	Terminal,
	InvalidLimit,
	AllocationFailed,
	DrainTimedOut,
	WorkerThreadDrainDenied
};

const char* ToString(DumpJobError error) noexcept;

enum class DumpJobState : std::uint8_t
{
	Queued,
	Running,
	Succeeded,
	Failed,
	Cancelled
};

const char* ToString(DumpJobState state) noexcept;

enum class DumpJobEventKind : std::uint8_t
{
	Progress,
	Diagnostic
};

const char* ToString(DumpJobEventKind kind) noexcept;

enum class DumpJobDiagnosticSeverity : std::uint8_t
{
	Info,
	Warning,
	Error
};

const char* ToString(DumpJobDiagnosticSeverity severity) noexcept;

struct DumpJobProgress
{
	std::string Phase;
	std::uint64_t Completed = 0;
	std::uint64_t Total = 0;
	std::string Message;
};

struct DumpJobDiagnostic
{
	DumpJobDiagnosticSeverity Severity = DumpJobDiagnosticSeverity::Info;
	std::string Code;
	std::string Message;
};

struct DumpJobEvent
{
	std::uint64_t Sequence = 0;
	std::uint64_t RecordedAtMonotonicUs = 0;
	DumpJobEventKind Kind = DumpJobEventKind::Diagnostic;
	DumpJobProgress Progress;
	DumpJobDiagnostic Diagnostic;
};

class IDumpJobInput
{
public:
	virtual ~IDumpJobInput() = default;
};

// OutputPathIdentity is an opaque logical identity at this boundary. The
// injected worker is responsible for resolving it inside its configured root.
struct DumpJobSpec
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::string OutputPathIdentity;
	std::vector<std::byte> OpaqueOptions;
	std::shared_ptr<const IDumpJobInput> Input;
};

// A request is copied into coordinator-owned storage before Submit returns and
// is exposed to the worker only through shared_ptr<const DumpJobRequest>.
struct DumpJobRequest
{
	DumpJobId Id = 0;
	DumpJobSpec Spec;
	std::chrono::steady_clock::time_point SubmittedAt;
	std::chrono::steady_clock::time_point Deadline;
};

enum class DumpJobWorkerStatus : std::uint8_t
{
	Succeeded,
	Failed,
	Cancelled
};

struct DumpJobWorkerResult
{
	DumpJobWorkerStatus Status = DumpJobWorkerStatus::Failed;
	std::string ErrorCode;
	std::string ErrorMessage;
	// Set only after the worker's externally visible success commit point. A
	// cancellation racing after that point cannot rewrite success as cancelled.
	bool SuccessCommitted = false;
};

class IDumpJobExecutionContext
{
public:
	virtual ~IDumpJobExecutionContext() = default;

	// Cancellation and deadlines are cooperative. Workers must poll at every
	// bounded unit of work and return without retaining this context reference.
	virtual bool IsCancellationRequested() noexcept = 0;
	virtual bool IsDeadlineExceeded() noexcept = 0;
	virtual bool ReportProgress(DumpJobProgress progress) noexcept = 0;
	virtual bool ReportDiagnostic(DumpJobDiagnostic diagnostic) noexcept = 0;
};

class IDumpJobWorker
{
public:
	virtual ~IDumpJobWorker() = default;

	// Execute is invoked without the coordinator mutex. The request and worker
	// are coordinator-owned for the complete callback lifetime.
	virtual DumpJobWorkerResult Execute(
		const std::shared_ptr<const DumpJobRequest>& request,
		IDumpJobExecutionContext& context) = 0;
};

struct DumpJobCoordinatorLimits
{
	std::size_t MaxRetainedJobs = 16;
	std::size_t MaxEventsPerJob = 256;
	std::size_t MaxEventBytesPerJob = 256 * 1024;
	std::size_t MaxOpaqueOptionsBytes = 64 * 1024;
	std::size_t MaxOutputPathIdentityBytes = 128;
	std::size_t MaxPhaseBytes = 256;
	std::size_t MaxDiagnosticCodeBytes = 256;
	std::size_t MaxMessageBytes = 4096;
};

struct DumpJobSummary
{
	std::shared_ptr<const DumpJobRequest> Request;
	DumpJobState State = DumpJobState::Queued;
	std::uint64_t StartedAtMonotonicUs = 0;
	std::uint64_t FinishedAtMonotonicUs = 0;
	bool CancellationRequested = false;
	bool DeadlineExceeded = false;
	std::string ErrorCode;
	std::string ErrorMessage;
	std::size_t RetainedEventCount = 0;
	std::size_t RetainedEventBytes = 0;
	std::uint64_t DroppedEventCount = 0;
	std::uint64_t LastEventSequence = 0;
};

struct DumpJobSubmitResult
{
	DumpJobError Error = DumpJobError::None;
	DumpJobId Id = 0;

	bool Ok() const noexcept { return Error == DumpJobError::None; }
};

enum class DumpJobCancelDisposition : std::uint8_t
{
	None,
	CancelledBeforeStart,
	CancellationRequested
};

struct DumpJobCancelResult
{
	DumpJobError Error = DumpJobError::None;
	DumpJobCancelDisposition Disposition = DumpJobCancelDisposition::None;

	bool Ok() const noexcept { return Error == DumpJobError::None; }
};

struct DumpJobSnapshotResult
{
	DumpJobError Error = DumpJobError::None;
	DumpJobSummary Summary;
	std::vector<DumpJobEvent> Events;
	bool MoreEventsAvailable = false;

	bool Ok() const noexcept { return Error == DumpJobError::None; }
};

struct DumpJobListResult
{
	DumpJobError Error = DumpJobError::None;
	std::vector<DumpJobSummary> Jobs;

	bool Ok() const noexcept { return Error == DumpJobError::None; }
};

struct DumpJobCoordinatorSnapshot
{
	bool Configured = false;
	bool Stopping = false;
	bool Stopped = false;
	bool WorkerRunning = false;
	std::size_t RetainedJobCount = 0;
	DumpJobId ActiveJobId = 0;
	DumpJobState ActiveJobState = DumpJobState::Cancelled;
	DumpJobId LastIssuedJobId = 0;
	std::uint64_t SubmittedJobCount = 0;
	std::uint64_t CompletedJobCount = 0;
	std::uint64_t RejectedBusyCount = 0;
	std::uint64_t DroppedEventCount = 0;
};

struct DumpJobStopResult
{
	DumpJobError Error = DumpJobError::None;

	bool Ok() const noexcept { return Error == DumpJobError::None; }
};

class DumpJobCoordinator final
{
public:
	static constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
	static constexpr std::size_t kHardMaxRetainedJobs = 256;
	static constexpr std::size_t kHardMaxEventsPerJob = 4096;
	static constexpr std::size_t kHardMaxEventBytesPerJob = 16 * 1024 * 1024;
	static constexpr std::size_t kHardMaxOpaqueOptionsBytes = 1024 * 1024;
	static constexpr std::size_t kHardMaxOutputPathIdentityBytes = 128;
	static constexpr std::int64_t kMaxDeadlineMs = 86'400'000;

	DumpJobCoordinator(
		std::string sessionId,
		std::uint64_t contextGeneration,
		std::shared_ptr<IDumpJobWorker> worker,
		DumpJobCoordinatorLimits limits = {});
	~DumpJobCoordinator();

	DumpJobCoordinator(const DumpJobCoordinator&) = delete;
	DumpJobCoordinator& operator=(const DumpJobCoordinator&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	DumpJobSubmitResult Submit(
		DumpJobSpec spec,
		std::chrono::milliseconds deadlineFromNow) noexcept;
	DumpJobCancelResult Cancel(DumpJobId id) noexcept;
	DumpJobSnapshotResult Snapshot(
		DumpJobId id,
		std::uint64_t afterEventSequence,
		std::size_t maxEvents) const noexcept;
	DumpJobListResult List(std::size_t maxJobs) const noexcept;
	DumpJobCoordinatorSnapshot Snapshot() const noexcept;
	DumpJobStopResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;

private:
	struct StoredEvent;
	struct JobRecord;
	class ExecutionContext;

	DumpJobError ValidateSpec(
		const DumpJobSpec& spec,
		std::chrono::milliseconds deadlineFromNow) const noexcept;
	std::shared_ptr<JobRecord> FindLocked(DumpJobId id) const noexcept;
	DumpJobSummary CopySummaryLocked(const JobRecord& record) const;
	bool AppendProgress(
		const std::shared_ptr<JobRecord>& record,
		DumpJobProgress progress) noexcept;
	bool AppendDiagnostic(
		const std::shared_ptr<JobRecord>& record,
		DumpJobDiagnostic diagnostic) noexcept;
	bool AppendEventLocked(JobRecord& record, DumpJobEvent event) noexcept;
	void FinalizeLocked(
		const std::shared_ptr<JobRecord>& record,
		DumpJobWorkerResult result,
		bool workerThrew) noexcept;
	void CancelQueuedLocked(
		const std::shared_ptr<JobRecord>& record,
		const char* code,
		const char* message) noexcept;
	void RequestStopLocked() noexcept;
	void WorkerLoop() noexcept;
	void JoinWorkerNoexcept() noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	std::shared_ptr<IDumpJobWorker> m_Worker;
	DumpJobCoordinatorLimits m_Limits;
	bool m_Configured = false;
	DumpJobError m_StartupError = DumpJobError::InvalidConfiguration;
	mutable std::mutex m_Mutex;
	std::mutex m_JoinMutex;
	std::condition_variable m_WorkAvailable;
	std::condition_variable m_Drained;
	std::deque<std::shared_ptr<JobRecord>> m_Jobs;
	std::shared_ptr<JobRecord> m_ActiveJob;
	std::thread m_WorkerThread;
	std::thread::id m_WorkerThreadId;
	bool m_StopRequested = false;
	bool m_Stopped = false;
	bool m_WorkerExited = true;
	DumpJobId m_NextJobId = 1;
	DumpJobId m_LastIssuedJobId = 0;
	std::uint64_t m_SubmittedJobCount = 0;
	std::uint64_t m_CompletedJobCount = 0;
	std::uint64_t m_RejectedBusyCount = 0;
	std::uint64_t m_DroppedEventCount = 0;
};

} // namespace UExplorer::Runtime
