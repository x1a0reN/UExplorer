#include "BlueprintBytecodeRuntime.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

bool SameBinding(
	const BlueprintEvidenceBinding& left,
	const BlueprintEvidenceBinding& right) noexcept
{
	return SameBlueprintEvidenceBinding(left, right);
}

BlueprintEvidenceBinding BindingFor(
	const EngineSnapshot& objects,
	const TypeSnapshot& types)
{
	return {
		.SessionId = objects.SessionId,
		.ContextGeneration = objects.ContextGeneration,
		.ObjectSnapshotGeneration = objects.Generation,
		.TypeSnapshotGeneration = types.Generation()
	};
}

} // namespace

struct BlueprintBytecodeRuntimeSource::GenerationState
{
	GenerationState(
		EngineFacade& engine,
		GameThreadExecutor& gameThread,
		BlueprintEvidenceBinding binding,
		BlueprintScriptArrayLayoutWitness layout)
		: Binding(std::move(binding)),
		  Evidence(Binding),
		  Source(engine, gameThread, Evidence, std::move(layout))
	{
	}

	BlueprintEvidenceBinding Binding;
	BlueprintBytecodeEvidenceStore Evidence;
	BlueprintBytecodeCaptureSource Source;
};

BlueprintBytecodeRuntimeSource::BlueprintBytecodeRuntimeSource(
	std::shared_ptr<const EngineContext> context,
	EngineFacade& engine,
	GameThreadExecutor& gameThread)
	: m_Context(std::move(context)),
	  m_Engine(engine),
	  m_GameThread(gameThread)
{
}

BlueprintBytecodeRuntimeSource::~BlueprintBytecodeRuntimeSource()
{
	(void)StopAndDrain(std::chrono::milliseconds(5000));
}

bool BlueprintBytecodeRuntimeSource::IsScriptLayoutReportValid() const noexcept
{
	const OffsetReport* report = m_Context
		? m_Context->FindOffset("ufunction.script")
		: nullptr;
	return report
		&& report->IsValidated()
		&& report->Value > 0
		&& report->Value <= static_cast<std::int64_t>(
			BlueprintBytecodeEvidenceStore::kMaxScriptFieldOffset)
		&& report->Source == "scored_runtime_blueprint_script_validation_v2"
		&& report->Confidence == "high";
}

bool BlueprintBytecodeRuntimeSource::IsConfigured() const noexcept
{
	return sizeof(std::uintptr_t) == 8
		&& m_Context
		&& m_Context->Generation() != 0
		&& m_Engine.IsConfigured()
		&& m_Engine.Context() == m_Context
		&& m_Engine.ContextGeneration() == m_Context->Generation()
		&& IsScriptLayoutReportValid()
		&& !m_Barrier.IsStopping();
}

bool BlueprintBytecodeRuntimeSource::IsCaptureConfigured() const noexcept
{
	if (!IsConfigured() || !m_GameThread.IsEnabled())
		return false;
	const std::shared_ptr<const EngineSnapshot> objects =
		m_Engine.Snapshots().Current();
	const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
	return objects
		&& types
		&& objects->SessionId == m_Engine.SessionId()
		&& objects->ContextGeneration == m_Engine.ContextGeneration()
		&& types->SessionId() == objects->SessionId
		&& types->ContextGeneration() == objects->ContextGeneration
		&& types->ObjectSnapshotGeneration() == objects->Generation;
}

std::size_t BlueprintBytecodeRuntimeSource::RetainedGenerationCount() const noexcept
{
	std::lock_guard lock(m_Mutex);
	return m_States.size();
}

bool BlueprintBytecodeRuntimeSource::BuildLayoutWitness(
	const BlueprintEvidenceBinding& binding,
	BlueprintScriptArrayLayoutWitness& witness) const noexcept
{
	witness = {};
	if (!IsScriptLayoutReportValid())
		return false;
	const OffsetReport* report = m_Context->FindOffset("ufunction.script");
	if (!report
		|| report->Value > (std::numeric_limits<std::uint32_t>::max)())
	{
		return false;
	}
	witness.Binding = binding;
	witness.Source =
		"engine_context.scored_runtime_blueprint_script_validation_v2+windows_x64_tarray_u8";
	witness.Validated = true;
	witness.ScriptFieldOffset = static_cast<std::uint32_t>(report->Value);
	witness.HeaderByteWidth = 16;
	witness.DataPointerOffset = 0;
	witness.DataPointerWidth = 8;
	witness.NumOffset = 8;
	witness.NumWidth = 4;
	witness.MaxOffset = 12;
	witness.MaxWidth = 4;
	witness.ElementWidth = 1;
	witness.CountsSigned = true;
	witness.ByteOrder = BlueprintScriptArrayByteOrder::LittleEndian;
	witness.EvidenceFingerprint =
		ComputeBlueprintScriptArrayLayoutWitnessFingerprint(witness);
	return IsBlueprintScriptArrayLayoutWitnessValid(witness);
}

void BlueprintBytecodeRuntimeSource::PruneRetiredStatesLocked(
	const BlueprintEvidenceBinding& currentBinding) noexcept
{
	for (auto iterator = m_States.begin(); iterator != m_States.end();)
	{
		const std::shared_ptr<GenerationState>& state = *iterator;
		if (!state
			|| (!SameBinding(state->Binding, currentBinding)
				&& state->Source.InFlight() == 0
				&& state->Source.StopAndDrain(std::chrono::milliseconds(0))))
		{
			if (state)
				state->Evidence.Stop();
			iterator = m_States.erase(iterator);
			continue;
		}
		++iterator;
	}
}

std::shared_ptr<BlueprintBytecodeRuntimeSource::GenerationState>
BlueprintBytecodeRuntimeSource::AcquireGenerationState(
	const BlueprintEvidenceBinding& binding,
	BlueprintEvidenceSourceError& error) noexcept
{
	error = BlueprintEvidenceSourceError::Unavailable;
	try
	{
		if (!IsCaptureConfigured())
			return nullptr;
		const std::shared_ptr<const EngineSnapshot> objects =
			m_Engine.Snapshots().Current();
		const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
		if (!objects || !types)
			return nullptr;
		const BlueprintEvidenceBinding current = BindingFor(*objects, *types);
		if (!SameBinding(binding, current))
		{
			error = BlueprintEvidenceSourceError::DependencyChanged;
			return nullptr;
		}

		std::lock_guard lock(m_Mutex);
		if (m_Barrier.IsStopping() || m_Drained)
		{
			error = BlueprintEvidenceSourceError::Stopped;
			return nullptr;
		}
		PruneRetiredStatesLocked(current);
		const auto existing = std::find_if(
			m_States.begin(),
			m_States.end(),
			[&current](const std::shared_ptr<GenerationState>& state) {
				return state && SameBinding(state->Binding, current);
			});
		if (existing != m_States.end())
		{
			error = BlueprintEvidenceSourceError::None;
			return *existing;
		}
		if (m_States.size() >= kMaxRetainedGenerationStates)
		{
			error = BlueprintEvidenceSourceError::LimitExceeded;
			return nullptr;
		}

		BlueprintScriptArrayLayoutWitness layout;
		if (!BuildLayoutWitness(current, layout))
		{
			error = BlueprintEvidenceSourceError::InvalidEvidence;
			return nullptr;
		}
		auto state = std::make_shared<GenerationState>(
			m_Engine,
			m_GameThread,
			current,
			std::move(layout));
		if (!state->Evidence.IsConfigured() || !state->Source.IsConfigured())
		{
			error = BlueprintEvidenceSourceError::InvalidEvidence;
			return nullptr;
		}
		m_States.push_back(state);
		error = BlueprintEvidenceSourceError::None;
		return state;
	}
	catch (const std::bad_alloc&)
	{
		error = BlueprintEvidenceSourceError::AllocationFailed;
		return nullptr;
	}
	catch (...)
	{
		error = BlueprintEvidenceSourceError::InternalError;
		return nullptr;
	}
}

BlueprintBytecodeCaptureResult BlueprintBytecodeRuntimeSource::Capture(
	const BlueprintBytecodeCaptureRequest& request)
{
	auto lease = m_Barrier.Enter();
	if (!lease.OwnedWorkAllowed())
		return {.Error = BlueprintEvidenceSourceError::Stopped};
	BlueprintEvidenceSourceError error = BlueprintEvidenceSourceError::Unavailable;
	std::shared_ptr<GenerationState> state =
		AcquireGenerationState(request.Binding, error);
	if (!state)
		return {.Error = error};
	return state->Source.Capture(request);
}

bool BlueprintBytecodeRuntimeSource::StopAndDrain(
	const std::chrono::milliseconds timeout)
{
	if (timeout.count() < 0)
		return false;
	m_Barrier.BeginStopping();
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	if (!m_Barrier.WaitForDrain(timeout))
		return false;

	std::vector<std::shared_ptr<GenerationState>> states;
	try
	{
		std::lock_guard lock(m_Mutex);
		if (m_Drained)
			return true;
		states = m_States;
	}
	catch (...)
	{
		return false;
	}

	for (const std::shared_ptr<GenerationState>& state : states)
	{
		if (!state)
			continue;
		const auto now = std::chrono::steady_clock::now();
		const auto remaining = now < deadline
			? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
			: std::chrono::milliseconds(0);
		if (!state->Source.StopAndDrain(remaining))
			return false;
	}
	for (const std::shared_ptr<GenerationState>& state : states)
	{
		if (state)
			state->Evidence.Stop();
	}
	{
		std::lock_guard lock(m_Mutex);
		m_Drained = true;
	}
	return true;
}

} // namespace UExplorer::Runtime
