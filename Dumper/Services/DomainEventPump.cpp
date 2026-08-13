#include "DomainEventPump.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace UExplorer::Services
{
namespace
{

constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;

bool ValidLimits(const DomainEventPumpLimits& limits) noexcept
{
	return limits.MaxWatchEventsPerBatch > 0
		&& limits.MaxWatchEventsPerBatch <= Runtime::WatchScheduler::kHardMaxPendingEvents
		&& limits.MaxHookEventsPerBatch > 0
		&& limits.MaxHookEventsPerBatch <= HookCommandService::kHardMaxDrainBatch
		&& limits.MaxSerializedDataBytes >= 1024
		&& limits.MaxSerializedDataBytes < IPC::NamedPipeRpcServer::kMaxEventPayloadBytes
		&& limits.ShutdownQuietPasses > 0
		&& limits.ShutdownQuietPasses <= 1024
		&& limits.PollInterval.count() > 0
		&& limits.PollInterval <= std::chrono::seconds(1)
		&& limits.ShutdownQuietInterval.count() > 0
		&& limits.ShutdownQuietInterval <= std::chrono::milliseconds(100);
}

std::string Base64(const std::span<const std::byte> bytes)
{
	static constexpr char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(((bytes.size() + 2) / 3) * 4);
	for (std::size_t offset = 0; offset < bytes.size(); offset += 3)
	{
		const std::uint32_t first = std::to_integer<std::uint8_t>(bytes[offset]);
		const std::uint32_t second = offset + 1 < bytes.size()
			? std::to_integer<std::uint8_t>(bytes[offset + 1]) : 0;
		const std::uint32_t third = offset + 2 < bytes.size()
			? std::to_integer<std::uint8_t>(bytes[offset + 2]) : 0;
		const std::uint32_t word = (first << 16) | (second << 8) | third;
		encoded.push_back(alphabet[(word >> 18) & 0x3F]);
		encoded.push_back(alphabet[(word >> 12) & 0x3F]);
		encoded.push_back(offset + 1 < bytes.size() ? alphabet[(word >> 6) & 0x3F] : '=');
		encoded.push_back(offset + 2 < bytes.size() ? alphabet[word & 0x3F] : '=');
	}
	return encoded;
}

std::string Hex(const std::span<const std::byte> bytes)
{
	static constexpr std::array<char, 16> digits{
		'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
	std::string encoded(bytes.size() * 2, '0');
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index]);
		encoded[index * 2] = digits[value >> 4];
		encoded[index * 2 + 1] = digits[value & 0x0F];
	}
	return encoded;
}

const char* WatchKind(const Runtime::WatchEventKind kind) noexcept
{
	switch (kind)
	{
	case Runtime::WatchEventKind::ValueChanged: return "watch.value_changed";
	case Runtime::WatchEventKind::SampleUnavailable: return "watch.sample_unavailable";
	case Runtime::WatchEventKind::SampleFailed: return "watch.sample_failed";
	case Runtime::WatchEventKind::TerminalStale: return "watch.terminal_stale";
	}
	return "watch.invalid";
}

const char* HookKind(const Runtime::HookEventKind kind) noexcept
{
	switch (kind)
	{
	case Runtime::HookEventKind::ProcessEventEnter: return "hook.process_event_enter";
	case Runtime::HookEventKind::ProcessEventExit: return "hook.process_event_exit";
	case Runtime::HookEventKind::PostRender: return "hook.post_render";
	case Runtime::HookEventKind::Diagnostic: return "hook.diagnostic";
	case Runtime::HookEventKind::Count: break;
	}
	return "hook.invalid";
}

nlohmann::json SerializeWatchValue(
	const std::shared_ptr<const Runtime::WatchSamplePayload>& value)
{
	if (!value)
		return nullptr;
	return {
		{"encoding", "uexplorer.property-value.v1.base64"},
		{"type_name", value->TypeName},
		{"canonical_value", Base64(value->CanonicalValue)},
		{"display_value", value->DisplayValue}
	};
}

} // namespace

const char* ToString(const DomainEventPumpError error) noexcept
{
	switch (error)
	{
	case DomainEventPumpError::None: return "NONE";
	case DomainEventPumpError::InvalidConfiguration: return "DOMAIN_EVENT_PUMP_INVALID_CONFIGURATION";
	case DomainEventPumpError::AlreadyStarted: return "DOMAIN_EVENT_PUMP_ALREADY_STARTED";
	case DomainEventPumpError::WorkerStartFailed: return "DOMAIN_EVENT_PUMP_WORKER_START_FAILED";
	case DomainEventPumpError::InvalidDeadline: return "DOMAIN_EVENT_PUMP_DEADLINE_INVALID";
	case DomainEventPumpError::WorkerThreadDrainDenied: return "DOMAIN_EVENT_PUMP_WORKER_DRAIN_DENIED";
	case DomainEventPumpError::DrainTimedOut: return "DOMAIN_EVENT_PUMP_DRAIN_TIMED_OUT";
	case DomainEventPumpError::WatchDrainFailed: return "DOMAIN_EVENT_PUMP_WATCH_DRAIN_FAILED";
	case DomainEventPumpError::HookDrainFailed: return "DOMAIN_EVENT_PUMP_HOOK_DRAIN_FAILED";
	case DomainEventPumpError::PublishFailed: return "DOMAIN_EVENT_PUMP_PUBLISH_FAILED";
	case DomainEventPumpError::WorkerExitedUnexpectedly: return "DOMAIN_EVENT_PUMP_WORKER_EXITED_UNEXPECTEDLY";
	}
	return "DOMAIN_EVENT_PUMP_UNKNOWN_ERROR";
}

DomainEventPump::DomainEventPump(
	Runtime::WatchScheduler& watches,
	HookCommandService& hooks,
	IPC::NamedPipeRpcServer& pipe,
	DomainEventPumpLimits limits) noexcept
	: m_Watches(watches), m_Hooks(hooks), m_Pipe(pipe), m_Limits(limits)
{
	m_Configured = watches.IsConfigured()
		&& hooks.IsConfigured()
		&& watches.SessionId() == hooks.SessionId()
		&& watches.ContextGeneration() == hooks.ContextGeneration()
		&& ValidLimits(m_Limits);
	if (!m_Configured)
		SetError(DomainEventPumpError::InvalidConfiguration);
}

DomainEventPump::~DomainEventPump()
{
	m_StopRequested.store(true, std::memory_order_release);
	m_LifecycleCondition.notify_all();
	JoinWorkerNoexcept();
}

void DomainEventPump::SetError(const DomainEventPumpError error) noexcept
{
	if (error != DomainEventPumpError::None)
		m_LastError.store(error, std::memory_order_release);
}

bool DomainEventPump::Start() noexcept
{
	if (!m_Configured)
		return false;
	bool expected = false;
	if (!m_Started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		SetError(DomainEventPumpError::AlreadyStarted);
		return false;
	}
	try
	{
		m_Worker = std::thread(&DomainEventPump::WorkerLoop, this);
		return true;
	}
	catch (...)
	{
		m_Started.store(false, std::memory_order_release);
		SetError(DomainEventPumpError::WorkerStartFailed);
		return false;
	}
}

bool DomainEventPump::Publish(
	const char* const kind,
	const std::uint64_t timestampUs,
	nlohmann::json data,
	const bool watchEvent) noexcept
{
	try
	{
		data["publisher_dropped_before"] =
			m_PublishDroppedEvents.load(std::memory_order_acquire);
		const IPC::EventPublishResult result = m_Pipe.PublishEvent(
			kind,
			(std::min)(timestampUs, kMaxProtocolInteger),
			std::move(data));
		if (result == IPC::EventPublishResult::Accepted
			|| result == IPC::EventPublishResult::DroppedOldest)
		{
			if (result == IPC::EventPublishResult::DroppedOldest)
				m_TransportQueueEvictions.fetch_add(1, std::memory_order_relaxed);
			if (watchEvent)
				m_PublishedWatchEvents.fetch_add(1, std::memory_order_relaxed);
			else
				m_PublishedHookEvents.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		m_PublishDroppedEvents.fetch_add(1, std::memory_order_relaxed);
		SetError(DomainEventPumpError::PublishFailed);
		return false;
	}
	catch (...)
	{
		m_PublishDroppedEvents.fetch_add(1, std::memory_order_relaxed);
		SetError(DomainEventPumpError::PublishFailed);
		return false;
	}
}

bool DomainEventPump::DrainWatchBatch() noexcept
{
	const Runtime::WatchPushDrainResult drained =
		m_Watches.DrainPushEvents(m_Limits.MaxWatchEventsPerBatch);
	if (!drained.Ok())
	{
		SetError(DomainEventPumpError::WatchDrainFailed);
		return false;
	}
	m_WatchSourceDroppedEvents.store(drained.DroppedTotal, std::memory_order_release);
	m_WatchSourceCoalescedEvents.store(drained.CoalescedTotal, std::memory_order_release);
	for (const Runtime::WatchEvent& event : drained.Events)
	{
		try
		{
			nlohmann::json data = {
				{"watch_id", std::to_string(event.Id)},
				{"id", event.Id},
				{"source_sequence", event.Sequence},
				{"captured_at_monotonic_us", event.CapturedAtMonotonicUs},
				{"value", nullptr},
				{"value_omitted", false},
				{"reason_code", event.ReasonCode.empty() ? nlohmann::json(nullptr) : nlohmann::json(event.ReasonCode)},
				{"reason", event.Reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(event.Reason)},
				{"pull_dropped_before", event.DropCount},
				{"pull_coalesced_before", event.CoalesceCount},
				{"push_dropped_before", event.PushDroppedBefore},
				{"push_coalesced_before", event.PushCoalescedBefore}
			};
			bool valueSerialized = false;
			if (event.Value)
			{
				const std::size_t estimatedEncodedBytes =
					((event.Value->CanonicalValue.size() + 2) / 3) * 4;
				const std::size_t estimatedValueBytes = estimatedEncodedBytes
					+ event.Value->TypeName.size()
					+ event.Value->DisplayValue.size()
					+ 256;
				if (estimatedValueBytes <= m_Limits.MaxSerializedDataBytes)
				{
					data["value"] = SerializeWatchValue(event.Value);
					valueSerialized = true;
				}
			}
			if (event.Value && (!valueSerialized
				|| data.dump().size() > m_Limits.MaxSerializedDataBytes))
			{
				data["value"] = nullptr;
				data["value_omitted"] = true;
				data["value_omission_code"] = "WATCH_PUSH_VALUE_TOO_LARGE";
				m_PayloadOmissions.fetch_add(1, std::memory_order_relaxed);
			}
			Publish(WatchKind(event.Kind), event.CapturedAtMonotonicUs, std::move(data), true);
		}
		catch (...)
		{
			m_PublishDroppedEvents.fetch_add(1, std::memory_order_relaxed);
			SetError(DomainEventPumpError::PublishFailed);
		}
	}
	return drained.MoreAvailable;
}

bool DomainEventPump::DrainHookBatch() noexcept
{
	const HookPushDrainResult drained = m_Hooks.DrainPushEvents(m_Limits.MaxHookEventsPerBatch);
	if (!drained.Ok())
	{
		SetError(DomainEventPumpError::HookDrainFailed);
		return false;
	}
	m_HookSourceDroppedEvents.store(drained.QueueDroppedTotal, std::memory_order_release);
	for (const HookPushEvent& event : drained.Events)
	{
		try
		{
			const std::size_t estimatedPayloadBytes = event.Payload.size() * 2 + 128;
			const bool includePayload = estimatedPayloadBytes <= m_Limits.MaxSerializedDataBytes;
			nlohmann::json data = {
				{"hook_name", event.FunctionPath},
				{"hook_id", std::to_string(event.SubscriptionId)},
				{"id", event.SubscriptionId},
				{"function_path", event.FunctionPath},
				{"source_sequence", event.Sequence},
				{"configuration_generation", event.ConfigurationGeneration},
				{"source", std::format("0x{:X}", event.Source)},
				{"correlation", event.Correlation},
				{"coalesced_before", event.CoalescedBefore},
				{"drained_at_monotonic_us", event.DrainedAtMonotonicUs},
				{"capture", {{"mode", ToString(event.Capture.Mode)}}},
				{"payload", includePayload
					? nlohmann::json{{"encoding", "hex"}, {"size", event.Payload.size()}, {"data", Hex(event.Payload)}}
					: nlohmann::json(nullptr)},
				{"payload_omitted", !includePayload},
				{"retained_log_dropped_before", event.RetainedLogDroppedBefore},
				{"collector_overflow_dropped_before", event.CollectorOverflowDroppedBefore},
				{"collector_oversize_dropped_before", event.CollectorOversizeDroppedBefore},
				{"collector_contention_dropped_before", event.CollectorContentionDroppedBefore},
				{"push_dropped_before", event.QueueDroppedBefore}
			};
			if (!includePayload || data.dump().size() > m_Limits.MaxSerializedDataBytes)
			{
				data["payload"] = nullptr;
				data["payload_omitted"] = true;
				data["payload_omission_code"] = "HOOK_PUSH_PAYLOAD_TOO_LARGE";
				m_PayloadOmissions.fetch_add(1, std::memory_order_relaxed);
			}
			Publish(HookKind(event.Kind), event.DrainedAtMonotonicUs, std::move(data), false);
		}
		catch (...)
		{
			m_PublishDroppedEvents.fetch_add(1, std::memory_order_relaxed);
			SetError(DomainEventPumpError::PublishFailed);
		}
	}
	return drained.MoreAvailable;
}

void DomainEventPump::WorkerLoop() noexcept
{
	bool expectedStop = false;
	try
	{
		{
			std::lock_guard<std::mutex> lock(m_LifecycleMutex);
			m_WorkerId = std::this_thread::get_id();
		}
		m_Running.store(true, std::memory_order_release);
		std::size_t idleShutdownBatches = 0;
		for (;;)
		{
			const bool watchMore = DrainWatchBatch();
			const bool hookMore = DrainHookBatch();
			const DomainEventPumpError error = m_LastError.load(std::memory_order_acquire);
			if (error == DomainEventPumpError::WatchDrainFailed
				|| error == DomainEventPumpError::HookDrainFailed)
			{
				break;
			}
			const bool stopping = m_StopRequested.load(std::memory_order_acquire);
			if (stopping)
			{
				if (watchMore || hookMore)
				{
					idleShutdownBatches = 0;
					continue;
				}
				if (++idleShutdownBatches >= m_Limits.ShutdownQuietPasses)
				{
					expectedStop = true;
					break;
				}
				std::this_thread::sleep_for(m_Limits.ShutdownQuietInterval);
				continue;
			}
			if (!watchMore && !hookMore)
			{
				std::unique_lock<std::mutex> lock(m_LifecycleMutex);
				m_LifecycleCondition.wait_for(lock, m_Limits.PollInterval, [this] {
					return m_StopRequested.load(std::memory_order_acquire);
				});
			}
		}
	}
	catch (...)
	{
		SetError(DomainEventPumpError::WorkerExitedUnexpectedly);
	}
	if (!expectedStop && m_LastError.load(std::memory_order_acquire) == DomainEventPumpError::None)
		SetError(DomainEventPumpError::WorkerExitedUnexpectedly);
	m_Running.store(false, std::memory_order_release);
	m_Stopped.store(true, std::memory_order_release);
	m_Exited.store(true, std::memory_order_release);
	m_LifecycleCondition.notify_all();
}

DomainEventPumpStopResult DomainEventPump::StopAndDrain(
	const std::chrono::milliseconds timeout) noexcept
{
	if (timeout.count() < 0)
		return {.Error = DomainEventPumpError::InvalidDeadline};
	if (!m_Started.load(std::memory_order_acquire))
		return {};
	{
		std::lock_guard<std::mutex> lock(m_LifecycleMutex);
		if (m_WorkerId == std::this_thread::get_id())
			return {.Error = DomainEventPumpError::WorkerThreadDrainDenied};
	}
	m_StopRequested.store(true, std::memory_order_release);
	m_LifecycleCondition.notify_all();
	std::unique_lock<std::mutex> lock(m_LifecycleMutex);
	if (!m_LifecycleCondition.wait_for(lock, timeout, [this] {
		return m_Exited.load(std::memory_order_acquire);
	}))
	{
		SetError(DomainEventPumpError::DrainTimedOut);
		return {.Error = DomainEventPumpError::DrainTimedOut};
	}
	lock.unlock();
	JoinWorkerNoexcept();
	const DomainEventPumpError workerError = m_LastError.load(std::memory_order_acquire);
	if (workerError == DomainEventPumpError::WatchDrainFailed
		|| workerError == DomainEventPumpError::HookDrainFailed
		|| workerError == DomainEventPumpError::WorkerExitedUnexpectedly)
	{
		return {.Error = workerError};
	}
	return {};
}

void DomainEventPump::JoinWorkerNoexcept() noexcept
{
	try
	{
		if (m_Worker.joinable() && m_Worker.get_id() != std::this_thread::get_id())
			m_Worker.join();
	}
	catch (...)
	{
	}
}

DomainEventPumpDiagnostics DomainEventPump::Diagnostics() const noexcept
{
	return {
		.Configured = m_Configured,
		.Started = m_Started.load(std::memory_order_acquire),
		.Running = m_Running.load(std::memory_order_acquire),
		.StopRequested = m_StopRequested.load(std::memory_order_acquire),
		.Stopped = m_Stopped.load(std::memory_order_acquire),
		.PublishedWatchEvents = m_PublishedWatchEvents.load(std::memory_order_acquire),
		.PublishedHookEvents = m_PublishedHookEvents.load(std::memory_order_acquire),
		.PublishDroppedEvents = m_PublishDroppedEvents.load(std::memory_order_acquire),
		.TransportQueueEvictions = m_TransportQueueEvictions.load(std::memory_order_acquire),
		.PayloadOmissions = m_PayloadOmissions.load(std::memory_order_acquire),
		.WatchSourceDroppedEvents = m_WatchSourceDroppedEvents.load(std::memory_order_acquire),
		.WatchSourceCoalescedEvents = m_WatchSourceCoalescedEvents.load(std::memory_order_acquire),
		.HookSourceDroppedEvents = m_HookSourceDroppedEvents.load(std::memory_order_acquire),
		.LastError = m_LastError.load(std::memory_order_acquire)
	};
}

} // namespace UExplorer::Services
