#pragma once

#include "CallbackBarrier.h"
#include "GameThreadExecutor.h"
#include "ObjectHandle.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace UExplorer::Runtime
{

using WatchId = std::uint64_t;

enum class WatchError : std::uint8_t
{
	None,
	InvalidConfiguration,
	Stopped,
	InvalidSpec,
	SessionMismatch,
	ContextGenerationMismatch,
	SnapshotGenerationInvalid,
	IntervalOutOfRange,
	Duplicate,
	CapacityExceeded,
	IdExhausted,
	NotFound,
	Terminal,
	InvalidLimit,
	AllocationFailed,
	BindingFailed,
	DrainTimedOut,
	ConcurrentPumpRejected
};

const char* ToString(WatchError error) noexcept;

enum class WatchSampleStatus : std::uint8_t
{
	Value,
	Unavailable,
	Failed,
	Stale
};

const char* ToString(WatchSampleStatus status) noexcept;

// The complete identity required to resolve one reflected property. No field
// may be substituted from a newer snapshot while this subscription is alive.
struct WatchSubscriptionSpec
{
	ObjectHandle Object;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::string DeclaringTypePath;
	std::string PropertyName;
	std::uint32_t ArrayIndex = 0;
	std::uint32_t IntervalMs = 0;
};

// CanonicalValue is the equality key and transport-neutral encoded value.
// DisplayValue is metadata only and is never used for change detection.
struct WatchSamplePayload
{
	std::string TypeName;
	std::vector<std::byte> CanonicalValue;
	std::string DisplayValue;
};

struct WatchSampleResult
{
	WatchSampleStatus Status = WatchSampleStatus::Failed;
	WatchSamplePayload Payload;
	std::string ReasonCode;
	std::string Reason;
};

// Opaque, source-owned immutable state acquired before a subscription is
// published. The scheduler only retains the token and returns it to the same
// source while the subscription record remains alive.
class IWatchSampleBinding
{
public:
	virtual ~IWatchSampleBinding() = default;
	const void* SourceIdentity() const noexcept { return m_SourceIdentity; }

protected:
	explicit IWatchSampleBinding(const void* sourceIdentity) noexcept
		: m_SourceIdentity(sourceIdentity)
	{
	}

private:
	const void* m_SourceIdentity = nullptr;
};

struct WatchBindingResult
{
	std::shared_ptr<const IWatchSampleBinding> Binding;
	std::string ReasonCode;
	std::string Reason;

	bool Ok() const noexcept { return Binding != nullptr; }
};

class IWatchSampleSource
{
public:
	virtual ~IWatchSampleSource() = default;
	virtual std::string_view SessionId() const noexcept = 0;
	virtual std::uint64_t ContextGeneration() const noexcept = 0;
	virtual bool IsCurrentExecutionThreadValid() const noexcept = 0;
	virtual WatchBindingResult Bind(const WatchSubscriptionSpec& spec) = 0;
	virtual WatchSampleResult Sample(
		const WatchSubscriptionSpec& spec,
		const std::shared_ptr<const IWatchSampleBinding>& binding,
		std::size_t maxValueBytes) = 0;
};

enum class WatchSubscriptionState : std::uint8_t
{
	Enabled,
	Disabled,
	Terminal
};

const char* ToString(WatchSubscriptionState state) noexcept;

struct WatchSubscription
{
	WatchId Id = 0;
	WatchSubscriptionSpec Spec;
	WatchSubscriptionState State = WatchSubscriptionState::Disabled;
	std::uint64_t CreatedAtMonotonicUs = 0;
	std::uint64_t NextDueMonotonicUs = 0;
	std::uint64_t LastSampledAtMonotonicUs = 0;
	std::uint64_t LastChangeSequence = 0;
	std::uint64_t SampleCount = 0;
	std::uint64_t FailureCount = 0;
	std::size_t HistoryCount = 0;
	std::size_t HistoryBytes = 0;
	std::uint64_t HistoryDropCount = 0;
	std::string TerminalReasonCode;
	std::string TerminalReason;
};

struct WatchHistoryEntry
{
	std::uint64_t Sequence = 0;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::shared_ptr<const WatchSamplePayload> Value;
};

enum class WatchEventKind : std::uint8_t
{
	ValueChanged,
	SampleUnavailable,
	SampleFailed,
	TerminalStale
};

const char* ToString(WatchEventKind kind) noexcept;

struct WatchEvent
{
	std::uint64_t Sequence = 0;
	WatchId Id = 0;
	WatchEventKind Kind = WatchEventKind::SampleFailed;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::shared_ptr<const WatchSamplePayload> Value;
	std::string ReasonCode;
	std::string Reason;
	std::uint64_t DropCount = 0;
	std::uint64_t CoalesceCount = 0;
	std::uint64_t PushDroppedBefore = 0;
	std::uint64_t PushCoalescedBefore = 0;
};

struct WatchSchedulerLimits
{
	std::size_t MaxSubscriptions = 256;
	std::size_t MaxHistoryEntriesPerWatch = 128;
	std::size_t MaxHistoryBytesPerWatch = 256 * 1024;
	std::size_t MaxPendingEvents = 1024;
	std::size_t MaxPendingEventBytes = 4 * 1024 * 1024;
	std::size_t MaxSampleBytes = 64 * 1024;
	std::size_t MaxFrameItems = 32;
	std::size_t MaxFrameBytes = 256 * 1024;
	std::uint64_t MaxFrameTimeUs = 1'000;
	std::uint32_t MinIntervalMs = 16;
	std::uint32_t MaxIntervalMs = 3'600'000;
};

struct WatchAddResult
{
	WatchError Error = WatchError::None;
	WatchId Id = 0;
	std::string ReasonCode;
	std::string Reason;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

struct WatchMutationResult
{
	WatchError Error = WatchError::None;
	WatchId Id = 0;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

struct WatchListResult
{
	WatchError Error = WatchError::None;
	std::uint64_t EnabledSnapshotGeneration = 0;
	std::vector<WatchSubscription> Subscriptions;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

struct WatchSnapshotResult
{
	WatchError Error = WatchError::None;
	WatchSubscription Subscription;
	std::shared_ptr<const WatchSamplePayload> LastValue;
	std::vector<WatchHistoryEntry> History;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

struct WatchDrainResult
{
	WatchError Error = WatchError::None;
	std::vector<WatchEvent> Events;
	std::uint64_t DroppedTotal = 0;
	std::uint64_t CoalescedTotal = 0;
	bool MoreAvailable = false;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

// The transport queue is independent from the command-side pull queue; a
// publisher cannot consume events intended for watch.events.drain callers.
struct WatchPushDrainResult
{
	WatchError Error = WatchError::None;
	std::vector<WatchEvent> Events;
	std::uint64_t DroppedTotal = 0;
	std::uint64_t CoalescedTotal = 0;
	bool MoreAvailable = false;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

struct WatchSchedulerSnapshot
{
	bool Configured = false;
	bool Stopping = false;
	bool Stopped = false;
	bool Pumping = false;
	std::uint64_t EnabledSnapshotGeneration = 0;
	std::size_t SubscriptionCount = 0;
	std::size_t EnabledCount = 0;
	std::size_t PendingEventCount = 0;
	std::size_t PendingEventBytes = 0;
	std::uint64_t DroppedEvents = 0;
	std::uint64_t CoalescedEvents = 0;
	std::size_t PendingPushEventCount = 0;
	std::size_t PendingPushEventBytes = 0;
	std::uint64_t DroppedPushEvents = 0;
	std::uint64_t CoalescedPushEvents = 0;
	std::uint64_t LastEventSequence = 0;
	std::uint64_t PumpCount = 0;
	std::uint64_t LastPumpDurationUs = 0;
	std::size_t LastPumpItems = 0;
	std::size_t LastPumpBytes = 0;
	std::uint64_t ItemBudgetExhaustions = 0;
	std::uint64_t ByteBudgetExhaustions = 0;
	std::uint64_t TimeBudgetExhaustions = 0;
	std::uint64_t ConcurrentPumpRejections = 0;
};

struct WatchStopResult
{
	WatchError Error = WatchError::None;

	bool Ok() const noexcept { return Error == WatchError::None; }
};

class WatchScheduler final : public IGameThreadFrameClient
{
public:
	static constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
	static constexpr std::size_t kHardMaxSubscriptions = 1024;
	static constexpr std::size_t kHardMaxHistoryEntries = 1024;
	static constexpr std::size_t kHardMaxPendingEvents = 4096;
	static constexpr std::size_t kHardMaxSampleBytes = 1024 * 1024;
	static constexpr std::size_t kHardMaxFrameItems = 128;
	static constexpr std::uint64_t kHardMaxFrameTimeUs = 2'000;

	WatchScheduler(
		std::string sessionId,
		std::uint64_t contextGeneration,
		IWatchSampleSource& source,
		WatchSchedulerLimits limits = {});
	WatchScheduler(const WatchScheduler&) = delete;
	WatchScheduler& operator=(const WatchScheduler&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	const std::string& SessionId() const noexcept { return m_SessionId; }
	std::uint64_t ContextGeneration() const noexcept { return m_ContextGeneration; }
	WatchAddResult Add(WatchSubscriptionSpec spec, bool enabled = true) noexcept;
	WatchListResult List() const noexcept;
	WatchMutationResult Enable(WatchId id, bool enabled) noexcept;
	WatchMutationResult Remove(WatchId id) noexcept;
	WatchSnapshotResult Snapshot(WatchId id) const noexcept;
	WatchDrainResult DrainEvents(std::size_t maxEvents) noexcept;
	WatchPushDrainResult DrainPushEvents(std::size_t maxEvents) noexcept;
	WatchSchedulerSnapshot Snapshot() const noexcept;
	IGameThreadFrameClient::PumpResult PumpFrame(std::size_t workBudget) noexcept override;
	WatchStopResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	struct SubscriptionRecord;
	using EnabledSnapshot = std::vector<std::shared_ptr<SubscriptionRecord>>;

	WatchError ValidateSpec(const WatchSubscriptionSpec& spec) const noexcept;
	std::shared_ptr<SubscriptionRecord> FindLocked(WatchId id) const noexcept;
	std::shared_ptr<const EnabledSnapshot> BuildEnabledSnapshotLocked(
		const SubscriptionRecord* overrideRecord = nullptr,
		bool overrideEnabled = false,
		const SubscriptionRecord* excludedRecord = nullptr) const;
	void PublishEnabledSnapshotLocked(std::shared_ptr<const EnabledSnapshot> snapshot) noexcept;
	WatchSubscription CopySubscriptionLocked(const SubscriptionRecord& record) const;
	bool ApplySampleLocked(
		const std::shared_ptr<SubscriptionRecord>& record,
		WatchSampleResult result,
		std::uint64_t capturedAt,
		std::size_t& bytesConsumed);
	void AppendHistoryLocked(
		SubscriptionRecord& record,
		std::uint64_t sequence,
		std::uint64_t capturedAt,
		const std::shared_ptr<const WatchSamplePayload>& value,
		std::size_t payloadBytes);
	void AppendEventLocked(WatchEvent event, std::size_t eventBytes, bool coalescible);
	void AppendPushEventLocked(
		WatchEvent event,
		std::size_t eventBytes,
		bool coalescible);
	std::uint64_t NextEventSequenceLocked() noexcept;
	bool HasDueWork(
		const std::shared_ptr<const EnabledSnapshot>& snapshot,
		std::uint64_t now) const noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	IWatchSampleSource& m_Source;
	WatchSchedulerLimits m_Limits;
	bool m_Configured = false;
	std::shared_ptr<const EnabledSnapshot> m_EmptyEnabledSnapshot;
	std::atomic<std::shared_ptr<const EnabledSnapshot>> m_EnabledSnapshot;
	CallbackBarrier m_PumpBarrier;
	mutable std::mutex m_Mutex;
	std::vector<std::shared_ptr<SubscriptionRecord>> m_Subscriptions;
	std::deque<WatchEvent> m_Events;
	std::size_t m_PendingEventBytes = 0;
	std::deque<WatchEvent> m_PushEvents;
	std::size_t m_PendingPushEventBytes = 0;
	WatchId m_NextId = 1;
	std::uint64_t m_NextEventSequence = 1;
	std::uint64_t m_EnabledSnapshotGeneration = 0;
	std::uint64_t m_DroppedEvents = 0;
	std::uint64_t m_CoalescedEvents = 0;
	std::uint64_t m_DroppedPushEvents = 0;
	std::uint64_t m_CoalescedPushEvents = 0;
	std::atomic<bool> m_StopRequested{false};
	std::atomic<bool> m_Stopped{false};
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic<bool> m_Pumping{false};
	std::atomic<std::size_t> m_NextPumpIndex{0};
	std::atomic<std::uint64_t> m_PumpCount{0};
	std::atomic<std::uint64_t> m_LastPumpDurationUs{0};
	std::atomic<std::size_t> m_LastPumpItems{0};
	std::atomic<std::size_t> m_LastPumpBytes{0};
	std::atomic<std::uint64_t> m_ItemBudgetExhaustions{0};
	std::atomic<std::uint64_t> m_ByteBudgetExhaustions{0};
	std::atomic<std::uint64_t> m_TimeBudgetExhaustions{0};
	std::atomic<std::uint64_t> m_ConcurrentPumpRejections{0};
};

} // namespace UExplorer::Runtime
