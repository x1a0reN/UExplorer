#include "HookEventCollector.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::uint64_t kEnabledBit = std::uint64_t{1} << 0;
constexpr std::uint64_t kCoalesceOnOverflowBit = std::uint64_t{1} << 1;
constexpr std::uint64_t kPayloadShift = 2;
constexpr std::uint64_t kPayloadMask = 0x3FF;
constexpr std::uint64_t kKindMaskShift = 16;
constexpr std::uint64_t kKindMaskMask = 0xFFFF'FFFFULL;
constexpr auto kStopQuietPeriod = std::chrono::milliseconds(1);
constexpr auto kMaxStopTimeout = std::chrono::milliseconds(60'000);

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
	"HookEventCollector requires lock-free 64-bit atomics on the hook path");

} // namespace

struct HookEventCollector::Slot
{
	struct StoredEvent
	{
		std::uint64_t ContextGeneration = 0;
		std::uint64_t Sequence = 0;
		std::uint64_t ConfigurationGeneration = 0;
		HookEventKind Kind = HookEventKind::Diagnostic;
		std::uint64_t Source = 0;
		std::uint64_t Subject = 0;
		std::uint64_t Correlation = 0;
		std::uint64_t CoalescedBefore = 0;
		std::size_t PayloadSize = 0;
		std::array<std::byte, kHardMaxPayloadBytes> Payload{};
	};

	std::atomic<std::uint64_t> Sequence{0};
	StoredEvent Event;
};

struct HookEventCollector::PackedConfiguration
{
	HookCollectorConfig Value;
	std::uint64_t Generation = 0;
};

enum class HookEventCollector::Admission : std::uint8_t
{
	Entered,
	Stopping,
	Stopped
};

enum class HookEventCollector::Reservation : std::uint8_t
{
	Reserved,
	Full,
	Contended,
	SequenceExhausted
};

const char* ToString(const HookEventKind kind) noexcept
{
	switch (kind)
	{
	case HookEventKind::PostRender: return "post_render";
	case HookEventKind::ProcessEventEnter: return "process_event_enter";
	case HookEventKind::ProcessEventExit: return "process_event_exit";
	case HookEventKind::Diagnostic: return "diagnostic";
	case HookEventKind::Count: break;
	}
	return "unknown";
}

const char* ToString(const HookCollectorLifecycle lifecycle) noexcept
{
	switch (lifecycle)
	{
	case HookCollectorLifecycle::Running: return "running";
	case HookCollectorLifecycle::Stopping: return "stopping";
	case HookCollectorLifecycle::Stopped: return "stopped";
	}
	return "unknown";
}

const char* ToString(const HookPublishStatus status) noexcept
{
	switch (status)
	{
	case HookPublishStatus::Published: return "HOOK_EVENT_PUBLISHED";
	case HookPublishStatus::CoalescedOnOverflow: return "HOOK_EVENT_COALESCED_ON_OVERFLOW";
	case HookPublishStatus::Disabled: return "HOOK_COLLECTOR_DISABLED";
	case HookPublishStatus::KindFiltered: return "HOOK_EVENT_KIND_FILTERED";
	case HookPublishStatus::InvalidCollector: return "HOOK_COLLECTOR_INVALID";
	case HookPublishStatus::InvalidKind: return "HOOK_EVENT_KIND_INVALID";
	case HookPublishStatus::PayloadTooLarge: return "HOOK_EVENT_PAYLOAD_TOO_LARGE";
	case HookPublishStatus::ConfigurationChanging: return "HOOK_CONFIGURATION_CHANGING";
	case HookPublishStatus::QueueFull: return "HOOK_EVENT_QUEUE_FULL";
	case HookPublishStatus::Contended: return "HOOK_EVENT_PUBLISH_CONTENDED";
	case HookPublishStatus::SequenceExhausted: return "HOOK_EVENT_SEQUENCE_EXHAUSTED";
	case HookPublishStatus::CollectorStopping: return "HOOK_COLLECTOR_STOPPING";
	case HookPublishStatus::CollectorStopped: return "HOOK_COLLECTOR_STOPPED";
	}
	return "HOOK_EVENT_UNKNOWN_STATUS";
}

const char* ToString(const HookCollectorError error) noexcept
{
	switch (error)
	{
	case HookCollectorError::None: return "NONE";
	case HookCollectorError::InvalidSession: return "HOOK_SESSION_INVALID";
	case HookCollectorError::InvalidContextGeneration:
		return "HOOK_CONTEXT_GENERATION_INVALID";
	case HookCollectorError::InvalidConfiguration: return "HOOK_CONFIGURATION_INVALID";
	case HookCollectorError::InvalidLimit: return "HOOK_LIMIT_INVALID";
	case HookCollectorError::AllocationFailed: return "HOOK_ALLOCATION_FAILED";
	case HookCollectorError::CollectorStopping: return "HOOK_COLLECTOR_STOPPING";
	case HookCollectorError::CollectorStopped: return "HOOK_COLLECTOR_STOPPED";
	case HookCollectorError::ConcurrentConfigurationPublish:
		return "HOOK_CONFIGURATION_PUBLISH_CONCURRENT";
	case HookCollectorError::ConfigurationGenerationExhausted:
		return "HOOK_CONFIGURATION_GENERATION_EXHAUSTED";
	case HookCollectorError::ConcurrentDrain: return "HOOK_DRAIN_CONCURRENT";
	case HookCollectorError::ConcurrentStop: return "HOOK_STOP_CONCURRENT";
	case HookCollectorError::DrainTimedOut: return "HOOK_PRODUCER_DRAIN_TIMED_OUT";
	}
	return "HOOK_COLLECTOR_UNKNOWN_ERROR";
}

HookEventCollector::OperationLease::OperationLease(OperationLease&& other) noexcept
	: m_Owner(std::exchange(other.m_Owner, nullptr))
{
}

HookEventCollector::OperationLease& HookEventCollector::OperationLease::operator=(
	OperationLease&& other) noexcept
{
	if (this == &other)
		return *this;
	Release();
	m_Owner = std::exchange(other.m_Owner, nullptr);
	return *this;
}

HookEventCollector::OperationLease::~OperationLease()
{
	Release();
}

void HookEventCollector::OperationLease::Release() noexcept
{
	if (HookEventCollector* owner = std::exchange(m_Owner, nullptr))
		owner->ExitOperation();
}

HookEventCollector::HookEventCollector(
	const std::string_view sessionId,
	const std::uint64_t contextGeneration,
	const HookCollectorLimits limits,
	const HookCollectorConfig initialConfiguration) noexcept
	: m_ContextGeneration(contextGeneration),
	  m_Limits(limits)
{
	const bool sessionValid = !sessionId.empty()
		&& sessionId.size() <= kMaxSessionIdBytes
		&& sessionId.find('\0') == std::string_view::npos;
	const bool limitsValid = IsPowerOfTwo(limits.Capacity)
		&& limits.Capacity >= kMinCapacity
		&& limits.Capacity <= kHardMaxCapacity
		&& limits.MaxPayloadBytes > 0
		&& limits.MaxPayloadBytes <= kHardMaxPayloadBytes;
	if (!sessionValid)
	{
		m_InitializationError = HookCollectorError::InvalidSession;
		return;
	}
	if (contextGeneration == 0 || contextGeneration > kMaxProtocolInteger)
	{
		m_InitializationError = HookCollectorError::InvalidContextGeneration;
		return;
	}
	if (!limitsValid)
	{
		m_InitializationError = HookCollectorError::InvalidLimit;
		return;
	}
	if (!IsValidConfiguration(initialConfiguration, limits))
	{
		m_InitializationError = HookCollectorError::InvalidConfiguration;
		return;
	}

	m_Slots.reset(new (std::nothrow) Slot[limits.Capacity]);
	if (!m_Slots)
	{
		m_InitializationError = HookCollectorError::AllocationFailed;
		return;
	}

	std::memcpy(m_SessionId.data(), sessionId.data(), sessionId.size());
	m_SessionIdSize = sessionId.size();
	m_CapacityMask = limits.Capacity - 1;
	for (std::size_t index = 0; index < limits.Capacity; ++index)
		m_Slots[index].Sequence.store(static_cast<std::uint64_t>(index), std::memory_order_relaxed);

	m_ConfigurationWord.store(PackConfiguration(initialConfiguration), std::memory_order_relaxed);
	m_ConfigurationSequence.store(2, std::memory_order_release);
	m_ProducerState.store(0, std::memory_order_release);
	m_InitializationError = HookCollectorError::None;
	m_Configured = true;
}

HookEventCollector::~HookEventCollector() = default;

bool HookEventCollector::IsPowerOfTwo(const std::size_t value) noexcept
{
	return value != 0 && (value & (value - 1)) == 0;
}

bool HookEventCollector::IsKnownKind(const HookEventKind kind) noexcept
{
	return kind < HookEventKind::Count;
}

bool HookEventCollector::IsValidConfiguration(
	const HookCollectorConfig& configuration,
	const HookCollectorLimits& limits) noexcept
{
	return (configuration.KindMask & ~kAllHookEventKinds) == 0
		&& configuration.MaxPayloadBytes > 0
		&& configuration.MaxPayloadBytes <= limits.MaxPayloadBytes
		&& configuration.MaxPayloadBytes <= kHardMaxPayloadBytes;
}

std::uint64_t HookEventCollector::PackConfiguration(
	const HookCollectorConfig& configuration) noexcept
{
	std::uint64_t word = 0;
	if (configuration.Enabled)
		word |= kEnabledBit;
	if (configuration.CoalesceOnOverflow)
		word |= kCoalesceOnOverflowBit;
	word |= (static_cast<std::uint64_t>(configuration.MaxPayloadBytes) & kPayloadMask)
		<< kPayloadShift;
	word |= (static_cast<std::uint64_t>(configuration.KindMask) & kKindMaskMask)
		<< kKindMaskShift;
	return word;
}

HookCollectorConfig HookEventCollector::UnpackConfiguration(const std::uint64_t word) noexcept
{
	return {
		.Enabled = (word & kEnabledBit) != 0,
		.KindMask = static_cast<std::uint32_t>((word >> kKindMaskShift) & kKindMaskMask),
		.MaxPayloadBytes = static_cast<std::size_t>((word >> kPayloadShift) & kPayloadMask),
		.CoalesceOnOverflow = (word & kCoalesceOnOverflowBit) != 0
	};
}

HookEventCollector::Admission HookEventCollector::TryEnter(OperationLease& lease) noexcept
{
	m_ActivitySequence.fetch_add(1, std::memory_order_acq_rel);
	const std::uint64_t state = m_ProducerState.fetch_add(1, std::memory_order_acq_rel);
	lease = OperationLease(this);
	if ((state & kStoppedBit) != 0)
		return Admission::Stopped;
	if ((state & kStoppingBit) != 0)
		return Admission::Stopping;
	return Admission::Entered;
}

void HookEventCollector::ExitOperation() noexcept
{
	m_ProducerState.fetch_sub(1, std::memory_order_acq_rel);
}

bool HookEventCollector::ReadConfiguration(PackedConfiguration& output) const noexcept
{
	const std::uint64_t first = m_ConfigurationSequence.load(std::memory_order_acquire);
	if ((first & 1) != 0 || first == 0)
		return false;
	const std::uint64_t word = m_ConfigurationWord.load(std::memory_order_relaxed);
	const std::uint64_t second = m_ConfigurationSequence.load(std::memory_order_acquire);
	if (first != second || (second & 1) != 0)
		return false;
	output.Value = UnpackConfiguration(word);
	output.Generation = second / 2;
	return true;
}

HookConfigPublishResult HookEventCollector::PublishConfiguration(
	const HookCollectorConfig configuration) noexcept
{
	if (!m_Configured)
		return {.Error = m_InitializationError};

	OperationLease lease;
	switch (TryEnter(lease))
	{
	case Admission::Entered: break;
	case Admission::Stopping:
		return {.Error = HookCollectorError::CollectorStopping};
	case Admission::Stopped:
		return {.Error = HookCollectorError::CollectorStopped};
	}
	if (!IsValidConfiguration(configuration, m_Limits))
		return {.Error = HookCollectorError::InvalidConfiguration};

	if (m_ConfigurationPublishOwned.test_and_set(std::memory_order_acquire))
		return {.Error = HookCollectorError::ConcurrentConfigurationPublish};

	const std::uint64_t current = m_ConfigurationSequence.load(std::memory_order_acquire);
	if ((current & 1) != 0)
	{
		m_ConfigurationPublishOwned.clear(std::memory_order_release);
		return {.Error = HookCollectorError::ConcurrentConfigurationPublish};
	}
	if (current / 2 >= kMaxProtocolInteger)
	{
		m_ConfigurationPublishOwned.clear(std::memory_order_release);
		return {.Error = HookCollectorError::ConfigurationGenerationExhausted};
	}

	m_ConfigurationSequence.store(current + 1, std::memory_order_release);
	m_ConfigurationWord.store(PackConfiguration(configuration), std::memory_order_relaxed);
	m_ConfigurationSequence.store(current + 2, std::memory_order_release);
	m_ConfigurationPublishOwned.clear(std::memory_order_release);
	return {
		.Error = HookCollectorError::None,
		.ConfigurationGeneration = (current + 2) / 2
	};
}

HookEventCollector::Reservation HookEventCollector::Reserve(
	std::uint64_t& position,
	Slot*& slot) noexcept
{
	position = m_EnqueuePosition.load(std::memory_order_relaxed);
	for (std::size_t attempt = 0; attempt < kMaxReservationAttempts; ++attempt)
	{
		if (position >= kMaxProtocolInteger)
			return Reservation::SequenceExhausted;
		slot = &m_Slots[static_cast<std::size_t>(position) & m_CapacityMask];
		const std::uint64_t sequence = slot->Sequence.load(std::memory_order_acquire);
		if (sequence == position)
		{
			if (m_EnqueuePosition.compare_exchange_weak(
				position,
				position + 1,
				std::memory_order_relaxed,
				std::memory_order_relaxed))
			{
				return Reservation::Reserved;
			}
			continue;
		}
		if (sequence < position)
			return Reservation::Full;
		position = m_EnqueuePosition.load(std::memory_order_relaxed);
	}
	return Reservation::Contended;
}

HookPublishResult HookEventCollector::TryPublish(const HookEventView& event) noexcept
{
	if (!m_Configured)
	{
		return {
			.Status = HookPublishStatus::InvalidCollector,
			.CollectorError = m_InitializationError
		};
	}

	OperationLease lease;
	switch (TryEnter(lease))
	{
	case Admission::Entered: break;
	case Admission::Stopping:
		m_StoppingRejectionTotal.fetch_add(1, std::memory_order_relaxed);
		return {.Status = HookPublishStatus::CollectorStopping};
	case Admission::Stopped:
		m_StoppedRejectionTotal.fetch_add(1, std::memory_order_relaxed);
		return {.Status = HookPublishStatus::CollectorStopped};
	}
	if (!IsKnownKind(event.Kind))
		return {.Status = HookPublishStatus::InvalidKind};

	PackedConfiguration configuration;
	if (!ReadConfiguration(configuration))
	{
		m_ConfigurationRaceTotal.fetch_add(1, std::memory_order_relaxed);
		return {.Status = HookPublishStatus::ConfigurationChanging};
	}
	if (!configuration.Value.Enabled)
	{
		m_DisabledTotal.fetch_add(1, std::memory_order_relaxed);
		return {
			.Status = HookPublishStatus::Disabled,
			.ConfigurationGeneration = configuration.Generation
		};
	}
	if ((configuration.Value.KindMask & HookEventKindBit(event.Kind)) == 0)
	{
		m_KindFilteredTotal.fetch_add(1, std::memory_order_relaxed);
		return {
			.Status = HookPublishStatus::KindFiltered,
			.ConfigurationGeneration = configuration.Generation
		};
	}
	if (event.Payload.size() > configuration.Value.MaxPayloadBytes)
	{
		m_DroppedOversizeTotal.fetch_add(1, std::memory_order_relaxed);
		return {
			.Status = HookPublishStatus::PayloadTooLarge,
			.ConfigurationGeneration = configuration.Generation
		};
	}

	std::uint64_t position = 0;
	Slot* slot = nullptr;
	const Reservation reservation = Reserve(position, slot);
	if (reservation != Reservation::Reserved)
	{
		if (reservation == Reservation::Full
			&& configuration.Value.CoalesceOnOverflow
			&& event.Coalescible)
		{
			const std::size_t kindIndex = static_cast<std::size_t>(event.Kind);
			m_PendingCoalescedByKind[kindIndex].fetch_add(1, std::memory_order_relaxed);
			m_CoalescedOverflowTotal.fetch_add(1, std::memory_order_relaxed);
			return {
				.Status = HookPublishStatus::CoalescedOnOverflow,
				.ConfigurationGeneration = configuration.Generation
			};
		}
		if (reservation == Reservation::SequenceExhausted)
		{
			return {
				.Status = HookPublishStatus::SequenceExhausted,
				.ConfigurationGeneration = configuration.Generation
			};
		}
		if (reservation == Reservation::Contended)
		{
			m_DroppedContentionTotal.fetch_add(1, std::memory_order_relaxed);
			return {
				.Status = HookPublishStatus::Contended,
				.ConfigurationGeneration = configuration.Generation
			};
		}
		m_DroppedOverflowTotal.fetch_add(1, std::memory_order_relaxed);
		return {
			.Status = HookPublishStatus::QueueFull,
			.ConfigurationGeneration = configuration.Generation
		};
	}

	const std::size_t kindIndex = static_cast<std::size_t>(event.Kind);
	Slot::StoredEvent& stored = slot->Event;
	stored.ContextGeneration = m_ContextGeneration;
	stored.Sequence = position + 1;
	stored.ConfigurationGeneration = configuration.Generation;
	stored.Kind = event.Kind;
	stored.Source = event.Source;
	stored.Subject = event.Subject;
	stored.Correlation = event.Correlation;
	stored.CoalescedBefore =
		m_PendingCoalescedByKind[kindIndex].exchange(0, std::memory_order_acq_rel);
	stored.PayloadSize = event.Payload.size();
	if (!event.Payload.empty())
		std::memcpy(stored.Payload.data(), event.Payload.data(), event.Payload.size());

	slot->Sequence.store(position + 1, std::memory_order_release);
	m_PublishedTotal.fetch_add(1, std::memory_order_relaxed);
	return {
		.Status = HookPublishStatus::Published,
		.Sequence = position + 1,
		.ConfigurationGeneration = configuration.Generation
	};
}

bool HookEventCollector::IsNextEventAvailable() const noexcept
{
	const std::uint64_t position = m_DequeuePosition.load(std::memory_order_relaxed);
	const Slot& slot = m_Slots[static_cast<std::size_t>(position) & m_CapacityMask];
	return slot.Sequence.load(std::memory_order_acquire) == position + 1;
}

std::size_t HookEventCollector::ReservedEventCount() const noexcept
{
	const std::uint64_t enqueued = m_EnqueuePosition.load(std::memory_order_acquire);
	const std::uint64_t dequeued = m_DequeuePosition.load(std::memory_order_acquire);
	const std::uint64_t difference = enqueued >= dequeued ? enqueued - dequeued : 0;
	return static_cast<std::size_t>((std::min)(
		difference,
		static_cast<std::uint64_t>(m_Limits.Capacity)));
}

HookDrainResult HookEventCollector::Drain(const std::span<HookEvent> output) noexcept
{
	if (!m_Configured)
		return {.Error = m_InitializationError};
	if (output.empty() || output.size() > kHardMaxDrainBatch)
		return {.Error = HookCollectorError::InvalidLimit};
	if (m_DrainOwned.test_and_set(std::memory_order_acquire))
		return {.Error = HookCollectorError::ConcurrentDrain};

	std::size_t count = 0;
	while (count < output.size())
	{
		const std::uint64_t position = m_DequeuePosition.load(std::memory_order_relaxed);
		Slot& slot = m_Slots[static_cast<std::size_t>(position) & m_CapacityMask];
		if (slot.Sequence.load(std::memory_order_acquire) != position + 1)
			break;

		const Slot::StoredEvent& stored = slot.Event;
		HookEvent& drained = output[count];
		drained = HookEvent{};
		std::memcpy(drained.SessionId.data(), m_SessionId.data(), m_SessionIdSize);
		drained.SessionIdSize = m_SessionIdSize;
		drained.ContextGeneration = stored.ContextGeneration;
		drained.Sequence = stored.Sequence;
		drained.ConfigurationGeneration = stored.ConfigurationGeneration;
		drained.Kind = stored.Kind;
		drained.Source = stored.Source;
		drained.Subject = stored.Subject;
		drained.Correlation = stored.Correlation;
		drained.CoalescedBefore = stored.CoalescedBefore;
		drained.PayloadSize = stored.PayloadSize;
		if (stored.PayloadSize != 0)
		{
			std::memcpy(
				drained.Payload.data(),
				stored.Payload.data(),
				stored.PayloadSize);
		}

		slot.Sequence.store(
			position + static_cast<std::uint64_t>(m_Limits.Capacity),
			std::memory_order_release);
		m_DequeuePosition.store(position + 1, std::memory_order_relaxed);
		++count;
	}
	if (count != 0)
		m_DrainedTotal.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);

	HookDrainResult result{
		.Error = HookCollectorError::None,
		.Count = count,
		.MoreAvailable = IsNextEventAvailable(),
		.PublishedTotal = m_PublishedTotal.load(std::memory_order_relaxed),
		.DrainedTotal = m_DrainedTotal.load(std::memory_order_relaxed),
		.DroppedOverflowTotal = m_DroppedOverflowTotal.load(std::memory_order_relaxed),
		.DroppedOversizeTotal = m_DroppedOversizeTotal.load(std::memory_order_relaxed),
		.DroppedContentionTotal = m_DroppedContentionTotal.load(std::memory_order_relaxed),
		.CoalescedOverflowTotal = m_CoalescedOverflowTotal.load(std::memory_order_relaxed)
	};
	m_DrainOwned.clear(std::memory_order_release);
	return result;
}

HookCollectorLifecycle HookEventCollector::Lifecycle() const noexcept
{
	const std::uint64_t state = m_ProducerState.load(std::memory_order_acquire);
	if ((state & kStoppedBit) != 0)
		return HookCollectorLifecycle::Stopped;
	if ((state & kStoppingBit) != 0)
		return HookCollectorLifecycle::Stopping;
	return HookCollectorLifecycle::Running;
}

HookCollectorSnapshot HookEventCollector::Snapshot() const noexcept
{
	HookCollectorSnapshot snapshot;
	snapshot.Configured = m_Configured;
	snapshot.InitializationError = m_InitializationError;
	snapshot.Lifecycle = Lifecycle();
	std::memcpy(snapshot.SessionId.data(), m_SessionId.data(), m_SessionIdSize);
	snapshot.SessionIdSize = m_SessionIdSize;
	snapshot.ContextGeneration = m_ContextGeneration;
	snapshot.Capacity = m_Configured ? m_Limits.Capacity : 0;
	snapshot.ReservedEventCount = m_Configured ? ReservedEventCount() : 0;
	const std::uint64_t producerState = m_ProducerState.load(std::memory_order_acquire);
	snapshot.InFlightProducers = producerState & kInFlightMask;
	const std::uint64_t enqueuePosition = m_EnqueuePosition.load(std::memory_order_relaxed);
	snapshot.NextSequence = enqueuePosition < kMaxProtocolInteger ? enqueuePosition + 1 : 0;
	snapshot.PublishedTotal = m_PublishedTotal.load(std::memory_order_relaxed);
	snapshot.DrainedTotal = m_DrainedTotal.load(std::memory_order_relaxed);
	snapshot.DisabledTotal = m_DisabledTotal.load(std::memory_order_relaxed);
	snapshot.KindFilteredTotal = m_KindFilteredTotal.load(std::memory_order_relaxed);
	snapshot.DroppedOverflowTotal = m_DroppedOverflowTotal.load(std::memory_order_relaxed);
	snapshot.DroppedOversizeTotal = m_DroppedOversizeTotal.load(std::memory_order_relaxed);
	snapshot.DroppedContentionTotal = m_DroppedContentionTotal.load(std::memory_order_relaxed);
	snapshot.CoalescedOverflowTotal = m_CoalescedOverflowTotal.load(std::memory_order_relaxed);
	snapshot.ConfigurationRaceTotal = m_ConfigurationRaceTotal.load(std::memory_order_relaxed);
	snapshot.StoppingRejectionTotal = m_StoppingRejectionTotal.load(std::memory_order_relaxed);
	snapshot.StoppedRejectionTotal = m_StoppedRejectionTotal.load(std::memory_order_relaxed);
	for (std::size_t index = 0; index < snapshot.PendingCoalescedByKind.size(); ++index)
	{
		snapshot.PendingCoalescedByKind[index] =
			m_PendingCoalescedByKind[index].load(std::memory_order_relaxed);
	}
	PackedConfiguration configuration;
	snapshot.ConfigurationSnapshotStable = ReadConfiguration(configuration);
	if (snapshot.ConfigurationSnapshotStable)
	{
		snapshot.Configuration = configuration.Value;
		snapshot.ConfigurationGeneration = configuration.Generation;
	}
	return snapshot;
}

HookStopResult HookEventCollector::StopAndDrain(
	const std::chrono::milliseconds timeout) noexcept
{
	if (!m_Configured)
		return {.Error = m_InitializationError};
	if (timeout.count() <= 0 || timeout > kMaxStopTimeout)
		return {.Error = HookCollectorError::InvalidLimit};
	if (Lifecycle() == HookCollectorLifecycle::Stopped)
	{
		return {
			.Error = HookCollectorError::None,
			.PendingEventCount = ReservedEventCount()
		};
	}
	if (m_StopOwned.test_and_set(std::memory_order_acquire))
		return {.Error = HookCollectorError::ConcurrentStop};

	m_ProducerState.fetch_or(kStoppingBit, std::memory_order_acq_rel);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	HookCollectorError error = HookCollectorError::DrainTimedOut;
	for (;;)
	{
		const std::uint64_t activity = m_ActivitySequence.load(std::memory_order_acquire);
		const std::uint64_t state = m_ProducerState.load(std::memory_order_acquire);
		if ((state & kInFlightMask) == 0)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now + kStopQuietPeriod > deadline)
				break;
			std::this_thread::sleep_until(now + kStopQuietPeriod);
			if ((m_ProducerState.load(std::memory_order_acquire) & kInFlightMask) == 0
				&& m_ActivitySequence.load(std::memory_order_acquire) == activity)
			{
				std::uint64_t expected = kStoppingBit;
				if (m_ProducerState.compare_exchange_strong(
					expected,
					kStoppingBit | kStoppedBit,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
				{
					error = HookCollectorError::None;
					break;
				}
			}
		}
		else
		{
			std::this_thread::sleep_for(kStopQuietPeriod);
		}
		if (std::chrono::steady_clock::now() >= deadline)
			break;
	}

	m_StopOwned.clear(std::memory_order_release);
	return {
		.Error = error,
		.PendingEventCount = ReservedEventCount()
	};
}

} // namespace UExplorer::Runtime
