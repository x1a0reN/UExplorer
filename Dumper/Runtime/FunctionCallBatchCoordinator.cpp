#include "FunctionCallBatchCoordinator.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaxSessionIdBytes = 128;
constexpr std::size_t kMaxFunctionPathBytes = 4096;
constexpr std::size_t kTerminalDiagnosticReserveBytes = 512;
constexpr std::size_t kMinDiagnosticCodeBytes = 64;
constexpr std::size_t kMinDiagnosticMessageBytes = 256;
constexpr std::size_t kHardMaxDiagnosticCodeBytes = 4096;
constexpr std::size_t kHardMaxDiagnosticMessageBytes = 64 * 1024;

std::uint64_t MonotonicMicroseconds() noexcept
{
	const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	if (value <= 0)
		return 0;
	const auto unsignedValue = static_cast<std::uint64_t>(value);
	return (std::min)(unsignedValue, FunctionCallBatchCoordinator::kMaxProtocolInteger);
}

std::uint64_t SaturatingIncrement(const std::uint64_t value) noexcept
{
	return value == (std::numeric_limits<std::uint64_t>::max)() ? value : value + 1;
}

bool ContainsNul(const std::string_view value) noexcept
{
	return value.find('\0') != std::string_view::npos;
}

bool TryAddSize(
	const std::size_t left,
	const std::size_t right,
	std::size_t& result) noexcept
{
	if (left > (std::numeric_limits<std::size_t>::max)() - right)
		return false;
	result = left + right;
	return true;
}

bool IsTerminal(const FunctionCallBatchState state) noexcept
{
	return state == FunctionCallBatchState::Succeeded
		|| state == FunctionCallBatchState::Failed
		|| state == FunctionCallBatchState::Cancelled
		|| state == FunctionCallBatchState::DeadlineExceeded;
}

bool IsValidPolicy(const FunctionCallBatchPolicy policy) noexcept
{
	return policy == FunctionCallBatchPolicy::ContinueOnError
		|| policy == FunctionCallBatchPolicy::StopOnFirstFailure;
}

bool IsCanonicalFunctionIdentityPath(const std::string_view path) noexcept
{
	constexpr std::string_view prefix = "Function ";
	constexpr std::string_view tokenPrefix = "fname:";
	if (!path.starts_with(prefix) || path.size() > kMaxFunctionPathBytes)
		return false;

	std::size_t cursor = prefix.size();
	std::size_t tokenCount = 0;
	for (;;)
	{
		if (!path.substr(cursor).starts_with(tokenPrefix))
			return false;
		cursor += tokenPrefix.size();
		const std::size_t hexBegin = cursor;
		while (cursor < path.size()
			&& ((path[cursor] >= '0' && path[cursor] <= '9')
				|| (path[cursor] >= 'a' && path[cursor] <= 'f')))
		{
			++cursor;
		}
		if (cursor == hexBegin || cursor >= path.size() || path[cursor++] != ':')
			return false;
		const std::size_t numberBegin = cursor;
		while (cursor < path.size() && path[cursor] >= '0' && path[cursor] <= '9')
			++cursor;
		if (cursor == numberBegin)
			return false;
		++tokenCount;
		if (cursor == path.size())
			return tokenCount >= 2;
		if (path[cursor++] != '.')
			return false;
	}
}

bool IsExactHandle(
	const ObjectHandle& handle,
	const std::string& sessionId,
	const std::uint64_t contextGeneration) noexcept
{
	return handle.SessionId == sessionId
		&& handle.ContextGeneration == contextGeneration
		&& handle.Index >= 0
		&& handle.SerialNumber > 0
		&& handle.Address != 0
		&& handle.ClassFingerprint != 0;
}

bool ValidLimits(const FunctionCallBatchCoordinatorLimits& limits) noexcept
{
	return limits.MaxItems > 0
		&& limits.MaxItems <= FunctionCallBatchCoordinator::kHardMaxItems
		&& limits.MaxRequestBytesPerItem > 0
		&& limits.MaxRequestBytesPerItem
			<= FunctionCallBatchCoordinator::kHardMaxRequestBytesPerItem
		&& limits.MaxTotalRequestBytes >= limits.MaxRequestBytesPerItem
		&& limits.MaxTotalRequestBytes
			<= FunctionCallBatchCoordinator::kHardMaxTotalRequestBytes
		&& limits.MaxResultBytesPerItem >= kTerminalDiagnosticReserveBytes
		&& limits.MaxResultBytesPerItem
			<= FunctionCallBatchCoordinator::kHardMaxResultBytesPerItem
		&& limits.MaxTotalResultBytes >= limits.MaxResultBytesPerItem
		&& limits.MaxTotalResultBytes
			>= limits.MaxItems * kTerminalDiagnosticReserveBytes
		&& limits.MaxTotalResultBytes
			<= FunctionCallBatchCoordinator::kHardMaxTotalResultBytes
		&& limits.MaxDiagnosticCodeBytes >= kMinDiagnosticCodeBytes
		&& limits.MaxDiagnosticCodeBytes <= kHardMaxDiagnosticCodeBytes
		&& limits.MaxDiagnosticMessageBytes >= kMinDiagnosticMessageBytes
		&& limits.MaxDiagnosticMessageBytes <= kHardMaxDiagnosticMessageBytes
		&& limits.MaxRetainedBatches > 0
		&& limits.MaxRetainedBatches
			<= FunctionCallBatchCoordinator::kHardMaxRetainedBatches;
}

} // namespace

struct FunctionCallBatchCoordinator::BatchRecord
{
	explicit BatchRecord(std::shared_ptr<const FunctionCallBatchRequest> request)
		: Request(std::move(request))
	{
	}

	std::shared_ptr<const FunctionCallBatchRequest> Request;
	FunctionCallBatchState State = FunctionCallBatchState::Queued;
	std::atomic<bool> CancellationRequested{false};
	std::atomic<bool> DeadlineExceeded{false};
	bool ShutdownCancellationRequested = false;
	bool AnyItemFailed = false;
	std::uint64_t StartedAtMonotonicUs = 0;
	std::uint64_t FinishedAtMonotonicUs = 0;
	std::size_t RetainedResultBytes = 0;
	std::vector<FunctionCallBatchItemResult> Items;
};

class FunctionCallBatchCoordinator::ExecutionContext final
	: public IFunctionCallBatchExecutionContext
{
public:
	ExecutionContext(
		std::shared_ptr<BatchRecord> record,
		const std::size_t maxResultBytesPerItem) noexcept
		: m_Record(std::move(record)),
		  m_MaxResultBytesPerItem(maxResultBytesPerItem)
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

	std::size_t MaxResultBytesPerItem() const noexcept override
	{
		return m_MaxResultBytesPerItem;
	}

private:
	std::shared_ptr<BatchRecord> m_Record;
	std::size_t m_MaxResultBytesPerItem = 0;
};

const char* ToString(const FunctionCallBatchError error) noexcept
{
	switch (error)
	{
	case FunctionCallBatchError::None: return "NONE";
	case FunctionCallBatchError::InvalidConfiguration: return "CALL_BATCH_INVALID_CONFIGURATION";
	case FunctionCallBatchError::WorkerStartFailed: return "CALL_BATCH_WORKER_START_FAILED";
	case FunctionCallBatchError::Stopped: return "CALL_BATCH_COORDINATOR_STOPPED";
	case FunctionCallBatchError::InvalidRequest: return "CALL_BATCH_REQUEST_INVALID";
	case FunctionCallBatchError::SessionMismatch: return "CALL_BATCH_SESSION_MISMATCH";
	case FunctionCallBatchError::ContextGenerationMismatch: return "CALL_BATCH_CONTEXT_GENERATION_MISMATCH";
	case FunctionCallBatchError::SnapshotGenerationInvalid: return "CALL_BATCH_SNAPSHOT_GENERATION_INVALID";
	case FunctionCallBatchError::DeadlineInvalid: return "CALL_BATCH_DEADLINE_INVALID";
	case FunctionCallBatchError::ItemLimitExceeded: return "CALL_BATCH_ITEM_LIMIT_EXCEEDED";
	case FunctionCallBatchError::RequestBytesLimitExceeded: return "CALL_BATCH_REQUEST_BYTES_LIMIT_EXCEEDED";
	case FunctionCallBatchError::Busy: return "CALL_BATCH_BUSY";
	case FunctionCallBatchError::IdExhausted: return "CALL_BATCH_ID_EXHAUSTED";
	case FunctionCallBatchError::NotFound: return "CALL_BATCH_NOT_FOUND";
	case FunctionCallBatchError::Terminal: return "CALL_BATCH_TERMINAL";
	case FunctionCallBatchError::AllocationFailed: return "CALL_BATCH_ALLOCATION_FAILED";
	case FunctionCallBatchError::DrainTimedOut: return "CALL_BATCH_DRAIN_TIMED_OUT";
	case FunctionCallBatchError::WorkerThreadDrainDenied: return "CALL_BATCH_WORKER_THREAD_DRAIN_DENIED";
	}
	return "CALL_BATCH_UNKNOWN_ERROR";
}

const char* ToString(const FunctionCallBatchPolicy policy) noexcept
{
	switch (policy)
	{
	case FunctionCallBatchPolicy::ContinueOnError: return "continue_on_error";
	case FunctionCallBatchPolicy::StopOnFirstFailure: return "stop_on_first_failure";
	}
	return "unknown";
}

const char* ToString(const FunctionCallBatchState state) noexcept
{
	switch (state)
	{
	case FunctionCallBatchState::Queued: return "queued";
	case FunctionCallBatchState::Running: return "running";
	case FunctionCallBatchState::Succeeded: return "succeeded";
	case FunctionCallBatchState::Failed: return "failed";
	case FunctionCallBatchState::Cancelled: return "cancelled";
	case FunctionCallBatchState::DeadlineExceeded: return "deadline_exceeded";
	}
	return "unknown";
}

const char* ToString(const FunctionCallBatchItemState state) noexcept
{
	switch (state)
	{
	case FunctionCallBatchItemState::Pending: return "pending";
	case FunctionCallBatchItemState::Running: return "running";
	case FunctionCallBatchItemState::Succeeded: return "succeeded";
	case FunctionCallBatchItemState::Failed: return "failed";
	case FunctionCallBatchItemState::Cancelled: return "cancelled";
	case FunctionCallBatchItemState::DeadlineExceeded: return "deadline_exceeded";
	case FunctionCallBatchItemState::SkippedFailFast: return "skipped_fail_fast";
	}
	return "unknown";
}

FunctionCallBatchCoordinator::FunctionCallBatchCoordinator(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	std::shared_ptr<IFunctionCallBatchWorker> worker,
	FunctionCallBatchCoordinatorLimits limits)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Worker(std::move(worker)),
	  m_Limits(limits)
{
	const bool valid = !m_SessionId.empty()
		&& m_SessionId.size() <= kMaxSessionIdBytes
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
		m_WorkerThread = std::thread(&FunctionCallBatchCoordinator::WorkerLoop, this);
		m_Configured = true;
		m_StartupError = FunctionCallBatchError::None;
	}
	catch (...)
	{
		m_WorkerExited = true;
		m_Stopped = true;
		m_StartupError = FunctionCallBatchError::WorkerStartFailed;
	}
}

FunctionCallBatchCoordinator::~FunctionCallBatchCoordinator()
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		RequestStopLocked();
	}
	m_WorkAvailable.notify_all();
	JoinWorkerNoexcept();
}

FunctionCallBatchError FunctionCallBatchCoordinator::ValidateSpec(
	const FunctionCallBatchSpec& spec,
	const std::chrono::milliseconds deadlineFromNow) const noexcept
{
	if (deadlineFromNow.count() <= 0 || deadlineFromNow.count() > kMaxDeadlineMs)
		return FunctionCallBatchError::DeadlineInvalid;
	if (spec.Binding.SessionId != m_SessionId)
		return FunctionCallBatchError::SessionMismatch;
	if (spec.Binding.ContextGeneration != m_ContextGeneration)
		return FunctionCallBatchError::ContextGenerationMismatch;
	if (spec.Binding.ObjectSnapshotGeneration == 0
		|| spec.Binding.ObjectSnapshotGeneration > kMaxProtocolInteger
		|| spec.Binding.TypeSnapshotGeneration == 0
		|| spec.Binding.TypeSnapshotGeneration > kMaxProtocolInteger)
	{
		return FunctionCallBatchError::SnapshotGenerationInvalid;
	}
	if (!IsValidPolicy(spec.Policy))
		return FunctionCallBatchError::InvalidRequest;
	if (spec.Items.empty() || spec.Items.size() > m_Limits.MaxItems)
		return FunctionCallBatchError::ItemLimitExceeded;

	std::size_t totalRequestBytes = 0;
	for (const FunctionCallBatchItem& item : spec.Items)
	{
		if (!IsExactHandle(item.Target, m_SessionId, m_ContextGeneration)
			|| !IsExactHandle(item.Function.Function, m_SessionId, m_ContextGeneration)
			|| !IsExactHandle(item.Function.Owner, m_SessionId, m_ContextGeneration)
			|| item.Function.SignatureFingerprint == 0
			|| item.ExpectedFunctionPath != item.Function.FullPath
			|| !IsCanonicalFunctionIdentityPath(item.ExpectedFunctionPath))
		{
			return FunctionCallBatchError::InvalidRequest;
		}
		if (item.SerializedRequest.size() > m_Limits.MaxRequestBytesPerItem)
			return FunctionCallBatchError::RequestBytesLimitExceeded;
		if (!TryAddSize(totalRequestBytes, item.SerializedRequest.size(), totalRequestBytes)
			|| totalRequestBytes > m_Limits.MaxTotalRequestBytes)
		{
			return FunctionCallBatchError::RequestBytesLimitExceeded;
		}
	}
	return FunctionCallBatchError::None;
}

FunctionCallBatchSubmitResult FunctionCallBatchCoordinator::Submit(
	FunctionCallBatchSpec spec,
	const std::chrono::milliseconds deadlineFromNow) noexcept
{
	if (!m_Configured)
		return {m_StartupError, 0};
	const FunctionCallBatchError validation = ValidateSpec(spec, deadlineFromNow);
	if (validation != FunctionCallBatchError::None)
		return {validation, 0};

	try
	{
		const auto submittedAt = std::chrono::steady_clock::now();
		const auto deadline = submittedAt + deadlineFromNow;
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (m_StopRequested || m_Stopped)
			return {FunctionCallBatchError::Stopped, 0};
		if (m_ActiveBatch)
		{
			m_RejectedBusyCount = SaturatingIncrement(m_RejectedBusyCount);
			return {FunctionCallBatchError::Busy, 0};
		}
		if (m_NextBatchId == 0 || m_NextBatchId > kMaxProtocolInteger)
			return {FunctionCallBatchError::IdExhausted, 0};

		while (m_Records.size() >= m_Limits.MaxRetainedBatches)
		{
			const auto terminal = std::find_if(m_Records.begin(), m_Records.end(), [](const auto& record) {
				return record && IsTerminal(record->State);
			});
			if (terminal == m_Records.end())
				return {FunctionCallBatchError::Busy, 0};
			m_Records.erase(terminal);
		}

		const FunctionCallBatchId id = m_NextBatchId;
		auto request = std::make_shared<FunctionCallBatchRequest>(FunctionCallBatchRequest{
			id,
			std::move(spec),
			submittedAt,
			deadline});
		std::shared_ptr<const FunctionCallBatchRequest> immutableRequest = std::move(request);
		auto record = std::make_shared<BatchRecord>(std::move(immutableRequest));
		record->Items.resize(record->Request->Spec.Items.size());
		for (std::size_t index = 0; index < record->Items.size(); ++index)
			record->Items[index].Index = index;

		m_Records.push_back(record);
		m_ActiveBatch = record;
		m_LastIssuedBatchId = id;
		m_SubmittedBatchCount = SaturatingIncrement(m_SubmittedBatchCount);
		m_NextBatchId = id == kMaxProtocolInteger ? 0 : id + 1;
		lock.unlock();
		m_WorkAvailable.notify_one();
		return {FunctionCallBatchError::None, id};
	}
	catch (...)
	{
		return {FunctionCallBatchError::AllocationFailed, 0};
	}
}

std::shared_ptr<FunctionCallBatchCoordinator::BatchRecord>
FunctionCallBatchCoordinator::FindLocked(const FunctionCallBatchId id) const noexcept
{
	const auto found = std::find_if(m_Records.begin(), m_Records.end(), [id](const auto& record) {
		return record && record->Request && record->Request->Id == id;
	});
	return found == m_Records.end() ? nullptr : *found;
}

FunctionCallBatchSummary FunctionCallBatchCoordinator::CopySummaryLocked(
	const BatchRecord& record) const
{
	FunctionCallBatchSummary summary;
	summary.Request = record.Request;
	summary.State = record.State;
	summary.StartedAtMonotonicUs = record.StartedAtMonotonicUs;
	summary.FinishedAtMonotonicUs = record.FinishedAtMonotonicUs;
	summary.CancellationRequested = record.CancellationRequested.load(std::memory_order_acquire);
	summary.DeadlineWasExceeded = record.DeadlineExceeded.load(std::memory_order_acquire);
	summary.ShutdownCancellationRequested = record.ShutdownCancellationRequested;
	summary.RetainedResultBytes = record.RetainedResultBytes;
	summary.Items = record.Items;
	return summary;
}

FunctionCallBatchSnapshotResult FunctionCallBatchCoordinator::Snapshot(
	const FunctionCallBatchId id) const noexcept
{
	FunctionCallBatchSnapshotResult result;
	if (!m_Configured)
	{
		result.Error = m_StartupError;
		return result;
	}
	if (id == 0 || id > kMaxProtocolInteger)
	{
		result.Error = FunctionCallBatchError::InvalidRequest;
		return result;
	}

	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto record = FindLocked(id);
		if (!record)
		{
			result.Error = FunctionCallBatchError::NotFound;
			return result;
		}
		result.Summary = CopySummaryLocked(*record);
		return result;
	}
	catch (...)
	{
		result = {};
		result.Error = FunctionCallBatchError::AllocationFailed;
		return result;
	}
}

FunctionCallBatchCoordinatorSnapshot FunctionCallBatchCoordinator::Snapshot() const noexcept
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	FunctionCallBatchCoordinatorSnapshot snapshot;
	snapshot.Configured = m_Configured;
	snapshot.Stopping = m_StopRequested && !m_Stopped;
	snapshot.Stopped = m_Stopped;
	snapshot.WorkerRunning = !m_WorkerExited;
	if (m_ActiveBatch && m_ActiveBatch->Request)
		snapshot.ActiveBatchId = m_ActiveBatch->Request->Id;
	snapshot.LastIssuedBatchId = m_LastIssuedBatchId;
	snapshot.RetainedBatchCount = m_Records.size();
	snapshot.SubmittedBatchCount = m_SubmittedBatchCount;
	snapshot.CompletedBatchCount = m_CompletedBatchCount;
	snapshot.RejectedBusyCount = m_RejectedBusyCount;
	return snapshot;
}

void FunctionCallBatchCoordinator::FinalizePendingLocked(
	BatchRecord& record,
	const FunctionCallBatchItemState state,
	const char* const code,
	const char* const message) noexcept
{
	for (FunctionCallBatchItemResult& item : record.Items)
	{
		if (item.State != FunctionCallBatchItemState::Pending)
			continue;
		item.State = state;
		item.FinishedAtMonotonicUs = MonotonicMicroseconds();
		try
		{
			item.DiagnosticCode = code;
			item.DiagnosticMessage = message;
			std::size_t bytes = 0;
			if (TryAddSize(item.DiagnosticCode.size(), item.DiagnosticMessage.size(), bytes)
				&& bytes <= m_Limits.MaxTotalResultBytes - record.RetainedResultBytes)
			{
				record.RetainedResultBytes += bytes;
			}
			else
			{
				item.DiagnosticCode.clear();
				item.DiagnosticMessage.clear();
			}
		}
		catch (...)
		{
			item.DiagnosticCode.clear();
			item.DiagnosticMessage.clear();
		}
	}
}

bool FunctionCallBatchCoordinator::StoreWorkerResultLocked(
	BatchRecord& record,
	const std::size_t itemIndex,
	FunctionCallBatchWorkerResult result,
	const bool workerThrew) noexcept
{
	if (itemIndex >= record.Items.size())
		return false;
	FunctionCallBatchItemResult& item = record.Items[itemIndex];
	if (item.State != FunctionCallBatchItemState::Running)
		return false;

	auto storeLiteral = [this, &record, &item](
		const FunctionCallBatchItemState state,
		const char* const code,
		const char* const message) noexcept {
		item.State = state;
		item.SerializedResponse.clear();
		item.DiagnosticCode.clear();
		item.DiagnosticMessage.clear();
		try
		{
			item.DiagnosticCode = code;
			item.DiagnosticMessage = message;
			std::size_t bytes = 0;
			if (TryAddSize(item.DiagnosticCode.size(), item.DiagnosticMessage.size(), bytes)
				&& bytes <= m_Limits.MaxTotalResultBytes - record.RetainedResultBytes)
			{
				record.RetainedResultBytes += bytes;
			}
			else
			{
				item.DiagnosticCode.clear();
				item.DiagnosticMessage.clear();
			}
		}
		catch (...)
		{
			item.DiagnosticCode.clear();
			item.DiagnosticMessage.clear();
		}
		item.FinishedAtMonotonicUs = MonotonicMicroseconds();
	};

	const bool deadlineExceeded = record.DeadlineExceeded.load(std::memory_order_acquire)
		|| std::chrono::steady_clock::now() >= record.Request->Deadline;
	if (deadlineExceeded)
	{
		record.DeadlineExceeded.store(true, std::memory_order_release);
		record.CancellationRequested.store(true, std::memory_order_release);
	}
	if (workerThrew)
	{
		storeLiteral(
			FunctionCallBatchItemState::Failed,
			"CALL_BATCH_WORKER_EXCEPTION",
			"The injected item worker threw an exception.");
		record.AnyItemFailed = true;
		return true;
	}

	const bool knownStatus = result.Status == FunctionCallBatchWorkerStatus::Succeeded
		|| result.Status == FunctionCallBatchWorkerStatus::Failed
		|| result.Status == FunctionCallBatchWorkerStatus::Cancelled;
	const bool diagnosticShapeValid = result.DiagnosticCode.size() <= m_Limits.MaxDiagnosticCodeBytes
		&& result.DiagnosticMessage.size() <= m_Limits.MaxDiagnosticMessageBytes
		&& !ContainsNul(result.DiagnosticCode)
		&& !ContainsNul(result.DiagnosticMessage)
		&& (result.DiagnosticCode.empty() == result.DiagnosticMessage.empty()
			|| !result.DiagnosticCode.empty());
	const bool statusShapeValid = knownStatus
		&& (result.Status == FunctionCallBatchWorkerStatus::Succeeded
			|| (!result.DiagnosticCode.empty() && result.SerializedResponse.empty()));

	std::size_t resultBytes = result.SerializedResponse.size();
	bool resultSizeValid = resultBytes <= m_Limits.MaxResultBytesPerItem;
	resultSizeValid = resultSizeValid
		&& TryAddSize(resultBytes, result.DiagnosticCode.size(), resultBytes)
		&& TryAddSize(resultBytes, result.DiagnosticMessage.size(), resultBytes)
		&& resultBytes <= m_Limits.MaxResultBytesPerItem;
	const std::size_t remainingItems = record.Items.size() - itemIndex - 1;
	const std::size_t reservedBytes = remainingItems * kTerminalDiagnosticReserveBytes;
	const bool totalSizeValid = reservedBytes <= m_Limits.MaxTotalResultBytes
		&& record.RetainedResultBytes <= m_Limits.MaxTotalResultBytes - reservedBytes
		&& resultBytes <= m_Limits.MaxTotalResultBytes
			- reservedBytes - record.RetainedResultBytes;
	if (!diagnosticShapeValid || !statusShapeValid || !resultSizeValid || !totalSizeValid)
	{
		storeLiteral(
			FunctionCallBatchItemState::Failed,
			"CALL_BATCH_WORKER_RESULT_INVALID",
			"The injected worker returned an invalid or over-budget item result.");
		record.AnyItemFailed = true;
		return true;
	}

	try
	{
		item.SerializedResponse = std::move(result.SerializedResponse);
		item.DiagnosticCode = std::move(result.DiagnosticCode);
		item.DiagnosticMessage = std::move(result.DiagnosticMessage);
		record.RetainedResultBytes += resultBytes;
	}
	catch (...)
	{
		storeLiteral(
			FunctionCallBatchItemState::Failed,
			"CALL_BATCH_RESULT_ALLOCATION_FAILED",
			"The coordinator could not retain the bounded item result.");
		record.AnyItemFailed = true;
		return true;
	}

	switch (result.Status)
	{
	case FunctionCallBatchWorkerStatus::Succeeded:
		item.State = FunctionCallBatchItemState::Succeeded;
		break;
	case FunctionCallBatchWorkerStatus::Failed:
		item.State = FunctionCallBatchItemState::Failed;
		record.AnyItemFailed = true;
		break;
	case FunctionCallBatchWorkerStatus::Cancelled:
		item.State = deadlineExceeded
			? FunctionCallBatchItemState::DeadlineExceeded
			: FunctionCallBatchItemState::Cancelled;
		break;
	}
	item.FinishedAtMonotonicUs = MonotonicMicroseconds();
	return true;
}

void FunctionCallBatchCoordinator::CompleteLocked(
	const std::shared_ptr<BatchRecord>& record,
	const FunctionCallBatchState state) noexcept
{
	if (!record || IsTerminal(record->State))
		return;
	record->State = state;
	record->FinishedAtMonotonicUs = MonotonicMicroseconds();
	m_CompletedBatchCount = SaturatingIncrement(m_CompletedBatchCount);
	if (m_ActiveBatch == record)
		m_ActiveBatch.reset();
}

FunctionCallBatchCancelResult FunctionCallBatchCoordinator::Cancel(
	const FunctionCallBatchId id) noexcept
{
	if (!m_Configured)
		return {m_StartupError, FunctionCallBatchCancelDisposition::None};
	if (id == 0 || id > kMaxProtocolInteger)
		return {FunctionCallBatchError::InvalidRequest, FunctionCallBatchCancelDisposition::None};

	std::unique_lock<std::mutex> lock(m_Mutex);
	const auto record = FindLocked(id);
	if (!record)
		return {FunctionCallBatchError::NotFound, FunctionCallBatchCancelDisposition::None};
	if (record->State == FunctionCallBatchState::Queued)
	{
		record->CancellationRequested.store(true, std::memory_order_release);
		FinalizePendingLocked(
			*record,
			FunctionCallBatchItemState::Cancelled,
			"CALL_BATCH_CANCELLED_BEFORE_START",
			"The owned batch was cancelled before its first item started.");
		CompleteLocked(record, FunctionCallBatchState::Cancelled);
		lock.unlock();
		m_WorkAvailable.notify_all();
		m_Drained.notify_all();
		return {FunctionCallBatchError::None, FunctionCallBatchCancelDisposition::CancelledBeforeStart};
	}
	if (record->State == FunctionCallBatchState::Running)
	{
		record->CancellationRequested.store(true, std::memory_order_release);
		return {FunctionCallBatchError::None, FunctionCallBatchCancelDisposition::CancellationRequested};
	}
	return {FunctionCallBatchError::Terminal, FunctionCallBatchCancelDisposition::None};
}

void FunctionCallBatchCoordinator::RequestStopLocked() noexcept
{
	if (m_StopRequested)
		return;
	m_StopRequested = true;
	if (!m_ActiveBatch)
		return;
	m_ActiveBatch->ShutdownCancellationRequested = true;
	m_ActiveBatch->CancellationRequested.store(true, std::memory_order_release);
	if (m_ActiveBatch->State == FunctionCallBatchState::Queued)
	{
		const auto record = m_ActiveBatch;
		FinalizePendingLocked(
			*record,
			FunctionCallBatchItemState::Cancelled,
			"CALL_BATCH_SHUTDOWN_CANCELLED",
			"Shutdown cancelled the queued owned batch.");
		CompleteLocked(record, FunctionCallBatchState::Cancelled);
	}
}

void FunctionCallBatchCoordinator::WorkerLoop() noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_WorkerThreadId = std::this_thread::get_id();
	}

	for (;;)
	{
		std::shared_ptr<BatchRecord> record;
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_WorkAvailable.wait(lock, [this] {
				return m_StopRequested
					|| (m_ActiveBatch && m_ActiveBatch->State == FunctionCallBatchState::Queued);
			});
			if (m_StopRequested && !m_ActiveBatch)
				break;
			record = m_ActiveBatch;
			if (!record || record->State != FunctionCallBatchState::Queued)
				continue;

			const bool deadlineExceeded = std::chrono::steady_clock::now()
				>= record->Request->Deadline;
			if (deadlineExceeded)
			{
				record->DeadlineExceeded.store(true, std::memory_order_release);
				record->CancellationRequested.store(true, std::memory_order_release);
				FinalizePendingLocked(
					*record,
					FunctionCallBatchItemState::DeadlineExceeded,
					"CALL_BATCH_DEADLINE_EXCEEDED",
					"The immutable total deadline elapsed before execution started.");
				CompleteLocked(record, FunctionCallBatchState::DeadlineExceeded);
				m_Drained.notify_all();
				continue;
			}
			if (record->CancellationRequested.load(std::memory_order_acquire))
			{
				FinalizePendingLocked(
					*record,
					FunctionCallBatchItemState::Cancelled,
					record->ShutdownCancellationRequested
						? "CALL_BATCH_SHUTDOWN_CANCELLED"
						: "CALL_BATCH_CANCELLED_BEFORE_START",
					record->ShutdownCancellationRequested
						? "Shutdown cancelled the queued owned batch."
						: "The owned batch was cancelled before execution started.");
				CompleteLocked(record, FunctionCallBatchState::Cancelled);
				m_Drained.notify_all();
				continue;
			}
			record->State = FunctionCallBatchState::Running;
			record->StartedAtMonotonicUs = MonotonicMicroseconds();
		}

		bool completed = false;
		for (std::size_t itemIndex = 0; itemIndex < record->Items.size(); ++itemIndex)
		{
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				const bool deadlineExceeded = record->DeadlineExceeded.load(std::memory_order_acquire)
					|| std::chrono::steady_clock::now() >= record->Request->Deadline;
				if (deadlineExceeded)
				{
					record->DeadlineExceeded.store(true, std::memory_order_release);
					record->CancellationRequested.store(true, std::memory_order_release);
					FinalizePendingLocked(
						*record,
						FunctionCallBatchItemState::DeadlineExceeded,
						"CALL_BATCH_DEADLINE_EXCEEDED",
						"The immutable total deadline elapsed before this item started.");
					CompleteLocked(record, FunctionCallBatchState::DeadlineExceeded);
					completed = true;
				}
				else if (record->CancellationRequested.load(std::memory_order_acquire))
				{
					FinalizePendingLocked(
						*record,
						FunctionCallBatchItemState::Cancelled,
						record->ShutdownCancellationRequested
							? "CALL_BATCH_SHUTDOWN_CANCELLED"
							: "CALL_BATCH_CANCELLED",
						record->ShutdownCancellationRequested
							? "Shutdown cancelled the running owned batch."
							: "Cancellation was requested for the running owned batch.");
					CompleteLocked(record, FunctionCallBatchState::Cancelled);
					completed = true;
				}
				else
				{
					record->Items[itemIndex].State = FunctionCallBatchItemState::Running;
					record->Items[itemIndex].StartedAtMonotonicUs = MonotonicMicroseconds();
				}
			}
			if (completed)
				break;

			FunctionCallBatchWorkerResult workerResult;
			bool workerThrew = false;
			ExecutionContext context(record, m_Limits.MaxResultBytesPerItem);
			try
			{
				workerResult = m_Worker->Execute(record->Request, itemIndex, context);
			}
			catch (...)
			{
				workerThrew = true;
			}

			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				StoreWorkerResultLocked(*record, itemIndex, std::move(workerResult), workerThrew);
				const FunctionCallBatchItemState itemState = record->Items[itemIndex].State;
				const bool deadlineExceeded =
					record->DeadlineExceeded.load(std::memory_order_acquire)
					|| std::chrono::steady_clock::now() >= record->Request->Deadline;
				if (itemState == FunctionCallBatchItemState::DeadlineExceeded
					|| deadlineExceeded)
				{
					record->DeadlineExceeded.store(true, std::memory_order_release);
					record->CancellationRequested.store(true, std::memory_order_release);
					FinalizePendingLocked(
						*record,
						FunctionCallBatchItemState::DeadlineExceeded,
						"CALL_BATCH_DEADLINE_EXCEEDED",
						"The immutable total deadline elapsed before this item started.");
					CompleteLocked(record, FunctionCallBatchState::DeadlineExceeded);
					completed = true;
				}
				else if (itemState == FunctionCallBatchItemState::Cancelled
					|| record->CancellationRequested.load(std::memory_order_acquire))
				{
					FinalizePendingLocked(
						*record,
						FunctionCallBatchItemState::Cancelled,
						record->ShutdownCancellationRequested
							? "CALL_BATCH_SHUTDOWN_CANCELLED"
							: "CALL_BATCH_CANCELLED",
						record->ShutdownCancellationRequested
							? "Shutdown cancelled the running owned batch."
							: "The owned batch stopped after item cancellation.");
					CompleteLocked(record, FunctionCallBatchState::Cancelled);
					completed = true;
				}
				else if (itemState == FunctionCallBatchItemState::Failed
					&& record->Request->Spec.Policy == FunctionCallBatchPolicy::StopOnFirstFailure)
				{
					FinalizePendingLocked(
						*record,
						FunctionCallBatchItemState::SkippedFailFast,
						"CALL_BATCH_SKIPPED_FAIL_FAST",
						"The explicit fail-fast policy skipped this item.");
					CompleteLocked(record, FunctionCallBatchState::Failed);
					completed = true;
				}
			}
			if (completed)
				break;
		}

		if (!completed)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (std::chrono::steady_clock::now() >= record->Request->Deadline)
			{
				record->DeadlineExceeded.store(true, std::memory_order_release);
				record->CancellationRequested.store(true, std::memory_order_release);
				CompleteLocked(record, FunctionCallBatchState::DeadlineExceeded);
			}
			else if (record->CancellationRequested.load(std::memory_order_acquire))
			{
				CompleteLocked(record, FunctionCallBatchState::Cancelled);
			}
			else
			{
				CompleteLocked(
					record,
					record->AnyItemFailed
						? FunctionCallBatchState::Failed
						: FunctionCallBatchState::Succeeded);
			}
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

FunctionCallBatchStopResult FunctionCallBatchCoordinator::StopAndDrain(
	const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0 || timeout.count() > kMaxDeadlineMs)
		return {FunctionCallBatchError::DeadlineInvalid};
	if (!m_Configured)
		return {m_StartupError};

	std::unique_lock<std::mutex> joinLock(m_JoinMutex);
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (std::this_thread::get_id() == m_WorkerThreadId && !m_WorkerExited)
		return {FunctionCallBatchError::WorkerThreadDrainDenied};
	RequestStopLocked();
	lock.unlock();
	m_WorkAvailable.notify_all();
	lock.lock();
	const bool drained = m_Drained.wait_for(lock, timeout, [this] {
		return m_WorkerExited;
	});
	if (!drained)
		return {FunctionCallBatchError::DrainTimedOut};
	lock.unlock();
	if (m_WorkerThread.joinable())
		m_WorkerThread.join();
	return {};
}

void FunctionCallBatchCoordinator::JoinWorkerNoexcept() noexcept
{
	std::lock_guard<std::mutex> joinLock(m_JoinMutex);
	if (!m_WorkerThread.joinable())
		return;
	if (std::this_thread::get_id() == m_WorkerThread.get_id())
		std::terminate();
	m_WorkerThread.join();
}

} // namespace UExplorer::Runtime
