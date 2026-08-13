#include "WatchScheduler.h"

#include <algorithm>
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

std::uint64_t ElapsedMicroseconds(
	const std::chrono::steady_clock::time_point started) noexcept
{
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - started).count();
	return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept
{
	return left > (std::numeric_limits<std::uint64_t>::max)() - right
		? (std::numeric_limits<std::uint64_t>::max)()
		: left + right;
}

std::size_t SaturatingSizeAdd(const std::size_t left, const std::size_t right) noexcept
{
	return left > (std::numeric_limits<std::size_t>::max)() - right
		? (std::numeric_limits<std::size_t>::max)()
		: left + right;
}

std::size_t PayloadBytes(const WatchSamplePayload& payload) noexcept
{
	return SaturatingSizeAdd(
		SaturatingSizeAdd(payload.TypeName.size(), payload.CanonicalValue.size()),
		payload.DisplayValue.size());
}

std::size_t EventBytes(const WatchEvent& event) noexcept
{
	std::size_t size = SaturatingSizeAdd(event.ReasonCode.size(), event.Reason.size());
	if (event.Value)
		size = SaturatingSizeAdd(size, PayloadBytes(*event.Value));
	return size;
}

bool ValidLimits(const WatchSchedulerLimits& limits) noexcept
{
	return limits.MaxSubscriptions > 0
		&& limits.MaxSubscriptions <= WatchScheduler::kHardMaxSubscriptions
		&& limits.MaxHistoryEntriesPerWatch > 0
		&& limits.MaxHistoryEntriesPerWatch <= WatchScheduler::kHardMaxHistoryEntries
		&& limits.MaxHistoryBytesPerWatch >= limits.MaxSampleBytes
		&& limits.MaxPendingEvents > 0
		&& limits.MaxPendingEvents <= WatchScheduler::kHardMaxPendingEvents
		&& limits.MaxPendingEventBytes >= limits.MaxSampleBytes
		&& limits.MaxSampleBytes > 0
		&& limits.MaxSampleBytes <= WatchScheduler::kHardMaxSampleBytes
		&& limits.MaxFrameItems > 0
		&& limits.MaxFrameItems <= WatchScheduler::kHardMaxFrameItems
		&& limits.MaxFrameBytes >= limits.MaxSampleBytes
		&& limits.MaxFrameTimeUs > 0
		&& limits.MaxFrameTimeUs <= WatchScheduler::kHardMaxFrameTimeUs
		&& limits.MinIntervalMs > 0
		&& limits.MinIntervalMs <= limits.MaxIntervalMs;
}

bool SameObjectHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameWatchIdentity(
	const WatchSubscriptionSpec& left,
	const WatchSubscriptionSpec& right) noexcept
{
	return SameObjectHandle(left.Object, right.Object)
		&& left.ContextGeneration == right.ContextGeneration
		&& left.ObjectSnapshotGeneration == right.ObjectSnapshotGeneration
		&& left.TypeSnapshotGeneration == right.TypeSnapshotGeneration
		&& left.DeclaringTypePath == right.DeclaringTypePath
		&& left.PropertyName == right.PropertyName
		&& left.ArrayIndex == right.ArrayIndex;
}

} // namespace

struct WatchScheduler::SubscriptionRecord
{
	WatchId Id = 0;
	WatchSubscriptionSpec Spec;
	bool Enabled = false;
	bool Terminal = false;
	std::atomic<bool> Active{false};
	std::uint64_t CreatedAtMonotonicUs = 0;
	std::atomic<std::uint64_t> NextDueMonotonicUs{0};
	std::uint64_t LastSampledAtMonotonicUs = 0;
	std::uint64_t LastChangeSequence = 0;
	std::uint64_t SampleCount = 0;
	std::uint64_t FailureCount = 0;
	std::shared_ptr<const IWatchSampleBinding> Binding;
	std::shared_ptr<const WatchSamplePayload> LastValue;
	std::deque<WatchHistoryEntry> History;
	std::size_t HistoryBytes = 0;
	std::uint64_t HistoryDropCount = 0;
	std::string TerminalReasonCode;
	std::string TerminalReason;
};

const char* ToString(const WatchError error) noexcept
{
	switch (error)
	{
	case WatchError::None: return "NONE";
	case WatchError::InvalidConfiguration: return "WATCH_INVALID_CONFIGURATION";
	case WatchError::Stopped: return "WATCH_SCHEDULER_STOPPED";
	case WatchError::InvalidSpec: return "WATCH_SPEC_INVALID";
	case WatchError::SessionMismatch: return "WATCH_SESSION_MISMATCH";
	case WatchError::ContextGenerationMismatch: return "WATCH_CONTEXT_GENERATION_MISMATCH";
	case WatchError::SnapshotGenerationInvalid: return "WATCH_SNAPSHOT_GENERATION_INVALID";
	case WatchError::IntervalOutOfRange: return "WATCH_INTERVAL_OUT_OF_RANGE";
	case WatchError::Duplicate: return "WATCH_DUPLICATE";
	case WatchError::CapacityExceeded: return "WATCH_CAPACITY_EXCEEDED";
	case WatchError::IdExhausted: return "WATCH_ID_EXHAUSTED";
	case WatchError::NotFound: return "WATCH_NOT_FOUND";
	case WatchError::Terminal: return "WATCH_TERMINAL";
	case WatchError::InvalidLimit: return "WATCH_LIMIT_INVALID";
	case WatchError::AllocationFailed: return "WATCH_ALLOCATION_FAILED";
	case WatchError::BindingFailed: return "WATCH_BIND_FAILED";
	case WatchError::DrainTimedOut: return "WATCH_DRAIN_TIMED_OUT";
	case WatchError::ConcurrentPumpRejected: return "WATCH_CONCURRENT_PUMP_REJECTED";
	}
	return "WATCH_UNKNOWN_ERROR";
}

const char* ToString(const WatchSampleStatus status) noexcept
{
	switch (status)
	{
	case WatchSampleStatus::Value: return "value";
	case WatchSampleStatus::Unavailable: return "unavailable";
	case WatchSampleStatus::Failed: return "failed";
	case WatchSampleStatus::Stale: return "stale";
	}
	return "unknown";
}

const char* ToString(const WatchSubscriptionState state) noexcept
{
	switch (state)
	{
	case WatchSubscriptionState::Enabled: return "enabled";
	case WatchSubscriptionState::Disabled: return "disabled";
	case WatchSubscriptionState::Terminal: return "terminal";
	}
	return "unknown";
}

const char* ToString(const WatchEventKind kind) noexcept
{
	switch (kind)
	{
	case WatchEventKind::ValueChanged: return "value_changed";
	case WatchEventKind::SampleUnavailable: return "sample_unavailable";
	case WatchEventKind::SampleFailed: return "sample_failed";
	case WatchEventKind::TerminalStale: return "terminal_stale";
	}
	return "unknown";
}

WatchScheduler::WatchScheduler(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	IWatchSampleSource& source,
	WatchSchedulerLimits limits)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Source(source),
	  m_Limits(limits),
	  m_EmptyEnabledSnapshot(std::make_shared<const EnabledSnapshot>()),
	  m_EnabledSnapshot(m_EmptyEnabledSnapshot)
{
	m_Configured = !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_ContextGeneration > 0
		&& m_ContextGeneration <= kMaxProtocolInteger
		&& m_Source.SessionId() == m_SessionId
		&& m_Source.ContextGeneration() == m_ContextGeneration
		&& ValidLimits(m_Limits);
}

WatchError WatchScheduler::ValidateSpec(const WatchSubscriptionSpec& spec) const noexcept
{
	if (!m_Configured)
		return WatchError::InvalidConfiguration;
	if (spec.Object.SessionId != m_SessionId)
		return WatchError::SessionMismatch;
	if (spec.ContextGeneration != m_ContextGeneration
		|| spec.Object.ContextGeneration != m_ContextGeneration)
	{
		return WatchError::ContextGenerationMismatch;
	}
	if (spec.ObjectSnapshotGeneration == 0
		|| spec.ObjectSnapshotGeneration > kMaxProtocolInteger
		|| spec.TypeSnapshotGeneration == 0
		|| spec.TypeSnapshotGeneration > kMaxProtocolInteger)
	{
		return WatchError::SnapshotGenerationInvalid;
	}
	if (spec.Object.Index < 0 || spec.Object.SerialNumber <= 0
		|| spec.Object.Address == 0 || spec.Object.ClassFingerprint == 0
		|| spec.DeclaringTypePath.empty() || spec.DeclaringTypePath.size() > 4096
		|| spec.PropertyName.empty() || spec.PropertyName.size() > 1024)
	{
		return WatchError::InvalidSpec;
	}
	if (spec.IntervalMs < m_Limits.MinIntervalMs
		|| spec.IntervalMs > m_Limits.MaxIntervalMs)
	{
		return WatchError::IntervalOutOfRange;
	}
	return WatchError::None;
}

std::shared_ptr<WatchScheduler::SubscriptionRecord> WatchScheduler::FindLocked(
	const WatchId id) const noexcept
{
	const auto found = std::ranges::find_if(m_Subscriptions, [id](const auto& record) {
		return record && record->Id == id;
	});
	return found == m_Subscriptions.end() ? nullptr : *found;
}

std::shared_ptr<const WatchScheduler::EnabledSnapshot>
WatchScheduler::BuildEnabledSnapshotLocked(
	const SubscriptionRecord* overrideRecord,
	const bool overrideEnabled,
	const SubscriptionRecord* excludedRecord) const
{
	auto snapshot = std::make_shared<EnabledSnapshot>();
	snapshot->reserve(m_Subscriptions.size() + (overrideRecord ? 1U : 0U));
	bool overridePresent = false;
	for (const auto& record : m_Subscriptions)
	{
		if (!record || record.get() == excludedRecord)
			continue;
		const bool enabled = record.get() == overrideRecord
			? overrideEnabled
			: record->Enabled;
		overridePresent = overridePresent || record.get() == overrideRecord;
		if (enabled && !record->Terminal)
			snapshot->push_back(record);
	}
	if (overrideRecord && !overridePresent && overrideEnabled && !overrideRecord->Terminal)
	{
		// Add builds its immutable snapshot before publishing the new record.
		for (const auto& record : m_Subscriptions)
		{
			if (record.get() == overrideRecord)
				return snapshot;
		}
	}
	return snapshot;
}

void WatchScheduler::PublishEnabledSnapshotLocked(
	std::shared_ptr<const EnabledSnapshot> snapshot) noexcept
{
	if (!snapshot)
		return;
	++m_EnabledSnapshotGeneration;
	m_EnabledSnapshot.store(std::move(snapshot), std::memory_order_release);
}

WatchSubscription WatchScheduler::CopySubscriptionLocked(
	const SubscriptionRecord& record) const
{
	return {
		.Id = record.Id,
		.Spec = record.Spec,
		.State = record.Terminal
			? WatchSubscriptionState::Terminal
			: (record.Enabled ? WatchSubscriptionState::Enabled : WatchSubscriptionState::Disabled),
		.CreatedAtMonotonicUs = record.CreatedAtMonotonicUs,
		.NextDueMonotonicUs = record.NextDueMonotonicUs.load(std::memory_order_acquire),
		.LastSampledAtMonotonicUs = record.LastSampledAtMonotonicUs,
		.LastChangeSequence = record.LastChangeSequence,
		.SampleCount = record.SampleCount,
		.FailureCount = record.FailureCount,
		.HistoryCount = record.History.size(),
		.HistoryBytes = record.HistoryBytes,
		.HistoryDropCount = record.HistoryDropCount,
		.TerminalReasonCode = record.TerminalReasonCode,
		.TerminalReason = record.TerminalReason
	};
}

WatchAddResult WatchScheduler::Add(WatchSubscriptionSpec spec, const bool enabled) noexcept
{
	if (m_StopRequested.load(std::memory_order_acquire))
		return {.Error = WatchError::Stopped};
	const WatchError specError = ValidateSpec(spec);
	if (specError != WatchError::None)
		return {.Error = specError};

	try
	{
		// Avoid invoking a potentially expensive source bind for a request that is
		// already known to be inadmissible. The same checks are repeated after the
		// lock-free bind because another command can publish in between.
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_StopRequested.load(std::memory_order_acquire))
				return {.Error = WatchError::Stopped};
			if (std::ranges::any_of(m_Subscriptions, [&spec](const auto& existing) {
				return existing
					&& SameWatchIdentity(existing->Spec, spec)
					&& existing->Spec.IntervalMs == spec.IntervalMs;
			}))
			{
				return {.Error = WatchError::Duplicate};
			}
			if (m_Subscriptions.size() >= m_Limits.MaxSubscriptions)
				return {.Error = WatchError::CapacityExceeded};
			if (m_NextId == 0 || m_NextId > kMaxProtocolInteger)
				return {.Error = WatchError::IdExhausted};
		}

		WatchBindingResult bound;
		try
		{
			bound = m_Source.Bind(spec);
		}
		catch (const std::bad_alloc&)
		{
			return {.Error = WatchError::AllocationFailed};
		}
		catch (...)
		{
			return {
				.Error = WatchError::BindingFailed,
				.ReasonCode = "WATCH_SOURCE_BIND_EXCEPTION",
				.Reason = "Watch sample source raised an unexpected exception while binding"
			};
		}
		if (!bound.Ok())
		{
			return {
				.Error = WatchError::BindingFailed,
				.ReasonCode = bound.ReasonCode.empty()
					? "WATCH_SOURCE_BIND_FAILED" : std::move(bound.ReasonCode),
				.Reason = bound.Reason.empty()
					? "Watch sample source could not bind the exact immutable dependencies"
					: std::move(bound.Reason)
			};
		}
		if (bound.Binding->SourceIdentity() != static_cast<const void*>(&m_Source)
			|| !bound.ReasonCode.empty() || !bound.Reason.empty())
		{
			return {
				.Error = WatchError::BindingFailed,
				.ReasonCode = "WATCH_SOURCE_BIND_CONTRACT_VIOLATION",
				.Reason = "Watch sample source returned an invalid owned binding contract"
			};
		}

		auto record = std::make_shared<SubscriptionRecord>();
		record->Spec = spec;
		record->Binding = std::move(bound.Binding);
		record->Enabled = enabled;
		record->Active.store(enabled, std::memory_order_release);
		record->CreatedAtMonotonicUs = MonotonicMicroseconds();
		record->NextDueMonotonicUs.store(
			enabled ? record->CreatedAtMonotonicUs : 0,
			std::memory_order_release);

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_StopRequested.load(std::memory_order_acquire))
			return {.Error = WatchError::Stopped};
		if (std::ranges::any_of(m_Subscriptions, [&record](const auto& existing) {
			return existing
				&& SameWatchIdentity(existing->Spec, record->Spec)
				&& existing->Spec.IntervalMs == record->Spec.IntervalMs;
		}))
		{
			return {.Error = WatchError::Duplicate};
		}
		if (m_Subscriptions.size() >= m_Limits.MaxSubscriptions)
			return {.Error = WatchError::CapacityExceeded};
		if (m_NextId == 0 || m_NextId > kMaxProtocolInteger)
			return {.Error = WatchError::IdExhausted};

		record->Id = m_NextId;
		auto nextSnapshot = std::make_shared<EnabledSnapshot>();
		nextSnapshot->reserve(m_Subscriptions.size() + 1);
		for (const auto& existing : m_Subscriptions)
		{
			if (existing && existing->Enabled && !existing->Terminal)
				nextSnapshot->push_back(existing);
		}
		if (enabled)
			nextSnapshot->push_back(record);
		m_Subscriptions.push_back(record);
		++m_NextId;
		PublishEnabledSnapshotLocked(std::move(nextSnapshot));
		return {.Id = record->Id};
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed};
	}
}

WatchListResult WatchScheduler::List() const noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		WatchListResult result;
		result.EnabledSnapshotGeneration = m_EnabledSnapshotGeneration;
		result.Subscriptions.reserve(m_Subscriptions.size());
		for (const auto& record : m_Subscriptions)
		{
			if (record)
				result.Subscriptions.push_back(CopySubscriptionLocked(*record));
		}
		return result;
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed};
	}
}

WatchMutationResult WatchScheduler::Enable(const WatchId id, const bool enabled) noexcept
{
	if (id == 0)
		return {.Error = WatchError::NotFound, .Id = id};
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_StopRequested.load(std::memory_order_acquire))
			return {.Error = WatchError::Stopped, .Id = id};
		const auto record = FindLocked(id);
		if (!record)
			return {.Error = WatchError::NotFound, .Id = id};
		if (record->Terminal)
			return {.Error = WatchError::Terminal, .Id = id};
		if (record->Enabled == enabled)
			return {.Id = id};

		auto nextSnapshot = BuildEnabledSnapshotLocked(record.get(), enabled);
		record->Enabled = enabled;
		record->Active.store(enabled, std::memory_order_release);
		record->NextDueMonotonicUs.store(
			enabled ? MonotonicMicroseconds() : 0,
			std::memory_order_release);
		PublishEnabledSnapshotLocked(std::move(nextSnapshot));
		return {.Id = id};
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed, .Id = id};
	}
}

WatchMutationResult WatchScheduler::Remove(const WatchId id) noexcept
{
	if (id == 0)
		return {.Error = WatchError::NotFound, .Id = id};
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_StopRequested.load(std::memory_order_acquire))
			return {.Error = WatchError::Stopped, .Id = id};
		const auto record = FindLocked(id);
		if (!record)
			return {.Error = WatchError::NotFound, .Id = id};
		auto nextSnapshot = BuildEnabledSnapshotLocked(nullptr, false, record.get());
		record->Enabled = false;
		record->Active.store(false, std::memory_order_release);
		const auto found = std::ranges::find(m_Subscriptions, record);
		if (found != m_Subscriptions.end())
			m_Subscriptions.erase(found);
		PublishEnabledSnapshotLocked(std::move(nextSnapshot));
		return {.Id = id};
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed, .Id = id};
	}
}

WatchSnapshotResult WatchScheduler::Snapshot(const WatchId id) const noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto record = FindLocked(id);
		if (!record)
			return {.Error = WatchError::NotFound};
		WatchSnapshotResult result;
		result.Subscription = CopySubscriptionLocked(*record);
		result.LastValue = record->LastValue;
		result.History.assign(record->History.begin(), record->History.end());
		return result;
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed};
	}
}

std::uint64_t WatchScheduler::NextEventSequenceLocked() noexcept
{
	if (m_NextEventSequence == 0 || m_NextEventSequence > kMaxProtocolInteger)
		return 0;
	return m_NextEventSequence++;
}

void WatchScheduler::AppendHistoryLocked(
	SubscriptionRecord& record,
	const std::uint64_t sequence,
	const std::uint64_t capturedAt,
	const std::shared_ptr<const WatchSamplePayload>& value,
	const std::size_t payloadBytes)
{
	if (!value || payloadBytes > m_Limits.MaxHistoryBytesPerWatch)
	{
		++record.HistoryDropCount;
		return;
	}
	while (!record.History.empty()
		&& (record.History.size() >= m_Limits.MaxHistoryEntriesPerWatch
			|| record.HistoryBytes > m_Limits.MaxHistoryBytesPerWatch - payloadBytes))
	{
		const auto& oldest = record.History.front();
		const std::size_t oldestBytes = oldest.Value ? PayloadBytes(*oldest.Value) : 0;
		record.HistoryBytes = oldestBytes <= record.HistoryBytes
			? record.HistoryBytes - oldestBytes
			: 0;
		record.History.pop_front();
		++record.HistoryDropCount;
	}
	record.History.push_back({
		.Sequence = sequence,
		.CapturedAtMonotonicUs = capturedAt,
		.Value = value
	});
	record.HistoryBytes += payloadBytes;
}

void WatchScheduler::AppendEventLocked(
	WatchEvent event,
	const std::size_t eventBytes,
	const bool coalescible)
{
	if (event.Sequence == 0 || eventBytes > m_Limits.MaxPendingEventBytes)
	{
		++m_DroppedEvents;
		return;
	}
	if (coalescible)
	{
		const auto existing = std::ranges::find_if(m_Events, [&event](const WatchEvent& item) {
			return item.Id == event.Id && item.Kind == event.Kind;
		});
		if (existing != m_Events.end())
		{
			const std::size_t oldBytes = EventBytes(*existing);
			m_PendingEventBytes = oldBytes <= m_PendingEventBytes
				? m_PendingEventBytes - oldBytes
				: 0;
			m_Events.erase(existing);
			++m_CoalescedEvents;
		}
	}
	while (!m_Events.empty()
		&& (m_Events.size() >= m_Limits.MaxPendingEvents
			|| m_PendingEventBytes > m_Limits.MaxPendingEventBytes - eventBytes))
	{
		const std::size_t oldestBytes = EventBytes(m_Events.front());
		m_PendingEventBytes = oldestBytes <= m_PendingEventBytes
			? m_PendingEventBytes - oldestBytes
			: 0;
		m_Events.pop_front();
		++m_DroppedEvents;
	}
	event.DropCount = m_DroppedEvents;
	event.CoalesceCount = m_CoalescedEvents;
	m_Events.push_back(std::move(event));
	m_PendingEventBytes += eventBytes;
	try
	{
		WatchEvent pushEvent = m_Events.back();
		AppendPushEventLocked(std::move(pushEvent), eventBytes, coalescible);
	}
	catch (...)
	{
		++m_DroppedPushEvents;
	}
}

void WatchScheduler::AppendPushEventLocked(
	WatchEvent event,
	const std::size_t eventBytes,
	const bool coalescible)
{
	if (event.Sequence == 0 || eventBytes > m_Limits.MaxPendingEventBytes)
	{
		++m_DroppedPushEvents;
		return;
	}
	if (coalescible)
	{
		const auto existing = std::ranges::find_if(
			m_PushEvents,
			[&event](const WatchEvent& item) {
				return item.Id == event.Id && item.Kind == event.Kind;
			});
		if (existing != m_PushEvents.end())
		{
			const std::size_t oldBytes = EventBytes(*existing);
			m_PendingPushEventBytes = oldBytes <= m_PendingPushEventBytes
				? m_PendingPushEventBytes - oldBytes
				: 0;
			m_PushEvents.erase(existing);
			++m_CoalescedPushEvents;
		}
	}
	while (!m_PushEvents.empty()
		&& (m_PushEvents.size() >= m_Limits.MaxPendingEvents
			|| m_PendingPushEventBytes > m_Limits.MaxPendingEventBytes - eventBytes))
	{
		const std::size_t oldestBytes = EventBytes(m_PushEvents.front());
		m_PendingPushEventBytes = oldestBytes <= m_PendingPushEventBytes
			? m_PendingPushEventBytes - oldestBytes
			: 0;
		m_PushEvents.pop_front();
		++m_DroppedPushEvents;
	}
	event.PushDroppedBefore = m_DroppedPushEvents;
	event.PushCoalescedBefore = m_CoalescedPushEvents;
	m_PushEvents.push_back(std::move(event));
	m_PendingPushEventBytes += eventBytes;
}

bool WatchScheduler::ApplySampleLocked(
	const std::shared_ptr<SubscriptionRecord>& record,
	WatchSampleResult result,
	const std::uint64_t capturedAt,
	std::size_t& bytesConsumed)
{
	bytesConsumed = 0;
	if (!record || !record->Active.load(std::memory_order_acquire)
		|| !record->Enabled || record->Terminal)
	{
		return false;
	}

	++record->SampleCount;
	record->LastSampledAtMonotonicUs = capturedAt;
	if (result.Status == WatchSampleStatus::Value)
	{
		bytesConsumed = PayloadBytes(result.Payload);
		auto value = std::make_shared<const WatchSamplePayload>(std::move(result.Payload));
		const bool changed = !record->LastValue
			|| record->LastValue->CanonicalValue != value->CanonicalValue;
		if (!changed)
			return false;

		const std::uint64_t sequence = NextEventSequenceLocked();
		if (sequence == 0)
		{
			record->Enabled = false;
			record->Terminal = true;
			record->Active.store(false, std::memory_order_release);
			record->TerminalReasonCode = "WATCH_EVENT_SEQUENCE_EXHAUSTED";
			record->TerminalReason = "Watch event sequence exhausted";
			return true;
		}
		record->LastValue = value;
		record->LastChangeSequence = sequence;
		AppendHistoryLocked(*record, sequence, capturedAt, value, bytesConsumed);
		AppendEventLocked(
			{
				.Sequence = sequence,
				.Id = record->Id,
				.Kind = WatchEventKind::ValueChanged,
				.CapturedAtMonotonicUs = capturedAt,
				.Value = std::move(value)
			},
			bytesConsumed,
			true);
		return false;
	}

	++record->FailureCount;
	if (result.ReasonCode.empty())
		result.ReasonCode = result.Status == WatchSampleStatus::Stale
			? "WATCH_TARGET_STALE"
			: (result.Status == WatchSampleStatus::Unavailable
				? "WATCH_SAMPLE_UNAVAILABLE"
				: "WATCH_SAMPLE_FAILED");
	if (result.Reason.empty())
		result.Reason = result.Status == WatchSampleStatus::Stale
			? "The exact watched object or property identity is stale"
			: "The watch sample source did not produce a value";
	const std::size_t diagnosticBytes =
		SaturatingSizeAdd(result.ReasonCode.size(), result.Reason.size());
	// The per-frame byte budget measures sampled value bytes. Diagnostic text is
	// independently bounded by the event ring contract below.
	bytesConsumed = 0;

	const std::uint64_t sequence = NextEventSequenceLocked();
	if (result.Status == WatchSampleStatus::Stale)
	{
		record->Enabled = false;
		record->Terminal = true;
		record->Active.store(false, std::memory_order_release);
		record->NextDueMonotonicUs.store(0, std::memory_order_release);
		record->TerminalReasonCode = result.ReasonCode;
		record->TerminalReason = result.Reason;
		AppendEventLocked(
			{
				.Sequence = sequence,
				.Id = record->Id,
				.Kind = WatchEventKind::TerminalStale,
				.CapturedAtMonotonicUs = capturedAt,
				.ReasonCode = std::move(result.ReasonCode),
				.Reason = std::move(result.Reason)
			},
			diagnosticBytes,
			false);
		return true;
	}

	AppendEventLocked(
		{
			.Sequence = sequence,
			.Id = record->Id,
			.Kind = result.Status == WatchSampleStatus::Unavailable
				? WatchEventKind::SampleUnavailable
				: WatchEventKind::SampleFailed,
			.CapturedAtMonotonicUs = capturedAt,
			.ReasonCode = std::move(result.ReasonCode),
			.Reason = std::move(result.Reason)
		},
		diagnosticBytes,
		true);
	return false;
}

bool WatchScheduler::HasDueWork(
	const std::shared_ptr<const EnabledSnapshot>& snapshot,
	const std::uint64_t now) const noexcept
{
	if (!snapshot)
		return false;
	return std::ranges::any_of(*snapshot, [now](const auto& record) {
		return record
			&& record->Active.load(std::memory_order_acquire)
			&& record->NextDueMonotonicUs.load(std::memory_order_acquire) <= now;
	});
}

IGameThreadFrameClient::PumpResult WatchScheduler::PumpFrame(
	const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > kHardMaxFrameItems
		|| !m_Configured || m_StopRequested.load(std::memory_order_acquire))
	{
		return {};
	}
	auto barrierLease = m_PumpBarrier.Enter();
	if (!barrierLease.OwnedWorkAllowed())
		return {};
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
	{
		m_ConcurrentPumpRejections.fetch_add(1, std::memory_order_acq_rel);
		return {};
	}
	struct PumpGuard
	{
		std::atomic_flag& Owned;
		std::atomic<bool>& Pumping;
		~PumpGuard()
		{
			Pumping.store(false, std::memory_order_release);
			Owned.clear(std::memory_order_release);
		}
	} pumpGuard{m_PumpOwned, m_Pumping};
	m_Pumping.store(true, std::memory_order_release);

	const auto started = std::chrono::steady_clock::now();
	const auto enabled = m_EnabledSnapshot.load(std::memory_order_acquire);
	if (!enabled || enabled->empty())
		return {};
	const std::size_t itemLimit = (std::min)(workBudget, m_Limits.MaxFrameItems);
	const std::size_t startIndex = m_NextPumpIndex.load(std::memory_order_acquire)
		% enabled->size();
	std::size_t examined = 0;
	std::size_t sampled = 0;
	std::size_t sampledBytes = 0;
	bool terminalObserved = false;
	bool timeExhausted = false;
	bool byteExhausted = false;

	while (examined < enabled->size() && sampled < itemLimit)
	{
		if (ElapsedMicroseconds(started) >= m_Limits.MaxFrameTimeUs)
		{
			timeExhausted = true;
			break;
		}
		if (sampledBytes >= m_Limits.MaxFrameBytes)
		{
			byteExhausted = true;
			break;
		}

		const std::size_t index = (startIndex + examined) % enabled->size();
		++examined;
		const auto& record = (*enabled)[index];
		if (!record || !record->Active.load(std::memory_order_acquire))
			continue;
		const std::uint64_t now = MonotonicMicroseconds();
		if (record->NextDueMonotonicUs.load(std::memory_order_acquire) > now)
			continue;

		const std::uint64_t intervalUs =
			static_cast<std::uint64_t>(record->Spec.IntervalMs) * 1000ULL;
		record->NextDueMonotonicUs.store(
			SaturatingAdd(now, intervalUs),
			std::memory_order_release);
		const std::size_t remainingBytes = m_Limits.MaxFrameBytes - sampledBytes;
		const std::size_t sampleLimit = (std::min)(m_Limits.MaxSampleBytes, remainingBytes);
		WatchSampleResult result;
		if (m_Source.SessionId() != m_SessionId
			|| m_Source.ContextGeneration() != m_ContextGeneration)
		{
			result.Status = WatchSampleStatus::Failed;
			result.ReasonCode = "WATCH_SOURCE_GENERATION_MISMATCH";
			result.Reason = "Watch sample source no longer matches the scheduler envelope";
		}
		else if (!m_Source.IsCurrentExecutionThreadValid())
		{
			result.Status = WatchSampleStatus::Failed;
			result.ReasonCode = "WATCH_EXECUTION_THREAD_INVALID";
			result.Reason = "Watch sampling must run on the witnessed game thread";
		}
		else
		{
			try
			{
				if (!record->Binding
					|| record->Binding->SourceIdentity() != static_cast<const void*>(&m_Source))
				{
					result = {
						.Status = WatchSampleStatus::Stale,
						.ReasonCode = "WATCH_BINDING_STALE",
						.Reason = "The subscription no longer owns its exact source binding"
					};
				}
				else
				{
					result = m_Source.Sample(record->Spec, record->Binding, sampleLimit);
				}
			}
			catch (...)
			{
				result = {
					.Status = WatchSampleStatus::Failed,
					.ReasonCode = "WATCH_SOURCE_EXCEPTION",
					.Reason = "Watch sample source raised an unexpected exception"
				};
			}
		}

		const std::size_t producedBytes = PayloadBytes(result.Payload);
		if ((result.Status == WatchSampleStatus::Value && producedBytes > sampleLimit)
			|| result.ReasonCode.size() > 1024 || result.Reason.size() > 4096)
		{
			result = {
				.Status = WatchSampleStatus::Failed,
				.ReasonCode = "WATCH_SOURCE_CONTRACT_VIOLATION",
				.Reason = "Watch sample source exceeded its bounded output contract"
			};
		}

		std::size_t consumed = 0;
		try
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			terminalObserved = ApplySampleLocked(
				record,
				std::move(result),
				MonotonicMicroseconds(),
				consumed) || terminalObserved;
		}
		catch (...)
		{
			// Allocation failure cannot be reported by invoking an unbounded fallback.
			// The next interval remains eligible and scheduler diagnostics stay live.
		}
		sampledBytes = SaturatingSizeAdd(sampledBytes, consumed);
		++sampled;
	}

	m_NextPumpIndex.store((startIndex + examined) % enabled->size(), std::memory_order_release);
	if (terminalObserved)
	{
		try
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			PublishEnabledSnapshotLocked(BuildEnabledSnapshotLocked());
		}
		catch (...)
		{
			// Terminal records are already atomically inactive, even if compaction fails.
		}
	}
	const std::uint64_t elapsed = ElapsedMicroseconds(started);
	const bool moreDue = HasDueWork(enabled, MonotonicMicroseconds());
	if (sampled >= itemLimit && moreDue)
		m_ItemBudgetExhaustions.fetch_add(1, std::memory_order_acq_rel);
	if (byteExhausted && moreDue)
		m_ByteBudgetExhaustions.fetch_add(1, std::memory_order_acq_rel);
	if (timeExhausted && moreDue)
		m_TimeBudgetExhaustions.fetch_add(1, std::memory_order_acq_rel);
	m_LastPumpDurationUs.store(elapsed, std::memory_order_release);
	m_LastPumpItems.store(sampled, std::memory_order_release);
	m_LastPumpBytes.store(sampledBytes, std::memory_order_release);
	m_PumpCount.fetch_add(1, std::memory_order_acq_rel);
	return {.WorkConsumed = sampled, .MoreWorkPending = moreDue};
}

WatchDrainResult WatchScheduler::DrainEvents(const std::size_t maxEvents) noexcept
{
	if (maxEvents == 0 || maxEvents > m_Limits.MaxPendingEvents)
		return {.Error = WatchError::InvalidLimit};
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const std::size_t count = (std::min)(maxEvents, m_Events.size());
		WatchDrainResult result;
		result.Events.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
			result.Events.push_back(m_Events[index]);
		for (std::size_t index = 0; index < count; ++index)
		{
			const std::size_t bytes = EventBytes(m_Events.front());
			m_PendingEventBytes = bytes <= m_PendingEventBytes
				? m_PendingEventBytes - bytes
				: 0;
			m_Events.pop_front();
		}
		result.DroppedTotal = m_DroppedEvents;
		result.CoalescedTotal = m_CoalescedEvents;
		result.MoreAvailable = !m_Events.empty();
		return result;
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed};
	}
}

WatchPushDrainResult WatchScheduler::DrainPushEvents(const std::size_t maxEvents) noexcept
{
	if (maxEvents == 0 || maxEvents > m_Limits.MaxPendingEvents)
		return {.Error = WatchError::InvalidLimit};
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const std::size_t count = (std::min)(maxEvents, m_PushEvents.size());
		WatchPushDrainResult result;
		result.Events.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
			result.Events.push_back(m_PushEvents[index]);
		for (std::size_t index = 0; index < count; ++index)
		{
			const std::size_t bytes = EventBytes(m_PushEvents.front());
			m_PendingPushEventBytes = bytes <= m_PendingPushEventBytes
				? m_PendingPushEventBytes - bytes
				: 0;
			m_PushEvents.pop_front();
		}
		result.DroppedTotal = m_DroppedPushEvents;
		result.CoalescedTotal = m_CoalescedPushEvents;
		result.MoreAvailable = !m_PushEvents.empty();
		return result;
	}
	catch (...)
	{
		return {.Error = WatchError::AllocationFailed};
	}
}

WatchSchedulerSnapshot WatchScheduler::Snapshot() const noexcept
{
	WatchSchedulerSnapshot snapshot{
		.Configured = m_Configured,
		.Stopping = m_StopRequested.load(std::memory_order_acquire),
		.Stopped = m_Stopped.load(std::memory_order_acquire),
		.Pumping = m_Pumping.load(std::memory_order_acquire),
		.PumpCount = m_PumpCount.load(std::memory_order_acquire),
		.LastPumpDurationUs = m_LastPumpDurationUs.load(std::memory_order_acquire),
		.LastPumpItems = m_LastPumpItems.load(std::memory_order_acquire),
		.LastPumpBytes = m_LastPumpBytes.load(std::memory_order_acquire),
		.ItemBudgetExhaustions = m_ItemBudgetExhaustions.load(std::memory_order_acquire),
		.ByteBudgetExhaustions = m_ByteBudgetExhaustions.load(std::memory_order_acquire),
		.TimeBudgetExhaustions = m_TimeBudgetExhaustions.load(std::memory_order_acquire),
		.ConcurrentPumpRejections = m_ConcurrentPumpRejections.load(std::memory_order_acquire)
	};
	std::lock_guard<std::mutex> lock(m_Mutex);
	snapshot.EnabledSnapshotGeneration = m_EnabledSnapshotGeneration;
	snapshot.SubscriptionCount = m_Subscriptions.size();
	for (const auto& record : m_Subscriptions)
	{
		if (record && record->Enabled && !record->Terminal)
			++snapshot.EnabledCount;
	}
	snapshot.PendingEventCount = m_Events.size();
	snapshot.PendingEventBytes = m_PendingEventBytes;
	snapshot.DroppedEvents = m_DroppedEvents;
	snapshot.CoalescedEvents = m_CoalescedEvents;
	snapshot.PendingPushEventCount = m_PushEvents.size();
	snapshot.PendingPushEventBytes = m_PendingPushEventBytes;
	snapshot.DroppedPushEvents = m_DroppedPushEvents;
	snapshot.CoalescedPushEvents = m_CoalescedPushEvents;
	snapshot.LastEventSequence = m_NextEventSequence > 1 ? m_NextEventSequence - 1 : 0;
	return snapshot;
}

WatchStopResult WatchScheduler::StopAndDrain(const std::chrono::milliseconds timeout)
{
	if (timeout.count() < 0)
		return {.Error = WatchError::InvalidLimit};
	if (m_Stopped.load(std::memory_order_acquire))
		return {};
	m_StopRequested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (const auto& record : m_Subscriptions)
		{
			if (!record)
				continue;
			record->Enabled = false;
			record->Active.store(false, std::memory_order_release);
			record->NextDueMonotonicUs.store(0, std::memory_order_release);
		}
		PublishEnabledSnapshotLocked(m_EmptyEnabledSnapshot);
	}
	m_PumpBarrier.BeginStopping();
	if (!m_PumpBarrier.WaitForDrain(timeout))
		return {.Error = WatchError::DrainTimedOut};
	m_Stopped.store(true, std::memory_order_release);
	return {};
}

} // namespace UExplorer::Runtime
