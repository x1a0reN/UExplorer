#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace UExplorer::Runtime
{

enum class HookEventKind : std::uint8_t
{
	PostRender = 0,
	ProcessEventEnter,
	ProcessEventExit,
	Diagnostic,
	Count
};

constexpr std::uint32_t HookEventKindBit(const HookEventKind kind) noexcept
{
	return kind < HookEventKind::Count
		? (std::uint32_t{1} << static_cast<std::uint8_t>(kind))
		: 0;
}

constexpr std::uint32_t kAllHookEventKinds =
	HookEventKindBit(HookEventKind::PostRender)
	| HookEventKindBit(HookEventKind::ProcessEventEnter)
	| HookEventKindBit(HookEventKind::ProcessEventExit)
	| HookEventKindBit(HookEventKind::Diagnostic);

const char* ToString(HookEventKind kind) noexcept;

enum class HookCollectorLifecycle : std::uint8_t
{
	Running,
	Stopping,
	Stopped
};

const char* ToString(HookCollectorLifecycle lifecycle) noexcept;

enum class HookPublishStatus : std::uint8_t
{
	Published,
	CoalescedOnOverflow,
	Disabled,
	KindFiltered,
	InvalidCollector,
	InvalidKind,
	PayloadTooLarge,
	ConfigurationChanging,
	QueueFull,
	Contended,
	SequenceExhausted,
	CollectorStopping,
	CollectorStopped
};

const char* ToString(HookPublishStatus status) noexcept;

enum class HookCollectorError : std::uint8_t
{
	None,
	InvalidSession,
	InvalidContextGeneration,
	InvalidConfiguration,
	InvalidLimit,
	AllocationFailed,
	CollectorStopping,
	CollectorStopped,
	ConcurrentConfigurationPublish,
	ConfigurationGenerationExhausted,
	ConcurrentDrain,
	ConcurrentStop,
	DrainTimedOut
};

const char* ToString(HookCollectorError error) noexcept;

struct HookCollectorLimits
{
	std::size_t Capacity = 1024;
	std::size_t MaxPayloadBytes = 256;
};

// This value is packed into one immutable publication snapshot. It deliberately
// contains no pointer-owning fields that could be reclaimed under a producer.
struct HookCollectorConfig
{
	bool Enabled = false;
	std::uint32_t KindMask = kAllHookEventKinds;
	std::size_t MaxPayloadBytes = 256;
	bool CoalesceOnOverflow = true;
};

// Hook code must prepare this view before calling TryPublish. The collector
// never dereferences Source/Subject and treats Payload as already encoded bytes.
struct HookEventView
{
	HookEventKind Kind = HookEventKind::Diagnostic;
	std::uint64_t Source = 0;
	std::uint64_t Subject = 0;
	std::uint64_t Correlation = 0;
	std::span<const std::byte> Payload;
	bool Coalescible = false;
};

struct HookEvent
{
	std::array<char, 129> SessionId{};
	std::size_t SessionIdSize = 0;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t Sequence = 0;
	std::uint64_t ConfigurationGeneration = 0;
	HookEventKind Kind = HookEventKind::Diagnostic;
	std::uint64_t Source = 0;
	std::uint64_t Subject = 0;
	std::uint64_t Correlation = 0;
	std::uint64_t CoalescedBefore = 0;
	std::size_t PayloadSize = 0;
	std::array<std::byte, 512> Payload{};
};

struct HookPublishResult
{
	HookPublishStatus Status = HookPublishStatus::InvalidCollector;
	HookCollectorError CollectorError = HookCollectorError::None;
	std::uint64_t Sequence = 0;
	std::uint64_t ConfigurationGeneration = 0;

	bool Published() const noexcept { return Status == HookPublishStatus::Published; }
};

struct HookConfigPublishResult
{
	HookCollectorError Error = HookCollectorError::None;
	std::uint64_t ConfigurationGeneration = 0;

	bool Ok() const noexcept { return Error == HookCollectorError::None; }
};

struct HookDrainResult
{
	HookCollectorError Error = HookCollectorError::None;
	std::size_t Count = 0;
	bool MoreAvailable = false;
	std::uint64_t PublishedTotal = 0;
	std::uint64_t DrainedTotal = 0;
	std::uint64_t DroppedOverflowTotal = 0;
	std::uint64_t DroppedOversizeTotal = 0;
	std::uint64_t DroppedContentionTotal = 0;
	std::uint64_t CoalescedOverflowTotal = 0;

	bool Ok() const noexcept { return Error == HookCollectorError::None; }
};

struct HookCollectorSnapshot
{
	bool Configured = false;
	HookCollectorError InitializationError = HookCollectorError::InvalidConfiguration;
	HookCollectorLifecycle Lifecycle = HookCollectorLifecycle::Stopped;
	std::array<char, 129> SessionId{};
	std::size_t SessionIdSize = 0;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ConfigurationGeneration = 0;
	bool ConfigurationSnapshotStable = false;
	HookCollectorConfig Configuration;
	std::size_t Capacity = 0;
	std::size_t ReservedEventCount = 0;
	std::uint64_t InFlightProducers = 0;
	std::uint64_t NextSequence = 0;
	std::uint64_t PublishedTotal = 0;
	std::uint64_t DrainedTotal = 0;
	std::uint64_t DisabledTotal = 0;
	std::uint64_t KindFilteredTotal = 0;
	std::uint64_t DroppedOverflowTotal = 0;
	std::uint64_t DroppedOversizeTotal = 0;
	std::uint64_t DroppedContentionTotal = 0;
	std::uint64_t CoalescedOverflowTotal = 0;
	std::uint64_t ConfigurationRaceTotal = 0;
	std::uint64_t StoppingRejectionTotal = 0;
	std::uint64_t StoppedRejectionTotal = 0;
	std::array<std::uint64_t, static_cast<std::size_t>(HookEventKind::Count)>
		PendingCoalescedByKind{};
};

struct HookStopResult
{
	HookCollectorError Error = HookCollectorError::None;
	std::size_t PendingEventCount = 0;

	bool Ok() const noexcept { return Error == HookCollectorError::None; }
};

// A bounded MPSC collector for hook callbacks. TryPublish has a fixed number of
// lock-free admission/reservation attempts and performs only atomics plus one
// bounded memcpy. The owner must unpublish the collector pointer or restore the
// hook before StopAndDrain; racing calls that already hold the pointer receive a
// unique stopping/stopped result and are included in the producer drain. Before
// destroying the collector, the hook owner's callback barrier must also prove
// that no callback still retains its pointer. Queued events remain available to
// Drain after the collector is stopped.
class HookEventCollector final
{
public:
	static constexpr std::size_t kMaxSessionIdBytes = 128;
	static constexpr std::size_t kHardMaxPayloadBytes = 512;
	static constexpr std::size_t kMinCapacity = 2;
	static constexpr std::size_t kHardMaxCapacity = 16 * 1024;
	static constexpr std::size_t kHardMaxDrainBatch = 4096;
	static constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;

	HookEventCollector(
		std::string_view sessionId,
		std::uint64_t contextGeneration,
		HookCollectorLimits limits = {},
		HookCollectorConfig initialConfiguration = {}) noexcept;
	~HookEventCollector();

	HookEventCollector(const HookEventCollector&) = delete;
	HookEventCollector& operator=(const HookEventCollector&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	HookCollectorError InitializationError() const noexcept { return m_InitializationError; }
	HookConfigPublishResult PublishConfiguration(HookCollectorConfig configuration) noexcept;
	HookPublishResult TryPublish(const HookEventView& event) noexcept;
	HookDrainResult Drain(std::span<HookEvent> output) noexcept;
	HookCollectorSnapshot Snapshot() const noexcept;
	HookStopResult StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) noexcept;

private:
	struct Slot;
	struct PackedConfiguration;
	enum class Admission : std::uint8_t;
	enum class Reservation : std::uint8_t;

	class OperationLease final
	{
	public:
		OperationLease() = default;
		OperationLease(const OperationLease&) = delete;
		OperationLease& operator=(const OperationLease&) = delete;
		OperationLease(OperationLease&& other) noexcept;
		OperationLease& operator=(OperationLease&& other) noexcept;
		~OperationLease();

		explicit operator bool() const noexcept { return m_Owner != nullptr; }

	private:
		friend class HookEventCollector;
		explicit OperationLease(HookEventCollector* owner) noexcept : m_Owner(owner) {}
		void Release() noexcept;
		HookEventCollector* m_Owner = nullptr;
	};

	static bool IsPowerOfTwo(std::size_t value) noexcept;
	static bool IsKnownKind(HookEventKind kind) noexcept;
	static bool IsValidConfiguration(
		const HookCollectorConfig& configuration,
		const HookCollectorLimits& limits) noexcept;
	static std::uint64_t PackConfiguration(const HookCollectorConfig& configuration) noexcept;
	static HookCollectorConfig UnpackConfiguration(std::uint64_t word) noexcept;

	Admission TryEnter(OperationLease& lease) noexcept;
	void ExitOperation() noexcept;
	bool ReadConfiguration(PackedConfiguration& output) const noexcept;
	Reservation Reserve(std::uint64_t& position, Slot*& slot) noexcept;
	bool IsNextEventAvailable() const noexcept;
	std::size_t ReservedEventCount() const noexcept;
	HookCollectorLifecycle Lifecycle() const noexcept;

	static constexpr std::uint64_t kStoppingBit = std::uint64_t{1} << 63;
	static constexpr std::uint64_t kStoppedBit = std::uint64_t{1} << 62;
	static constexpr std::uint64_t kInFlightMask = kStoppedBit - 1;
	static constexpr std::size_t kMaxReservationAttempts = 8;

	std::array<char, kMaxSessionIdBytes + 1> m_SessionId{};
	std::size_t m_SessionIdSize = 0;
	std::uint64_t m_ContextGeneration = 0;
	HookCollectorLimits m_Limits;
	bool m_Configured = false;
	HookCollectorError m_InitializationError = HookCollectorError::InvalidConfiguration;
	std::unique_ptr<Slot[]> m_Slots;
	std::size_t m_CapacityMask = 0;
	std::atomic<std::uint64_t> m_EnqueuePosition{0};
	std::atomic<std::uint64_t> m_DequeuePosition{0};
	std::atomic<std::uint64_t> m_ProducerState{kStoppingBit | kStoppedBit};
	std::atomic<std::uint64_t> m_ActivitySequence{0};
	std::atomic<std::uint64_t> m_ConfigurationSequence{0};
	std::atomic<std::uint64_t> m_ConfigurationWord{0};
	std::atomic_flag m_ConfigurationPublishOwned = ATOMIC_FLAG_INIT;
	std::atomic_flag m_DrainOwned = ATOMIC_FLAG_INIT;
	std::atomic_flag m_StopOwned = ATOMIC_FLAG_INIT;
	std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(HookEventKind::Count)>
		m_PendingCoalescedByKind{};
	std::atomic<std::uint64_t> m_PublishedTotal{0};
	std::atomic<std::uint64_t> m_DrainedTotal{0};
	std::atomic<std::uint64_t> m_DisabledTotal{0};
	std::atomic<std::uint64_t> m_KindFilteredTotal{0};
	std::atomic<std::uint64_t> m_DroppedOverflowTotal{0};
	std::atomic<std::uint64_t> m_DroppedOversizeTotal{0};
	std::atomic<std::uint64_t> m_DroppedContentionTotal{0};
	std::atomic<std::uint64_t> m_CoalescedOverflowTotal{0};
	std::atomic<std::uint64_t> m_ConfigurationRaceTotal{0};
	std::atomic<std::uint64_t> m_StoppingRejectionTotal{0};
	std::atomic<std::uint64_t> m_StoppedRejectionTotal{0};
};

} // namespace UExplorer::Runtime
