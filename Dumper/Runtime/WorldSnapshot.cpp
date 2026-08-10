#include "WorldSnapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

bool SameHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool IsHandleValid(
	const ObjectHandle& handle,
	const std::string& sessionId,
	const std::uint64_t contextGeneration) noexcept
{
	return handle.SessionId == sessionId
		&& handle.ContextGeneration == contextGeneration
		&& handle.Index >= 0
		&& handle.SerialNumber > 0
		&& handle.Address != 0
		&& handle.ClassFingerprint != 0;
}

bool IsTextValid(const std::string& value, const std::size_t maximum) noexcept
{
	return !value.empty() && value.size() <= maximum;
}

bool IsObjectValid(
	const WorldSnapshotObject& object,
	const std::string& sessionId,
	const std::uint64_t contextGeneration) noexcept
{
	return IsHandleValid(object.Handle, sessionId, contextGeneration)
		&& IsTextValid(object.Name, WorldSnapshotStore::kMaxNameBytes)
		&& IsTextValid(object.FullPath, WorldSnapshotStore::kMaxPathBytes)
		&& IsTextValid(object.ClassPath, WorldSnapshotStore::kMaxPathBytes);
}

} // namespace

const WorldSnapshotLevel* WorldSnapshot::FindLevelByIndex(
	const std::int32_t index) const noexcept
{
	const auto found = std::lower_bound(
		Levels.begin(),
		Levels.end(),
		index,
		[](const WorldSnapshotLevel& level, const std::int32_t expected) {
			return level.Object.Handle.Index < expected;
		});
	return found != Levels.end() && found->Object.Handle.Index == index
		? &*found
		: nullptr;
}

const WorldSnapshotActor* WorldSnapshot::FindActorByIndex(
	const std::int32_t index) const noexcept
{
	const auto found = std::lower_bound(
		Actors.begin(),
		Actors.end(),
		index,
		[](const WorldSnapshotActor& actor, const std::int32_t expected) {
			return actor.Object.Handle.Index < expected;
		});
	return found != Actors.end() && found->Object.Handle.Index == index
		? &*found
		: nullptr;
}

const char* ToString(const WorldSnapshotPublishError error) noexcept
{
	switch (error)
	{
	case WorldSnapshotPublishError::None: return "NONE";
	case WorldSnapshotPublishError::StoreInvalid: return "WORLD_SNAPSHOT_STORE_INVALID";
	case WorldSnapshotPublishError::StoreStopped: return "WORLD_SNAPSHOT_STORE_STOPPED";
	case WorldSnapshotPublishError::EnvelopeInvalid: return "WORLD_SNAPSHOT_ENVELOPE_INVALID";
	case WorldSnapshotPublishError::GenerationNotMonotonic: return "WORLD_SNAPSHOT_GENERATION_NOT_MONOTONIC";
	case WorldSnapshotPublishError::CountLimitExceeded: return "WORLD_SNAPSHOT_COUNT_LIMIT_EXCEEDED";
	case WorldSnapshotPublishError::WorldInvalid: return "WORLD_SNAPSHOT_WORLD_INVALID";
	case WorldSnapshotPublishError::LevelInvalid: return "WORLD_SNAPSHOT_LEVEL_INVALID";
	case WorldSnapshotPublishError::ActorInvalid: return "WORLD_SNAPSHOT_ACTOR_INVALID";
	case WorldSnapshotPublishError::RelationshipInvalid: return "WORLD_SNAPSHOT_RELATIONSHIP_INVALID";
	case WorldSnapshotPublishError::AllocationFailed: return "WORLD_SNAPSHOT_ALLOCATION_FAILED";
	}
	return "WORLD_SNAPSHOT_UNKNOWN_ERROR";
}

WorldSnapshotStore::WorldSnapshotStore(
	std::string sessionId,
	const std::uint64_t contextGeneration)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration)
{
}

bool WorldSnapshotStore::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_ContextGeneration != 0
		&& m_ContextGeneration <= kMaxProtocolGeneration;
}

WorldSnapshotPublishResult WorldSnapshotStore::Publish(
	WorldSnapshot snapshot) noexcept
{
	try
	{
		std::lock_guard lock(m_PublishMutex);
		if (!IsConfigured())
			return {.Error = WorldSnapshotPublishError::StoreInvalid};
		if (IsStopped())
			return {.Error = WorldSnapshotPublishError::StoreStopped};
		if (snapshot.SessionId != m_SessionId
			|| snapshot.ContextGeneration != m_ContextGeneration
			|| snapshot.Generation == 0
			|| snapshot.Generation > kMaxProtocolGeneration
			|| snapshot.ObjectSnapshotGeneration == 0
			|| snapshot.ObjectSnapshotGeneration > kMaxProtocolGeneration
			|| snapshot.TypeSnapshotGeneration == 0
			|| snapshot.TypeSnapshotGeneration > kMaxProtocolGeneration
			|| snapshot.CapturedAtMonotonicUs == 0)
		{
			return {.Error = WorldSnapshotPublishError::EnvelopeInvalid};
		}
		const std::shared_ptr<const WorldSnapshot> current = Current();
		if (current && snapshot.Generation <= current->Generation)
			return {.Error = WorldSnapshotPublishError::GenerationNotMonotonic};
		if (snapshot.Levels.size() > kMaxLevels || snapshot.Actors.size() > kMaxActors)
			return {.Error = WorldSnapshotPublishError::CountLimitExceeded};
		if (!IsObjectValid(snapshot.World, m_SessionId, m_ContextGeneration))
			return {.Error = WorldSnapshotPublishError::WorldInvalid};

		std::set<std::int32_t> objectIndices{snapshot.World.Handle.Index};
		std::set<std::uintptr_t> objectAddresses{snapshot.World.Handle.Address};
		std::int32_t previousIndex = -1;
		for (std::size_t index = 0; index < snapshot.Levels.size(); ++index)
		{
			WorldSnapshotLevel& level = snapshot.Levels[index];
			if (!IsObjectValid(level.Object, m_SessionId, m_ContextGeneration)
				|| level.Object.Handle.Index <= previousIndex
				|| level.ActorCount > snapshot.Actors.size()
				|| !objectIndices.emplace(level.Object.Handle.Index).second
				|| !objectAddresses.emplace(level.Object.Handle.Address).second)
			{
				return {
					.Error = WorldSnapshotPublishError::LevelInvalid,
					.RecordIndex = static_cast<std::int32_t>(index)
				};
			}
			previousIndex = level.Object.Handle.Index;
		}

		std::vector<std::uint32_t> observedActorCounts(snapshot.Levels.size(), 0);
		previousIndex = -1;
		for (std::size_t index = 0; index < snapshot.Actors.size(); ++index)
		{
			const WorldSnapshotActor& actor = snapshot.Actors[index];
			if (!IsObjectValid(actor.Object, m_SessionId, m_ContextGeneration)
				|| !IsHandleValid(actor.Level, m_SessionId, m_ContextGeneration)
				|| actor.Object.Handle.Index <= previousIndex
				|| !objectIndices.emplace(actor.Object.Handle.Index).second
				|| !objectAddresses.emplace(actor.Object.Handle.Address).second)
			{
				return {
					.Error = WorldSnapshotPublishError::ActorInvalid,
					.RecordIndex = static_cast<std::int32_t>(index)
				};
			}
			const auto level = std::lower_bound(
				snapshot.Levels.begin(),
				snapshot.Levels.end(),
				actor.Level.Index,
				[](const WorldSnapshotLevel& candidate, const std::int32_t expected) {
					return candidate.Object.Handle.Index < expected;
				});
			if (level == snapshot.Levels.end()
				|| !SameHandle(level->Object.Handle, actor.Level))
			{
				return {
					.Error = WorldSnapshotPublishError::RelationshipInvalid,
					.RecordIndex = static_cast<std::int32_t>(index)
				};
			}
			const std::size_t levelIndex = static_cast<std::size_t>(
				std::distance(snapshot.Levels.begin(), level));
			++observedActorCounts[levelIndex];
			previousIndex = actor.Object.Handle.Index;
		}
		for (std::size_t index = 0; index < snapshot.Levels.size(); ++index)
		{
			if (snapshot.Levels[index].ActorCount != observedActorCounts[index])
			{
				return {
					.Error = WorldSnapshotPublishError::RelationshipInvalid,
					.RecordIndex = static_cast<std::int32_t>(index)
				};
			}
		}

		auto published = std::make_shared<const WorldSnapshot>(std::move(snapshot));
		m_Current.store(published, std::memory_order_release);
		return {.Snapshot = std::move(published)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = WorldSnapshotPublishError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = WorldSnapshotPublishError::EnvelopeInvalid};
	}
}

std::shared_ptr<const WorldSnapshot> WorldSnapshotStore::Current() const noexcept
{
	return m_Current.load(std::memory_order_acquire);
}

std::uint64_t WorldSnapshotStore::CurrentGeneration() const noexcept
{
	const std::shared_ptr<const WorldSnapshot> current = Current();
	return current ? current->Generation : 0;
}

void WorldSnapshotStore::Stop() noexcept
{
	std::lock_guard lock(m_PublishMutex);
	m_Stopped.store(true, std::memory_order_release);
	m_Current.store({}, std::memory_order_release);
}

} // namespace UExplorer::Runtime
