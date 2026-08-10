#include "WorldSnapshotCapture.h"

#include "SafeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
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

bool SameHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

WorldSnapshotObject CopyObject(const EngineSnapshotObject& object)
{
	return {
		.Handle = object.Handle,
		.Name = object.Name,
		.FullPath = object.FullPath,
		.ClassPath = object.ClassPath
	};
}

bool IsDerivedFrom(
	const TypeSnapshot& types,
	const ReflectedType& candidate,
	const std::int32_t baseIndex) noexcept
{
	const ReflectedType* current = &candidate;
	for (std::size_t depth = 0;
		current && depth <= TypeSnapshotStore::kMaxHierarchyDepth;
		++depth)
	{
		if (current->Handle.Index == baseIndex)
			return true;
		if (!current->Super)
			return false;
		current = types.FindByObjectIndex(current->Super->Index);
	}
	return false;
}

bool BuildClassPaths(
	const TypeSnapshot& types,
	const std::string_view basePath,
	std::set<std::string, std::less<>>& paths)
{
	paths.clear();
	const ReflectedType* base = types.FindByFullPath(basePath);
	if (!base || base->Kind != ReflectedTypeKind::Class)
		return false;
	for (const ReflectedType& type : types.Types())
	{
		if (type.Kind == ReflectedTypeKind::Class
			&& IsDerivedFrom(types, type, base->Handle.Index))
		{
			paths.emplace(type.FullPath);
		}
	}
	return !paths.empty();
}

} // namespace

const char* ToString(const WorldSnapshotCaptureState state) noexcept
{
	switch (state)
	{
	case WorldSnapshotCaptureState::Idle: return "idle";
	case WorldSnapshotCaptureState::Requested: return "requested";
	case WorldSnapshotCaptureState::Scanning: return "scanning";
	case WorldSnapshotCaptureState::Sealing: return "sealing";
	case WorldSnapshotCaptureState::Validating: return "validating";
	case WorldSnapshotCaptureState::Publishing: return "publishing";
	case WorldSnapshotCaptureState::Completed: return "completed";
	case WorldSnapshotCaptureState::Failed: return "failed";
	case WorldSnapshotCaptureState::Stopping: return "stopping";
	case WorldSnapshotCaptureState::Stopped: return "stopped";
	}
	return "unknown";
}

const char* ToString(const WorldSnapshotCaptureError error) noexcept
{
	switch (error)
	{
	case WorldSnapshotCaptureError::None: return "NONE";
	case WorldSnapshotCaptureError::Busy: return "WORLD_SNAPSHOT_CAPTURE_BUSY";
	case WorldSnapshotCaptureError::Stopped: return "WORLD_SNAPSHOT_CAPTURE_STOPPED";
	case WorldSnapshotCaptureError::InvalidConfiguration: return "WORLD_SNAPSHOT_CAPTURE_INVALID";
	case WorldSnapshotCaptureError::DependenciesUnavailable: return "WORLD_SNAPSHOT_DEPENDENCIES_UNAVAILABLE";
	case WorldSnapshotCaptureError::DependencyChanged: return "WORLD_SNAPSHOT_DEPENDENCY_CHANGED";
	case WorldSnapshotCaptureError::TypeMetadataUnavailable: return "WORLD_SNAPSHOT_TYPE_METADATA_UNAVAILABLE";
	case WorldSnapshotCaptureError::ExecutionThreadInvalid: return "WORLD_SNAPSHOT_EXECUTION_THREAD_INVALID";
	case WorldSnapshotCaptureError::WorldPointerUnavailable: return "WORLD_POINTER_UNAVAILABLE";
	case WorldSnapshotCaptureError::WorldIdentityInvalid: return "WORLD_IDENTITY_INVALID";
	case WorldSnapshotCaptureError::SourceReadFailed: return "WORLD_SNAPSHOT_SOURCE_READ_FAILED";
	case WorldSnapshotCaptureError::SourceValidationFailed: return "WORLD_SNAPSHOT_SOURCE_VALIDATION_FAILED";
	case WorldSnapshotCaptureError::CountLimitExceeded: return "WORLD_SNAPSHOT_COUNT_LIMIT_EXCEEDED";
	case WorldSnapshotCaptureError::PublicationRejected: return "WORLD_SNAPSHOT_PUBLICATION_REJECTED";
	case WorldSnapshotCaptureError::GenerationExhausted: return "WORLD_SNAPSHOT_GENERATION_EXHAUSTED";
	case WorldSnapshotCaptureError::AllocationFailed: return "WORLD_SNAPSHOT_ALLOCATION_FAILED";
	case WorldSnapshotCaptureError::UnexpectedException: return "WORLD_SNAPSHOT_CAPTURE_EXCEPTION";
	}
	return "WORLD_SNAPSHOT_CAPTURE_UNKNOWN_ERROR";
}

WorldSnapshotCapture::WorldSnapshotCapture(
	std::shared_ptr<const EngineContext> context,
	std::string sessionId,
	IHandleIdentitySource& identities,
	const EngineSnapshotStore& objects,
	const TypeSnapshotStore& types,
	WorldSnapshotStore& worlds)
	: m_Context(std::move(context)),
	  m_SessionId(std::move(sessionId)),
	  m_Identities(identities),
	  m_Objects(objects),
	  m_Types(types),
	  m_Worlds(worlds)
{
	if (m_Context)
	{
		if (const OffsetReport* world = m_Context->FindOffset("gworld");
			world && world->IsValidated())
		{
			m_GWorldOffset = world->Value;
		}
		if (const OffsetReport* outer = m_Context->FindOffset("uobject.outer");
			outer && outer->IsValidated())
		{
			m_OuterOffset = outer->Value;
		}
	}
}

bool WorldSnapshotCapture::IsConfigured() const noexcept
{
	return m_Context
		&& m_Context->Generation() != 0
		&& m_Context->Generation() == m_Identities.ContextGeneration()
		&& !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_GWorldOffset > 0
		&& m_OuterOffset >= 0
		&& m_Worlds.IsConfigured()
		&& !m_Worlds.IsStopped();
}

bool WorldSnapshotCapture::IsActive(
	const WorldSnapshotCaptureState state) const noexcept
{
	return state == WorldSnapshotCaptureState::Requested
		|| state == WorldSnapshotCaptureState::Scanning
		|| state == WorldSnapshotCaptureState::Sealing
		|| state == WorldSnapshotCaptureState::Validating
		|| state == WorldSnapshotCaptureState::Publishing;
}

bool WorldSnapshotCapture::NeedsGameThreadPump(
	const WorldSnapshotCaptureState state) const noexcept
{
	return state == WorldSnapshotCaptureState::Requested
		|| state == WorldSnapshotCaptureState::Scanning
		|| state == WorldSnapshotCaptureState::Validating;
}

bool WorldSnapshotCapture::PrepareWorkingCapture(WorkingCapture& working)
{
	working.Objects = m_Objects.Current();
	working.Types = m_Types.Current();
	if (!working.Objects || !working.Types
		|| working.Objects->SessionId != m_SessionId
		|| working.Objects->ContextGeneration != m_Context->Generation()
		|| !working.Types->IsConfigured(m_Context->Generation())
		|| working.Types->ObjectSnapshotGeneration() != working.Objects->Generation)
	{
		return false;
	}
	if (!BuildClassPaths(*working.Types, kActorClassPath, working.ActorClassPaths)
		|| !BuildClassPaths(*working.Types, kLevelClassPath, working.LevelClassPaths)
		|| !BuildClassPaths(*working.Types, kWorldClassPath, working.WorldClassPaths))
	{
		m_Error.store(WorldSnapshotCaptureError::TypeMetadataUnavailable, std::memory_order_release);
		return false;
	}

	const TypePropertyQueryResult properties = QueryTypeProperties(
		working.Types,
		kLevelClassPath,
		TypeMemberScope::IncludeInherited);
	if (!properties.Ok())
	{
		m_Error.store(WorldSnapshotCaptureError::TypeMetadataUnavailable, std::memory_order_release);
		return false;
	}
	const ReflectedProperty* owningWorld = nullptr;
	for (const TypeMemberView<ReflectedProperty>& member : properties.Members)
	{
		if (member.Member && member.Member->Name == "OwningWorld")
		{
			if (owningWorld)
			{
				m_Error.store(WorldSnapshotCaptureError::TypeMetadataUnavailable, std::memory_order_release);
				return false;
			}
			owningWorld = member.Member;
		}
	}
	if (!owningWorld
		|| owningWorld->State != ReflectedMemberState::Supported
		|| owningWorld->Kind != PropertyKind::Object
		|| owningWorld->ArrayDim != 1
		|| owningWorld->Size != sizeof(std::uintptr_t)
		|| !owningWorld->Descriptor
		|| owningWorld->Descriptor->Kind != PropertyKind::Object
		|| owningWorld->Descriptor->Size != sizeof(std::uintptr_t))
	{
		m_Error.store(WorldSnapshotCaptureError::TypeMetadataUnavailable, std::memory_order_release);
		return false;
	}
	working.OwningWorld = *owningWorld;

	std::size_t levelCandidates = 0;
	std::size_t actorCandidates = 0;
	working.Candidates.reserve((std::min)(
		working.Objects->Objects.size(),
		WorldSnapshotStore::kMaxLevels + WorldSnapshotStore::kMaxActors));
	for (const EngineSnapshotObject& object : working.Objects->Objects)
	{
		if (working.LevelClassPaths.contains(object.ClassPath))
		{
			if (levelCandidates == WorldSnapshotStore::kMaxLevels)
			{
				m_Error.store(WorldSnapshotCaptureError::CountLimitExceeded, std::memory_order_release);
				return false;
			}
			working.Candidates.push_back({.Object = &object, .Kind = CandidateKind::Level});
			++levelCandidates;
		}
		else if (working.ActorClassPaths.contains(object.ClassPath))
		{
			if (actorCandidates == WorldSnapshotStore::kMaxActors)
			{
				m_Error.store(WorldSnapshotCaptureError::CountLimitExceeded, std::memory_order_release);
				return false;
			}
			working.Candidates.push_back({.Object = &object, .Kind = CandidateKind::Actor});
			++actorCandidates;
		}
	}
	working.Levels.reserve(levelCandidates);
	working.LevelByIndex.reserve(levelCandidates);
	working.Actors.reserve(actorCandidates);
	return true;
}

WorldSnapshotCaptureRequestResult WorldSnapshotCapture::RequestCapture() noexcept
{
	try
	{
		std::lock_guard lock(m_RequestMutex);
		if (m_StopRequested.load(std::memory_order_acquire) || m_Worlds.IsStopped())
			return {.Error = WorldSnapshotCaptureError::Stopped};
		if (!IsConfigured())
			return {.Error = WorldSnapshotCaptureError::InvalidConfiguration};
		if (IsActive(m_State.load(std::memory_order_acquire)))
			return {.Error = WorldSnapshotCaptureError::Busy};
		const std::uint64_t current = m_Worlds.CurrentGeneration();
		if (current >= WorldSnapshotStore::kMaxProtocolGeneration)
			return {.Error = WorldSnapshotCaptureError::GenerationExhausted};
		if (m_NextGeneration <= current)
			m_NextGeneration = current + 1;
		if (m_NextGeneration == 0
			|| m_NextGeneration > WorldSnapshotStore::kMaxProtocolGeneration)
		{
			return {.Error = WorldSnapshotCaptureError::GenerationExhausted};
		}

		WorkingCapture working{
			.Generation = m_NextGeneration++,
			.StartedAtMonotonicUs = MonotonicMicroseconds()
		};
		m_Error.store(WorldSnapshotCaptureError::None, std::memory_order_release);
		m_PublishError.store(WorldSnapshotPublishError::None, std::memory_order_release);
		if (!PrepareWorkingCapture(working))
		{
			const WorldSnapshotCaptureError error =
				m_Error.load(std::memory_order_acquire) == WorldSnapshotCaptureError::None
					? WorldSnapshotCaptureError::DependenciesUnavailable
					: m_Error.load(std::memory_order_acquire);
			m_Error.store(error, std::memory_order_release);
			m_State.store(WorldSnapshotCaptureState::Failed, std::memory_order_release);
			return {.Error = error};
		}

		m_RequestedGeneration.store(working.Generation, std::memory_order_release);
		m_ObjectSnapshotGeneration.store(working.Objects->Generation, std::memory_order_release);
		m_TypeSnapshotGeneration.store(working.Types->Generation(), std::memory_order_release);
		m_CandidateCount.store(working.Candidates.size(), std::memory_order_release);
		m_NextCandidate.store(0, std::memory_order_release);
		m_CapturedLevels.store(0, std::memory_order_release);
		m_CapturedActors.store(0, std::memory_order_release);
		m_ValidationIndex.store(0, std::memory_order_release);
		m_ErrorObjectIndex.store(-1, std::memory_order_release);
		const std::uint64_t generation = working.Generation;
		m_Working.emplace(std::move(working));
		m_State.store(WorldSnapshotCaptureState::Requested, std::memory_order_release);
		return {.Generation = generation};
	}
	catch (const std::bad_alloc&)
	{
		m_Error.store(WorldSnapshotCaptureError::AllocationFailed, std::memory_order_release);
		m_State.store(WorldSnapshotCaptureState::Failed, std::memory_order_release);
		return {.Error = WorldSnapshotCaptureError::AllocationFailed};
	}
	catch (...)
	{
		m_Error.store(WorldSnapshotCaptureError::UnexpectedException, std::memory_order_release);
		m_State.store(WorldSnapshotCaptureState::Failed, std::memory_order_release);
		return {.Error = WorldSnapshotCaptureError::UnexpectedException};
	}
}

bool WorldSnapshotCapture::ReadStablePointer(
	const std::uintptr_t address,
	std::uintptr_t& value) const noexcept
{
	value = 0;
	std::uintptr_t first = 0;
	std::uintptr_t second = 0;
	return address != 0
		&& ReadValue(address, first).Ok()
		&& ReadValue(address, second).Ok()
		&& first == second
		&& ((value = first), true);
}

bool WorldSnapshotCapture::ReadCurrentWorldRecord(
	const WorkingCapture& working,
	const EngineSnapshotObject*& world) const noexcept
{
	world = nullptr;
	if (!m_Context
		|| m_GWorldOffset <= 0
		|| static_cast<std::uint64_t>(m_GWorldOffset)
			> (std::numeric_limits<std::uintptr_t>::max)() - m_Context->ModuleBase())
	{
		return false;
	}
	std::uintptr_t worldAddress = 0;
	if (!ReadStablePointer(
		m_Context->ModuleBase() + static_cast<std::uintptr_t>(m_GWorldOffset),
		worldAddress)
		|| worldAddress == 0)
	{
		return false;
	}
	const EngineSnapshotObject* candidate = working.Objects->FindByAddress(worldAddress);
	if (!candidate || !working.WorldClassPaths.contains(candidate->ClassPath))
		return false;
	world = candidate;
	return true;
}

bool WorldSnapshotCapture::ValidateHandle(const ObjectHandle& handle) noexcept
{
	if (handle.SessionId != m_SessionId
		|| !m_Context
		|| handle.ContextGeneration != m_Context->Generation()
		|| !m_Identities.IsCurrentExecutionThreadValid())
	{
		return false;
	}
	ObjectIdentity current;
	return m_Identities.TryReadObject(handle.Index, current)
		&& current.Index == handle.Index
		&& current.SerialNumber == handle.SerialNumber
		&& current.Address == handle.Address
		&& current.ClassFingerprint == handle.ClassFingerprint;
}

bool WorldSnapshotCapture::ResolveWorld(WorkingCapture& working)
{
	const EngineSnapshotObject* world = nullptr;
	if (!ReadCurrentWorldRecord(working, world))
		return false;
	if (!ValidateHandle(world->Handle))
	{
		m_Error.store(WorldSnapshotCaptureError::WorldIdentityInvalid, std::memory_order_release);
		return false;
	}
	working.World = CopyObject(*world);
	return true;
}

bool WorldSnapshotCapture::ReadOuter(
	const std::uintptr_t address,
	std::uintptr_t& outer) const noexcept
{
	outer = 0;
	if (m_OuterOffset < 0
		|| static_cast<std::uint64_t>(m_OuterOffset)
			> (std::numeric_limits<std::uintptr_t>::max)() - address)
	{
		return false;
	}
	return ReadStablePointer(
		address + static_cast<std::uintptr_t>(m_OuterOffset),
		outer);
}

bool WorldSnapshotCapture::ReadOwningWorld(
	const WorkingCapture& working,
	const EngineSnapshotObject& level,
	std::uintptr_t& owningWorld) const noexcept
{
	owningWorld = 0;
	if (working.OwningWorld.Offset
		> (std::numeric_limits<std::uintptr_t>::max)() - level.Handle.Address)
	{
		return false;
	}
	return ReadStablePointer(
		level.Handle.Address + working.OwningWorld.Offset,
		owningWorld);
}

const EngineSnapshotObject* WorldSnapshotCapture::FindActorLevel(
	const WorkingCapture& working,
	const EngineSnapshotObject& actor,
	bool& readFailed) const noexcept
{
	readFailed = false;
	std::uintptr_t current = actor.Handle.Address;
	std::array<std::uintptr_t, kMaxOuterDepth> visited{};
	std::size_t visitedCount = 0;
	for (std::size_t depth = 0; depth < kMaxOuterDepth; ++depth)
	{
		std::uintptr_t outer = 0;
		if (!ReadOuter(current, outer))
		{
			readFailed = true;
			return nullptr;
		}
		if (outer == 0)
			return nullptr;
		if (std::find(visited.begin(), visited.begin() + visitedCount, outer)
			!= visited.begin() + visitedCount)
		{
			readFailed = true;
			return nullptr;
		}
		visited[visitedCount++] = outer;
		const EngineSnapshotObject* object = working.Objects->FindByAddress(outer);
		if (!object)
			return nullptr;
		if (working.LevelClassPaths.contains(object->ClassPath))
			return object;
		current = outer;
	}
	readFailed = true;
	return nullptr;
}

bool WorldSnapshotCapture::AddLevel(
	WorkingCapture& working,
	const EngineSnapshotObject& level)
{
	if (working.LevelByIndex.contains(level.Handle.Index))
		return true;
	if (working.Levels.size() >= WorldSnapshotStore::kMaxLevels)
		return false;
	working.LevelByIndex.emplace(level.Handle.Index, working.Levels.size());
	working.Levels.push_back({.Object = CopyObject(level)});
	return true;
}

bool WorldSnapshotCapture::AddActor(
	WorkingCapture& working,
	const EngineSnapshotObject& actor,
	const EngineSnapshotObject& level)
{
	if (working.Actors.size() >= WorldSnapshotStore::kMaxActors
		|| !AddLevel(working, level))
	{
		return false;
	}
	working.Actors.push_back({
		.Object = CopyObject(actor),
		.Level = level.Handle
	});
	return true;
}

bool WorldSnapshotCapture::ScanCandidate(
	WorkingCapture& working,
	const Candidate& candidate)
{
	if (!candidate.Object)
		return false;
	if (candidate.Kind == CandidateKind::Level)
	{
		std::uintptr_t owningWorld = 0;
		if (!ReadOwningWorld(working, *candidate.Object, owningWorld))
			return false;
		return owningWorld != working.World.Handle.Address
			|| AddLevel(working, *candidate.Object);
	}

	bool readFailed = false;
	const EngineSnapshotObject* level = FindActorLevel(
		working,
		*candidate.Object,
		readFailed);
	if (readFailed)
		return false;
	if (!level)
		return true;
	std::uintptr_t owningWorld = 0;
	if (!ReadOwningWorld(working, *level, owningWorld))
		return false;
	return owningWorld != working.World.Handle.Address
		|| AddActor(working, *candidate.Object, *level);
}

bool WorldSnapshotCapture::ValidateDependencies(
	const WorkingCapture& working) const noexcept
{
	const std::shared_ptr<const EngineSnapshot> currentObjects = m_Objects.Current();
	const std::shared_ptr<const TypeSnapshot> currentTypes = m_Types.Current();
	return working.Objects
		&& working.Types
		&& currentObjects == working.Objects
		&& currentTypes == working.Types
		&& working.Objects->SessionId == m_SessionId
		&& working.Objects->ContextGeneration == m_Context->Generation()
		&& working.Types->IsConfigured(m_Context->Generation())
		&& working.Types->ObjectSnapshotGeneration() == working.Objects->Generation;
}

bool WorldSnapshotCapture::ValidateRecord(
	WorkingCapture& working,
	const std::size_t validationIndex)
{
	if (validationIndex == 0)
	{
		const EngineSnapshotObject* current = nullptr;
		return ReadCurrentWorldRecord(working, current)
			&& current
			&& SameHandle(current->Handle, working.World.Handle)
			&& ValidateHandle(working.World.Handle);
	}
	const std::size_t levelIndex = validationIndex - 1;
	if (levelIndex < working.Levels.size())
	{
		const WorldSnapshotLevel& level = working.Levels[levelIndex];
		const EngineSnapshotObject* source = working.Objects->FindByIndex(level.Object.Handle.Index);
		std::uintptr_t owningWorld = 0;
		return source
			&& SameHandle(source->Handle, level.Object.Handle)
			&& ValidateHandle(level.Object.Handle)
			&& ReadOwningWorld(working, *source, owningWorld)
			&& owningWorld == working.World.Handle.Address;
	}

	const std::size_t actorIndex = levelIndex - working.Levels.size();
	if (actorIndex >= working.Actors.size())
		return false;
	const WorldSnapshotActor& actor = working.Actors[actorIndex];
	const EngineSnapshotObject* source = working.Objects->FindByIndex(actor.Object.Handle.Index);
	if (!source
		|| !SameHandle(source->Handle, actor.Object.Handle)
		|| !ValidateHandle(actor.Object.Handle))
	{
		return false;
	}
	bool readFailed = false;
	const EngineSnapshotObject* level = FindActorLevel(working, *source, readFailed);
	std::uintptr_t owningWorld = 0;
	return !readFailed
		&& level
		&& SameHandle(level->Handle, actor.Level)
		&& ReadOwningWorld(working, *level, owningWorld)
		&& owningWorld == working.World.Handle.Address;
}

void WorldSnapshotCapture::CompleteCounts(WorkingCapture& working) noexcept
{
	std::ranges::sort(working.Levels, {}, [](const WorldSnapshotLevel& level) {
		return level.Object.Handle.Index;
	});
	std::ranges::sort(working.Actors, {}, [](const WorldSnapshotActor& actor) {
		return actor.Object.Handle.Index;
	});
	for (WorldSnapshotLevel& level : working.Levels)
		level.ActorCount = 0;
	for (const WorldSnapshotActor& actor : working.Actors)
	{
		const auto level = std::lower_bound(
			working.Levels.begin(),
			working.Levels.end(),
			actor.Level.Index,
			[](const WorldSnapshotLevel& candidate, const std::int32_t expected) {
				return candidate.Object.Handle.Index < expected;
			});
		if (level != working.Levels.end()
			&& SameHandle(level->Object.Handle, actor.Level))
		{
			++level->ActorCount;
		}
	}
}

bool WorldSnapshotCapture::SealForValidation() noexcept
{
	try
	{
		std::lock_guard lock(m_RequestMutex);
		if (m_StopRequested.load(std::memory_order_acquire)
			|| m_State.load(std::memory_order_acquire) != WorldSnapshotCaptureState::Sealing
			|| !m_Working)
		{
			return false;
		}
		if (!ValidateDependencies(*m_Working))
		{
			Fail(WorldSnapshotCaptureError::DependencyChanged);
			return false;
		}
		CompleteCounts(*m_Working);
		m_CapturedLevels.store(m_Working->Levels.size(), std::memory_order_release);
		m_CapturedActors.store(m_Working->Actors.size(), std::memory_order_release);
		m_State.store(WorldSnapshotCaptureState::Validating, std::memory_order_release);
		return true;
	}
	catch (const std::bad_alloc&)
	{
		Fail(WorldSnapshotCaptureError::AllocationFailed);
	}
	catch (...)
	{
		Fail(WorldSnapshotCaptureError::UnexpectedException);
	}
	return false;
}

bool WorldSnapshotCapture::PublishReady() noexcept
{
	try
	{
		std::lock_guard lock(m_RequestMutex);
		if (m_StopRequested.load(std::memory_order_acquire)
			|| m_State.load(std::memory_order_acquire) != WorldSnapshotCaptureState::Publishing
			|| !m_Working)
		{
			return false;
		}
		WorkingCapture& working = *m_Working;
		if (!ValidateDependencies(working))
		{
			Fail(WorldSnapshotCaptureError::DependencyChanged);
			return false;
		}
		const std::uint64_t capturedAt = MonotonicMicroseconds();
		WorldSnapshot snapshot{
			.SessionId = m_SessionId,
			.ContextGeneration = m_Context->Generation(),
			.Generation = working.Generation,
			.ObjectSnapshotGeneration = working.Objects->Generation,
			.TypeSnapshotGeneration = working.Types->Generation(),
			.CapturedAtMonotonicUs = capturedAt,
			.CaptureDurationUs = capturedAt >= working.StartedAtMonotonicUs
				? capturedAt - working.StartedAtMonotonicUs
				: 0,
			.World = std::move(working.World),
			.Levels = std::move(working.Levels),
			.Actors = std::move(working.Actors)
		};
		const WorldSnapshotPublishResult published = m_Worlds.Publish(std::move(snapshot));
		if (!published.Ok())
		{
			m_PublishError.store(published.Error, std::memory_order_release);
			Fail(WorldSnapshotCaptureError::PublicationRejected, published.RecordIndex);
			return false;
		}
		m_Working.reset();
		m_Error.store(WorldSnapshotCaptureError::None, std::memory_order_release);
		m_PublishError.store(WorldSnapshotPublishError::None, std::memory_order_release);
		m_State.store(WorldSnapshotCaptureState::Completed, std::memory_order_release);
		return true;
	}
	catch (const std::bad_alloc&)
	{
		Fail(WorldSnapshotCaptureError::AllocationFailed);
	}
	catch (...)
	{
		Fail(WorldSnapshotCaptureError::UnexpectedException);
	}
	return false;
}

void WorldSnapshotCapture::Fail(
	const WorldSnapshotCaptureError error,
	const std::int32_t objectIndex) noexcept
{
	m_Error.store(error, std::memory_order_release);
	m_ErrorObjectIndex.store(objectIndex, std::memory_order_release);
	m_Working.reset();
	m_State.store(WorldSnapshotCaptureState::Failed, std::memory_order_release);
}

IGameThreadFrameClient::PumpResult WorldSnapshotCapture::PumpFrame(
	const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > PostRenderPumpBackend::kFrameWorkBudget)
		return {};
	auto lease = m_PumpBarrier.Enter();
	if (!lease.OwnedWorkAllowed() || m_StopRequested.load(std::memory_order_acquire))
		return {};
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
		return {.MoreWorkPending = true};
	struct PumpGuard
	{
		std::atomic_flag& Owned;
		~PumpGuard() { Owned.clear(std::memory_order_release); }
	} pumpGuard{m_PumpOwned};

	std::size_t consumed = 0;
	try
	{
		const WorldSnapshotCaptureState initialState =
			m_State.load(std::memory_order_acquire);
		if (!NeedsGameThreadPump(initialState) || !m_Working)
			return {};
		if (!m_Identities.IsCurrentExecutionThreadValid())
		{
			Fail(WorldSnapshotCaptureError::ExecutionThreadInvalid);
			return {};
		}
		if (!ValidateDependencies(*m_Working))
		{
			Fail(WorldSnapshotCaptureError::DependencyChanged);
			return {};
		}
		while (consumed < workBudget)
		{
			WorldSnapshotCaptureState state = m_State.load(std::memory_order_acquire);
			if (!NeedsGameThreadPump(state) || !m_Working)
				break;
			WorkingCapture& working = *m_Working;

			if (state == WorldSnapshotCaptureState::Requested)
			{
				++consumed;
				if (!ResolveWorld(working))
				{
					const WorldSnapshotCaptureError error =
						m_Error.load(std::memory_order_acquire) == WorldSnapshotCaptureError::WorldIdentityInvalid
							? WorldSnapshotCaptureError::WorldIdentityInvalid
							: WorldSnapshotCaptureError::WorldPointerUnavailable;
					Fail(error);
					break;
				}
				m_State.store(WorldSnapshotCaptureState::Scanning, std::memory_order_release);
				continue;
			}
			if (state == WorldSnapshotCaptureState::Scanning)
			{
				if (working.NextCandidate < working.Candidates.size())
				{
					const Candidate& candidate = working.Candidates[working.NextCandidate];
					++consumed;
					if (!ScanCandidate(working, candidate))
					{
						Fail(
							working.Levels.size() > WorldSnapshotStore::kMaxLevels
								|| working.Actors.size() > WorldSnapshotStore::kMaxActors
									? WorldSnapshotCaptureError::CountLimitExceeded
									: WorldSnapshotCaptureError::SourceReadFailed,
							candidate.Object ? candidate.Object->Handle.Index : -1);
						break;
					}
					++working.NextCandidate;
					m_NextCandidate.store(working.NextCandidate, std::memory_order_release);
					m_CapturedLevels.store(working.Levels.size(), std::memory_order_release);
					m_CapturedActors.store(working.Actors.size(), std::memory_order_release);
					continue;
				}
				m_State.store(WorldSnapshotCaptureState::Sealing, std::memory_order_release);
				break;
			}
			if (state == WorldSnapshotCaptureState::Validating)
			{
				const std::size_t validationCount =
					1 + working.Levels.size() + working.Actors.size();
				if (working.ValidationIndex < validationCount)
				{
					const std::size_t index = working.ValidationIndex;
					++consumed;
					if (!ValidateRecord(working, index))
					{
						std::int32_t objectIndex = working.World.Handle.Index;
						if (index > 0 && index - 1 < working.Levels.size())
							objectIndex = working.Levels[index - 1].Object.Handle.Index;
						else if (index > working.Levels.size())
							objectIndex = working.Actors[index - 1 - working.Levels.size()].Object.Handle.Index;
						Fail(WorldSnapshotCaptureError::SourceValidationFailed, objectIndex);
						break;
					}
					++working.ValidationIndex;
					m_ValidationIndex.store(working.ValidationIndex, std::memory_order_release);
					continue;
				}
				m_State.store(WorldSnapshotCaptureState::Publishing, std::memory_order_release);
				break;
			}
		}
	}
	catch (const std::bad_alloc&)
	{
		Fail(WorldSnapshotCaptureError::AllocationFailed);
	}
	catch (...)
	{
		Fail(WorldSnapshotCaptureError::UnexpectedException);
	}
	const bool pending = NeedsGameThreadPump(m_State.load(std::memory_order_acquire));
	return {.WorkConsumed = consumed, .MoreWorkPending = pending};
}

WorldSnapshotCaptureDiagnostics WorldSnapshotCapture::Diagnostics() const noexcept
{
	return {
		.State = m_State.load(std::memory_order_acquire),
		.Error = m_Error.load(std::memory_order_acquire),
		.PublishError = m_PublishError.load(std::memory_order_acquire),
		.RequestedGeneration = m_RequestedGeneration.load(std::memory_order_acquire),
		.ObjectSnapshotGeneration = m_ObjectSnapshotGeneration.load(std::memory_order_acquire),
		.TypeSnapshotGeneration = m_TypeSnapshotGeneration.load(std::memory_order_acquire),
		.CandidateCount = m_CandidateCount.load(std::memory_order_acquire),
		.NextCandidate = m_NextCandidate.load(std::memory_order_acquire),
		.CapturedLevels = m_CapturedLevels.load(std::memory_order_acquire),
		.CapturedActors = m_CapturedActors.load(std::memory_order_acquire),
		.ValidationIndex = m_ValidationIndex.load(std::memory_order_acquire),
		.ErrorObjectIndex = m_ErrorObjectIndex.load(std::memory_order_acquire)
	};
}

bool WorldSnapshotCapture::StopAndDrain(const std::chrono::milliseconds timeout)
{
	std::lock_guard lock(m_RequestMutex);
	if (m_State.load(std::memory_order_acquire) == WorldSnapshotCaptureState::Stopped)
		return true;
	m_StopRequested.store(true, std::memory_order_release);
	m_State.store(WorldSnapshotCaptureState::Stopping, std::memory_order_release);
	m_PumpBarrier.BeginStopping();
	if (!m_PumpBarrier.WaitForDrain(timeout))
		return false;
	m_Working.reset();
	m_State.store(WorldSnapshotCaptureState::Stopped, std::memory_order_release);
	return true;
}

} // namespace UExplorer::Runtime
