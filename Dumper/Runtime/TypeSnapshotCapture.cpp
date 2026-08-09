#include "TypeSnapshotCapture.h"

#include "EngineFacade.h"

#include <limits>
#include <new>
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

const char* ToString(const TypeSnapshotSourceError error) noexcept
{
	switch (error)
	{
	case TypeSnapshotSourceError::None: return "NONE";
	case TypeSnapshotSourceError::InvalidConfiguration: return "TYPE_SOURCE_INVALID";
	case TypeSnapshotSourceError::DependencyChanged: return "TYPE_SOURCE_DEPENDENCY_CHANGED";
	case TypeSnapshotSourceError::MemoryUnavailable: return "TYPE_SOURCE_MEMORY_UNAVAILABLE";
	case TypeSnapshotSourceError::EvidenceUnavailable: return "TYPE_SOURCE_EVIDENCE_UNAVAILABLE";
	case TypeSnapshotSourceError::EvidenceAmbiguous: return "TYPE_SOURCE_EVIDENCE_AMBIGUOUS";
	case TypeSnapshotSourceError::ContractViolation: return "TYPE_SOURCE_CONTRACT_VIOLATION";
	case TypeSnapshotSourceError::AllocationFailed: return "TYPE_SOURCE_ALLOCATION_FAILED";
	case TypeSnapshotSourceError::UnexpectedException: return "TYPE_SOURCE_EXCEPTION";
	}
	return "TYPE_SOURCE_UNKNOWN";
}

const char* ToString(const TypeSnapshotCaptureState state) noexcept
{
	switch (state)
	{
	case TypeSnapshotCaptureState::Idle: return "idle";
	case TypeSnapshotCaptureState::Requested: return "requested";
	case TypeSnapshotCaptureState::Capturing: return "capturing";
	case TypeSnapshotCaptureState::Validating: return "validating";
	case TypeSnapshotCaptureState::Sealing: return "sealing";
	case TypeSnapshotCaptureState::Ready: return "ready";
	case TypeSnapshotCaptureState::Publishing: return "publishing";
	case TypeSnapshotCaptureState::Completed: return "completed";
	case TypeSnapshotCaptureState::Failed: return "failed";
	case TypeSnapshotCaptureState::Stopping: return "stopping";
	case TypeSnapshotCaptureState::Stopped: return "stopped";
	}
	return "unknown";
}

const char* ToString(const TypeSnapshotCaptureError error) noexcept
{
	switch (error)
	{
	case TypeSnapshotCaptureError::None: return "NONE";
	case TypeSnapshotCaptureError::Busy: return "TYPE_CAPTURE_BUSY";
	case TypeSnapshotCaptureError::Stopped: return "TYPE_CAPTURE_STOPPED";
	case TypeSnapshotCaptureError::InvalidConfiguration: return "TYPE_CAPTURE_INVALID";
	case TypeSnapshotCaptureError::GenerationExhausted: return "TYPE_CAPTURE_GENERATION_EXHAUSTED";
	case TypeSnapshotCaptureError::InvalidPumpBudget: return "TYPE_CAPTURE_BUDGET_INVALID";
	case TypeSnapshotCaptureError::ExecutionThreadInvalid: return "TYPE_CAPTURE_THREAD_INVALID";
	case TypeSnapshotCaptureError::SourceContextMismatch: return "TYPE_CAPTURE_CONTEXT_MISMATCH";
	case TypeSnapshotCaptureError::SourceRejected: return "TYPE_CAPTURE_SOURCE_REJECTED";
	case TypeSnapshotCaptureError::SourceContractViolation:
		return "TYPE_CAPTURE_SOURCE_CONTRACT_VIOLATION";
	case TypeSnapshotCaptureError::DependencyChanged: return "TYPE_CAPTURE_DEPENDENCY_CHANGED";
	case TypeSnapshotCaptureError::PublicationRejected: return "TYPE_CAPTURE_PUBLICATION_REJECTED";
	case TypeSnapshotCaptureError::RetirementBackpressure:
		return "TYPE_CAPTURE_RETIREMENT_BACKPRESSURE";
	case TypeSnapshotCaptureError::AllocationFailed: return "TYPE_CAPTURE_ALLOCATION_FAILED";
	case TypeSnapshotCaptureError::UnexpectedException: return "TYPE_CAPTURE_EXCEPTION";
	}
	return "TYPE_CAPTURE_UNKNOWN";
}

TypeSnapshotCapture::TypeSnapshotCapture(
	const std::uint64_t contextGeneration,
	ITypeSnapshotSource& source,
	EngineFacade& engine)
	: m_ContextGeneration(contextGeneration),
	  m_Source(source),
	  m_Engine(engine)
{
}

bool TypeSnapshotCapture::IsConfigured() const noexcept
{
	return m_ContextGeneration != 0
		&& m_Source.ContextGeneration() == m_ContextGeneration
		&& m_Source.IsConfigured()
		&& m_Engine.ContextGeneration() == m_ContextGeneration
		&& m_Engine.IsConfigured()
		&& m_Engine.Types().IsConfigured()
		&& !m_Engine.Types().IsStopped();
}

TypeSnapshotCaptureError TypeSnapshotCapture::RequestCapture() noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_StateMutex);
		if (StopRequested() || m_Engine.Types().IsStopped())
			return TypeSnapshotCaptureError::Stopped;
		if (m_RetirementBackpressure.load(std::memory_order_acquire))
			return TypeSnapshotCaptureError::RetirementBackpressure;
		if (!IsConfigured())
		{
			return m_Source.ContextGeneration() == m_ContextGeneration
				? TypeSnapshotCaptureError::InvalidConfiguration
				: TypeSnapshotCaptureError::SourceContextMismatch;
		}
		const TypeSnapshotCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == TypeSnapshotCaptureState::Requested
			|| state == TypeSnapshotCaptureState::Capturing
			|| state == TypeSnapshotCaptureState::Validating
			|| state == TypeSnapshotCaptureState::Sealing
			|| state == TypeSnapshotCaptureState::Ready
			|| state == TypeSnapshotCaptureState::Publishing)
		{
			return TypeSnapshotCaptureError::Busy;
		}

		const std::uint64_t current = m_Engine.Types().CurrentGeneration();
		if (current >= EngineSnapshotStore::kMaxProtocolGeneration)
			return TypeSnapshotCaptureError::GenerationExhausted;
		const std::uint64_t generation = current + 1;
		if (generation == 0)
			return TypeSnapshotCaptureError::GenerationExhausted;

		m_Source.Cancel();
		m_Candidate.reset();
		m_ObjectDependency.reset();
		m_ReflectionDependency.reset();
		m_ExpectedTypeCount = 0;
		m_ExpectedFunctionCount = 0;
		m_FunctionCount = 0;
		m_MemberCount = 0;
		m_TypeOpen = false;
		m_FunctionOpen = false;
		m_ValidationBegun = false;
		m_StartedAtMonotonicUs = 0;
		m_Error.store(TypeSnapshotCaptureError::None, std::memory_order_release);
		m_SourceError.store(TypeSnapshotSourceError::None, std::memory_order_release);
		m_PublishError.store(TypeSnapshotPublishError::None, std::memory_order_release);
		m_RequestedGeneration.store(generation, std::memory_order_release);
		m_ActiveGeneration.store(0, std::memory_order_release);
		m_ObjectSnapshotGeneration.store(0, std::memory_order_release);
		m_ReflectionLayoutFingerprint.store(0, std::memory_order_release);
		m_CapturedTypes.store(0, std::memory_order_release);
		m_CapturedFunctions.store(0, std::memory_order_release);
		m_CapturedMembers.store(0, std::memory_order_release);
		m_SourceSteps.store(0, std::memory_order_release);
		m_ValidationSteps.store(0, std::memory_order_release);
		m_State.store(TypeSnapshotCaptureState::Requested, std::memory_order_release);
		return TypeSnapshotCaptureError::None;
	}
	catch (...)
	{
		return TypeSnapshotCaptureError::UnexpectedException;
	}
}

bool TypeSnapshotCapture::StartRequestedCapture() noexcept
{
	if (m_Source.ContextGeneration() != m_ContextGeneration)
	{
		Fail(TypeSnapshotCaptureError::SourceContextMismatch);
		return false;
	}
	if (!m_Source.IsCurrentExecutionThreadValid()
		|| !m_Engine.IsCurrentExecutionThreadValid())
	{
		Fail(TypeSnapshotCaptureError::ExecutionThreadInvalid);
		return false;
	}

	try
	{
		TypeSnapshotSourceBeginResult begun = m_Source.Begin();
		if (!begun.Ok())
		{
			Fail(TypeSnapshotCaptureError::SourceRejected, begun.Error);
			return false;
		}
		const std::shared_ptr<const EngineSnapshot> currentObjects =
			m_Engine.Snapshots().Current();
		const std::shared_ptr<const ReflectionRuntimeSnapshot> currentReflection =
			m_Engine.Reflection();
		if (begun.Source.empty()
			|| begun.Source.size() > TypeSnapshotStore::kMaxSourceBytes
			|| !begun.ObjectSnapshot
			|| !begun.Reflection
			|| !begun.Reflection->Layout
			|| begun.ObjectSnapshot != currentObjects
			|| begun.Reflection != currentReflection
			|| begun.ObjectSnapshot->ContextGeneration != m_ContextGeneration
			|| begun.ObjectSnapshot->SessionId != m_Engine.SessionId()
			|| !begun.Reflection->IsLayoutConfigured(m_ContextGeneration)
			|| begun.ExpectedTypeCount > TypeSnapshotStore::kMaxTypeRecords
			|| begun.ExpectedFunctionCount > TypeSnapshotStore::kMaxTotalMembers)
		{
			Fail(
				TypeSnapshotCaptureError::SourceContractViolation,
				TypeSnapshotSourceError::ContractViolation);
			return false;
		}

		m_ObjectDependency = std::move(begun.ObjectSnapshot);
		m_ReflectionDependency = std::move(begun.Reflection);
		m_ExpectedTypeCount = begun.ExpectedTypeCount;
		m_ExpectedFunctionCount = begun.ExpectedFunctionCount;
		m_StartedAtMonotonicUs = MonotonicMicroseconds();
		m_Candidate.emplace(TypeSnapshotCandidate{
			.SessionId = m_Engine.SessionId(),
			.ContextGeneration = m_ContextGeneration,
			.Generation = m_RequestedGeneration.load(std::memory_order_acquire),
			.ObjectSnapshotGeneration = m_ObjectDependency->Generation,
			.ReflectionLayoutFingerprint = m_ReflectionDependency->Layout->Fingerprint(),
			.Source = std::move(begun.Source)
		});
		m_ActiveGeneration.store(m_Candidate->Generation, std::memory_order_release);
		m_ObjectSnapshotGeneration.store(
			m_Candidate->ObjectSnapshotGeneration,
			std::memory_order_release);
		m_ReflectionLayoutFingerprint.store(
			m_Candidate->ReflectionLayoutFingerprint,
			std::memory_order_release);
		m_State.store(TypeSnapshotCaptureState::Capturing, std::memory_order_release);
		return true;
	}
	catch (const std::bad_alloc&)
	{
		Fail(
			TypeSnapshotCaptureError::AllocationFailed,
			TypeSnapshotSourceError::AllocationFailed);
		return false;
	}
	catch (...)
	{
		Fail(
			TypeSnapshotCaptureError::UnexpectedException,
			TypeSnapshotSourceError::UnexpectedException);
		return false;
	}
}

TypeSnapshotSourceError TypeSnapshotCapture::ApplyRecord(
	TypeSnapshotSourceRecord record) noexcept
{
	if (!m_Candidate)
		return TypeSnapshotSourceError::ContractViolation;
	try
	{
		if (auto* begin = std::get_if<TypeSnapshotTypeBegin>(&record))
		{
			if (m_TypeOpen
				|| m_FunctionOpen
				|| m_Candidate->Types.size() >= m_ExpectedTypeCount
				|| m_Candidate->Types.size() >= TypeSnapshotStore::kMaxTypeRecords
				|| !begin->Type.DirectProperties.empty()
				|| !begin->Type.DirectFunctions.empty()
				|| !begin->Type.EnumEntries.empty())
			{
				return TypeSnapshotSourceError::ContractViolation;
			}
			m_Candidate->Types.push_back(std::move(begin->Type));
			m_TypeOpen = true;
		}
		else if (auto* property = std::get_if<TypeSnapshotPropertyRecord>(&record))
		{
			if (!m_TypeOpen || m_FunctionOpen || m_Candidate->Types.back().Kind == ReflectedTypeKind::Enum
				|| m_MemberCount >= TypeSnapshotStore::kMaxTotalMembers)
			{
				return TypeSnapshotSourceError::ContractViolation;
			}
			m_Candidate->Types.back().DirectProperties.push_back(std::move(property->Property));
			++m_MemberCount;
		}
		else if (auto* function = std::get_if<TypeSnapshotFunctionBegin>(&record))
		{
			if (!m_TypeOpen || m_FunctionOpen || m_Candidate->Types.back().Kind == ReflectedTypeKind::Enum
				|| !function->Function.Parameters.empty()
				|| m_FunctionCount >= m_ExpectedFunctionCount
				|| m_MemberCount >= TypeSnapshotStore::kMaxTotalMembers)
			{
				return TypeSnapshotSourceError::ContractViolation;
			}
			m_Candidate->Types.back().DirectFunctions.push_back(std::move(function->Function));
			m_FunctionOpen = true;
			++m_FunctionCount;
			++m_MemberCount;
		}
		else if (auto* parameter = std::get_if<TypeSnapshotParameterRecord>(&record))
		{
			if (!m_TypeOpen || !m_FunctionOpen
				|| m_MemberCount >= TypeSnapshotStore::kMaxTotalMembers)
			{
				return TypeSnapshotSourceError::ContractViolation;
			}
			m_Candidate->Types.back().DirectFunctions.back().Parameters.push_back(
				std::move(parameter->Parameter));
			++m_MemberCount;
		}
		else if (std::holds_alternative<TypeSnapshotFunctionEnd>(record))
		{
			if (!m_TypeOpen || !m_FunctionOpen)
				return TypeSnapshotSourceError::ContractViolation;
			m_FunctionOpen = false;
		}
		else if (auto* entry = std::get_if<TypeSnapshotEnumEntryRecord>(&record))
		{
			if (!m_TypeOpen || m_FunctionOpen
				|| m_Candidate->Types.back().Kind != ReflectedTypeKind::Enum
				|| m_MemberCount >= TypeSnapshotStore::kMaxTotalMembers
				|| m_Candidate->Types.back().EnumEntries.size()
					>= TypeSnapshotStore::kMaxEnumEntries)
			{
				return TypeSnapshotSourceError::ContractViolation;
			}
			m_Candidate->Types.back().EnumEntries.push_back(std::move(entry->Entry));
			++m_MemberCount;
		}
		else if (std::holds_alternative<TypeSnapshotTypeEnd>(record))
		{
			if (!m_TypeOpen || m_FunctionOpen)
				return TypeSnapshotSourceError::ContractViolation;
			m_TypeOpen = false;
		}
		else
		{
			return TypeSnapshotSourceError::ContractViolation;
		}
		PublishCandidateDiagnostics();
		return TypeSnapshotSourceError::None;
	}
	catch (const std::bad_alloc&)
	{
		return TypeSnapshotSourceError::AllocationFailed;
	}
	catch (...)
	{
		return TypeSnapshotSourceError::UnexpectedException;
	}
}

bool TypeSnapshotCapture::CandidateAssemblyComplete() const noexcept
{
	return m_Candidate
		&& !m_TypeOpen
		&& !m_FunctionOpen
		&& m_Candidate->Types.size() == m_ExpectedTypeCount
		&& m_FunctionCount == m_ExpectedFunctionCount;
}

TypeSnapshotPumpResult TypeSnapshotCapture::Pump(const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > kMaxPumpBudget)
		return {.Status = TypeSnapshotPumpStatus::InvalidBudget};
	auto workLease = m_WorkBarrier.Enter();
	if (!workLease.OwnedWorkAllowed() || StopRequested())
		return {.Status = TypeSnapshotPumpStatus::Stopping};
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
		return {.Status = TypeSnapshotPumpStatus::Busy, .MoreWorkPending = true};
	struct PumpOwnerGuard final
	{
		std::atomic_flag& Owned;
		~PumpOwnerGuard() { Owned.clear(std::memory_order_release); }
	} pumpOwner{m_PumpOwned};

	std::size_t consumed = 0;
	try
	{
		TypeSnapshotCaptureState state = m_State.load(std::memory_order_acquire);
		if (state == TypeSnapshotCaptureState::Idle
			|| state == TypeSnapshotCaptureState::Completed
			|| state == TypeSnapshotCaptureState::Failed)
		{
			return {.Status = TypeSnapshotPumpStatus::Idle};
		}
		if (state == TypeSnapshotCaptureState::Ready)
			return {.Status = TypeSnapshotPumpStatus::Ready};
		if (state == TypeSnapshotCaptureState::Publishing)
			return {.Status = TypeSnapshotPumpStatus::Idle};
		if (state == TypeSnapshotCaptureState::Stopping
			|| state == TypeSnapshotCaptureState::Stopped)
		{
			return {.Status = TypeSnapshotPumpStatus::Stopping};
		}

		if (state == TypeSnapshotCaptureState::Requested && consumed < workBudget)
		{
			++consumed;
			if (!StartRequestedCapture())
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			state = TypeSnapshotCaptureState::Capturing;
		}

		while (state == TypeSnapshotCaptureState::Capturing && consumed < workBudget)
		{
			if (!m_Candidate)
			{
				Fail(
					TypeSnapshotCaptureError::SourceContractViolation,
					TypeSnapshotSourceError::ContractViolation);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (m_Source.ContextGeneration() != m_ContextGeneration)
			{
				Fail(TypeSnapshotCaptureError::SourceContextMismatch);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (!m_Source.IsCurrentExecutionThreadValid()
				|| !m_Engine.IsCurrentExecutionThreadValid())
			{
				Fail(TypeSnapshotCaptureError::ExecutionThreadInvalid);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			TypeSnapshotSourceStepResult step = m_Source.CaptureNext();
			++consumed;
			const std::size_t steps = m_SourceSteps.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (!step.Ok())
			{
				Fail(TypeSnapshotCaptureError::SourceRejected, step.Error);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (!step.Progressed || steps > kMaxSourceSteps)
			{
				Fail(
					TypeSnapshotCaptureError::SourceContractViolation,
					TypeSnapshotSourceError::ContractViolation);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (step.Record)
			{
				const TypeSnapshotSourceError assemblyError =
					ApplyRecord(std::move(*step.Record));
				if (assemblyError != TypeSnapshotSourceError::None)
				{
					const TypeSnapshotCaptureError captureError =
						assemblyError == TypeSnapshotSourceError::AllocationFailed
							? TypeSnapshotCaptureError::AllocationFailed
							: assemblyError == TypeSnapshotSourceError::UnexpectedException
								? TypeSnapshotCaptureError::UnexpectedException
								: TypeSnapshotCaptureError::SourceContractViolation;
					Fail(captureError, assemblyError);
					return {
						.Status = TypeSnapshotPumpStatus::Failed,
						.WorkConsumed = consumed
					};
				}
			}
			if (step.Complete)
			{
				if (!CandidateAssemblyComplete())
				{
					Fail(
						TypeSnapshotCaptureError::SourceContractViolation,
						TypeSnapshotSourceError::ContractViolation);
					return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
				}
				m_State.store(TypeSnapshotCaptureState::Validating, std::memory_order_release);
				state = TypeSnapshotCaptureState::Validating;
			}
			if (StopRequested())
				return {.Status = TypeSnapshotPumpStatus::Stopping, .WorkConsumed = consumed};
		}

		while (state == TypeSnapshotCaptureState::Validating && consumed < workBudget)
		{
			if (m_Source.ContextGeneration() != m_ContextGeneration)
			{
				Fail(TypeSnapshotCaptureError::SourceContextMismatch);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (!m_Source.IsCurrentExecutionThreadValid()
				|| !m_Engine.IsCurrentExecutionThreadValid())
			{
				Fail(TypeSnapshotCaptureError::ExecutionThreadInvalid);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			++consumed;
			if (!m_ValidationBegun)
			{
				const TypeSnapshotSourceError error = m_Source.BeginValidation();
				if (error != TypeSnapshotSourceError::None)
				{
					Fail(TypeSnapshotCaptureError::SourceRejected, error);
					return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
				}
				m_ValidationBegun = true;
				continue;
			}
			const TypeSnapshotSourceValidationResult validation = m_Source.ValidateNext();
			const std::size_t steps =
				m_ValidationSteps.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (!validation.Ok())
			{
				Fail(TypeSnapshotCaptureError::SourceRejected, validation.Error);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (!validation.Progressed || steps > kMaxValidationSteps)
			{
				Fail(
					TypeSnapshotCaptureError::SourceContractViolation,
					TypeSnapshotSourceError::ContractViolation);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (validation.Complete)
			{
				m_State.store(TypeSnapshotCaptureState::Sealing, std::memory_order_release);
				state = TypeSnapshotCaptureState::Sealing;
			}
			if (StopRequested())
				return {.Status = TypeSnapshotPumpStatus::Stopping, .WorkConsumed = consumed};
		}

		if (state == TypeSnapshotCaptureState::Sealing && consumed < workBudget)
		{
			if (m_Source.ContextGeneration() != m_ContextGeneration)
			{
				Fail(TypeSnapshotCaptureError::SourceContextMismatch);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			if (!m_Source.IsCurrentExecutionThreadValid()
				|| !m_Engine.IsCurrentExecutionThreadValid())
			{
				Fail(TypeSnapshotCaptureError::ExecutionThreadInvalid);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			++consumed;
			const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection =
				m_Engine.Reflection();
			if (!m_Candidate
				|| !m_Source.ValidateDependencies()
				|| m_Engine.Snapshots().Current() != m_ObjectDependency
				|| !reflection
				|| reflection != m_ReflectionDependency)
			{
				Fail(
					TypeSnapshotCaptureError::DependencyChanged,
					TypeSnapshotSourceError::DependencyChanged);
				return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
			}
			const std::uint64_t finishedAt = MonotonicMicroseconds();
			m_Candidate->CapturedAtMonotonicUs = finishedAt;
			m_Candidate->CaptureDurationUs = finishedAt >= m_StartedAtMonotonicUs
				? finishedAt - m_StartedAtMonotonicUs
				: 0;
			m_Source.Cancel();
			m_State.store(TypeSnapshotCaptureState::Ready, std::memory_order_release);
			return {
				.Status = TypeSnapshotPumpStatus::Ready,
				.WorkConsumed = consumed
			};
		}

		const bool pending = state == TypeSnapshotCaptureState::Capturing
			|| state == TypeSnapshotCaptureState::Validating
			|| state == TypeSnapshotCaptureState::Sealing;
		return {
			.Status = pending ? TypeSnapshotPumpStatus::Progress : TypeSnapshotPumpStatus::Idle,
			.WorkConsumed = consumed,
			.MoreWorkPending = pending
		};
	}
	catch (...)
	{
		Fail(
			TypeSnapshotCaptureError::UnexpectedException,
			TypeSnapshotSourceError::UnexpectedException);
		return {.Status = TypeSnapshotPumpStatus::Failed, .WorkConsumed = consumed};
	}
}

IGameThreadFrameClient::PumpResult TypeSnapshotCapture::PumpFrame(
	const std::size_t workBudget) noexcept
{
	const TypeSnapshotPumpResult result = Pump(workBudget);
	return {
		.WorkConsumed = result.WorkConsumed,
		.MoreWorkPending = result.MoreWorkPending
	};
}

TypeSnapshotPublishResult TypeSnapshotCapture::PublishReady() noexcept
{
	auto workLease = m_WorkBarrier.Enter();
	if (!workLease.OwnedWorkAllowed() || StopRequested())
		return {.Error = TypeSnapshotPublishError::StoreStopped};
	if (m_PublishOwned.test_and_set(std::memory_order_acquire))
		return {.Error = TypeSnapshotPublishError::StoreInvalid};
	struct PublishOwnerGuard final
	{
		std::atomic_flag& Owned;
		~PublishOwnerGuard() { Owned.clear(std::memory_order_release); }
	} publishOwner{m_PublishOwned};

	std::optional<TypeSnapshotCandidate> candidate;
	{
		std::lock_guard<std::mutex> lock(m_StateMutex);
		if (m_State.load(std::memory_order_acquire) != TypeSnapshotCaptureState::Ready
			|| !m_Candidate)
		{
			return {.Error = TypeSnapshotPublishError::StoreInvalid};
		}
		if (m_Engine.IsCurrentExecutionThreadValid())
		{
			m_PublishError.store(
				TypeSnapshotPublishError::WorkerThreadRequired,
				std::memory_order_release);
			return {.Error = TypeSnapshotPublishError::WorkerThreadRequired};
		}
		const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection =
			m_Engine.Reflection();
		if (m_Engine.Snapshots().Current() != m_ObjectDependency
			|| !reflection
			|| reflection != m_ReflectionDependency)
		{
			Fail(
				TypeSnapshotCaptureError::DependencyChanged,
				TypeSnapshotSourceError::DependencyChanged,
				TypeSnapshotPublishError::DependencyInvalid);
			return {.Error = TypeSnapshotPublishError::DependencyInvalid};
		}
		candidate.emplace(std::move(*m_Candidate));
		m_Candidate.reset();
		m_State.store(TypeSnapshotCaptureState::Publishing, std::memory_order_release);
	}

	TypeSnapshotPublishResult published =
		m_Engine.PublishTypeSnapshot(std::move(*candidate));
	{
		std::lock_guard<std::mutex> lock(m_StateMutex);
		m_ObjectDependency.reset();
		m_ReflectionDependency.reset();
		if (!published.Ok())
		{
			m_Error.store(TypeSnapshotCaptureError::PublicationRejected, std::memory_order_release);
			m_PublishError.store(published.Error, std::memory_order_release);
			m_State.store(TypeSnapshotCaptureState::Failed, std::memory_order_release);
		}
		else
		{
			m_Error.store(TypeSnapshotCaptureError::None, std::memory_order_release);
			m_PublishError.store(TypeSnapshotPublishError::None, std::memory_order_release);
			m_State.store(TypeSnapshotCaptureState::Completed, std::memory_order_release);
		}
	}
	return published;
}

void TypeSnapshotCapture::Fail(
	const TypeSnapshotCaptureError error,
	const TypeSnapshotSourceError sourceError,
	const TypeSnapshotPublishError publishError) noexcept
{
	m_Source.Cancel();
	const bool retired = RetireCandidate();
	m_ObjectDependency.reset();
	m_ReflectionDependency.reset();
	m_TypeOpen = false;
	m_FunctionOpen = false;
	m_Error.store(
		retired ? error : TypeSnapshotCaptureError::RetirementBackpressure,
		std::memory_order_release);
	m_SourceError.store(sourceError, std::memory_order_release);
	m_PublishError.store(publishError, std::memory_order_release);
	if (!retired)
		m_RetirementBackpressure.store(true, std::memory_order_release);
	m_State.store(TypeSnapshotCaptureState::Failed, std::memory_order_release);
}

bool TypeSnapshotCapture::RetireCandidate() noexcept
{
	if (!m_Candidate)
		return true;
	try
	{
		std::lock_guard<std::mutex> lock(m_RetirementMutex);
		for (std::optional<TypeSnapshotCandidate>& retired : m_RetiredCandidates)
		{
			if (retired)
				continue;
			retired.emplace(std::move(*m_Candidate));
			m_Candidate.reset();
			m_RetiredCandidateCount.fetch_add(1, std::memory_order_acq_rel);
			return true;
		}
	}
	catch (...)
	{
		return false;
	}
	return false;
}

std::size_t TypeSnapshotCapture::ReclaimRetired() noexcept
{
	std::array<std::optional<TypeSnapshotCandidate>, kMaxRetiredCandidates> retired;
	std::optional<TypeSnapshotCandidate> stalled;
	std::size_t reclaimed = 0;
	try
	{
		std::lock_guard<std::mutex> stateLock(m_StateMutex);
		std::lock_guard<std::mutex> retirementLock(m_RetirementMutex);
		for (std::optional<TypeSnapshotCandidate>& entry : m_RetiredCandidates)
		{
			if (!entry)
				continue;
			retired[reclaimed].emplace(std::move(*entry));
			entry.reset();
			++reclaimed;
		}
		m_RetiredCandidateCount.store(0, std::memory_order_release);
		if (m_RetirementBackpressure.load(std::memory_order_acquire)
			&& m_State.load(std::memory_order_acquire) == TypeSnapshotCaptureState::Failed
			&& m_WorkBarrier.InFlight() == 0)
		{
			if (m_Candidate)
			{
				stalled.emplace(std::move(*m_Candidate));
				m_Candidate.reset();
				++reclaimed;
			}
			m_RetirementBackpressure.store(false, std::memory_order_release);
		}
	}
	catch (...)
	{
		return reclaimed;
	}
	return reclaimed;
}

void TypeSnapshotCapture::PublishCandidateDiagnostics() noexcept
{
	if (!m_Candidate)
		return;
	m_CapturedTypes.store(m_Candidate->Types.size(), std::memory_order_release);
	m_CapturedFunctions.store(m_FunctionCount, std::memory_order_release);
	m_CapturedMembers.store(m_MemberCount, std::memory_order_release);
}

bool TypeSnapshotCapture::StopRequested() const noexcept
{
	return m_StopRequested.load(std::memory_order_acquire);
}

TypeSnapshotCaptureDiagnostics TypeSnapshotCapture::Diagnostics() const noexcept
{
	return {
		.State = m_State.load(std::memory_order_acquire),
		.Error = m_Error.load(std::memory_order_acquire),
		.SourceError = m_SourceError.load(std::memory_order_acquire),
		.PublishError = m_PublishError.load(std::memory_order_acquire),
		.RequestedGeneration = m_RequestedGeneration.load(std::memory_order_acquire),
		.ActiveGeneration = m_ActiveGeneration.load(std::memory_order_acquire),
		.ObjectSnapshotGeneration = m_ObjectSnapshotGeneration.load(std::memory_order_acquire),
		.ReflectionLayoutFingerprint =
			m_ReflectionLayoutFingerprint.load(std::memory_order_acquire),
		.CapturedTypes = m_CapturedTypes.load(std::memory_order_acquire),
		.CapturedFunctions = m_CapturedFunctions.load(std::memory_order_acquire),
		.CapturedMembers = m_CapturedMembers.load(std::memory_order_acquire),
		.SourceSteps = m_SourceSteps.load(std::memory_order_acquire),
		.ValidationSteps = m_ValidationSteps.load(std::memory_order_acquire),
		.WorkInFlight = m_WorkBarrier.InFlight(),
		.RetiredCandidates = m_RetiredCandidateCount.load(std::memory_order_acquire)
	};
}

bool TypeSnapshotCapture::StopAndDrain(const std::chrono::milliseconds timeout)
{
	{
		std::lock_guard<std::mutex> lock(m_StateMutex);
		if (m_State.load(std::memory_order_acquire) == TypeSnapshotCaptureState::Stopped)
			return true;
		m_StopRequested.store(true, std::memory_order_release);
		m_State.store(TypeSnapshotCaptureState::Stopping, std::memory_order_release);
		m_WorkBarrier.BeginStopping();
	}
	if (!m_WorkBarrier.WaitForDrain(timeout))
		return false;
	m_Source.Cancel();
	{
		std::lock_guard<std::mutex> stateLock(m_StateMutex);
		std::lock_guard<std::mutex> retirementLock(m_RetirementMutex);
		m_Candidate.reset();
		for (std::optional<TypeSnapshotCandidate>& retired : m_RetiredCandidates)
			retired.reset();
		m_ObjectDependency.reset();
		m_ReflectionDependency.reset();
		m_RetiredCandidateCount.store(0, std::memory_order_release);
		m_RetirementBackpressure.store(false, std::memory_order_release);
		m_State.store(TypeSnapshotCaptureState::Stopped, std::memory_order_release);
	}
	return true;
}

} // namespace UExplorer::Runtime
