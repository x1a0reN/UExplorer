#pragma once

#include "CallbackBarrier.h"
#include "EngineContext.h"
#include "EngineSnapshot.h"
#include "GameThreadExecutor.h"
#include "ObjectHandle.h"
#include "TypeSnapshot.h"
#include "WorldSnapshot.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace UExplorer::Runtime
{

enum class WorldSnapshotCaptureState : std::uint8_t
{
	Idle,
	Requested,
	Scanning,
	Sealing,
	Validating,
	Publishing,
	Completed,
	Failed,
	Stopping,
	Stopped
};

const char* ToString(WorldSnapshotCaptureState state) noexcept;

enum class WorldSnapshotCaptureError : std::uint8_t
{
	None,
	Busy,
	Stopped,
	InvalidConfiguration,
	DependenciesUnavailable,
	DependencyChanged,
	TypeMetadataUnavailable,
	ExecutionThreadInvalid,
	WorldPointerUnavailable,
	WorldIdentityInvalid,
	SourceReadFailed,
	SourceValidationFailed,
	CountLimitExceeded,
	PublicationRejected,
	GenerationExhausted,
	AllocationFailed,
	UnexpectedException
};

const char* ToString(WorldSnapshotCaptureError error) noexcept;

struct WorldSnapshotCaptureRequestResult
{
	WorldSnapshotCaptureError Error = WorldSnapshotCaptureError::None;
	std::uint64_t Generation = 0;

	bool Ok() const noexcept { return Error == WorldSnapshotCaptureError::None; }
};

struct WorldSnapshotCaptureDiagnostics
{
	WorldSnapshotCaptureState State = WorldSnapshotCaptureState::Idle;
	WorldSnapshotCaptureError Error = WorldSnapshotCaptureError::None;
	WorldSnapshotPublishError PublishError = WorldSnapshotPublishError::None;
	std::uint64_t RequestedGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::size_t CandidateCount = 0;
	std::size_t NextCandidate = 0;
	std::size_t CapturedLevels = 0;
	std::size_t CapturedActors = 0;
	std::size_t ValidationIndex = 0;
	std::int32_t ErrorObjectIndex = -1;
};

class WorldSnapshotCapture final : public IGameThreadFrameClient
{
public:
	static constexpr std::string_view kActorClassPath = "/Script/Engine.Actor";
	static constexpr std::string_view kLevelClassPath = "/Script/Engine.Level";
	static constexpr std::string_view kWorldClassPath = "/Script/Engine.World";
	static constexpr std::size_t kMaxOuterDepth = 32;

	WorldSnapshotCapture(
		std::shared_ptr<const EngineContext> context,
		std::string sessionId,
		IHandleIdentitySource& identities,
		const EngineSnapshotStore& objects,
		const TypeSnapshotStore& types,
		WorldSnapshotStore& worlds);
	WorldSnapshotCapture(const WorldSnapshotCapture&) = delete;
	WorldSnapshotCapture& operator=(const WorldSnapshotCapture&) = delete;

	bool IsConfigured() const noexcept;
	WorldSnapshotCaptureRequestResult RequestCapture() noexcept;
	bool SealForValidation() noexcept;
	bool PublishReady() noexcept;
	IGameThreadFrameClient::PumpResult PumpFrame(std::size_t workBudget) noexcept override;
	WorldSnapshotCaptureDiagnostics Diagnostics() const noexcept;
	bool StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	enum class CandidateKind : std::uint8_t
	{
		Level,
		Actor
	};

	struct Candidate
	{
		const EngineSnapshotObject* Object = nullptr;
		CandidateKind Kind = CandidateKind::Actor;
	};

	struct WorkingCapture
	{
		std::uint64_t Generation = 0;
		std::uint64_t StartedAtMonotonicUs = 0;
		std::shared_ptr<const EngineSnapshot> Objects;
		std::shared_ptr<const TypeSnapshot> Types;
		std::set<std::string, std::less<>> ActorClassPaths;
		std::set<std::string, std::less<>> LevelClassPaths;
		std::set<std::string, std::less<>> WorldClassPaths;
		ReflectedProperty OwningWorld;
		std::vector<Candidate> Candidates;
		WorldSnapshotObject World;
		std::vector<WorldSnapshotLevel> Levels;
		std::unordered_map<std::int32_t, std::size_t> LevelByIndex;
		std::vector<WorldSnapshotActor> Actors;
		std::size_t NextCandidate = 0;
		std::size_t ValidationIndex = 0;
	};

	bool PrepareWorkingCapture(WorkingCapture& working);
	bool ResolveWorld(WorkingCapture& working);
	bool ScanCandidate(WorkingCapture& working, const Candidate& candidate);
	bool ValidateRecord(WorkingCapture& working, std::size_t validationIndex);
	bool ValidateDependencies(const WorkingCapture& working) const noexcept;
	bool ValidateHandle(const ObjectHandle& handle) noexcept;
	bool ReadStablePointer(std::uintptr_t address, std::uintptr_t& value) const noexcept;
	bool ReadOuter(std::uintptr_t address, std::uintptr_t& outer) const noexcept;
	bool ReadOwningWorld(
		const WorkingCapture& working,
		const EngineSnapshotObject& level,
		std::uintptr_t& owningWorld) const noexcept;
	const EngineSnapshotObject* FindActorLevel(
		const WorkingCapture& working,
		const EngineSnapshotObject& actor,
		bool& readFailed) const noexcept;
	bool AddLevel(WorkingCapture& working, const EngineSnapshotObject& level);
	bool AddActor(
		WorkingCapture& working,
		const EngineSnapshotObject& actor,
		const EngineSnapshotObject& level);
	bool ReadCurrentWorldRecord(
		const WorkingCapture& working,
		const EngineSnapshotObject*& world) const noexcept;
	void CompleteCounts(WorkingCapture& working) noexcept;
	void Fail(WorldSnapshotCaptureError error, std::int32_t objectIndex = -1) noexcept;
	bool IsActive(WorldSnapshotCaptureState state) const noexcept;
	bool NeedsGameThreadPump(WorldSnapshotCaptureState state) const noexcept;

	std::shared_ptr<const EngineContext> m_Context;
	std::string m_SessionId;
	IHandleIdentitySource& m_Identities;
	const EngineSnapshotStore& m_Objects;
	const TypeSnapshotStore& m_Types;
	WorldSnapshotStore& m_Worlds;
	std::int64_t m_GWorldOffset = -1;
	std::int64_t m_OuterOffset = -1;
	CallbackBarrier m_PumpBarrier;
	mutable std::mutex m_RequestMutex;
	std::optional<WorkingCapture> m_Working;
	std::uint64_t m_NextGeneration = 1;
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic<bool> m_StopRequested{false};
	std::atomic<WorldSnapshotCaptureState> m_State{WorldSnapshotCaptureState::Idle};
	std::atomic<WorldSnapshotCaptureError> m_Error{WorldSnapshotCaptureError::None};
	std::atomic<WorldSnapshotPublishError> m_PublishError{WorldSnapshotPublishError::None};
	std::atomic<std::uint64_t> m_RequestedGeneration{0};
	std::atomic<std::uint64_t> m_ObjectSnapshotGeneration{0};
	std::atomic<std::uint64_t> m_TypeSnapshotGeneration{0};
	std::atomic<std::size_t> m_CandidateCount{0};
	std::atomic<std::size_t> m_NextCandidate{0};
	std::atomic<std::size_t> m_CapturedLevels{0};
	std::atomic<std::size_t> m_CapturedActors{0};
	std::atomic<std::size_t> m_ValidationIndex{0};
	std::atomic<std::int32_t> m_ErrorObjectIndex{-1};
};

} // namespace UExplorer::Runtime
