#pragma once

#include "CallbackBarrier.h"
#include "GameThreadExecutor.h"
#include "ReflectionLayout.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace UExplorer::Runtime
{

class EngineFacade;

enum class ReflectionCandidateSourceError : std::uint8_t
{
	None,
	InvalidConfiguration,
	SnapshotUnavailable,
	DependencyChanged,
	EvidenceUnavailable,
	EvidenceAmbiguous,
	MemoryUnavailable,
	ContractViolation,
	UnexpectedException
};

const char* ToString(ReflectionCandidateSourceError error) noexcept;

struct ReflectionCandidateSourceStepResult
{
	ReflectionCandidateSourceError Error = ReflectionCandidateSourceError::None;
	bool Complete = false;

	bool Ok() const noexcept
	{
		return Error == ReflectionCandidateSourceError::None;
	}
};

class IReflectionCandidateSource
{
public:
	virtual ~IReflectionCandidateSource() = default;
	virtual std::uint64_t ContextGeneration() const noexcept = 0;
	virtual bool IsConfigured() const noexcept = 0;
	virtual bool IsCurrentExecutionThreadValid() const noexcept = 0;
	virtual ReflectionCandidateSourceStepResult Begin(
		ReflectionLayoutCandidate& candidate) noexcept = 0;
	virtual ReflectionCandidateSourceStepResult CaptureNext(
		ReflectionLayoutCandidate& candidate) noexcept = 0;
	virtual bool ValidateDependencies() const noexcept = 0;
	virtual void Cancel() noexcept = 0;
};

enum class ReflectionLayoutCaptureState : std::uint8_t
{
	Idle,
	Requested,
	Capturing,
	Validating,
	Publishing,
	Completed,
	Failed,
	Stopping,
	Stopped
};

const char* ToString(ReflectionLayoutCaptureState state) noexcept;

enum class ReflectionLayoutCaptureError : std::uint8_t
{
	None,
	Busy,
	Stopped,
	AlreadyConfigured,
	InvalidConfiguration,
	InvalidPumpBudget,
	ExecutionThreadInvalid,
	SourceContextMismatch,
	SourceRejected,
	SourceContractViolation,
	DependencyChanged,
	CandidateRejected,
	PublicationRejected,
	UnexpectedException
};

const char* ToString(ReflectionLayoutCaptureError error) noexcept;

enum class ReflectionLayoutPumpStatus : std::uint8_t
{
	Idle,
	Progress,
	Published,
	Failed,
	Stopping,
	Busy,
	InvalidBudget
};

struct ReflectionLayoutPumpResult
{
	ReflectionLayoutPumpStatus Status = ReflectionLayoutPumpStatus::Idle;
	std::size_t WorkConsumed = 0;
	bool MoreWorkPending = false;
};

struct ReflectionLayoutCaptureDiagnostics
{
	ReflectionLayoutCaptureState State = ReflectionLayoutCaptureState::Idle;
	ReflectionLayoutCaptureError Error = ReflectionLayoutCaptureError::None;
	ReflectionCandidateSourceError SourceError = ReflectionCandidateSourceError::None;
	ReflectionValidationError ValidationError = ReflectionValidationError::None;
	std::size_t CapturedFields = 0;
	std::size_t CapturedWitnesses = 0;
	std::size_t SourceSteps = 0;
	std::uint32_t PumpInFlight = 0;
};

class ReflectionLayoutCapture final : public IGameThreadFrameClient
{
public:
	static constexpr std::size_t kMaxPumpBudget = 64;
	static constexpr std::size_t kMaxWitnessesPerSourceStep = 8;

	ReflectionLayoutCapture(
		std::uint64_t contextGeneration,
		IReflectionCandidateSource& source,
		EngineFacade& engine);
	ReflectionLayoutCapture(const ReflectionLayoutCapture&) = delete;
	ReflectionLayoutCapture& operator=(const ReflectionLayoutCapture&) = delete;

	bool IsConfigured() const noexcept;
	ReflectionLayoutCaptureError RequestCapture() noexcept;
	ReflectionLayoutPumpResult Pump(std::size_t workBudget) noexcept;
	IGameThreadFrameClient::PumpResult PumpFrame(std::size_t workBudget) noexcept override;
	ReflectionLayoutCaptureDiagnostics Diagnostics() const noexcept;
	bool StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	bool StartRequestedCapture() noexcept;
	void Fail(
		ReflectionLayoutCaptureError error,
		ReflectionCandidateSourceError sourceError = ReflectionCandidateSourceError::None,
		ReflectionValidationError validationError = ReflectionValidationError::None) noexcept;
	bool StopRequested() const noexcept;
	void PublishCandidateDiagnostics() noexcept;

	std::uint64_t m_ContextGeneration = 0;
	IReflectionCandidateSource& m_Source;
	EngineFacade& m_Engine;
	CallbackBarrier m_PumpBarrier;
	mutable std::mutex m_RequestMutex;
	std::optional<ReflectionLayoutCandidate> m_Candidate;
	std::shared_ptr<const ReflectionLayout> m_ValidatedLayout;
	std::atomic<bool> m_StopRequested{false};
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic<ReflectionLayoutCaptureState> m_State{ReflectionLayoutCaptureState::Idle};
	std::atomic<ReflectionLayoutCaptureError> m_Error{ReflectionLayoutCaptureError::None};
	std::atomic<ReflectionCandidateSourceError> m_SourceError{
		ReflectionCandidateSourceError::None};
	std::atomic<ReflectionValidationError> m_ValidationError{
		ReflectionValidationError::None};
	std::atomic<std::size_t> m_CapturedFields{0};
	std::atomic<std::size_t> m_CapturedWitnesses{0};
	std::atomic<std::size_t> m_SourceSteps{0};
};

} // namespace UExplorer::Runtime
