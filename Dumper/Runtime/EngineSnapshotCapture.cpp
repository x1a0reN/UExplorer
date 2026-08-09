#include "EngineSnapshotCapture.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

std::uint64_t MonotonicMicroseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

const char* ToString(const SnapshotCaptureState state) noexcept
{
	switch (state)
	{
	case SnapshotCaptureState::Idle: return "idle";
	case SnapshotCaptureState::Requested: return "requested";
	case SnapshotCaptureState::Capturing: return "capturing";
	case SnapshotCaptureState::Validating: return "validating";
	case SnapshotCaptureState::Publishing: return "publishing";
	case SnapshotCaptureState::Completed: return "completed";
	case SnapshotCaptureState::Failed: return "failed";
	case SnapshotCaptureState::Stopping: return "stopping";
	case SnapshotCaptureState::Stopped: return "stopped";
	}
	return "unknown";
}

const char* ToString(const SnapshotCaptureError error) noexcept
{
	switch (error)
	{
	case SnapshotCaptureError::None: return "NONE";
	case SnapshotCaptureError::Busy: return "SNAPSHOT_CAPTURE_BUSY";
	case SnapshotCaptureError::Stopped: return "SNAPSHOT_CAPTURE_STOPPED";
	case SnapshotCaptureError::InvalidConfiguration: return "SNAPSHOT_CAPTURE_INVALID";
	case SnapshotCaptureError::GenerationExhausted: return "SNAPSHOT_GENERATION_EXHAUSTED";
	case SnapshotCaptureError::InvalidPumpBudget: return "SNAPSHOT_PUMP_BUDGET_INVALID";
	case SnapshotCaptureError::ExecutionThreadInvalid: return "SNAPSHOT_EXECUTION_THREAD_INVALID";
	case SnapshotCaptureError::SourceContextMismatch: return "SNAPSHOT_SOURCE_CONTEXT_MISMATCH";
	case SnapshotCaptureError::SourceCountInvalid: return "SNAPSHOT_SOURCE_COUNT_INVALID";
	case SnapshotCaptureError::SourceReadFailed: return "SNAPSHOT_SOURCE_READ_FAILED";
	case SnapshotCaptureError::SourceValidationFailed: return "SNAPSHOT_SOURCE_VALIDATION_FAILED";
	case SnapshotCaptureError::PublicationRejected: return "SNAPSHOT_PUBLICATION_REJECTED";
	case SnapshotCaptureError::UnexpectedException: return "SNAPSHOT_CAPTURE_EXCEPTION";
	}
	return "SNAPSHOT_CAPTURE_UNKNOWN_ERROR";
}

EngineSnapshotCapture::EngineSnapshotCapture(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	IEngineSnapshotSource& source,
	EngineSnapshotStore& store)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Source(source),
	  m_Store(store)
{
}

bool EngineSnapshotCapture::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_ContextGeneration != 0
		&& m_ContextGeneration <= EngineSnapshotStore::kMaxProtocolGeneration
		&& m_Source.ContextGeneration() == m_ContextGeneration
		&& m_Store.IsConfigured()
		&& !m_Store.IsStopped();
}

std::uint64_t EngineSnapshotCapture::AllocateGenerationLocked() noexcept
{
	const std::uint64_t current = m_Store.CurrentGeneration();
	if (current >= EngineSnapshotStore::kMaxProtocolGeneration)
		return 0;
	if (m_NextGeneration <= current)
		m_NextGeneration = current + 1;
	if (m_NextGeneration == 0
		|| m_NextGeneration > EngineSnapshotStore::kMaxProtocolGeneration)
	{
		return 0;
	}
	return m_NextGeneration++;
}

SnapshotCaptureRequestResult EngineSnapshotCapture::RequestCapture() noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_RequestMutex);
		if (StopRequested())
			return {.Error = SnapshotCaptureError::Stopped};
		if (m_Store.IsStopped())
			return {.Error = SnapshotCaptureError::Stopped};
		if (!IsConfigured())
			return {.Error = m_Source.ContextGeneration() == m_ContextGeneration
				? SnapshotCaptureError::InvalidConfiguration
				: SnapshotCaptureError::SourceContextMismatch};
		const SnapshotCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == SnapshotCaptureState::Requested
			|| state == SnapshotCaptureState::Capturing
			|| state == SnapshotCaptureState::Validating
			|| state == SnapshotCaptureState::Publishing)
		{
			return {.Error = SnapshotCaptureError::Busy};
		}
		const std::uint64_t generation = AllocateGenerationLocked();
		if (generation == 0)
			return {.Error = SnapshotCaptureError::GenerationExhausted};

		m_Error.store(SnapshotCaptureError::None, std::memory_order_release);
		m_ErrorIndex.store(-1, std::memory_order_release);
		m_RequestedGeneration.store(generation, std::memory_order_release);
		m_ActiveGeneration.store(0, std::memory_order_release);
		m_SourceObjectCount.store(0, std::memory_order_release);
		m_NextSlot.store(0, std::memory_order_release);
		m_CapturedObjects.store(0, std::memory_order_release);
		m_SkippedSlots.store(0, std::memory_order_release);
		m_State.store(SnapshotCaptureState::Requested, std::memory_order_release);
		return {.Generation = generation};
	}
	catch (...)
	{
		return {.Error = SnapshotCaptureError::UnexpectedException};
	}
}

bool EngineSnapshotCapture::StartRequestedCapture()
{
	if (m_Source.ContextGeneration() != m_ContextGeneration)
	{
		Fail(SnapshotCaptureError::SourceContextMismatch, -1);
		return false;
	}
	if (!m_Source.IsCurrentExecutionThreadValid())
	{
		Fail(SnapshotCaptureError::ExecutionThreadInvalid, -1);
		return false;
	}

	std::int32_t objectCount = -1;
	if (!m_Source.TryGetObjectCount(objectCount)
		|| objectCount < 0
		|| objectCount > EngineSnapshotStore::kMaxSourceObjectCount)
	{
		Fail(SnapshotCaptureError::SourceCountInvalid, -1);
		return false;
	}

	WorkingCapture working{
		.Generation = m_RequestedGeneration.load(std::memory_order_acquire),
		.StartedAtMonotonicUs = MonotonicMicroseconds(),
		.SourceObjectCount = objectCount
	};
	m_Working.emplace(std::move(working));
	m_ActiveGeneration.store(m_Working->Generation, std::memory_order_release);
	m_SourceObjectCount.store(objectCount, std::memory_order_release);
	m_State.store(SnapshotCaptureState::Capturing, std::memory_order_release);
	return true;
}

SnapshotPumpResult EngineSnapshotCapture::Pump(const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > kMaxPumpBudget)
		return SnapshotPumpResult::InvalidBudget;
	auto pumpLease = m_PumpBarrier.Enter();
	if (!pumpLease.OwnedWorkAllowed() || StopRequested())
		return SnapshotPumpResult::Stopping;
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
		return SnapshotPumpResult::Busy;
	struct PumpOwnerGuard
	{
		std::atomic_flag& Owned;
		~PumpOwnerGuard() { Owned.clear(std::memory_order_release); }
	} pumpOwner{m_PumpOwned};

	try
	{
		SnapshotCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == SnapshotCaptureState::Idle
			|| state == SnapshotCaptureState::Completed
			|| state == SnapshotCaptureState::Failed)
		{
			return SnapshotPumpResult::Idle;
		}
		if (state == SnapshotCaptureState::Stopping
			|| state == SnapshotCaptureState::Stopped)
		{
			return SnapshotPumpResult::Stopping;
		}
		if (state == SnapshotCaptureState::Requested)
		{
			if (!StartRequestedCapture())
				return SnapshotPumpResult::Failed;
			state = SnapshotCaptureState::Capturing;
		}
		if (!m_Working)
		{
			Fail(SnapshotCaptureError::UnexpectedException, -1);
			return SnapshotPumpResult::Failed;
		}
		if (m_Source.ContextGeneration() != m_ContextGeneration)
		{
			Fail(SnapshotCaptureError::SourceContextMismatch, m_Working->CaptureIndex);
			return SnapshotPumpResult::Failed;
		}
		if (!m_Source.IsCurrentExecutionThreadValid())
		{
			Fail(SnapshotCaptureError::ExecutionThreadInvalid, m_Working->CaptureIndex);
			return SnapshotPumpResult::Failed;
		}

		std::size_t remaining = workBudget;
		while (remaining > 0 && m_Working->CaptureIndex < m_Working->SourceObjectCount)
		{
			const std::int32_t index = m_Working->CaptureIndex;
			EngineSnapshotObject object;
			const SnapshotSlotReadResult read = m_Source.TryCaptureObject(index, object);
			if (StopRequested())
				return SnapshotPumpResult::Stopping;
			if (read == SnapshotSlotReadResult::Failed)
			{
				Fail(SnapshotCaptureError::SourceReadFailed, index);
				return SnapshotPumpResult::Failed;
			}
			if (read == SnapshotSlotReadResult::Captured)
			{
				if (object.Handle.Index != index)
				{
					Fail(SnapshotCaptureError::SourceReadFailed, index);
					return SnapshotPumpResult::Failed;
				}
				m_Working->CapturedObjects.push_back(std::move(object));
			}
			else if (read == SnapshotSlotReadResult::Empty)
			{
				++m_Working->SkippedSlots;
			}
			else
			{
				Fail(SnapshotCaptureError::SourceReadFailed, index);
				return SnapshotPumpResult::Failed;
			}
			++m_Working->CaptureIndex;
			--remaining;
		}

		if (m_Working->CaptureIndex == m_Working->SourceObjectCount
			&& m_State.load(std::memory_order_acquire) == SnapshotCaptureState::Capturing)
		{
			m_State.store(SnapshotCaptureState::Validating, std::memory_order_release);
		}

		while (remaining > 0 && m_Working->ValidationIndex < m_Working->SourceObjectCount)
		{
			const std::int32_t index = m_Working->ValidationIndex;
			const EngineSnapshotObject* expected = nullptr;
			if (m_Working->ValidationRecordIndex < m_Working->CapturedObjects.size()
				&& m_Working->CapturedObjects[m_Working->ValidationRecordIndex].Handle.Index == index)
			{
				expected = &m_Working->CapturedObjects[m_Working->ValidationRecordIndex];
			}
			if (!m_Source.ValidateSlot(index, expected))
			{
				Fail(SnapshotCaptureError::SourceValidationFailed, index);
				return SnapshotPumpResult::Failed;
			}
			if (expected)
				++m_Working->ValidationRecordIndex;
			++m_Working->ValidationIndex;
			--remaining;
			if (StopRequested())
				return SnapshotPumpResult::Stopping;
		}

		if (m_Working->ValidationIndex == m_Working->SourceObjectCount
			&& m_State.load(std::memory_order_acquire) == SnapshotCaptureState::Validating)
		{
			m_Working->PublishedObjects.reserve(m_Working->CapturedObjects.size());
			m_State.store(SnapshotCaptureState::Publishing, std::memory_order_release);
		}

		while (remaining > 0
			&& m_Working->PublishRecordIndex < m_Working->CapturedObjects.size())
		{
			m_Working->PublishedObjects.push_back(std::move(
				m_Working->CapturedObjects[m_Working->PublishRecordIndex]));
			++m_Working->PublishRecordIndex;
			--remaining;
			if (StopRequested())
				return SnapshotPumpResult::Stopping;
		}

		PublishDiagnostics(*m_Working);
		if (m_Working->ValidationIndex < m_Working->SourceObjectCount
			|| m_Working->PublishRecordIndex < m_Working->CapturedObjects.size())
			return SnapshotPumpResult::Progress;
		if (StopRequested())
			return SnapshotPumpResult::Stopping;

		const std::uint64_t finishedAt = MonotonicMicroseconds();
		EngineSnapshot snapshot{
			.SessionId = m_SessionId,
			.ContextGeneration = m_ContextGeneration,
			.Generation = m_Working->Generation,
			.CapturedAtMonotonicUs = finishedAt,
			.CaptureDurationUs = finishedAt >= m_Working->StartedAtMonotonicUs
				? finishedAt - m_Working->StartedAtMonotonicUs
				: 0,
			.SourceObjectCount = m_Working->SourceObjectCount,
			.SkippedSlots = m_Working->SkippedSlots,
			.Objects = std::move(m_Working->PublishedObjects)
		};
		const SnapshotPublishResult published = m_Store.Publish(std::move(snapshot));
		if (!published.Ok())
		{
			Fail(SnapshotCaptureError::PublicationRejected, published.RecordIndex);
			return SnapshotPumpResult::Failed;
		}

		m_Working.reset();
		m_Error.store(SnapshotCaptureError::None, std::memory_order_release);
		m_ErrorIndex.store(-1, std::memory_order_release);
		m_State.store(SnapshotCaptureState::Completed, std::memory_order_release);
		return SnapshotPumpResult::Published;
	}
	catch (...)
	{
		Fail(SnapshotCaptureError::UnexpectedException, -1);
		return SnapshotPumpResult::Failed;
	}
}

void EngineSnapshotCapture::Fail(
	const SnapshotCaptureError error,
	const std::int32_t index) noexcept
{
	if (m_Working)
		PublishDiagnostics(*m_Working);
	m_Working.reset();
	m_Error.store(error, std::memory_order_release);
	m_ErrorIndex.store(index, std::memory_order_release);
	m_State.store(SnapshotCaptureState::Failed, std::memory_order_release);
}

void EngineSnapshotCapture::PublishDiagnostics(const WorkingCapture& working) noexcept
{
	m_ActiveGeneration.store(working.Generation, std::memory_order_release);
	m_SourceObjectCount.store(working.SourceObjectCount, std::memory_order_release);
	m_NextSlot.store(
		m_State.load(std::memory_order_acquire) == SnapshotCaptureState::Validating
			? working.ValidationIndex
			: working.CaptureIndex,
		std::memory_order_release);
	m_CapturedObjects.store(
		static_cast<std::uint32_t>(working.CapturedObjects.size()),
		std::memory_order_release);
	m_SkippedSlots.store(working.SkippedSlots, std::memory_order_release);
}

bool EngineSnapshotCapture::StopRequested() const noexcept
{
	return m_StopRequested.load(std::memory_order_acquire);
}

SnapshotCaptureDiagnostics EngineSnapshotCapture::Diagnostics() const noexcept
{
	return {
		.State = m_State.load(std::memory_order_acquire),
		.Error = m_Error.load(std::memory_order_acquire),
		.RequestedGeneration = m_RequestedGeneration.load(std::memory_order_acquire),
		.ActiveGeneration = m_ActiveGeneration.load(std::memory_order_acquire),
		.SourceObjectCount = m_SourceObjectCount.load(std::memory_order_acquire),
		.NextSlot = m_NextSlot.load(std::memory_order_acquire),
		.CapturedObjects = m_CapturedObjects.load(std::memory_order_acquire),
		.SkippedSlots = m_SkippedSlots.load(std::memory_order_acquire),
		.ErrorIndex = m_ErrorIndex.load(std::memory_order_acquire),
		.PumpInFlight = m_PumpBarrier.InFlight()
	};
}

bool EngineSnapshotCapture::StopAndDrain(const std::chrono::milliseconds timeout)
{
	std::lock_guard<std::mutex> lock(m_RequestMutex);
	if (m_State.load(std::memory_order_acquire) == SnapshotCaptureState::Stopped)
		return true;
	m_StopRequested.store(true, std::memory_order_release);
	m_State.store(SnapshotCaptureState::Stopping, std::memory_order_release);
	m_PumpBarrier.BeginStopping();
	if (!m_PumpBarrier.WaitForDrain(timeout))
		return false;
	m_Working.reset();
	m_State.store(SnapshotCaptureState::Stopped, std::memory_order_release);
	return true;
}

} // namespace UExplorer::Runtime
