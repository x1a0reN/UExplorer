#pragma once

#include "ObjectHandle.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace UExplorer::Runtime
{

enum class EngineObjectKind : std::uint8_t
{
	Object,
	Package,
	Class,
	Struct,
	Enum,
	Function
};

const char* ToString(EngineObjectKind kind) noexcept;

struct EngineSnapshotObject
{
	ObjectHandle Handle;
	std::string Name;
	std::string FullPath;
	std::string ClassPath;
	std::string PackagePath;
	EngineObjectKind Kind = EngineObjectKind::Object;
};

struct EngineSnapshot
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t Generation = 0;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::uint64_t CaptureDurationUs = 0;
	std::int32_t SourceObjectCount = 0;
	std::uint32_t SkippedSlots = 0;
	std::deque<EngineSnapshotObject> Objects;
};

enum class SnapshotPublishError : std::uint8_t
{
	None,
	StoreInvalid,
	StoreStopped,
	EnvelopeInvalid,
	GenerationNotMonotonic,
	SourceLimitExceeded,
	RecordInvalid,
	RecordsNotOrdered,
	RetirementBackpressure,
	PublishFailed
};

const char* ToString(SnapshotPublishError error) noexcept;

struct SnapshotPublishResult
{
	SnapshotPublishError Error = SnapshotPublishError::None;
	std::int32_t RecordIndex = -1;
	std::shared_ptr<const EngineSnapshot> Snapshot;

	bool Ok() const noexcept { return Error == SnapshotPublishError::None; }
};

class EngineSnapshotCapture;

class ValidatedEngineSnapshot final
{
public:
	ValidatedEngineSnapshot(const ValidatedEngineSnapshot&) = delete;
	ValidatedEngineSnapshot& operator=(const ValidatedEngineSnapshot&) = delete;
	ValidatedEngineSnapshot(ValidatedEngineSnapshot&&) noexcept = default;
	ValidatedEngineSnapshot& operator=(ValidatedEngineSnapshot&&) noexcept = default;

private:
	friend class EngineSnapshotCapture;
	friend class EngineSnapshotStore;

	explicit ValidatedEngineSnapshot(EngineSnapshot snapshot) noexcept
		: m_Snapshot(std::move(snapshot))
	{
	}

	EngineSnapshot m_Snapshot;
};

class EngineSnapshotStore final
{
public:
	static constexpr std::int32_t kMaxSourceObjectCount = 8'000'000;
	static constexpr std::size_t kMaxNameBytes = 1024;
	static constexpr std::size_t kMaxPathBytes = 4096;
	static constexpr std::size_t kMaxRetiredSnapshots = 8;
	static constexpr std::uint64_t kMaxProtocolGeneration = 9'007'199'254'740'991ULL;

	EngineSnapshotStore(std::string sessionId, std::uint64_t contextGeneration);
	EngineSnapshotStore(const EngineSnapshotStore&) = delete;
	EngineSnapshotStore& operator=(const EngineSnapshotStore&) = delete;

	bool IsConfigured() const noexcept;
	bool IsStopped() const noexcept { return m_Stopped.load(std::memory_order_acquire); }
	static SnapshotPublishResult ValidateRecordForPublication(
		const EngineSnapshotObject& record,
		const std::string& sessionId,
		std::uint64_t contextGeneration,
		std::int32_t sourceObjectCount,
		std::int32_t previousObjectIndex,
		std::int32_t recordIndex) noexcept;
	SnapshotPublishResult Publish(EngineSnapshot snapshot) noexcept;
	SnapshotPublishResult PublishValidated(ValidatedEngineSnapshot snapshot) noexcept;
	std::shared_ptr<const EngineSnapshot> Current() const noexcept;
	std::uint64_t CurrentGeneration() const noexcept;
	std::size_t ReclaimRetired() noexcept;
	std::size_t RetiredSnapshotCount() const noexcept;
	void Stop() noexcept;

private:
	SnapshotPublishResult ValidateEnvelope(const EngineSnapshot& snapshot) const noexcept;
	bool RetireRejectedSnapshot(EngineSnapshot snapshot) noexcept;
	bool RetireRejectedSnapshotLocked(EngineSnapshot snapshot) noexcept;
	SnapshotPublishResult PublishValidatedSnapshot(
		EngineSnapshot snapshot,
		bool deferPreviousSnapshot) noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	mutable std::mutex m_PublishMutex;
	std::atomic<std::shared_ptr<const EngineSnapshot>> m_Current;
	std::array<std::shared_ptr<const EngineSnapshot>, kMaxRetiredSnapshots> m_RetiredSnapshots;
	std::array<std::optional<EngineSnapshot>, kMaxRetiredSnapshots> m_RejectedSnapshots;
	std::size_t m_RetiredSnapshotCount = 0;
	std::size_t m_RejectedSnapshotCount = 0;
	std::atomic<bool> m_Stopped{false};
};

} // namespace UExplorer::Runtime
