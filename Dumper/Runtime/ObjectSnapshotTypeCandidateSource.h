#pragma once

#include "EngineContext.h"
#include "TypeSnapshotCapture.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace UExplorer::Runtime
{

class EngineFacade;

enum class TypeCandidatePreparationError : std::uint8_t
{
	None,
	Busy,
	InvalidConfiguration,
	SnapshotUnavailable,
	SnapshotInvalid,
	ReflectionUnavailable,
	ReflectionInvalid,
	TypeCoverageExceeded,
	FunctionOwnerMissing,
	FunctionOwnerAmbiguous,
	AllocationFailed
};

const char* ToString(TypeCandidatePreparationError error) noexcept;

struct TypeCandidatePreparationResult
{
	TypeCandidatePreparationError Error = TypeCandidatePreparationError::None;
	std::uint64_t SnapshotGeneration = 0;
	std::uint64_t ReflectionLayoutFingerprint = 0;
	std::size_t TypeCount = 0;
	std::size_t FunctionCount = 0;

	bool Ok() const noexcept
	{
		return Error == TypeCandidatePreparationError::None;
	}
};

struct ObjectSnapshotTypeSourceDiagnostics
{
	TypeCandidatePreparationError PreparationError =
		TypeCandidatePreparationError::None;
	TypeSnapshotSourceError SourceError = TypeSnapshotSourceError::None;
	std::uint64_t PreparedSnapshotGeneration = 0;
	std::uint64_t ReflectionLayoutFingerprint = 0;
	std::size_t PreparedTypes = 0;
	std::size_t PreparedFunctions = 0;
	std::size_t CapturePhase = 0;
	std::size_t SourceSteps = 0;
	std::size_t ValidationSteps = 0;
	std::size_t CapturedEvidence = 0;
	bool Active = false;
};

// Captures structural type metadata from one exact object/reflection generation.
// It never consults Off, Settings, or legacy Unreal wrapper objects.
class ObjectSnapshotTypeCandidateSource final : public ITypeSnapshotSource
{
public:
	ObjectSnapshotTypeCandidateSource(
		std::shared_ptr<const EngineContext> context,
		EngineFacade& engine);
	~ObjectSnapshotTypeCandidateSource() override;
	ObjectSnapshotTypeCandidateSource(const ObjectSnapshotTypeCandidateSource&) = delete;
	ObjectSnapshotTypeCandidateSource& operator=(
		const ObjectSnapshotTypeCandidateSource&) = delete;

	TypeCandidatePreparationResult Prepare() noexcept;
	bool ReleasePreparedPlan() noexcept;
	ObjectSnapshotTypeSourceDiagnostics Diagnostics() const noexcept;

	std::uint64_t ContextGeneration() const noexcept override;
	bool IsConfigured() const noexcept override;
	bool IsCurrentExecutionThreadValid() const noexcept override;
	TypeSnapshotSourceBeginResult Begin() noexcept override;
	TypeSnapshotSourceStepResult CaptureNext() noexcept override;
	TypeSnapshotSourceError BeginValidation() noexcept override;
	TypeSnapshotSourceValidationResult ValidateNext() noexcept override;
	bool ValidateDependencies() noexcept override;
	void Cancel() noexcept override;

private:
	class Impl;
	std::unique_ptr<Impl> m_Impl;
};

} // namespace UExplorer::Runtime
