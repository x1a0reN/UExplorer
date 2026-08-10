#include "EngineSnapshot.h"

#include <atomic>
#include <algorithm>
#include <limits>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

bool IsValidHandleEnvelope(
	const ObjectHandle& handle,
	const std::string& sessionId,
	const std::uint64_t contextGeneration,
	const std::int32_t sourceObjectCount) noexcept
{
	return handle.SessionId == sessionId
		&& handle.ContextGeneration == contextGeneration
		&& handle.Index >= 0
		&& handle.Index < sourceObjectCount
		&& handle.SerialNumber > 0
		&& handle.Address != 0
		&& handle.ClassFingerprint != 0;
}

bool IsValidMetadata(const EngineSnapshotObject& record) noexcept
{
	return !record.Name.empty()
		&& record.Name.size() <= EngineSnapshotStore::kMaxNameBytes
		&& !record.FullPath.empty()
		&& record.FullPath.size() <= EngineSnapshotStore::kMaxPathBytes
		&& !record.ClassPath.empty()
		&& record.ClassPath.size() <= EngineSnapshotStore::kMaxPathBytes
		&& !record.PackagePath.empty()
		&& record.PackagePath.size() <= EngineSnapshotStore::kMaxPathBytes;
}

} // namespace

const char* ToString(const EngineObjectKind kind) noexcept
{
	switch (kind)
	{
	case EngineObjectKind::Object: return "object";
	case EngineObjectKind::Package: return "package";
	case EngineObjectKind::Class: return "class";
	case EngineObjectKind::Struct: return "struct";
	case EngineObjectKind::Enum: return "enum";
	case EngineObjectKind::Function: return "function";
	}
	return "unknown";
}

const char* ToString(const SnapshotPublishError error) noexcept
{
	switch (error)
	{
	case SnapshotPublishError::None: return "NONE";
	case SnapshotPublishError::StoreInvalid: return "SNAPSHOT_STORE_INVALID";
	case SnapshotPublishError::StoreStopped: return "SNAPSHOT_STORE_STOPPED";
	case SnapshotPublishError::EnvelopeInvalid: return "SNAPSHOT_ENVELOPE_INVALID";
	case SnapshotPublishError::GenerationNotMonotonic: return "SNAPSHOT_GENERATION_NOT_MONOTONIC";
	case SnapshotPublishError::SourceLimitExceeded: return "SNAPSHOT_SOURCE_LIMIT_EXCEEDED";
	case SnapshotPublishError::RecordInvalid: return "SNAPSHOT_RECORD_INVALID";
	case SnapshotPublishError::RecordsNotOrdered: return "SNAPSHOT_RECORDS_NOT_ORDERED";
	case SnapshotPublishError::RetirementBackpressure: return "SNAPSHOT_RETIREMENT_BACKPRESSURE";
	case SnapshotPublishError::PublishFailed: return "SNAPSHOT_PUBLISH_FAILED";
	}
	return "SNAPSHOT_UNKNOWN_ERROR";
}

const EngineSnapshotObject* EngineSnapshot::FindByIndex(const std::int32_t index) const noexcept
{
	const auto found = std::lower_bound(
		Objects.begin(),
		Objects.end(),
		index,
		[](const EngineSnapshotObject& object, const std::int32_t candidate) {
			return object.Handle.Index < candidate;
		});
	return found != Objects.end() && found->Handle.Index == index ? &*found : nullptr;
}

const EngineSnapshotObject* EngineSnapshot::FindByAddress(
	const std::uintptr_t address) const noexcept
{
	const auto found = std::lower_bound(
		AddressIndex.begin(),
		AddressIndex.end(),
		address,
		[](const auto& entry, const std::uintptr_t candidate) {
			return entry.first < candidate;
		});
	if (found == AddressIndex.end() || found->first != address
		|| found->second >= Objects.size())
	{
		return nullptr;
	}
	const EngineSnapshotObject& object = Objects[found->second];
	return object.Handle.Address == address ? &object : nullptr;
}

EngineSnapshotStore::EngineSnapshotStore(
	std::string sessionId,
	const std::uint64_t contextGeneration)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration)
{
}

bool EngineSnapshotStore::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_ContextGeneration != 0
		&& m_ContextGeneration <= kMaxProtocolGeneration;
}

SnapshotPublishResult EngineSnapshotStore::Publish(EngineSnapshot snapshot) noexcept
{
	if (!IsConfigured())
		return {.Error = SnapshotPublishError::StoreInvalid};
	if (IsStopped())
		return {.Error = SnapshotPublishError::StoreStopped};
	const SnapshotPublishResult envelope = ValidateEnvelope(snapshot);
	if (!envelope.Ok())
		return envelope;

	std::int32_t previousIndex = -1;
	for (std::size_t recordIndex = 0; recordIndex < snapshot.Objects.size(); ++recordIndex)
	{
		const SnapshotPublishResult validated = ValidateRecordForPublication(
			snapshot.Objects[recordIndex],
			m_SessionId,
			m_ContextGeneration,
			snapshot.SourceObjectCount,
			previousIndex,
			static_cast<std::int32_t>(recordIndex));
		if (!validated.Ok())
			return validated;
		previousIndex = snapshot.Objects[recordIndex].Handle.Index;
	}
	return PublishValidatedSnapshot(std::move(snapshot), false);
}

SnapshotPublishResult EngineSnapshotStore::ValidateRecordForPublication(
	const EngineSnapshotObject& record,
	const std::string& sessionId,
	const std::uint64_t contextGeneration,
	const std::int32_t sourceObjectCount,
	const std::int32_t previousObjectIndex,
	const std::int32_t recordIndex) noexcept
{
	if (!IsValidHandleEnvelope(record.Handle, sessionId, contextGeneration, sourceObjectCount)
		|| !IsValidMetadata(record))
	{
		return {
			.Error = SnapshotPublishError::RecordInvalid,
			.RecordIndex = recordIndex
		};
	}
	if (record.Handle.Index <= previousObjectIndex)
	{
		return {
			.Error = SnapshotPublishError::RecordsNotOrdered,
			.RecordIndex = recordIndex
		};
	}
	return {};
}

SnapshotPublishResult EngineSnapshotStore::ValidateEnvelope(
	const EngineSnapshot& snapshot) const noexcept
{
	if (snapshot.SessionId != m_SessionId
		|| snapshot.ContextGeneration != m_ContextGeneration
		|| snapshot.Generation == 0
		|| snapshot.Generation > kMaxProtocolGeneration
		|| snapshot.CapturedAtMonotonicUs == 0
		|| snapshot.SourceObjectCount < 0)
	{
		return {.Error = SnapshotPublishError::EnvelopeInvalid};
	}
	if (snapshot.SourceObjectCount > kMaxSourceObjectCount
		|| snapshot.Objects.size() > static_cast<std::size_t>(snapshot.SourceObjectCount))
	{
		return {.Error = SnapshotPublishError::SourceLimitExceeded};
	}
	const std::size_t expectedSkipped = static_cast<std::size_t>(snapshot.SourceObjectCount)
		- snapshot.Objects.size();
	if (expectedSkipped > (std::numeric_limits<std::uint32_t>::max)()
		|| snapshot.SkippedSlots != static_cast<std::uint32_t>(expectedSkipped))
	{
		return {.Error = SnapshotPublishError::EnvelopeInvalid};
	}
	return {};
}

SnapshotPublishResult EngineSnapshotStore::PublishValidated(
	ValidatedEngineSnapshot snapshot) noexcept
{
	EngineSnapshot prepared = std::move(snapshot.m_Snapshot);
	if (!IsConfigured())
	{
		(void)RetireRejectedSnapshot(std::move(prepared));
		return {.Error = SnapshotPublishError::StoreInvalid};
	}
	if (IsStopped())
	{
		(void)RetireRejectedSnapshot(std::move(prepared));
		return {.Error = SnapshotPublishError::StoreStopped};
	}
	const SnapshotPublishResult envelope = ValidateEnvelope(prepared);
	if (!envelope.Ok())
	{
		(void)RetireRejectedSnapshot(std::move(prepared));
		return envelope;
	}
	return PublishValidatedSnapshot(std::move(prepared), true);
}

bool EngineSnapshotStore::RetireRejectedSnapshot(EngineSnapshot snapshot) noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_PublishMutex);
		return RetireRejectedSnapshotLocked(std::move(snapshot));
	}
	catch (...)
	{
		return false;
	}
}

bool EngineSnapshotStore::RetireRejectedSnapshotLocked(EngineSnapshot snapshot) noexcept
{
	try
	{
		if (m_RejectedSnapshotCount >= kMaxRetiredSnapshots)
			return false;
		m_RejectedSnapshots[m_RejectedSnapshotCount].emplace(std::move(snapshot));
		++m_RejectedSnapshotCount;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

SnapshotPublishResult EngineSnapshotStore::PublishValidatedSnapshot(
	EngineSnapshot snapshot,
	const bool deferPreviousSnapshot) noexcept
{
	try
	{
		snapshot.AddressIndex.clear();
		snapshot.AddressIndex.reserve(snapshot.Objects.size());
		for (std::size_t index = 0; index < snapshot.Objects.size(); ++index)
		{
			snapshot.AddressIndex.emplace_back(snapshot.Objects[index].Handle.Address, index);
		}
		std::sort(snapshot.AddressIndex.begin(), snapshot.AddressIndex.end());
		for (std::size_t index = 1; index < snapshot.AddressIndex.size(); ++index)
		{
			if (snapshot.AddressIndex[index - 1].first == snapshot.AddressIndex[index].first)
				return {.Error = SnapshotPublishError::RecordInvalid};
		}
		std::lock_guard<std::mutex> lock(m_PublishMutex);
		if (IsStopped())
		{
			if (deferPreviousSnapshot)
				(void)RetireRejectedSnapshotLocked(std::move(snapshot));
			return {.Error = SnapshotPublishError::StoreStopped};
		}
		const std::shared_ptr<const EngineSnapshot> current = Current();
		if (current && snapshot.Generation <= current->Generation)
		{
			if (deferPreviousSnapshot)
				(void)RetireRejectedSnapshotLocked(std::move(snapshot));
			return {.Error = SnapshotPublishError::GenerationNotMonotonic};
		}
		if (deferPreviousSnapshot
			&& current
			&& m_RetiredSnapshotCount >= kMaxRetiredSnapshots)
		{
			(void)RetireRejectedSnapshotLocked(std::move(snapshot));
			return {.Error = SnapshotPublishError::RetirementBackpressure};
		}

		auto published = std::make_shared<const EngineSnapshot>(std::move(snapshot));
		std::shared_ptr<const EngineSnapshot> replaced =
			m_Current.exchange(published, std::memory_order_acq_rel);
		if (deferPreviousSnapshot && replaced)
		{
			m_RetiredSnapshots[m_RetiredSnapshotCount] = std::move(replaced);
			++m_RetiredSnapshotCount;
		}
		return {.Snapshot = std::move(published)};
	}
	catch (...)
	{
		if (deferPreviousSnapshot)
			(void)RetireRejectedSnapshot(std::move(snapshot));
		return {.Error = SnapshotPublishError::PublishFailed};
	}
}

std::shared_ptr<const EngineSnapshot> EngineSnapshotStore::Current() const noexcept
{
	return m_Current.load(std::memory_order_acquire);
}

std::uint64_t EngineSnapshotStore::CurrentGeneration() const noexcept
{
	const std::shared_ptr<const EngineSnapshot> current = Current();
	return current ? current->Generation : 0;
}

std::size_t EngineSnapshotStore::ReclaimRetired() noexcept
{
	std::array<std::shared_ptr<const EngineSnapshot>, kMaxRetiredSnapshots> retired;
	std::array<std::optional<EngineSnapshot>, kMaxRetiredSnapshots> rejected;
	std::size_t retiredCount = 0;
	std::size_t rejectedCount = 0;
	{
		std::lock_guard<std::mutex> lock(m_PublishMutex);
		retiredCount = m_RetiredSnapshotCount;
		for (std::size_t index = 0; index < retiredCount; ++index)
			retired[index] = std::move(m_RetiredSnapshots[index]);
		rejectedCount = m_RejectedSnapshotCount;
		for (std::size_t index = 0; index < rejectedCount; ++index)
		{
			rejected[index].emplace(std::move(*m_RejectedSnapshots[index]));
			m_RejectedSnapshots[index].reset();
		}
		m_RetiredSnapshotCount = 0;
		m_RejectedSnapshotCount = 0;
	}
	return retiredCount + rejectedCount;
}

std::size_t EngineSnapshotStore::RetiredSnapshotCount() const noexcept
{
	std::lock_guard<std::mutex> lock(m_PublishMutex);
	return m_RetiredSnapshotCount + m_RejectedSnapshotCount;
}

void EngineSnapshotStore::Stop() noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_PublishMutex);
		m_Stopped.store(true, std::memory_order_release);
	}
	(void)ReclaimRetired();
}

} // namespace UExplorer::Runtime
