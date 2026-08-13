#pragma once

#include "BlueprintBytecodeCapture.h"
#include "CallbackBarrier.h"
#include "EngineContext.h"
#include "EngineFacade.h"
#include "GameThreadExecutor.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace UExplorer::Runtime
{

// Production generation router for bounded Blueprint Script capture. It keeps
// old generation sources alive until any timed-out game-thread work releases
// its source barrier; no request is rebound to a newer snapshot generation.
class BlueprintBytecodeRuntimeSource final
	: public IBlueprintBytecodeCaptureSource
{
public:
	static constexpr std::size_t kMaxRetainedGenerationStates = 8;

	BlueprintBytecodeRuntimeSource(
		std::shared_ptr<const EngineContext> context,
		EngineFacade& engine,
		GameThreadExecutor& gameThread);
	~BlueprintBytecodeRuntimeSource();

	BlueprintBytecodeRuntimeSource(const BlueprintBytecodeRuntimeSource&) = delete;
	BlueprintBytecodeRuntimeSource& operator=(
		const BlueprintBytecodeRuntimeSource&) = delete;

	bool IsConfigured() const noexcept;
	bool IsCaptureConfigured() const noexcept;
	std::size_t RetainedGenerationCount() const noexcept;

	BlueprintBytecodeCaptureResult Capture(
		const BlueprintBytecodeCaptureRequest& request) override;
	bool StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	struct GenerationState;

	bool IsScriptLayoutReportValid() const noexcept;
	bool BuildLayoutWitness(
		const BlueprintEvidenceBinding& binding,
		BlueprintScriptArrayLayoutWitness& witness) const noexcept;
	std::shared_ptr<GenerationState> AcquireGenerationState(
		const BlueprintEvidenceBinding& binding,
		BlueprintEvidenceSourceError& error) noexcept;
	void PruneRetiredStatesLocked(
		const BlueprintEvidenceBinding& currentBinding) noexcept;

	std::shared_ptr<const EngineContext> m_Context;
	EngineFacade& m_Engine;
	GameThreadExecutor& m_GameThread;
	mutable std::mutex m_Mutex;
	std::vector<std::shared_ptr<GenerationState>> m_States;
	CallbackBarrier m_Barrier;
	bool m_Drained = false;
};

} // namespace UExplorer::Runtime
