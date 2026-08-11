#pragma once

#include "ObjectHandle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace UExplorer::Runtime
{

struct WorldSnapshotObject
{
	ObjectHandle Handle;
	std::string Name;
	std::string FullPath;
	std::string ClassPath;
};

enum class WorldReferenceState : std::uint8_t
{
	Present,
	NotPresent,
	Unavailable
};

const char* ToString(WorldReferenceState state) noexcept;

struct WorldSnapshotReference
{
	WorldReferenceState State = WorldReferenceState::Unavailable;
	std::optional<WorldSnapshotObject> Object;
	std::string ReasonCode;
	std::string Reason;
};

struct WorldSnapshotLevel
{
	WorldSnapshotObject Object;
	std::uint32_t ActorCount = 0;
};

struct WorldSnapshotActor
{
	WorldSnapshotObject Object;
	ObjectHandle Level;
	WorldSnapshotReference RootComponent;
	std::uint32_t ComponentCount = 0;
};

struct WorldSnapshotComponent
{
	WorldSnapshotObject Object;
	ObjectHandle Owner;
};

struct WorldSnapshot
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t Generation = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::uint64_t CaptureDurationUs = 0;
	WorldSnapshotObject World;
	WorldSnapshotReference GameMode;
	WorldSnapshotReference GameState;
	WorldSnapshotReference PlayerController;
	WorldSnapshotReference Pawn;
	bool ComponentsAvailable = false;
	std::string ComponentsReasonCode;
	std::string ComponentsReason;
	std::vector<WorldSnapshotLevel> Levels;
	std::vector<WorldSnapshotActor> Actors;
	std::vector<WorldSnapshotComponent> Components;

	const WorldSnapshotLevel* FindLevelByIndex(std::int32_t index) const noexcept;
	const WorldSnapshotActor* FindActorByIndex(std::int32_t index) const noexcept;
	const WorldSnapshotComponent* FindComponentByIndex(std::int32_t index) const noexcept;
};

enum class WorldSnapshotPublishError : std::uint8_t
{
	None,
	StoreInvalid,
	StoreStopped,
	EnvelopeInvalid,
	GenerationNotMonotonic,
	CountLimitExceeded,
	WorldInvalid,
	LevelInvalid,
	ActorInvalid,
	ComponentInvalid,
	ReferenceInvalid,
	RelationshipInvalid,
	AllocationFailed
};

const char* ToString(WorldSnapshotPublishError error) noexcept;

struct WorldSnapshotPublishResult
{
	WorldSnapshotPublishError Error = WorldSnapshotPublishError::None;
	std::int32_t RecordIndex = -1;
	std::shared_ptr<const WorldSnapshot> Snapshot;

	bool Ok() const noexcept
	{
		return Error == WorldSnapshotPublishError::None
			&& static_cast<bool>(Snapshot);
	}
};

class WorldSnapshotStore final
{
public:
	static constexpr std::size_t kMaxLevels = 65'536;
	static constexpr std::size_t kMaxActors = 2'000'000;
	static constexpr std::size_t kMaxComponents = 4'000'000;
	static constexpr std::size_t kMaxNameBytes = 1024;
	static constexpr std::size_t kMaxPathBytes = 4096;
	static constexpr std::size_t kMaxReasonCodeBytes = 128;
	static constexpr std::size_t kMaxReasonBytes = 1024;
	static constexpr std::uint64_t kMaxProtocolGeneration =
		9'007'199'254'740'991ULL;

	WorldSnapshotStore(std::string sessionId, std::uint64_t contextGeneration);
	WorldSnapshotStore(const WorldSnapshotStore&) = delete;
	WorldSnapshotStore& operator=(const WorldSnapshotStore&) = delete;

	bool IsConfigured() const noexcept;
	bool IsStopped() const noexcept { return m_Stopped.load(std::memory_order_acquire); }
	WorldSnapshotPublishResult Publish(WorldSnapshot snapshot) noexcept;
	std::shared_ptr<const WorldSnapshot> Current() const noexcept;
	std::uint64_t CurrentGeneration() const noexcept;
	void Stop() noexcept;

private:
	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	mutable std::mutex m_PublishMutex;
	std::atomic<std::shared_ptr<const WorldSnapshot>> m_Current;
	std::atomic<bool> m_Stopped{false};
};

} // namespace UExplorer::Runtime
