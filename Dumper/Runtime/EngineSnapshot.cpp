#include "EngineSnapshot.h"

#include <atomic>
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
	case SnapshotPublishError::PublishFailed: return "SNAPSHOT_PUBLISH_FAILED";
	}
	return "SNAPSHOT_UNKNOWN_ERROR";
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

	std::int32_t previousIndex = -1;
	for (std::size_t recordIndex = 0; recordIndex < snapshot.Objects.size(); ++recordIndex)
	{
		const EngineSnapshotObject& record = snapshot.Objects[recordIndex];
		if (!IsValidHandleEnvelope(
			record.Handle,
			m_SessionId,
			m_ContextGeneration,
			snapshot.SourceObjectCount)
			|| !IsValidMetadata(record))
		{
			return {
				.Error = SnapshotPublishError::RecordInvalid,
				.RecordIndex = static_cast<std::int32_t>(recordIndex)
			};
		}
		if (record.Handle.Index <= previousIndex)
		{
			return {
				.Error = SnapshotPublishError::RecordsNotOrdered,
				.RecordIndex = static_cast<std::int32_t>(recordIndex)
			};
		}
		previousIndex = record.Handle.Index;
	}

	try
	{
		std::lock_guard<std::mutex> lock(m_PublishMutex);
		if (IsStopped())
			return {.Error = SnapshotPublishError::StoreStopped};
		const std::shared_ptr<const EngineSnapshot> current = Current();
		if (current && snapshot.Generation <= current->Generation)
			return {.Error = SnapshotPublishError::GenerationNotMonotonic};

		auto published = std::make_shared<const EngineSnapshot>(std::move(snapshot));
		m_Current.store(published, std::memory_order_release);
		return {.Snapshot = std::move(published)};
	}
	catch (...)
	{
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

void EngineSnapshotStore::Stop() noexcept
{
	std::lock_guard<std::mutex> lock(m_PublishMutex);
	m_Stopped.store(true, std::memory_order_release);
}

} // namespace UExplorer::Runtime
