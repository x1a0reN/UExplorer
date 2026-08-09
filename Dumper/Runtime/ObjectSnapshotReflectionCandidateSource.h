#pragma once

#include "EngineContext.h"
#include "ReflectionLayoutCapture.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace UExplorer::Runtime
{

class EngineFacade;

enum class ReflectionCandidatePreparationError : std::uint8_t
{
	None,
	Busy,
	InvalidConfiguration,
	SnapshotUnavailable,
	SnapshotInvalid,
	RequiredObjectMissing,
	RequiredObjectAmbiguous,
	PropertySystemAmbiguous,
	PropertySystemMismatch,
	AllocationFailed
};

const char* ToString(ReflectionCandidatePreparationError error) noexcept;

struct ReflectionCandidatePreparationResult
{
	ReflectionCandidatePreparationError Error = ReflectionCandidatePreparationError::None;
	std::uint64_t SnapshotGeneration = 0;
	ReflectionPropertySystem PropertySystem = ReflectionPropertySystem::Unavailable;
	std::string EvidencePath;

	bool Ok() const noexcept
	{
		return Error == ReflectionCandidatePreparationError::None;
	}
};

struct ObjectSnapshotReflectionSourceDiagnostics
{
	ReflectionCandidatePreparationError PreparationError =
		ReflectionCandidatePreparationError::None;
	ReflectionCandidateSourceError SourceError = ReflectionCandidateSourceError::None;
	std::uint64_t PreparedSnapshotGeneration = 0;
	ReflectionPropertySystem PropertySystem = ReflectionPropertySystem::Unavailable;
	std::size_t DiscoveryPhase = 0;
	std::size_t SourceSteps = 0;
	std::size_t EmittedFields = 0;
	bool Active = false;
};

// Builds a complete reflection candidate from one immutable object generation and
// checked live witnesses. It never reads Off/Settings or legacy Unreal wrappers.
class ObjectSnapshotReflectionCandidateSource final : public IReflectionCandidateSource
{
public:
	ObjectSnapshotReflectionCandidateSource(
		std::shared_ptr<const EngineContext> context,
		EngineFacade& engine);
	~ObjectSnapshotReflectionCandidateSource() override;
	ObjectSnapshotReflectionCandidateSource(
		const ObjectSnapshotReflectionCandidateSource&) = delete;
	ObjectSnapshotReflectionCandidateSource& operator=(
		const ObjectSnapshotReflectionCandidateSource&) = delete;

	ReflectionCandidatePreparationResult Prepare() noexcept;
	bool ReleasePreparedPlan() noexcept;
	ObjectSnapshotReflectionSourceDiagnostics Diagnostics() const noexcept;

	std::uint64_t ContextGeneration() const noexcept override;
	bool IsConfigured() const noexcept override;
	bool IsCurrentExecutionThreadValid() const noexcept override;
	ReflectionCandidateSourceBeginResult Begin() noexcept override;
	ReflectionCandidateSourceStepResult CaptureNext() noexcept override;
	bool ValidateDependencies() noexcept override;
	void Cancel() noexcept override;

private:
	class Impl;
	std::unique_ptr<Impl> m_Impl;
};

} // namespace UExplorer::Runtime
