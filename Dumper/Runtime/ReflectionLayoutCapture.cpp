#include "ReflectionLayoutCapture.h"

#include "EngineFacade.h"

#include <utility>

namespace UExplorer::Runtime
{

const char* ToString(const ReflectionCandidateSourceError error) noexcept
{
	switch (error)
	{
	case ReflectionCandidateSourceError::None: return "NONE";
	case ReflectionCandidateSourceError::InvalidConfiguration:
		return "REFLECTION_SOURCE_INVALID";
	case ReflectionCandidateSourceError::SnapshotUnavailable:
		return "REFLECTION_SOURCE_SNAPSHOT_UNAVAILABLE";
	case ReflectionCandidateSourceError::DependencyChanged:
		return "REFLECTION_SOURCE_DEPENDENCY_CHANGED";
	case ReflectionCandidateSourceError::EvidenceUnavailable:
		return "REFLECTION_SOURCE_EVIDENCE_UNAVAILABLE";
	case ReflectionCandidateSourceError::EvidenceAmbiguous:
		return "REFLECTION_SOURCE_EVIDENCE_AMBIGUOUS";
	case ReflectionCandidateSourceError::MemoryUnavailable:
		return "REFLECTION_SOURCE_MEMORY_UNAVAILABLE";
	case ReflectionCandidateSourceError::ContractViolation:
		return "REFLECTION_SOURCE_CONTRACT_VIOLATION";
	case ReflectionCandidateSourceError::UnexpectedException:
		return "REFLECTION_SOURCE_EXCEPTION";
	}
	return "REFLECTION_SOURCE_UNKNOWN";
}

const char* ToString(const ReflectionLayoutCaptureState state) noexcept
{
	switch (state)
	{
	case ReflectionLayoutCaptureState::Idle: return "idle";
	case ReflectionLayoutCaptureState::Requested: return "requested";
	case ReflectionLayoutCaptureState::Capturing: return "capturing";
	case ReflectionLayoutCaptureState::Validating: return "validating";
	case ReflectionLayoutCaptureState::Publishing: return "publishing";
	case ReflectionLayoutCaptureState::Completed: return "completed";
	case ReflectionLayoutCaptureState::Failed: return "failed";
	case ReflectionLayoutCaptureState::Stopping: return "stopping";
	case ReflectionLayoutCaptureState::Stopped: return "stopped";
	}
	return "unknown";
}

const char* ToString(const ReflectionLayoutCaptureError error) noexcept
{
	switch (error)
	{
	case ReflectionLayoutCaptureError::None: return "NONE";
	case ReflectionLayoutCaptureError::Busy: return "REFLECTION_CAPTURE_BUSY";
	case ReflectionLayoutCaptureError::Stopped: return "REFLECTION_CAPTURE_STOPPED";
	case ReflectionLayoutCaptureError::AlreadyConfigured:
		return "REFLECTION_ALREADY_CONFIGURED";
	case ReflectionLayoutCaptureError::InvalidConfiguration:
		return "REFLECTION_CAPTURE_INVALID";
	case ReflectionLayoutCaptureError::InvalidPumpBudget:
		return "REFLECTION_CAPTURE_BUDGET_INVALID";
	case ReflectionLayoutCaptureError::ExecutionThreadInvalid:
		return "REFLECTION_CAPTURE_THREAD_INVALID";
	case ReflectionLayoutCaptureError::SourceContextMismatch:
		return "REFLECTION_CAPTURE_CONTEXT_MISMATCH";
	case ReflectionLayoutCaptureError::SourceRejected:
		return "REFLECTION_CAPTURE_SOURCE_REJECTED";
	case ReflectionLayoutCaptureError::SourceContractViolation:
		return "REFLECTION_CAPTURE_SOURCE_CONTRACT_VIOLATION";
	case ReflectionLayoutCaptureError::DependencyChanged:
		return "REFLECTION_CAPTURE_DEPENDENCY_CHANGED";
	case ReflectionLayoutCaptureError::CandidateRejected:
		return "REFLECTION_CAPTURE_CANDIDATE_REJECTED";
	case ReflectionLayoutCaptureError::PublicationRejected:
		return "REFLECTION_CAPTURE_PUBLICATION_REJECTED";
	case ReflectionLayoutCaptureError::UnexpectedException:
		return "REFLECTION_CAPTURE_EXCEPTION";
	}
	return "REFLECTION_CAPTURE_UNKNOWN";
}

ReflectionLayoutCapture::ReflectionLayoutCapture(
	const std::uint64_t contextGeneration,
	IReflectionCandidateSource& source,
	EngineFacade& engine,
	const bool includeFlatPropertyCodec)
	: m_ContextGeneration(contextGeneration),
	  m_Source(source),
	  m_Engine(engine),
	  m_IncludeFlatPropertyCodec(includeFlatPropertyCodec)
{
}

bool ReflectionLayoutCapture::IsConfigured() const noexcept
{
	return m_ContextGeneration != 0
		&& m_Source.ContextGeneration() == m_ContextGeneration
		&& m_Source.IsConfigured()
		&& m_Engine.ContextGeneration() == m_ContextGeneration
		&& m_Engine.IsConfigured();
}

ReflectionLayoutCaptureError ReflectionLayoutCapture::RequestCapture() noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_RequestMutex);
		if (StopRequested())
			return ReflectionLayoutCaptureError::Stopped;
		if (m_Engine.Reflection())
			return ReflectionLayoutCaptureError::AlreadyConfigured;
		if (!IsConfigured())
		{
			return m_Source.ContextGeneration() == m_ContextGeneration
				? ReflectionLayoutCaptureError::InvalidConfiguration
				: ReflectionLayoutCaptureError::SourceContextMismatch;
		}
		const ReflectionLayoutCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == ReflectionLayoutCaptureState::Requested
			|| state == ReflectionLayoutCaptureState::Capturing
			|| state == ReflectionLayoutCaptureState::Validating
			|| state == ReflectionLayoutCaptureState::Publishing)
		{
			return ReflectionLayoutCaptureError::Busy;
		}

		m_Source.Cancel();
		m_Candidate.reset();
		m_ValidatedLayout.reset();
		m_Error.store(ReflectionLayoutCaptureError::None, std::memory_order_release);
		m_SourceError.store(ReflectionCandidateSourceError::None, std::memory_order_release);
		m_ValidationError.store(ReflectionValidationError::None, std::memory_order_release);
		m_CapturedFields.store(0, std::memory_order_release);
		m_CapturedWitnesses.store(0, std::memory_order_release);
		m_SourceSteps.store(0, std::memory_order_release);
		m_State.store(ReflectionLayoutCaptureState::Requested, std::memory_order_release);
		return ReflectionLayoutCaptureError::None;
	}
	catch (...)
	{
		return ReflectionLayoutCaptureError::UnexpectedException;
	}
}

bool ReflectionLayoutCapture::StartRequestedCapture() noexcept
{
	if (m_Source.ContextGeneration() != m_ContextGeneration)
	{
		Fail(ReflectionLayoutCaptureError::SourceContextMismatch);
		return false;
	}
	if (!m_Source.IsCurrentExecutionThreadValid()
		|| !m_Engine.IsCurrentExecutionThreadValid())
	{
		Fail(ReflectionLayoutCaptureError::ExecutionThreadInvalid);
		return false;
	}

	try
	{
		const ReflectionCandidateSourceBeginResult begun = m_Source.Begin();
		if (!begun.Ok())
		{
			Fail(ReflectionLayoutCaptureError::SourceRejected, begun.Error);
			return false;
		}
		if (begun.PropertySystem == ReflectionPropertySystem::Unavailable
			|| begun.Source.empty()
			|| begun.Source.size() > ReflectionLayoutLimits::MaxSourceBytes)
		{
			Fail(
				ReflectionLayoutCaptureError::SourceContractViolation,
				ReflectionCandidateSourceError::ContractViolation);
			return false;
		}
		m_Candidate.emplace(ReflectionLayoutCandidate{
			.ContextGeneration = m_ContextGeneration,
			.PropertySystem = begun.PropertySystem,
			.Source = begun.Source
		});
		m_State.store(ReflectionLayoutCaptureState::Capturing, std::memory_order_release);
		return true;
	}
	catch (...)
	{
		Fail(
			ReflectionLayoutCaptureError::UnexpectedException,
			ReflectionCandidateSourceError::UnexpectedException);
		return false;
	}
}

ReflectionLayoutPumpResult ReflectionLayoutCapture::Pump(
	const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > kMaxPumpBudget)
	{
		return {
			.Status = ReflectionLayoutPumpStatus::InvalidBudget
		};
	}
	auto pumpLease = m_PumpBarrier.Enter();
	if (!pumpLease.OwnedWorkAllowed() || StopRequested())
		return {.Status = ReflectionLayoutPumpStatus::Stopping};
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
		return {.Status = ReflectionLayoutPumpStatus::Busy, .MoreWorkPending = true};
	struct PumpOwnerGuard final
	{
		std::atomic_flag& Owned;
		~PumpOwnerGuard() { Owned.clear(std::memory_order_release); }
	} pumpOwner{m_PumpOwned};

	std::size_t consumed = 0;
	try
	{
		ReflectionLayoutCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == ReflectionLayoutCaptureState::Idle
			|| state == ReflectionLayoutCaptureState::Completed
			|| state == ReflectionLayoutCaptureState::Failed)
		{
			return {.Status = ReflectionLayoutPumpStatus::Idle};
		}
		if (state == ReflectionLayoutCaptureState::Stopping
			|| state == ReflectionLayoutCaptureState::Stopped)
		{
			return {.Status = ReflectionLayoutPumpStatus::Stopping};
		}

		if (state == ReflectionLayoutCaptureState::Requested && consumed < workBudget)
		{
			++consumed;
			if (!StartRequestedCapture())
			{
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			state = ReflectionLayoutCaptureState::Capturing;
		}

		while (state == ReflectionLayoutCaptureState::Capturing && consumed < workBudget)
		{
			if (!m_Candidate
				|| m_Source.ContextGeneration() != m_ContextGeneration
				|| !m_Source.IsCurrentExecutionThreadValid()
				|| !m_Engine.IsCurrentExecutionThreadValid())
			{
				Fail(ReflectionLayoutCaptureError::ExecutionThreadInvalid);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}

			ReflectionCandidateSourceStepResult step = m_Source.CaptureNext();
			++consumed;
			const std::size_t sourceSteps =
				m_SourceSteps.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (!step.Ok())
			{
				Fail(ReflectionLayoutCaptureError::SourceRejected, step.Error);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}

			bool evidenceContractValid = step.Progressed
				&& sourceSteps <= kMaxSourceSteps;
			if (evidenceContractValid && step.Evidence)
			{
				const ReflectionCandidateEvidence& evidence = *step.Evidence;
				evidenceContractValid = !evidence.Witnesses.empty()
					&& evidence.Witnesses.size() <= kMaxWitnessesPerSourceStep
					&& m_Candidate->Fields.size() < ReflectionLayoutLimits::MaxFields
					&& evidence.Witnesses.size()
						<= ReflectionLayoutLimits::MaxWitnesses - m_Candidate->Witnesses.size();
				for (const ReflectionFieldWitness& witness : evidence.Witnesses)
				{
					if (witness.Field != evidence.Field.Field)
					{
						evidenceContractValid = false;
						break;
					}
				}
			}
			if (!evidenceContractValid)
			{
				Fail(
					ReflectionLayoutCaptureError::SourceContractViolation,
					ReflectionCandidateSourceError::ContractViolation);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			if (step.Evidence)
			{
				m_Candidate->Fields.push_back(std::move(step.Evidence->Field));
				for (ReflectionFieldWitness& witness : step.Evidence->Witnesses)
					m_Candidate->Witnesses.push_back(std::move(witness));
			}
			PublishCandidateDiagnostics();
			if (step.Complete)
			{
				m_State.store(ReflectionLayoutCaptureState::Validating, std::memory_order_release);
				state = ReflectionLayoutCaptureState::Validating;
			}
			if (StopRequested())
			{
				return {
					.Status = ReflectionLayoutPumpStatus::Stopping,
					.WorkConsumed = consumed
				};
			}
		}

		if (state == ReflectionLayoutCaptureState::Validating && consumed < workBudget)
		{
			++consumed;
			if (!m_Candidate || !m_Source.ValidateDependencies())
			{
				Fail(
					ReflectionLayoutCaptureError::DependencyChanged,
					ReflectionCandidateSourceError::DependencyChanged);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			const ReflectionLayoutValidationResult validated =
				ValidateReflectionLayout(*m_Candidate, m_Engine.Names());
			if (!validated.Ok())
			{
				Fail(
					ReflectionLayoutCaptureError::CandidateRejected,
					ReflectionCandidateSourceError::None,
					validated.Error);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			m_ValidatedLayout = validated.Layout;
			m_State.store(ReflectionLayoutCaptureState::Publishing, std::memory_order_release);
			state = ReflectionLayoutCaptureState::Publishing;
		}

		if (state == ReflectionLayoutCaptureState::Publishing && consumed < workBudget)
		{
			++consumed;
			if (!m_ValidatedLayout || !m_Source.ValidateDependencies())
			{
				Fail(
					ReflectionLayoutCaptureError::DependencyChanged,
					ReflectionCandidateSourceError::DependencyChanged);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			if (!m_Engine.ConfigureReflectionLayout(
				m_ValidatedLayout,
				m_IncludeFlatPropertyCodec))
			{
				Fail(ReflectionLayoutCaptureError::PublicationRejected);
				return {
					.Status = ReflectionLayoutPumpStatus::Failed,
					.WorkConsumed = consumed
				};
			}
			m_Candidate.reset();
			m_ValidatedLayout.reset();
			m_Source.Cancel();
			m_State.store(ReflectionLayoutCaptureState::Completed, std::memory_order_release);
			return {
				.Status = ReflectionLayoutPumpStatus::Published,
				.WorkConsumed = consumed
			};
		}

		const bool pending = state == ReflectionLayoutCaptureState::Capturing
			|| state == ReflectionLayoutCaptureState::Validating
			|| state == ReflectionLayoutCaptureState::Publishing;
		return {
			.Status = pending
				? ReflectionLayoutPumpStatus::Progress
				: ReflectionLayoutPumpStatus::Idle,
			.WorkConsumed = consumed,
			.MoreWorkPending = pending
		};
	}
	catch (...)
	{
		Fail(
			ReflectionLayoutCaptureError::UnexpectedException,
			ReflectionCandidateSourceError::UnexpectedException);
		return {
			.Status = ReflectionLayoutPumpStatus::Failed,
			.WorkConsumed = consumed
		};
	}
}

IGameThreadFrameClient::PumpResult ReflectionLayoutCapture::PumpFrame(
	const std::size_t workBudget) noexcept
{
	const ReflectionLayoutPumpResult result = Pump(workBudget);
	return {
		.WorkConsumed = result.WorkConsumed,
		.MoreWorkPending = result.MoreWorkPending
	};
}

void ReflectionLayoutCapture::Fail(
	const ReflectionLayoutCaptureError error,
	const ReflectionCandidateSourceError sourceError,
	const ReflectionValidationError validationError) noexcept
{
	PublishCandidateDiagnostics();
	m_Source.Cancel();
	m_Candidate.reset();
	m_ValidatedLayout.reset();
	m_Error.store(error, std::memory_order_release);
	m_SourceError.store(sourceError, std::memory_order_release);
	m_ValidationError.store(validationError, std::memory_order_release);
	m_State.store(ReflectionLayoutCaptureState::Failed, std::memory_order_release);
}

bool ReflectionLayoutCapture::StopRequested() const noexcept
{
	return m_StopRequested.load(std::memory_order_acquire);
}

void ReflectionLayoutCapture::PublishCandidateDiagnostics() noexcept
{
	if (!m_Candidate)
		return;
	m_CapturedFields.store(m_Candidate->Fields.size(), std::memory_order_release);
	m_CapturedWitnesses.store(m_Candidate->Witnesses.size(), std::memory_order_release);
}

ReflectionLayoutCaptureDiagnostics ReflectionLayoutCapture::Diagnostics() const noexcept
{
	return {
		.State = m_State.load(std::memory_order_acquire),
		.Error = m_Error.load(std::memory_order_acquire),
		.SourceError = m_SourceError.load(std::memory_order_acquire),
		.ValidationError = m_ValidationError.load(std::memory_order_acquire),
		.CapturedFields = m_CapturedFields.load(std::memory_order_acquire),
		.CapturedWitnesses = m_CapturedWitnesses.load(std::memory_order_acquire),
		.SourceSteps = m_SourceSteps.load(std::memory_order_acquire),
		.PumpInFlight = m_PumpBarrier.InFlight()
	};
}

bool ReflectionLayoutCapture::StopAndDrain(const std::chrono::milliseconds timeout)
{
	std::lock_guard<std::mutex> lock(m_RequestMutex);
	if (m_State.load(std::memory_order_acquire) == ReflectionLayoutCaptureState::Stopped)
		return true;
	m_StopRequested.store(true, std::memory_order_release);
	m_State.store(ReflectionLayoutCaptureState::Stopping, std::memory_order_release);
	m_PumpBarrier.BeginStopping();
	if (!m_PumpBarrier.WaitForDrain(timeout))
		return false;
	m_Source.Cancel();
	m_Candidate.reset();
	m_ValidatedLayout.reset();
	m_State.store(ReflectionLayoutCaptureState::Stopped, std::memory_order_release);
	return true;
}

} // namespace UExplorer::Runtime
