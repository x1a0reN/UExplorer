#include "DumpJobCoordinator.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

std::uint64_t MonotonicMicroseconds() noexcept
{
	const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t SaturatingIncrement(const std::uint64_t value) noexcept
{
	return value == (std::numeric_limits<std::uint64_t>::max)() ? value : value + 1;
}

std::size_t SaturatingAdd(const std::size_t left, const std::size_t right) noexcept
{
	return left > (std::numeric_limits<std::size_t>::max)() - right
		? (std::numeric_limits<std::size_t>::max)()
		: left + right;
}

bool ContainsNul(const std::string& value) noexcept
{
	return value.find('\0') != std::string::npos;
}

bool IsTerminal(const DumpJobState state) noexcept
{
	return state == DumpJobState::Succeeded
		|| state == DumpJobState::Failed
		|| state == DumpJobState::Cancelled;
}

bool ValidLimits(const DumpJobCoordinatorLimits& limits) noexcept
{
	return limits.MaxRetainedJobs > 0
		&& limits.MaxRetainedJobs <= DumpJobCoordinator::kHardMaxRetainedJobs
		&& limits.MaxEventsPerJob > 0
		&& limits.MaxEventsPerJob <= DumpJobCoordinator::kHardMaxEventsPerJob
		&& limits.MaxEventBytesPerJob > 0
		&& limits.MaxEventBytesPerJob <= DumpJobCoordinator::kHardMaxEventBytesPerJob
		&& limits.MaxOpaqueOptionsBytes <= DumpJobCoordinator::kHardMaxOpaqueOptionsBytes
		&& limits.MaxOutputPathIdentityBytes > 0
		&& limits.MaxOutputPathIdentityBytes <= DumpJobCoordinator::kHardMaxOutputPathIdentityBytes
		&& limits.MaxPhaseBytes > 0
		&& limits.MaxPhaseBytes <= limits.MaxEventBytesPerJob
		&& limits.MaxDiagnosticCodeBytes > 0
		&& limits.MaxDiagnosticCodeBytes <= limits.MaxEventBytesPerJob
		&& limits.MaxMessageBytes > 0
		&& limits.MaxMessageBytes <= limits.MaxEventBytesPerJob;
}

std::size_t EventBytes(const DumpJobEvent& event) noexcept
{
	if (event.Kind == DumpJobEventKind::Progress)
	{
		return SaturatingAdd(event.Progress.Phase.size(), event.Progress.Message.size());
	}
	return SaturatingAdd(event.Diagnostic.Code.size(), event.Diagnostic.Message.size());
}

} // namespace

struct DumpJobCoordinator::StoredEvent
{
	DumpJobEvent Event;
	std::size_t Bytes = 0;
};

struct DumpJobCoordinator::JobRecord
{
	JobRecord(
		std::shared_ptr<const DumpJobRequest> request,
		std::shared_ptr<const DumpJobRequest> retainedRequest)
		: Request(std::move(request)),
		  RetainedRequest(std::move(retainedRequest))
	{
	}

	std::shared_ptr<const DumpJobRequest> Request;
	// The terminal summary never pins the potentially large execution input.
	std::shared_ptr<const DumpJobRequest> RetainedRequest;
	DumpJobState State = DumpJobState::Queued;
	std::atomic<bool> CancellationRequested{false};
	std::atomic<bool> DeadlineExceeded{false};
	bool ShutdownCancellationRequested = false;
	std::uint64_t StartedAtMonotonicUs = 0;
	std::uint64_t FinishedAtMonotonicUs = 0;
	std::string ErrorCode;
	std::string ErrorMessage;
	std::deque<StoredEvent> Events;
	std::size_t RetainedEventBytes = 0;
	std::uint64_t DroppedEventCount = 0;
	std::uint64_t NextEventSequence = 1;
	std::uint64_t LastEventSequence = 0;
};

class DumpJobCoordinator::ExecutionContext final : public IDumpJobExecutionContext
{
public:
	ExecutionContext(
		DumpJobCoordinator& coordinator,
		std::shared_ptr<JobRecord> record) noexcept
		: m_Coordinator(coordinator),
		  m_Record(std::move(record))
	{
	}

	bool IsCancellationRequested() noexcept override
	{
		return IsDeadlineExceeded()
			|| m_Record->CancellationRequested.load(std::memory_order_acquire);
	}

	bool IsDeadlineExceeded() noexcept override
	{
		if (m_Record->DeadlineExceeded.load(std::memory_order_acquire))
			return true;
		if (std::chrono::steady_clock::now() < m_Record->Request->Deadline)
			return false;
		m_Record->DeadlineExceeded.store(true, std::memory_order_release);
		m_Record->CancellationRequested.store(true, std::memory_order_release);
		return true;
	}

	bool ReportProgress(DumpJobProgress progress) noexcept override
	{
		if (IsCancellationRequested())
			return false;
		return m_Coordinator.AppendProgress(m_Record, std::move(progress));
	}

	bool ReportDiagnostic(DumpJobDiagnostic diagnostic) noexcept override
	{
		if (IsCancellationRequested())
			return false;
		return m_Coordinator.AppendDiagnostic(m_Record, std::move(diagnostic));
	}

private:
	DumpJobCoordinator& m_Coordinator;
	std::shared_ptr<JobRecord> m_Record;
};

const char* ToString(const DumpJobError error) noexcept
{
	switch (error)
	{
	case DumpJobError::None: return "NONE";
	case DumpJobError::InvalidConfiguration: return "DUMP_INVALID_CONFIGURATION";
	case DumpJobError::WorkerStartFailed: return "DUMP_WORKER_START_FAILED";
	case DumpJobError::Stopped: return "DUMP_COORDINATOR_STOPPED";
	case DumpJobError::InvalidRequest: return "DUMP_REQUEST_INVALID";
	case DumpJobError::SessionMismatch: return "DUMP_SESSION_MISMATCH";
	case DumpJobError::ContextGenerationMismatch: return "DUMP_CONTEXT_GENERATION_MISMATCH";
	case DumpJobError::SnapshotGenerationInvalid: return "DUMP_SNAPSHOT_GENERATION_INVALID";
	case DumpJobError::DeadlineInvalid: return "DUMP_DEADLINE_INVALID";
	case DumpJobError::Busy: return "DUMP_JOB_BUSY";
	case DumpJobError::IdExhausted: return "DUMP_JOB_ID_EXHAUSTED";
	case DumpJobError::NotFound: return "DUMP_JOB_NOT_FOUND";
	case DumpJobError::Terminal: return "DUMP_JOB_TERMINAL";
	case DumpJobError::InvalidLimit: return "DUMP_LIMIT_INVALID";
	case DumpJobError::AllocationFailed: return "DUMP_ALLOCATION_FAILED";
	case DumpJobError::DrainTimedOut: return "DUMP_DRAIN_TIMED_OUT";
	case DumpJobError::WorkerThreadDrainDenied: return "DUMP_WORKER_THREAD_DRAIN_DENIED";
	}
	return "DUMP_UNKNOWN_ERROR";
}

const char* ToString(const DumpJobState state) noexcept
{
	switch (state)
	{
	case DumpJobState::Queued: return "queued";
	case DumpJobState::Running: return "running";
	case DumpJobState::Succeeded: return "succeeded";
	case DumpJobState::Failed: return "failed";
	case DumpJobState::Cancelled: return "cancelled";
	}
	return "unknown";
}

const char* ToString(const DumpJobEventKind kind) noexcept
{
	switch (kind)
	{
	case DumpJobEventKind::Progress: return "progress";
	case DumpJobEventKind::Diagnostic: return "diagnostic";
	}
	return "unknown";
}

const char* ToString(const DumpJobDiagnosticSeverity severity) noexcept
{
	switch (severity)
	{
	case DumpJobDiagnosticSeverity::Info: return "info";
	case DumpJobDiagnosticSeverity::Warning: return "warning";
	case DumpJobDiagnosticSeverity::Error: return "error";
	}
	return "unknown";
}

DumpJobCoordinator::DumpJobCoordinator(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	std::shared_ptr<IDumpJobWorker> worker,
	DumpJobCoordinatorLimits limits)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Worker(std::move(worker)),
	  m_Limits(limits)
{
	const bool valid = !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& !ContainsNul(m_SessionId)
		&& m_ContextGeneration > 0
		&& m_ContextGeneration <= kMaxProtocolInteger
		&& m_Worker
		&& ValidLimits(m_Limits);
	if (!valid)
	{
		m_Stopped = true;
		return;
	}

	try
	{
		m_WorkerExited = false;
		m_WorkerThread = std::thread(&DumpJobCoordinator::WorkerLoop, this);
		m_Configured = true;
		m_StartupError = DumpJobError::None;
	}
	catch (...)
	{
		m_WorkerExited = true;
		m_Stopped = true;
		m_StartupError = DumpJobError::WorkerStartFailed;
	}
}

DumpJobCoordinator::~DumpJobCoordinator()
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		RequestStopLocked();
	}
	m_WorkAvailable.notify_all();
	JoinWorkerNoexcept();
}

DumpJobError DumpJobCoordinator::ValidateSpec(
	const DumpJobSpec& spec,
	const std::chrono::milliseconds deadlineFromNow) const noexcept
{
	if (deadlineFromNow.count() <= 0 || deadlineFromNow.count() > kMaxDeadlineMs)
		return DumpJobError::DeadlineInvalid;
	if (spec.SessionId != m_SessionId)
		return DumpJobError::SessionMismatch;
	if (spec.ContextGeneration != m_ContextGeneration)
		return DumpJobError::ContextGenerationMismatch;
	if (spec.ObjectSnapshotGeneration == 0
		|| spec.ObjectSnapshotGeneration > kMaxProtocolInteger
		|| spec.TypeSnapshotGeneration == 0
		|| spec.TypeSnapshotGeneration > kMaxProtocolInteger)
	{
		return DumpJobError::SnapshotGenerationInvalid;
	}
	if (spec.OutputPathIdentity.empty()
		|| spec.OutputPathIdentity.size() > m_Limits.MaxOutputPathIdentityBytes
		|| ContainsNul(spec.OutputPathIdentity)
		|| spec.OpaqueOptions.size() > m_Limits.MaxOpaqueOptionsBytes
		|| !spec.Input)
	{
		return DumpJobError::InvalidRequest;
	}
	return DumpJobError::None;
}

DumpJobSubmitResult DumpJobCoordinator::Submit(
	DumpJobSpec spec,
	const std::chrono::milliseconds deadlineFromNow) noexcept
{
	if (!m_Configured)
		return {m_StartupError, 0};
	const DumpJobError validation = ValidateSpec(spec, deadlineFromNow);
	if (validation != DumpJobError::None)
		return {validation, 0};

	try
	{
		const auto submittedAt = std::chrono::steady_clock::now();
		const auto deadline = submittedAt + deadlineFromNow;
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (m_StopRequested || m_Stopped)
			return {DumpJobError::Stopped, 0};
		if (m_ActiveJob)
		{
			m_RejectedBusyCount = SaturatingIncrement(m_RejectedBusyCount);
			return {DumpJobError::Busy, 0};
		}
		if (m_NextJobId == 0 || m_NextJobId > kMaxProtocolInteger)
			return {DumpJobError::IdExhausted, 0};

		while (m_Jobs.size() >= m_Limits.MaxRetainedJobs)
		{
			const auto terminal = std::find_if(m_Jobs.begin(), m_Jobs.end(), [](const auto& job) {
				return job && IsTerminal(job->State);
			});
			if (terminal == m_Jobs.end())
			{
				m_RejectedBusyCount = SaturatingIncrement(m_RejectedBusyCount);
				return {DumpJobError::Busy, 0};
			}
			m_Jobs.erase(terminal);
		}

		const DumpJobId id = m_NextJobId;
		DumpJobSpec retainedSpec = spec;
		retainedSpec.Input.reset();
		auto retainedRequest = std::make_shared<DumpJobRequest>(DumpJobRequest{
			id,
			std::move(retainedSpec),
			submittedAt,
			deadline});
		auto request = std::make_shared<DumpJobRequest>(DumpJobRequest{
			id,
			std::move(spec),
			submittedAt,
			deadline});
		std::shared_ptr<const DumpJobRequest> immutableRequest = std::move(request);
		std::shared_ptr<const DumpJobRequest> immutableRetainedRequest =
			std::move(retainedRequest);
		auto record = std::make_shared<JobRecord>(
			std::move(immutableRequest),
			std::move(immutableRetainedRequest));
		m_Jobs.push_back(record);
		m_ActiveJob = record;
		m_LastIssuedJobId = id;
		m_SubmittedJobCount = SaturatingIncrement(m_SubmittedJobCount);
		m_NextJobId = id == kMaxProtocolInteger ? 0 : id + 1;
		lock.unlock();
		m_WorkAvailable.notify_one();
		return {DumpJobError::None, id};
	}
	catch (...)
	{
		return {DumpJobError::AllocationFailed, 0};
	}
}

std::shared_ptr<DumpJobCoordinator::JobRecord> DumpJobCoordinator::FindLocked(
	const DumpJobId id) const noexcept
{
	const auto found = std::find_if(m_Jobs.begin(), m_Jobs.end(), [id](const auto& record) {
		return record && record->Request && record->Request->Id == id;
	});
	return found == m_Jobs.end() ? nullptr : *found;
}

DumpJobCancelResult DumpJobCoordinator::Cancel(const DumpJobId id) noexcept
{
	if (!m_Configured)
		return {m_StartupError, DumpJobCancelDisposition::None};
	if (id == 0 || id > kMaxProtocolInteger)
		return {DumpJobError::InvalidRequest, DumpJobCancelDisposition::None};

	std::unique_lock<std::mutex> lock(m_Mutex);
	const auto record = FindLocked(id);
	if (!record)
		return {DumpJobError::NotFound, DumpJobCancelDisposition::None};
	if (record->State == DumpJobState::Queued)
	{
		record->CancellationRequested.store(true, std::memory_order_release);
		CancelQueuedLocked(
			record,
			"DUMP_JOB_CANCELLED_BEFORE_START",
			"The job was cancelled before its worker callback started.");
		if (m_ActiveJob == record)
			m_ActiveJob.reset();
		lock.unlock();
		m_WorkAvailable.notify_all();
		m_Drained.notify_all();
		return {DumpJobError::None, DumpJobCancelDisposition::CancelledBeforeStart};
	}
	if (record->State == DumpJobState::Running)
	{
		const bool alreadyRequested = record->CancellationRequested.exchange(
			true,
			std::memory_order_acq_rel);
		if (!alreadyRequested)
		{
			try
			{
				DumpJobEvent event;
				event.Kind = DumpJobEventKind::Diagnostic;
				event.Diagnostic.Severity = DumpJobDiagnosticSeverity::Warning;
				event.Diagnostic.Code = "DUMP_CANCELLATION_REQUESTED";
				event.Diagnostic.Message = "Cancellation was requested for the running job.";
				AppendEventLocked(*record, std::move(event));
			}
			catch (...)
			{
				record->DroppedEventCount = SaturatingIncrement(record->DroppedEventCount);
				m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
			}
		}
		return {DumpJobError::None, DumpJobCancelDisposition::CancellationRequested};
	}
	return {DumpJobError::Terminal, DumpJobCancelDisposition::None};
}

DumpJobSummary DumpJobCoordinator::CopySummaryLocked(const JobRecord& record) const
{
	DumpJobSummary summary;
	summary.Request = record.Request;
	summary.State = record.State;
	summary.StartedAtMonotonicUs = record.StartedAtMonotonicUs;
	summary.FinishedAtMonotonicUs = record.FinishedAtMonotonicUs;
	summary.CancellationRequested = record.CancellationRequested.load(std::memory_order_acquire);
	summary.DeadlineExceeded = record.DeadlineExceeded.load(std::memory_order_acquire);
	summary.ErrorCode = record.ErrorCode;
	summary.ErrorMessage = record.ErrorMessage;
	summary.RetainedEventCount = record.Events.size();
	summary.RetainedEventBytes = record.RetainedEventBytes;
	summary.DroppedEventCount = record.DroppedEventCount;
	summary.LastEventSequence = record.LastEventSequence;
	return summary;
}

DumpJobSnapshotResult DumpJobCoordinator::Snapshot(
	const DumpJobId id,
	const std::uint64_t afterEventSequence,
	const std::size_t maxEvents) const noexcept
{
	DumpJobSnapshotResult result;
	if (!m_Configured)
	{
		result.Error = m_StartupError;
		return result;
	}
	if (id == 0 || id > kMaxProtocolInteger || afterEventSequence > kMaxProtocolInteger)
	{
		result.Error = DumpJobError::InvalidRequest;
		return result;
	}
	if (maxEvents == 0 || maxEvents > m_Limits.MaxEventsPerJob)
	{
		result.Error = DumpJobError::InvalidLimit;
		return result;
	}

	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto record = FindLocked(id);
		if (!record)
		{
			result.Error = DumpJobError::NotFound;
			return result;
		}
		result.Summary = CopySummaryLocked(*record);
		result.Events.reserve((std::min)(maxEvents, record->Events.size()));
		for (const StoredEvent& stored : record->Events)
		{
			if (stored.Event.Sequence <= afterEventSequence)
				continue;
			if (result.Events.size() >= maxEvents)
			{
				result.MoreEventsAvailable = true;
				break;
			}
			result.Events.push_back(stored.Event);
		}
		return result;
	}
	catch (...)
	{
		result = {};
		result.Error = DumpJobError::AllocationFailed;
		return result;
	}
}

DumpJobListResult DumpJobCoordinator::List(const std::size_t maxJobs) const noexcept
{
	DumpJobListResult result;
	if (!m_Configured)
	{
		result.Error = m_StartupError;
		return result;
	}
	if (maxJobs == 0 || maxJobs > m_Limits.MaxRetainedJobs)
	{
		result.Error = DumpJobError::InvalidLimit;
		return result;
	}

	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		result.Jobs.reserve((std::min)(maxJobs, m_Jobs.size()));
		for (auto it = m_Jobs.rbegin(); it != m_Jobs.rend() && result.Jobs.size() < maxJobs; ++it)
		{
			if (*it)
				result.Jobs.push_back(CopySummaryLocked(**it));
		}
		return result;
	}
	catch (...)
	{
		result = {};
		result.Error = DumpJobError::AllocationFailed;
		return result;
	}
}

DumpJobCoordinatorSnapshot DumpJobCoordinator::Snapshot() const noexcept
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	DumpJobCoordinatorSnapshot snapshot;
	snapshot.Configured = m_Configured;
	snapshot.Stopping = m_StopRequested && !m_Stopped;
	snapshot.Stopped = m_Stopped;
	snapshot.WorkerRunning = !m_WorkerExited;
	snapshot.RetainedJobCount = m_Jobs.size();
	if (m_ActiveJob && m_ActiveJob->Request)
	{
		snapshot.ActiveJobId = m_ActiveJob->Request->Id;
		snapshot.ActiveJobState = m_ActiveJob->State;
	}
	snapshot.LastIssuedJobId = m_LastIssuedJobId;
	snapshot.SubmittedJobCount = m_SubmittedJobCount;
	snapshot.CompletedJobCount = m_CompletedJobCount;
	snapshot.RejectedBusyCount = m_RejectedBusyCount;
	snapshot.DroppedEventCount = m_DroppedEventCount;
	return snapshot;
}

bool DumpJobCoordinator::AppendProgress(
	const std::shared_ptr<JobRecord>& record,
	DumpJobProgress progress) noexcept
{
	if (!record
		|| progress.Phase.empty()
		|| progress.Phase.size() > m_Limits.MaxPhaseBytes
		|| ContainsNul(progress.Phase)
		|| progress.Message.size() > m_Limits.MaxMessageBytes
		|| ContainsNul(progress.Message)
		|| progress.Completed > kMaxProtocolInteger
		|| progress.Total > kMaxProtocolInteger
		|| (progress.Total != 0 && progress.Completed > progress.Total))
	{
		return false;
	}

	try
	{
		DumpJobEvent event;
		event.Kind = DumpJobEventKind::Progress;
		event.Progress = std::move(progress);
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (record->State != DumpJobState::Running || m_ActiveJob != record)
			return false;
		return AppendEventLocked(*record, std::move(event));
	}
	catch (...)
	{
		return false;
	}
}

bool DumpJobCoordinator::AppendDiagnostic(
	const std::shared_ptr<JobRecord>& record,
	DumpJobDiagnostic diagnostic) noexcept
{
	if (!record
		|| diagnostic.Code.empty()
		|| diagnostic.Code.size() > m_Limits.MaxDiagnosticCodeBytes
		|| ContainsNul(diagnostic.Code)
		|| diagnostic.Message.size() > m_Limits.MaxMessageBytes
		|| ContainsNul(diagnostic.Message))
	{
		return false;
	}

	try
	{
		DumpJobEvent event;
		event.Kind = DumpJobEventKind::Diagnostic;
		event.Diagnostic = std::move(diagnostic);
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (record->State != DumpJobState::Running || m_ActiveJob != record)
			return false;
		return AppendEventLocked(*record, std::move(event));
	}
	catch (...)
	{
		return false;
	}
}

bool DumpJobCoordinator::AppendEventLocked(JobRecord& record, DumpJobEvent event) noexcept
{
	const std::size_t bytes = EventBytes(event);
	if (bytes > m_Limits.MaxEventBytesPerJob
		|| record.NextEventSequence == 0
		|| record.NextEventSequence > kMaxProtocolInteger)
	{
		record.DroppedEventCount = SaturatingIncrement(record.DroppedEventCount);
		m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
		return false;
	}

	while (!record.Events.empty()
		&& (record.Events.size() >= m_Limits.MaxEventsPerJob
			|| record.RetainedEventBytes > m_Limits.MaxEventBytesPerJob - bytes))
	{
		record.RetainedEventBytes -= record.Events.front().Bytes;
		record.Events.pop_front();
		record.DroppedEventCount = SaturatingIncrement(record.DroppedEventCount);
		m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
	}

	try
	{
		event.Sequence = record.NextEventSequence;
		event.RecordedAtMonotonicUs = MonotonicMicroseconds();
		record.Events.push_back(StoredEvent{std::move(event), bytes});
		record.RetainedEventBytes += bytes;
		record.LastEventSequence = record.NextEventSequence;
		record.NextEventSequence = record.NextEventSequence == kMaxProtocolInteger
			? 0
			: record.NextEventSequence + 1;
		return true;
	}
	catch (...)
	{
		record.DroppedEventCount = SaturatingIncrement(record.DroppedEventCount);
		m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
		return false;
	}
}

void DumpJobCoordinator::CancelQueuedLocked(
	const std::shared_ptr<JobRecord>& record,
	const char* const code,
	const char* const message) noexcept
{
	if (!record || record->State != DumpJobState::Queued)
		return;
	record->State = DumpJobState::Cancelled;
	record->FinishedAtMonotonicUs = MonotonicMicroseconds();
	try
	{
		record->ErrorCode = code;
		record->ErrorMessage = message;
		DumpJobEvent event;
		event.Kind = DumpJobEventKind::Diagnostic;
		event.Diagnostic.Severity = DumpJobDiagnosticSeverity::Warning;
		event.Diagnostic.Code = code;
		event.Diagnostic.Message = message;
		AppendEventLocked(*record, std::move(event));
	}
	catch (...)
	{
		record->DroppedEventCount = SaturatingIncrement(record->DroppedEventCount);
		m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
	}
	m_CompletedJobCount = SaturatingIncrement(m_CompletedJobCount);
	if (record->RetainedRequest)
		record->Request = record->RetainedRequest;
}

void DumpJobCoordinator::FinalizeLocked(
	const std::shared_ptr<JobRecord>& record,
	DumpJobWorkerResult result,
	const bool workerThrew) noexcept
{
	if (!record || record->State != DumpJobState::Running)
		return;

	const bool committedSuccess = result.Status == DumpJobWorkerStatus::Succeeded
		&& result.SuccessCommitted;
	const bool deadlineExceeded = !committedSuccess
		&& (record->DeadlineExceeded.load(std::memory_order_acquire)
			|| std::chrono::steady_clock::now() >= record->Request->Deadline);
	if (deadlineExceeded)
	{
		record->DeadlineExceeded.store(true, std::memory_order_release);
		record->CancellationRequested.store(true, std::memory_order_release);
	}

	const bool cancellationRequested = record->CancellationRequested.load(std::memory_order_acquire);
	const char* fallbackCode = nullptr;
	const char* fallbackMessage = nullptr;
	DumpJobDiagnosticSeverity severity = DumpJobDiagnosticSeverity::Error;
	if (committedSuccess)
	{
		record->State = DumpJobState::Succeeded;
		fallbackCode = "DUMP_JOB_SUCCEEDED";
		fallbackMessage = "The owned dump job completed successfully.";
		severity = DumpJobDiagnosticSeverity::Info;
	}
	else if (deadlineExceeded)
	{
		record->State = DumpJobState::Cancelled;
		fallbackCode = "DUMP_DEADLINE_EXCEEDED";
		fallbackMessage = "The owned job exceeded its immutable deadline.";
		severity = DumpJobDiagnosticSeverity::Warning;
	}
	else if (cancellationRequested)
	{
		record->State = DumpJobState::Cancelled;
		fallbackCode = record->ShutdownCancellationRequested
			? "DUMP_SHUTDOWN_CANCELLED"
			: "DUMP_JOB_CANCELLED";
		fallbackMessage = record->ShutdownCancellationRequested
			? "Shutdown cancelled the running owned job."
			: "The running owned job observed cancellation.";
		severity = DumpJobDiagnosticSeverity::Warning;
	}
	else if (workerThrew)
	{
		record->State = DumpJobState::Failed;
		fallbackCode = "DUMP_WORKER_EXCEPTION";
		fallbackMessage = "The injected dump worker threw an exception.";
	}
	else if (result.Status == DumpJobWorkerStatus::Succeeded)
	{
		record->State = DumpJobState::Succeeded;
		fallbackCode = "DUMP_JOB_SUCCEEDED";
		fallbackMessage = "The owned dump job completed successfully.";
		severity = DumpJobDiagnosticSeverity::Info;
	}
	else if (result.Status == DumpJobWorkerStatus::Cancelled)
	{
		record->State = DumpJobState::Cancelled;
		fallbackCode = "DUMP_WORKER_CANCELLED";
		fallbackMessage = "The injected dump worker cancelled the owned job.";
		severity = DumpJobDiagnosticSeverity::Warning;
	}
	else
	{
		record->State = DumpJobState::Failed;
		const bool validWorkerError = !result.ErrorCode.empty()
			&& result.ErrorCode.size() <= m_Limits.MaxDiagnosticCodeBytes
			&& !ContainsNul(result.ErrorCode)
			&& result.ErrorMessage.size() <= m_Limits.MaxMessageBytes
			&& !ContainsNul(result.ErrorMessage);
		if (validWorkerError)
		{
			try
			{
				record->ErrorCode = std::move(result.ErrorCode);
				record->ErrorMessage = std::move(result.ErrorMessage);
			}
			catch (...)
			{
			}
		}
		else
		{
			fallbackCode = "DUMP_WORKER_RESULT_INVALID";
			fallbackMessage = "The injected worker returned an invalid failure diagnostic.";
		}
	}

	if (record->State == DumpJobState::Succeeded)
	{
		record->ErrorCode.clear();
		record->ErrorMessage.clear();
	}
	else if (record->ErrorCode.empty())
	{
		try
		{
			record->ErrorCode = fallbackCode ? fallbackCode : "DUMP_WORKER_FAILED";
			record->ErrorMessage = fallbackMessage ? fallbackMessage : "The injected dump worker failed.";
		}
		catch (...)
		{
		}
	}

	record->FinishedAtMonotonicUs = MonotonicMicroseconds();
	try
	{
		DumpJobEvent event;
		event.Kind = DumpJobEventKind::Diagnostic;
		event.Diagnostic.Severity = severity;
		if (record->State == DumpJobState::Succeeded)
		{
			event.Diagnostic.Code = fallbackCode;
			event.Diagnostic.Message = fallbackMessage;
		}
		else
		{
			event.Diagnostic.Code = record->ErrorCode.empty()
				? "DUMP_WORKER_FAILED"
				: record->ErrorCode;
			event.Diagnostic.Message = record->ErrorMessage;
		}
		AppendEventLocked(*record, std::move(event));
	}
	catch (...)
	{
		record->DroppedEventCount = SaturatingIncrement(record->DroppedEventCount);
		m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
	}
	m_CompletedJobCount = SaturatingIncrement(m_CompletedJobCount);
	if (record->RetainedRequest)
		record->Request = record->RetainedRequest;
}

void DumpJobCoordinator::RequestStopLocked() noexcept
{
	if (m_StopRequested)
		return;
	m_StopRequested = true;
	if (!m_ActiveJob)
		return;
	if (m_ActiveJob->State == DumpJobState::Queued)
	{
		m_ActiveJob->CancellationRequested.store(true, std::memory_order_release);
		m_ActiveJob->ShutdownCancellationRequested = true;
		CancelQueuedLocked(
			m_ActiveJob,
			"DUMP_SHUTDOWN_CANCELLED",
			"Shutdown cancelled the queued owned job.");
		m_ActiveJob.reset();
		return;
	}
	if (m_ActiveJob->State == DumpJobState::Running)
	{
		m_ActiveJob->ShutdownCancellationRequested = true;
		const bool alreadyRequested = m_ActiveJob->CancellationRequested.exchange(
			true,
			std::memory_order_acq_rel);
		if (!alreadyRequested)
		{
			try
			{
				DumpJobEvent event;
				event.Kind = DumpJobEventKind::Diagnostic;
				event.Diagnostic.Severity = DumpJobDiagnosticSeverity::Warning;
				event.Diagnostic.Code = "DUMP_SHUTDOWN_REQUESTED";
				event.Diagnostic.Message = "Shutdown requested cancellation of the running owned job.";
				AppendEventLocked(*m_ActiveJob, std::move(event));
			}
			catch (...)
			{
				m_ActiveJob->DroppedEventCount = SaturatingIncrement(m_ActiveJob->DroppedEventCount);
				m_DroppedEventCount = SaturatingIncrement(m_DroppedEventCount);
			}
		}
	}
}

void DumpJobCoordinator::WorkerLoop() noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_WorkerThreadId = std::this_thread::get_id();
	}

	for (;;)
	{
		std::shared_ptr<JobRecord> record;
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_WorkAvailable.wait(lock, [this] {
				return m_StopRequested
					|| (m_ActiveJob && m_ActiveJob->State == DumpJobState::Queued);
			});
			if (m_StopRequested && !m_ActiveJob)
				break;
			record = m_ActiveJob;
			if (!record || record->State != DumpJobState::Queued)
				continue;
			if (record->CancellationRequested.load(std::memory_order_acquire)
				|| std::chrono::steady_clock::now() >= record->Request->Deadline)
			{
				const bool deadlineExceeded = std::chrono::steady_clock::now() >= record->Request->Deadline;
				if (deadlineExceeded)
				{
					record->DeadlineExceeded.store(true, std::memory_order_release);
					CancelQueuedLocked(
						record,
						"DUMP_DEADLINE_EXCEEDED",
						"The queued owned job reached its deadline before starting.");
				}
				else
				{
					CancelQueuedLocked(
						record,
						"DUMP_JOB_CANCELLED_BEFORE_START",
						"The job was cancelled before its worker callback started.");
				}
				m_ActiveJob.reset();
				m_Drained.notify_all();
				continue;
			}
			record->State = DumpJobState::Running;
			record->StartedAtMonotonicUs = MonotonicMicroseconds();
		}

		DumpJobWorkerResult workerResult;
		bool workerThrew = false;
		ExecutionContext context(*this, record);
		try
		{
			workerResult = m_Worker->Execute(record->Request, context);
		}
		catch (...)
		{
			workerThrew = true;
		}

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			FinalizeLocked(record, std::move(workerResult), workerThrew);
			if (m_ActiveJob == record)
				m_ActiveJob.reset();
		}
		m_Drained.notify_all();
	}

	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_WorkerExited = true;
		m_Stopped = true;
	}
	m_Drained.notify_all();
}

DumpJobStopResult DumpJobCoordinator::StopAndDrain(
	const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0 || timeout.count() > kMaxDeadlineMs)
		return {DumpJobError::DeadlineInvalid};
	if (!m_Configured)
		return {m_StartupError};

	std::unique_lock<std::mutex> joinLock(m_JoinMutex);
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (std::this_thread::get_id() == m_WorkerThreadId && !m_WorkerExited)
		return {DumpJobError::WorkerThreadDrainDenied};
	RequestStopLocked();
	lock.unlock();
	m_WorkAvailable.notify_all();
	lock.lock();
	const bool drained = m_Drained.wait_for(lock, timeout, [this] {
		return m_WorkerExited;
	});
	if (!drained)
		return {DumpJobError::DrainTimedOut};
	lock.unlock();
	if (m_WorkerThread.joinable())
		m_WorkerThread.join();
	return {};
}

void DumpJobCoordinator::JoinWorkerNoexcept() noexcept
{
	std::lock_guard<std::mutex> joinLock(m_JoinMutex);
	if (!m_WorkerThread.joinable())
		return;
	if (std::this_thread::get_id() == m_WorkerThread.get_id())
		std::terminate();
	m_WorkerThread.join();
}

} // namespace UExplorer::Runtime
