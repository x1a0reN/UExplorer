#pragma once

#include "CallbackBarrier.h"
#include "GameThreadExecutor.h"
#include "TypeSnapshot.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace UExplorer::Runtime
{

class EngineFacade;

enum class TypeSnapshotSourceError : std::uint8_t
{
	None,
	InvalidConfiguration,
	DependencyChanged,
	MemoryUnavailable,
	EvidenceUnavailable,
	EvidenceAmbiguous,
	ContractViolation,
	AllocationFailed,
	UnexpectedException
};

const char* ToString(TypeSnapshotSourceError error) noexcept;

struct TypeSnapshotSourceBeginResult
{
	TypeSnapshotSourceError Error = TypeSnapshotSourceError::None;
	std::string Source;
	std::shared_ptr<const EngineSnapshot> ObjectSnapshot;
	std::shared_ptr<const ReflectionRuntimeSnapshot> Reflection;
	std::size_t ExpectedTypeCount = 0;
	std::size_t ExpectedFunctionCount = 0;

	bool Ok() const noexcept { return Error == TypeSnapshotSourceError::None; }
};

struct TypeSnapshotTypeBegin
{
	ReflectedType Type;
};

struct TypeSnapshotPropertyRecord
{
	ReflectedProperty Property;
};

struct TypeSnapshotFunctionBegin
{
	ReflectedFunction Function;
};

struct TypeSnapshotParameterRecord
{
	ReflectedParameter Parameter;
};

struct TypeSnapshotFunctionEnd final {};
struct TypeSnapshotEnumEntryRecord
{
	ReflectedEnumEntry Entry;
};
struct TypeSnapshotTypeEnd final {};

using TypeSnapshotSourceRecord = std::variant<
	TypeSnapshotTypeBegin,
	TypeSnapshotPropertyRecord,
	TypeSnapshotFunctionBegin,
	TypeSnapshotParameterRecord,
	TypeSnapshotFunctionEnd,
	TypeSnapshotEnumEntryRecord,
	TypeSnapshotTypeEnd>;

struct TypeSnapshotSourceStepResult
{
	TypeSnapshotSourceError Error = TypeSnapshotSourceError::None;
	bool Progressed = false;
	bool Complete = false;
	std::optional<TypeSnapshotSourceRecord> Record;

	bool Ok() const noexcept { return Error == TypeSnapshotSourceError::None; }
};

struct TypeSnapshotSourceValidationResult
{
	TypeSnapshotSourceError Error = TypeSnapshotSourceError::None;
	bool Progressed = false;
	bool Complete = false;

	bool Ok() const noexcept { return Error == TypeSnapshotSourceError::None; }
};

class ITypeSnapshotSource
{
public:
	virtual ~ITypeSnapshotSource() = default;
	virtual std::uint64_t ContextGeneration() const noexcept = 0;
	virtual bool IsConfigured() const noexcept = 0;
	virtual bool IsCurrentExecutionThreadValid() const noexcept = 0;
	virtual TypeSnapshotSourceBeginResult Begin() noexcept = 0;
	virtual TypeSnapshotSourceStepResult CaptureNext() noexcept = 0;
	virtual TypeSnapshotSourceError BeginValidation() noexcept = 0;
	virtual TypeSnapshotSourceValidationResult ValidateNext() noexcept = 0;
	virtual bool ValidateDependencies() noexcept = 0;
	virtual void Cancel() noexcept = 0;
};

enum class TypeSnapshotCaptureState : std::uint8_t
{
	Idle,
	Requested,
	Capturing,
	Validating,
	Sealing,
	Ready,
	Publishing,
	Completed,
	Failed,
	Stopping,
	Stopped
};

const char* ToString(TypeSnapshotCaptureState state) noexcept;

enum class TypeSnapshotCaptureError : std::uint8_t
{
	None,
	Busy,
	Stopped,
	InvalidConfiguration,
	GenerationExhausted,
	InvalidPumpBudget,
	ExecutionThreadInvalid,
	SourceContextMismatch,
	SourceRejected,
	SourceContractViolation,
	DependencyChanged,
	PublicationRejected,
	RetirementBackpressure,
	AllocationFailed,
	UnexpectedException
};

const char* ToString(TypeSnapshotCaptureError error) noexcept;

enum class TypeSnapshotPumpStatus : std::uint8_t
{
	Idle,
	Progress,
	Ready,
	Failed,
	Stopping,
	Busy,
	InvalidBudget
};

struct TypeSnapshotPumpResult
{
	TypeSnapshotPumpStatus Status = TypeSnapshotPumpStatus::Idle;
	std::size_t WorkConsumed = 0;
	bool MoreWorkPending = false;
};

struct TypeSnapshotCaptureDiagnostics
{
	TypeSnapshotCaptureState State = TypeSnapshotCaptureState::Idle;
	TypeSnapshotCaptureError Error = TypeSnapshotCaptureError::None;
	TypeSnapshotSourceError SourceError = TypeSnapshotSourceError::None;
	TypeSnapshotPublishError PublishError = TypeSnapshotPublishError::None;
	std::uint64_t RequestedGeneration = 0;
	std::uint64_t ActiveGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t ReflectionLayoutFingerprint = 0;
	std::size_t CapturedTypes = 0;
	std::size_t CapturedFunctions = 0;
	std::size_t CapturedMembers = 0;
	std::size_t SourceSteps = 0;
	std::size_t ValidationSteps = 0;
	std::uint32_t WorkInFlight = 0;
	std::size_t RetiredCandidates = 0;
};

class TypeSnapshotCapture final : public IGameThreadFrameClient
{
public:
	static constexpr std::size_t kMaxPumpBudget = 64;
	static constexpr std::size_t kMaxSourceSteps =
		TypeSnapshotStore::kMaxTotalMembers * 4;
	static constexpr std::size_t kMaxValidationSteps =
		TypeSnapshotStore::kMaxTotalMembers * 2;
	static constexpr std::size_t kMaxRetiredCandidates = 4;

	TypeSnapshotCapture(
		std::uint64_t contextGeneration,
		ITypeSnapshotSource& source,
		EngineFacade& engine);
	TypeSnapshotCapture(const TypeSnapshotCapture&) = delete;
	TypeSnapshotCapture& operator=(const TypeSnapshotCapture&) = delete;

	bool IsConfigured() const noexcept;
	TypeSnapshotCaptureError RequestCapture() noexcept;
	TypeSnapshotPumpResult Pump(std::size_t workBudget) noexcept;
	IGameThreadFrameClient::PumpResult PumpFrame(std::size_t workBudget) noexcept override;
	TypeSnapshotPublishResult PublishReady() noexcept;
	TypeSnapshotCaptureDiagnostics Diagnostics() const noexcept;
	std::size_t ReclaimRetired() noexcept;
	bool StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	bool StartRequestedCapture() noexcept;
	TypeSnapshotSourceError ApplyRecord(TypeSnapshotSourceRecord record) noexcept;
	bool CandidateAssemblyComplete() const noexcept;
	void Fail(
		TypeSnapshotCaptureError error,
		TypeSnapshotSourceError sourceError = TypeSnapshotSourceError::None,
		TypeSnapshotPublishError publishError = TypeSnapshotPublishError::None) noexcept;
	bool RetireCandidate() noexcept;
	void PublishCandidateDiagnostics() noexcept;
	bool StopRequested() const noexcept;

	std::uint64_t m_ContextGeneration = 0;
	ITypeSnapshotSource& m_Source;
	EngineFacade& m_Engine;
	CallbackBarrier m_WorkBarrier;
	mutable std::mutex m_StateMutex;
	mutable std::mutex m_RetirementMutex;
	std::optional<TypeSnapshotCandidate> m_Candidate;
	std::array<std::optional<TypeSnapshotCandidate>, kMaxRetiredCandidates>
		m_RetiredCandidates;
	std::shared_ptr<const EngineSnapshot> m_ObjectDependency;
	std::shared_ptr<const ReflectionRuntimeSnapshot> m_ReflectionDependency;
	std::size_t m_ExpectedTypeCount = 0;
	std::size_t m_ExpectedFunctionCount = 0;
	std::size_t m_FunctionCount = 0;
	std::size_t m_MemberCount = 0;
	bool m_TypeOpen = false;
	bool m_FunctionOpen = false;
	bool m_ValidationBegun = false;
	std::uint64_t m_StartedAtMonotonicUs = 0;
	std::atomic<bool> m_StopRequested{false};
	std::atomic<bool> m_RetirementBackpressure{false};
	std::atomic<std::size_t> m_RetiredCandidateCount{0};
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic_flag m_PublishOwned = ATOMIC_FLAG_INIT;
	std::atomic<TypeSnapshotCaptureState> m_State{TypeSnapshotCaptureState::Idle};
	std::atomic<TypeSnapshotCaptureError> m_Error{TypeSnapshotCaptureError::None};
	std::atomic<TypeSnapshotSourceError> m_SourceError{TypeSnapshotSourceError::None};
	std::atomic<TypeSnapshotPublishError> m_PublishError{TypeSnapshotPublishError::None};
	std::atomic<std::uint64_t> m_RequestedGeneration{0};
	std::atomic<std::uint64_t> m_ActiveGeneration{0};
	std::atomic<std::uint64_t> m_ObjectSnapshotGeneration{0};
	std::atomic<std::uint64_t> m_ReflectionLayoutFingerprint{0};
	std::atomic<std::size_t> m_CapturedTypes{0};
	std::atomic<std::size_t> m_CapturedFunctions{0};
	std::atomic<std::size_t> m_CapturedMembers{0};
	std::atomic<std::size_t> m_SourceSteps{0};
	std::atomic<std::size_t> m_ValidationSteps{0};
};

} // namespace UExplorer::Runtime
