#include "HookCommandService.h"

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <system_error>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxReasonCodeBytes = 128;
constexpr std::size_t kMaxReasonBytes = 2048;

std::uint64_t MonotonicMicroseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsBoundedText(const std::string& value, const std::size_t maximum) noexcept
{
	return !value.empty() && value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

bool TryUnsigned(
	const json& value,
	const std::uint64_t minimum,
	const std::uint64_t maximum,
	std::uint64_t& parsed)
{
	parsed = 0;
	if (!value.is_number_integer())
		return false;
	if (value.is_number_unsigned())
	{
		const std::uint64_t candidate = value.get<std::uint64_t>();
		if (candidate < minimum || candidate > maximum)
			return false;
		parsed = candidate;
		return true;
	}
	const std::int64_t candidate = value.get<std::int64_t>();
	if (candidate < 0)
		return false;
	const auto converted = static_cast<std::uint64_t>(candidate);
	if (converted < minimum || converted > maximum)
		return false;
	parsed = converted;
	return true;
}

bool IsUpperHex(const char character) noexcept
{
	return (character >= '0' && character <= '9')
		|| (character >= 'A' && character <= 'F');
}

bool TryCanonicalHex(
	const std::string& encoded,
	const bool prefixed,
	const bool fixedWidth,
	std::uint64_t& parsed) noexcept
{
	parsed = 0;
	const std::size_t prefix = prefixed ? 2 : 0;
	if ((prefixed && !encoded.starts_with("0x"))
		|| encoded.size() <= prefix
		|| encoded.size() - prefix > 16
		|| (fixedWidth && encoded.size() - prefix != 16)
		|| (!fixedWidth && encoded.size() - prefix > 1 && encoded[prefix] == '0'))
	{
		return false;
	}
	const std::string_view digits(encoded.data() + prefix, encoded.size() - prefix);
	if (!std::all_of(digits.begin(), digits.end(), IsUpperHex))
		return false;
	const auto converted = std::from_chars(
		digits.data(), digits.data() + digits.size(), parsed, 16);
	return converted.ec == std::errc{} && converted.ptr == digits.data() + digits.size();
}

bool TryParseObjectHandle(const json& data, Runtime::ObjectHandle& handle)
{
	handle = {};
	if (!data.is_object() || data.size() != 6
		|| !data.contains("session_id")
		|| !data.contains("context_generation")
		|| !data.contains("index")
		|| !data.contains("serial")
		|| !data.contains("address")
		|| !data.contains("class_fingerprint")
		|| !data.at("session_id").is_string()
		|| !data.at("address").is_string()
		|| !data.at("class_fingerprint").is_string())
	{
		return false;
	}

	handle.SessionId = data.at("session_id").get<std::string>();
	std::uint64_t contextGeneration = 0;
	std::uint64_t index = 0;
	std::uint64_t serial = 0;
	std::uint64_t address = 0;
	std::uint64_t classFingerprint = 0;
	if (!IsBoundedText(handle.SessionId, kMaxSessionBytes)
		|| !TryUnsigned(
			data.at("context_generation"),
			1,
			HookCommandService::kMaxProtocolInteger,
			contextGeneration)
		|| !TryUnsigned(
			data.at("index"),
			0,
			static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()),
			index)
		|| !TryUnsigned(
			data.at("serial"),
			1,
			static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()),
			serial)
		|| !TryCanonicalHex(data.at("address").get_ref<const std::string&>(), true, false, address)
		|| address == 0
		|| !TryCanonicalHex(
			data.at("class_fingerprint").get_ref<const std::string&>(),
			false,
			true,
			classFingerprint)
		|| classFingerprint == 0
		|| address > (std::numeric_limits<std::uintptr_t>::max)())
	{
		return false;
	}
	handle.ContextGeneration = contextGeneration;
	handle.Index = static_cast<std::int32_t>(index);
	handle.SerialNumber = static_cast<std::int32_t>(serial);
	handle.Address = static_cast<std::uintptr_t>(address);
	handle.ClassFingerprint = classFingerprint;
	return true;
}

bool TryParseFunctionHandle(const json& data, Runtime::FunctionHandle& handle)
{
	handle = {};
	if (!data.is_object() || data.size() != 4
		|| !data.contains("function")
		|| !data.contains("owner")
		|| !data.contains("full_path")
		|| !data.contains("signature_fingerprint")
		|| !data.at("full_path").is_string()
		|| !data.at("signature_fingerprint").is_string()
		|| !TryParseObjectHandle(data.at("function"), handle.Function)
		|| !TryParseObjectHandle(data.at("owner"), handle.Owner))
	{
		return false;
	}
	handle.FullPath = data.at("full_path").get<std::string>();
	std::uint64_t signature = 0;
	if (!IsBoundedText(handle.FullPath, kMaxPathBytes)
		|| !TryCanonicalHex(
			data.at("signature_fingerprint").get_ref<const std::string&>(),
			false,
			true,
			signature)
		|| signature == 0)
	{
		return false;
	}
	handle.SignatureFingerprint = signature;
	return true;
}

bool SameObjectHandle(
	const Runtime::ObjectHandle& left,
	const Runtime::ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameFunctionHandle(
	const Runtime::FunctionHandle& left,
	const Runtime::FunctionHandle& right) noexcept
{
	return SameObjectHandle(left.Function, right.Function)
		&& SameObjectHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
}

bool SameProducerKey(
	const HookSubscriptionSpec& left,
	const HookSubscriptionSpec& right) noexcept
{
	return left.ObjectSnapshotGeneration == right.ObjectSnapshotGeneration
		&& left.TypeSnapshotGeneration == right.TypeSnapshotGeneration
		&& SameFunctionHandle(left.Function, right.Function);
}

bool TryParseCapturePolicy(const json& data, HookCapturePolicy& policy)
{
	policy = {};
	if (!data.is_object() || !data.contains("mode") || !data.at("mode").is_string())
		return false;
	const std::string& mode = data.at("mode").get_ref<const std::string&>();
	if (mode == "fixed_metadata")
	{
		if (data.size() != 1)
			return false;
		policy.Mode = HookCaptureMode::FixedMetadata;
		policy.MaxPayloadBytes = 0;
		return true;
	}
	if (mode != "preencoded_payload" || data.size() != 2
		|| !data.contains("max_payload_bytes"))
	{
		return false;
	}
	std::uint64_t maximum = 0;
	if (!TryUnsigned(
		data.at("max_payload_bytes"),
		1,
		Runtime::HookEventCollector::kHardMaxPayloadBytes,
		maximum))
	{
		return false;
	}
	policy.Mode = HookCaptureMode::PreEncodedPayload;
	policy.MaxPayloadBytes = static_cast<std::size_t>(maximum);
	return true;
}

std::uint64_t Mix64(std::uint64_t value) noexcept
{
	value ^= value >> 30;
	value *= 0xBF58476D1CE4E5B9ULL;
	value ^= value >> 27;
	value *= 0x94D049BB133111EBULL;
	return value ^ (value >> 31);
}

std::uint64_t HashText(const std::string_view text) noexcept
{
	std::uint64_t value = 0xCBF29CE484222325ULL;
	for (const unsigned char character : text)
	{
		value ^= character;
		value *= 0x100000001B3ULL;
	}
	return Mix64(value);
}

void MixProducerKey(std::uint64_t& hash, const std::uint64_t value) noexcept
{
	hash = Mix64(hash ^ Mix64(value + 0x9E3779B97F4A7C15ULL));
}

std::uint64_t ProducerKeyHash(
	const Runtime::FunctionHandle& function,
	const std::uint64_t objectSnapshotGeneration,
	const std::uint64_t typeSnapshotGeneration) noexcept
{
	std::uint64_t value = HashText(function.Function.SessionId);
	MixProducerKey(value, function.Function.ContextGeneration);
	MixProducerKey(value, objectSnapshotGeneration);
	MixProducerKey(value, typeSnapshotGeneration);
	MixProducerKey(value, static_cast<std::uint32_t>(function.Function.Index));
	MixProducerKey(value, static_cast<std::uint32_t>(function.Function.SerialNumber));
	MixProducerKey(value, static_cast<std::uint64_t>(function.Function.Address));
	MixProducerKey(value, function.Function.ClassFingerprint);
	MixProducerKey(value, HashText(function.Owner.SessionId));
	MixProducerKey(value, function.Owner.ContextGeneration);
	MixProducerKey(value, static_cast<std::uint32_t>(function.Owner.Index));
	MixProducerKey(value, static_cast<std::uint32_t>(function.Owner.SerialNumber));
	MixProducerKey(value, static_cast<std::uint64_t>(function.Owner.Address));
	MixProducerKey(value, function.Owner.ClassFingerprint);
	MixProducerKey(value, HashText(function.FullPath));
	MixProducerKey(value, function.SignatureFingerprint);
	return value;
}

bool ValidLimits(const HookCommandLimits& limits) noexcept
{
	return limits.MaxSubscriptions > 0
		&& limits.MaxSubscriptions <= HookCommandService::kHardMaxSubscriptions
		&& limits.MaxLogEntriesPerSubscription > 0
		&& limits.MaxLogEntriesPerSubscription <= HookCommandService::kHardMaxLogEntries
		&& limits.MaxLogBytesPerSubscription > 0
		&& limits.MaxLogBytesPerSubscription <= HookCommandService::kHardMaxLogBytes
		&& limits.MaxDrainBatch > 0
		&& limits.MaxDrainBatch <= HookCommandService::kHardMaxDrainBatch;
}

HookCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Data = nullptr,
		.Error = HookCommandError{
			.Code = std::move(code),
			.Message = std::move(message),
			.Details = std::move(details)
		}
	};
}

const char* ErrorMessage(const HookSubscriptionError error) noexcept
{
	switch (error)
	{
	case HookSubscriptionError::None: return "The hook operation succeeded";
	case HookSubscriptionError::InvalidConfiguration:
		return "The hook subscription service is not configured";
	case HookSubscriptionError::InvalidSpec:
		return "The exact function subscription identity is invalid";
	case HookSubscriptionError::SessionMismatch:
		return "The function handle belongs to a different session";
	case HookSubscriptionError::ContextGenerationMismatch:
		return "The function handle belongs to a different engine context generation";
	case HookSubscriptionError::SnapshotGenerationInvalid:
		return "The object or type snapshot generation is invalid";
	case HookSubscriptionError::CurrentSnapshotUnavailable:
		return "No current immutable object/type snapshot proof is available";
	case HookSubscriptionError::SnapshotGenerationMismatch:
		return "The requested object/type generations are not the current immutable generations";
	case HookSubscriptionError::FunctionSnapshotMismatch:
		return "The function handle, owner, path, or signature is absent from the current immutable snapshots";
	case HookSubscriptionError::CapturePolicyInvalid:
		return "The hook capture policy is unsupported or exceeds its byte bound";
	case HookSubscriptionError::Duplicate:
		return "A subscription already owns the exact producer lookup identity";
	case HookSubscriptionError::CapacityExceeded:
		return "The bounded hook subscription capacity is exhausted";
	case HookSubscriptionError::IdExhausted:
		return "The monotonic hook subscription identifier is exhausted";
	case HookSubscriptionError::SnapshotGenerationExhausted:
		return "The immutable enabled-snapshot generation is exhausted";
	case HookSubscriptionError::NotFound:
		return "The hook subscription identifier does not exist";
	case HookSubscriptionError::Terminal:
		return "The hook subscription is terminal and cannot be re-enabled";
	case HookSubscriptionError::InvalidLimit:
		return "The requested hook log or drain limit is invalid";
	case HookSubscriptionError::AllocationFailed:
		return "The bounded hook operation could not allocate owned state";
	case HookSubscriptionError::CollectorDrainFailed:
		return "The hook event collector could not be drained by the worker";
	}
	return "The hook operation failed";
}

HookCommandResult SubscriptionFailure(
	const HookSubscriptionError error,
	json details = json::object())
{
	return Failure(ToString(error), ErrorMessage(error), std::move(details));
}

json SerializeObjectHandle(const Runtime::ObjectHandle& handle)
{
	return {
		{"session_id", handle.SessionId},
		{"context_generation", handle.ContextGeneration},
		{"index", handle.Index},
		{"serial", handle.SerialNumber},
		{"address", std::format("0x{:X}", handle.Address)},
		{"class_fingerprint", std::format("{:016X}", handle.ClassFingerprint)}
	};
}

json SerializeFunctionHandle(const Runtime::FunctionHandle& handle)
{
	return {
		{"function", SerializeObjectHandle(handle.Function)},
		{"owner", SerializeObjectHandle(handle.Owner)},
		{"full_path", handle.FullPath},
		{"signature_fingerprint", std::format("{:016X}", handle.SignatureFingerprint)}
	};
}

json SerializeCapturePolicy(const HookCapturePolicy& policy)
{
	json output = {{"mode", ToString(policy.Mode)}};
	if (policy.Mode == HookCaptureMode::PreEncodedPayload)
		output["max_payload_bytes"] = policy.MaxPayloadBytes;
	return output;
}

json SerializeSpec(const HookSubscriptionSpec& spec)
{
	return {
		{"session_id", spec.SessionId},
		{"context_generation", spec.ContextGeneration},
		{"object_snapshot_generation", spec.ObjectSnapshotGeneration},
		{"type_snapshot_generation", spec.TypeSnapshotGeneration},
		{"function", SerializeFunctionHandle(spec.Function)},
		{"function_path", spec.FunctionPath},
		{"capture", SerializeCapturePolicy(spec.Capture)}
	};
}

json SerializeSubscription(const HookSubscription& subscription)
{
	return {
		{"id", subscription.Id},
		{"state", ToString(subscription.State)},
		{"enabled", subscription.State == HookSubscriptionState::Enabled},
		{"spec", SerializeSpec(subscription.Spec)},
		{"function_path", subscription.Spec.FunctionPath},
		{"created_at_monotonic_us", subscription.CreatedAtMonotonicUs},
		{"hit_count", subscription.HitCount},
		{"last_event_sequence", subscription.LastEventSequence},
		{"last_correlation", subscription.LastCorrelation},
		{"log_count", subscription.LogCount},
		{"log_bytes", subscription.LogBytes},
		{"log_drop_count", subscription.LogDropCount},
		{"terminal_reason_code", subscription.TerminalReasonCode.empty()
			? json(nullptr) : json(subscription.TerminalReasonCode)},
		{"terminal_reason", subscription.TerminalReason.empty()
			? json(nullptr) : json(subscription.TerminalReason)}
	};
}

std::string HexEncode(const std::span<const std::byte> bytes)
{
	constexpr char kHex[] = "0123456789ABCDEF";
	std::string encoded(bytes.size() * 2, '0');
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		const auto value = static_cast<unsigned char>(bytes[index]);
		encoded[index * 2] = kHex[value >> 4];
		encoded[index * 2 + 1] = kHex[value & 0x0F];
	}
	return encoded;
}

HookCommandResult BoundedSuccess(json data)
{
	try
	{
		if (data.dump().size() > HookCommandService::kMaxSerializedDataBytes)
		{
			return Failure(
				"HOOK_RESPONSE_TOO_LARGE",
				"The bounded hook response exceeds the serialized response limit");
		}
		return {.Data = std::move(data)};
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure(
			"HOOK_SERIALIZATION_FAILED",
			"The hook command could not serialize its bounded result");
	}
}

} // namespace

struct HookCommandService::LogRecord
{
	std::uint64_t Sequence = 0;
	std::uint64_t ConfigurationGeneration = 0;
	Runtime::HookEventKind Kind = Runtime::HookEventKind::Diagnostic;
	std::uint64_t Source = 0;
	std::uint64_t Correlation = 0;
	std::uint64_t CoalescedBefore = 0;
	std::uint64_t DrainedAtMonotonicUs = 0;
	std::vector<std::byte> Payload;
};

struct HookCommandService::SubscriptionRecord
{
	HookSubscriptionId Id = 0;
	HookSubscriptionSpec Spec;
	bool Enabled = false;
	bool Terminal = false;
	std::uint64_t CreatedAtMonotonicUs = 0;
	std::uint64_t HitCount = 0;
	std::uint64_t LastEventSequence = 0;
	std::uint64_t LastCorrelation = 0;
	std::deque<LogRecord> Logs;
	std::size_t LogBytes = 0;
	std::uint64_t LogDropCount = 0;
	std::string TerminalReasonCode;
	std::string TerminalReason;
};

struct HookCommandService::CollectorDrainSummary
{
	HookSubscriptionError Error = HookSubscriptionError::None;
	Runtime::HookCollectorError CollectorError = Runtime::HookCollectorError::None;
	std::size_t Count = 0;
	bool MoreAvailable = false;
	std::uint64_t PublishedTotal = 0;
	std::uint64_t DrainedTotal = 0;
	std::uint64_t DroppedOverflowTotal = 0;
	std::uint64_t DroppedOversizeTotal = 0;
	std::uint64_t DroppedContentionTotal = 0;
	std::uint64_t CoalescedOverflowTotal = 0;
};

const char* ToString(const HookCaptureMode mode) noexcept
{
	switch (mode)
	{
	case HookCaptureMode::FixedMetadata: return "fixed_metadata";
	case HookCaptureMode::PreEncodedPayload: return "preencoded_payload";
	}
	return "unknown";
}

const char* ToString(const HookSubscriptionState state) noexcept
{
	switch (state)
	{
	case HookSubscriptionState::Enabled: return "enabled";
	case HookSubscriptionState::Disabled: return "disabled";
	case HookSubscriptionState::Terminal: return "terminal";
	}
	return "unknown";
}

const char* ToString(const HookSubscriptionError error) noexcept
{
	switch (error)
	{
	case HookSubscriptionError::None: return "NONE";
	case HookSubscriptionError::InvalidConfiguration: return "HOOK_INVALID_CONFIGURATION";
	case HookSubscriptionError::InvalidSpec: return "HOOK_SPEC_INVALID";
	case HookSubscriptionError::SessionMismatch: return "HOOK_SESSION_MISMATCH";
	case HookSubscriptionError::ContextGenerationMismatch:
		return "HOOK_CONTEXT_GENERATION_MISMATCH";
	case HookSubscriptionError::SnapshotGenerationInvalid:
		return "HOOK_SNAPSHOT_GENERATION_INVALID";
	case HookSubscriptionError::CurrentSnapshotUnavailable:
		return "HOOK_CURRENT_SNAPSHOT_UNAVAILABLE";
	case HookSubscriptionError::SnapshotGenerationMismatch:
		return "HOOK_SNAPSHOT_GENERATION_MISMATCH";
	case HookSubscriptionError::FunctionSnapshotMismatch:
		return "HOOK_FUNCTION_SNAPSHOT_MISMATCH";
	case HookSubscriptionError::CapturePolicyInvalid: return "HOOK_CAPTURE_POLICY_INVALID";
	case HookSubscriptionError::Duplicate: return "HOOK_DUPLICATE";
	case HookSubscriptionError::CapacityExceeded: return "HOOK_CAPACITY_EXCEEDED";
	case HookSubscriptionError::IdExhausted: return "HOOK_ID_EXHAUSTED";
	case HookSubscriptionError::SnapshotGenerationExhausted:
		return "HOOK_ENABLED_SNAPSHOT_GENERATION_EXHAUSTED";
	case HookSubscriptionError::NotFound: return "HOOK_NOT_FOUND";
	case HookSubscriptionError::Terminal: return "HOOK_TERMINAL";
	case HookSubscriptionError::InvalidLimit: return "HOOK_LIMIT_INVALID";
	case HookSubscriptionError::AllocationFailed: return "HOOK_ALLOCATION_FAILED";
	case HookSubscriptionError::CollectorDrainFailed: return "HOOK_COLLECTOR_DRAIN_FAILED";
	}
	return "HOOK_UNKNOWN_ERROR";
}

const HookProducerSubscription* HookEnabledSnapshot::Find(
	const Runtime::FunctionHandle& function,
	const std::uint64_t objectSnapshotGeneration,
	const std::uint64_t typeSnapshotGeneration) const noexcept
{
	if (function.Function.Index < 0
		|| function.Function.SerialNumber <= 0
		|| function.Function.Address == 0
		|| function.Function.ClassFingerprint == 0
		|| function.Owner.Index < 0
		|| function.Owner.SerialNumber <= 0
		|| function.Owner.Address == 0
		|| function.Owner.ClassFingerprint == 0
		|| function.Function.SessionId.empty()
		|| function.Owner.SessionId.empty()
		|| function.Function.ContextGeneration == 0
		|| function.Owner.ContextGeneration == 0
		|| function.FullPath.empty()
		|| function.SignatureFingerprint == 0
		|| objectSnapshotGeneration == 0
		|| typeSnapshotGeneration == 0
		|| m_Buckets.empty())
	{
		return nullptr;
	}
	const std::size_t mask = m_Buckets.size() - 1;
	std::size_t bucket = static_cast<std::size_t>(ProducerKeyHash(
		function,
		objectSnapshotGeneration,
		typeSnapshotGeneration)) & mask;
	for (std::size_t probe = 0; probe < m_Buckets.size(); ++probe)
	{
		const std::uint32_t stored = m_Buckets[bucket];
		if (stored == 0)
			return nullptr;
		const HookProducerSubscription& candidate = m_Entries[stored - 1];
		if (candidate.Spec.ObjectSnapshotGeneration == objectSnapshotGeneration
			&& candidate.Spec.TypeSnapshotGeneration == typeSnapshotGeneration
			&& SameFunctionHandle(candidate.Spec.Function, function))
		{
			return &candidate;
		}
		bucket = (bucket + 1) & mask;
	}
	return nullptr;
}

HookCommandService::HookCommandService(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	Runtime::HookEventCollector& collector,
	HookCommandLimits limits,
	Runtime::CoreRuntime* const runtime,
	Runtime::EngineFacade* const engine)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Collector(collector),
	  m_Limits(limits),
	  m_Runtime(runtime),
	  m_Engine(engine),
	  m_EmptyEnabledSnapshot(std::make_shared<const HookEnabledSnapshot>()),
	  m_EnabledSnapshot(m_EmptyEnabledSnapshot)
{
	const Runtime::HookCollectorSnapshot collectorSnapshot = m_Collector.Snapshot();
	const std::string_view collectorSession(
		collectorSnapshot.SessionId.data(),
		collectorSnapshot.SessionIdSize);
	m_Configured = IsBoundedText(m_SessionId, kMaxSessionBytes)
		&& m_ContextGeneration > 0
		&& m_ContextGeneration <= kMaxProtocolInteger
		&& ValidLimits(m_Limits)
		&& collectorSnapshot.Configured
		&& collectorSession == m_SessionId
		&& collectorSnapshot.ContextGeneration == m_ContextGeneration;
}

HookCommandService::~HookCommandService() = default;

bool HookCommandService::Handles(const std::string_view operation) noexcept
{
	return operation == "hook.add"
		|| operation == "hook.list"
		|| operation == "hook.enable"
		|| operation == "hook.remove"
		|| operation == "hook.log";
}

std::shared_ptr<const HookEnabledSnapshot>
HookCommandService::AcquireEnabledSnapshot() const noexcept
{
	return m_EnabledSnapshot.load(std::memory_order_acquire);
}

HookSubscriptionError HookCommandService::ValidateSpec(
	const HookSubscriptionSpec& spec) const noexcept
{
	if (!m_Configured)
		return HookSubscriptionError::InvalidConfiguration;
	if (spec.SessionId != m_SessionId
		|| spec.Function.Function.SessionId != m_SessionId
		|| spec.Function.Owner.SessionId != m_SessionId)
	{
		return HookSubscriptionError::SessionMismatch;
	}
	if (spec.ContextGeneration != m_ContextGeneration
		|| spec.Function.Function.ContextGeneration != m_ContextGeneration
		|| spec.Function.Owner.ContextGeneration != m_ContextGeneration)
	{
		return HookSubscriptionError::ContextGenerationMismatch;
	}
	if (spec.ObjectSnapshotGeneration == 0
		|| spec.ObjectSnapshotGeneration > kMaxProtocolInteger
		|| spec.TypeSnapshotGeneration == 0
		|| spec.TypeSnapshotGeneration > kMaxProtocolInteger)
	{
		return HookSubscriptionError::SnapshotGenerationInvalid;
	}
	if (spec.Function.Function.Index < 0 || spec.Function.Function.SerialNumber <= 0
		|| spec.Function.Function.Address == 0
		|| spec.Function.Function.ClassFingerprint == 0
		|| spec.Function.Owner.Index < 0 || spec.Function.Owner.SerialNumber <= 0
		|| spec.Function.Owner.Address == 0 || spec.Function.Owner.ClassFingerprint == 0
		|| spec.Function.SignatureFingerprint == 0
		|| !IsBoundedText(spec.Function.FullPath, kMaxPathBytes)
		|| !IsBoundedText(spec.FunctionPath, kMaxPathBytes))
	{
		return HookSubscriptionError::InvalidSpec;
	}
	if ((spec.Capture.Mode == HookCaptureMode::FixedMetadata
			&& spec.Capture.MaxPayloadBytes != 0)
		|| (spec.Capture.Mode == HookCaptureMode::PreEncodedPayload
			&& (spec.Capture.MaxPayloadBytes == 0
				|| spec.Capture.MaxPayloadBytes
					> Runtime::HookEventCollector::kHardMaxPayloadBytes)))
	{
		return HookSubscriptionError::CapturePolicyInvalid;
	}
	const Runtime::HookCollectorSnapshot collector = m_Collector.Snapshot();
	if (spec.Capture.Mode == HookCaptureMode::PreEncodedPayload
		&& (!collector.ConfigurationSnapshotStable
			|| spec.Capture.MaxPayloadBytes > collector.Configuration.MaxPayloadBytes))
	{
		return HookSubscriptionError::CapturePolicyInvalid;
	}
	return HookSubscriptionError::None;
}

HookSubscriptionError HookCommandService::ValidateCurrentSpec(
	const HookSubscriptionSpec& spec) const noexcept
{
	if (!m_Runtime || !m_Engine || !m_Engine->IsConfigured())
		return HookSubscriptionError::CurrentSnapshotUnavailable;

	try
	{
		std::string admissionError;
		auto lease = m_Runtime->TryAcquireRequest(&admissionError);
		if (!lease || !lease->Context())
			return HookSubscriptionError::CurrentSnapshotUnavailable;
		if (m_Engine->SessionId() != spec.SessionId)
			return HookSubscriptionError::SessionMismatch;
		if (m_Engine->ContextGeneration() != spec.ContextGeneration
			|| lease->Context()->Generation() != spec.ContextGeneration)
		{
			return HookSubscriptionError::ContextGenerationMismatch;
		}

		const std::shared_ptr<const Runtime::EngineSnapshot> objects =
			m_Engine->Snapshots().Current();
		const std::shared_ptr<const Runtime::TypeSnapshot> types =
			m_Engine->Types().Current();
		if (!objects || !types)
			return HookSubscriptionError::CurrentSnapshotUnavailable;
		if (objects->SessionId != spec.SessionId
			|| objects->ContextGeneration != spec.ContextGeneration
			|| types->SessionId() != spec.SessionId
			|| types->ContextGeneration() != spec.ContextGeneration
			|| !types->IsConfigured(spec.ContextGeneration)
			|| objects->Generation != spec.ObjectSnapshotGeneration
			|| types->Generation() != spec.TypeSnapshotGeneration
			|| types->ObjectSnapshotGeneration() != objects->Generation)
		{
			return HookSubscriptionError::SnapshotGenerationMismatch;
		}

		const Runtime::EngineSnapshotObject* functionRecord =
			objects->FindByIndex(spec.Function.Function.Index);
		const Runtime::EngineSnapshotObject* ownerRecord =
			objects->FindByIndex(spec.Function.Owner.Index);
		const Runtime::ReflectedFunctionLookup lookup =
			types->FindFunctionByFullPath(spec.FunctionPath);
		const Runtime::ReflectedType* ownerType =
			types->FindByObjectIndex(spec.Function.Owner.Index);
		if (!functionRecord || !ownerRecord || !lookup.Found() || !ownerType
			|| functionRecord->Kind != Runtime::EngineObjectKind::Function
			|| ownerRecord->Kind != Runtime::EngineObjectKind::Class
			|| lookup.DeclaringType->Kind != Runtime::ReflectedTypeKind::Class
			|| ownerType != lookup.DeclaringType
			|| lookup.Function->FullPath != spec.FunctionPath
			|| !SameObjectHandle(functionRecord->Handle, spec.Function.Function)
			|| !SameObjectHandle(ownerRecord->Handle, spec.Function.Owner)
			|| !SameObjectHandle(lookup.DeclaringType->Handle, spec.Function.Owner)
			|| !SameFunctionHandle(lookup.Function->Handle, spec.Function))
		{
			return HookSubscriptionError::FunctionSnapshotMismatch;
		}

		if (m_Engine->SessionId() != spec.SessionId
			|| m_Engine->ContextGeneration() != spec.ContextGeneration
			|| m_Engine->Snapshots().Current() != objects
			|| m_Engine->Types().Current() != types)
		{
			return HookSubscriptionError::SnapshotGenerationMismatch;
		}
		return HookSubscriptionError::None;
	}
	catch (...)
	{
		return HookSubscriptionError::CurrentSnapshotUnavailable;
	}
}

HookCommandService::SubscriptionRecord* HookCommandService::FindLocked(
	const HookSubscriptionId id) const noexcept
{
	const auto found = std::ranges::find_if(m_Subscriptions, [id](const auto& record) {
		return record && record->Id == id;
	});
	return found == m_Subscriptions.end() ? nullptr : found->get();
}

std::shared_ptr<const HookEnabledSnapshot>
HookCommandService::BuildEnabledSnapshotLocked(
	const SubscriptionRecord* overrideRecord,
	const bool overrideEnabled,
	const SubscriptionRecord* excludedRecord) const
{
	auto snapshot = std::make_shared<HookEnabledSnapshot>();
	snapshot->m_Generation = m_EnabledSnapshotGeneration + 1;
	snapshot->m_Entries.reserve(m_Subscriptions.size() + (overrideRecord ? 1U : 0U));
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
		{
			snapshot->m_Entries.push_back({
				.Id = record->Id,
				.Spec = record->Spec
			});
		}
	}
	if (overrideRecord && !overridePresent && overrideEnabled && !overrideRecord->Terminal)
	{
		snapshot->m_Entries.push_back({
			.Id = overrideRecord->Id,
			.Spec = overrideRecord->Spec
		});
	}

	if (snapshot->m_Entries.empty())
		return snapshot;
	std::size_t bucketCount = 2;
	while (bucketCount < snapshot->m_Entries.size() * 2)
		bucketCount *= 2;
	snapshot->m_Buckets.assign(bucketCount, 0);
	const std::size_t mask = bucketCount - 1;
	for (std::size_t index = 0; index < snapshot->m_Entries.size(); ++index)
	{
		const HookSubscriptionSpec& spec = snapshot->m_Entries[index].Spec;
		std::size_t bucket = static_cast<std::size_t>(ProducerKeyHash(
			spec.Function,
			spec.ObjectSnapshotGeneration,
			spec.TypeSnapshotGeneration)) & mask;
		while (snapshot->m_Buckets[bucket] != 0)
			bucket = (bucket + 1) & mask;
		snapshot->m_Buckets[bucket] = static_cast<std::uint32_t>(index + 1);
	}
	return snapshot;
}

void HookCommandService::PublishEnabledSnapshotLocked(
	std::shared_ptr<const HookEnabledSnapshot> snapshot) noexcept
{
	if (!snapshot)
		return;
	m_EnabledSnapshotGeneration = snapshot->Generation();
	m_EnabledSnapshot.store(std::move(snapshot), std::memory_order_release);
}

HookSubscription HookCommandService::CopySubscriptionLocked(
	const SubscriptionRecord& record) const
{
	return {
		.Id = record.Id,
		.Spec = record.Spec,
		.State = record.Terminal
			? HookSubscriptionState::Terminal
			: (record.Enabled
				? HookSubscriptionState::Enabled
				: HookSubscriptionState::Disabled),
		.CreatedAtMonotonicUs = record.CreatedAtMonotonicUs,
		.HitCount = record.HitCount,
		.LastEventSequence = record.LastEventSequence,
		.LastCorrelation = record.LastCorrelation,
		.LogCount = record.Logs.size(),
		.LogBytes = record.LogBytes,
		.LogDropCount = record.LogDropCount,
		.TerminalReasonCode = record.TerminalReasonCode,
		.TerminalReason = record.TerminalReason
	};
}

HookCommandService::CollectorDrainSummary HookCommandService::DrainCollectorWorker(
	const std::size_t maximum) noexcept
{
	if (maximum == 0 || maximum > m_Limits.MaxDrainBatch)
		return {.Error = HookSubscriptionError::InvalidLimit};
	try
	{
		std::lock_guard<std::mutex> drainOwner(m_DrainMutex);
		std::vector<Runtime::HookEvent> events(maximum);
		const Runtime::HookDrainResult drained = m_Collector.Drain(events);
		if (!drained.Ok())
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			++m_DrainFailureCount;
			return {
				.Error = HookSubscriptionError::CollectorDrainFailed,
				.CollectorError = drained.Error
			};
		}

		const std::uint64_t drainedAt = MonotonicMicroseconds();
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (std::size_t index = 0; index < drained.Count; ++index)
		{
			const Runtime::HookEvent& event = events[index];
			const std::string_view eventSession(event.SessionId.data(), event.SessionIdSize);
			if (eventSession != m_SessionId
				|| event.ContextGeneration != m_ContextGeneration
				|| event.Subject == 0
				|| event.Subject > kMaxProtocolInteger)
			{
				++m_UnmatchedEventCount;
				continue;
			}
			SubscriptionRecord* record = FindLocked(event.Subject);
			if (!record
				|| event.Source != record->Spec.Function.Function.Address
				|| (event.Kind != Runtime::HookEventKind::ProcessEventEnter
					&& event.Kind != Runtime::HookEventKind::ProcessEventExit))
			{
				++m_UnmatchedEventCount;
				continue;
			}
			const HookCapturePolicy& policy = record->Spec.Capture;
			const bool policyAccepted = policy.Mode == HookCaptureMode::FixedMetadata
				? event.PayloadSize == 0
				: event.PayloadSize <= policy.MaxPayloadBytes;
			if (!policyAccepted || event.PayloadSize > m_Limits.MaxLogBytesPerSubscription)
			{
				++m_PolicyRejectedEventCount;
				continue;
			}

			LogRecord log{
				.Sequence = event.Sequence,
				.ConfigurationGeneration = event.ConfigurationGeneration,
				.Kind = event.Kind,
				.Source = event.Source,
				.Correlation = event.Correlation,
				.CoalescedBefore = event.CoalescedBefore,
				.DrainedAtMonotonicUs = drainedAt
			};
			log.Payload.assign(event.Payload.begin(), event.Payload.begin() + event.PayloadSize);
			while (!record->Logs.empty()
				&& (record->Logs.size() >= m_Limits.MaxLogEntriesPerSubscription
					|| record->LogBytes > m_Limits.MaxLogBytesPerSubscription
						- log.Payload.size()))
			{
				record->LogBytes -= record->Logs.front().Payload.size();
				record->Logs.pop_front();
				++record->LogDropCount;
			}
			record->LogBytes += log.Payload.size();
			record->Logs.push_back(std::move(log));
			record->LastEventSequence = event.Sequence;
			record->LastCorrelation = event.Correlation;
			if (event.Kind == Runtime::HookEventKind::ProcessEventEnter)
				++record->HitCount;
		}
		return {
			.Error = HookSubscriptionError::None,
			.Count = drained.Count,
			.MoreAvailable = drained.MoreAvailable,
			.PublishedTotal = drained.PublishedTotal,
			.DrainedTotal = drained.DrainedTotal,
			.DroppedOverflowTotal = drained.DroppedOverflowTotal,
			.DroppedOversizeTotal = drained.DroppedOversizeTotal,
			.DroppedContentionTotal = drained.DroppedContentionTotal,
			.CoalescedOverflowTotal = drained.CoalescedOverflowTotal
		};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = HookSubscriptionError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = HookSubscriptionError::CollectorDrainFailed};
	}
}

HookCommandResult HookCommandService::Execute(
	const std::string_view operation,
	const json& data) noexcept
{
	try
	{
		if (!m_Configured)
			return SubscriptionFailure(HookSubscriptionError::InvalidConfiguration);
		if (operation == "hook.add") return Add(data);
		if (operation == "hook.list") return List(data);
		if (operation == "hook.enable") return Enable(data);
		if (operation == "hook.remove") return Remove(data);
		if (operation == "hook.log") return Log(data);
		return Failure(
			"OPERATION_NOT_SUPPORTED",
			"The hook command service does not implement the requested operation",
			{{"operation", operation}});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure(
			"HOOK_COMMAND_FAILED",
			"The hook command could not validate or serialize its bounded data");
	}
}

HookCommandResult HookCommandService::Add(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 6
			|| !data.contains("object_snapshot_generation")
			|| !data.contains("type_snapshot_generation")
			|| !data.contains("function")
			|| !data.contains("function_path")
			|| !data.contains("capture")
			|| !data.contains("enabled")
			|| !data.at("function_path").is_string()
			|| !data.at("enabled").is_boolean())
		{
			return Failure(
				"INVALID_ARGUMENT",
				"hook.add data must contain exactly object_snapshot_generation, type_snapshot_generation, function, function_path, capture, and enabled");
		}

		auto record = std::make_unique<SubscriptionRecord>();
		std::uint64_t objectGeneration = 0;
		std::uint64_t typeGeneration = 0;
		if (!TryUnsigned(
				data.at("object_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				objectGeneration)
			|| !TryUnsigned(
				data.at("type_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				typeGeneration)
			|| !TryParseFunctionHandle(data.at("function"), record->Spec.Function)
			|| !TryParseCapturePolicy(data.at("capture"), record->Spec.Capture))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"hook.add contains a malformed generation, function handle, or capture policy");
		}
		record->Spec.SessionId = record->Spec.Function.Function.SessionId;
		record->Spec.ContextGeneration = record->Spec.Function.Function.ContextGeneration;
		record->Spec.ObjectSnapshotGeneration = objectGeneration;
		record->Spec.TypeSnapshotGeneration = typeGeneration;
		record->Spec.FunctionPath = data.at("function_path").get<std::string>();
		record->Enabled = data.at("enabled").get<bool>();
		record->CreatedAtMonotonicUs = MonotonicMicroseconds();
		const HookSubscriptionError specError = ValidateSpec(record->Spec);
		if (specError != HookSubscriptionError::None)
			return SubscriptionFailure(specError);
		const HookSubscriptionError currentError = ValidateCurrentSpec(record->Spec);
		if (currentError != HookSubscriptionError::None)
			return SubscriptionFailure(currentError);

		HookSubscription subscription;
		std::uint64_t snapshotGeneration = 0;
		HookSubscriptionError mutationError = HookSubscriptionError::None;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (std::ranges::any_of(m_Subscriptions, [&record](const auto& existing) {
				return existing && (SameFunctionHandle(
					existing->Spec.Function,
					record->Spec.Function)
					|| SameProducerKey(existing->Spec, record->Spec));
			}))
			{
				mutationError = HookSubscriptionError::Duplicate;
			}
			else if (m_Subscriptions.size() >= m_Limits.MaxSubscriptions)
				mutationError = HookSubscriptionError::CapacityExceeded;
			else if (m_NextId == 0 || m_NextId > kMaxProtocolInteger)
				mutationError = HookSubscriptionError::IdExhausted;
			else if (m_EnabledSnapshotGeneration >= kMaxProtocolInteger)
				mutationError = HookSubscriptionError::SnapshotGenerationExhausted;
			else
			{
				record->Id = m_NextId;
				auto nextSnapshot = BuildEnabledSnapshotLocked(record.get(), record->Enabled);
				SubscriptionRecord* published = record.get();
				m_Subscriptions.push_back(std::move(record));
				++m_NextId;
				PublishEnabledSnapshotLocked(std::move(nextSnapshot));
				snapshotGeneration = m_EnabledSnapshotGeneration;
				subscription = CopySubscriptionLocked(*published);
			}
		}
		if (mutationError != HookSubscriptionError::None)
			return SubscriptionFailure(mutationError);
		return BoundedSuccess({
			{"subscription", SerializeSubscription(subscription)},
			{"enabled_snapshot_generation", snapshotGeneration}
		});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "hook.add data could not be parsed");
	}
}

HookCommandResult HookCommandService::List(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || !data.empty())
			return Failure("INVALID_ARGUMENT", "hook.list data must be an empty object");
		const CollectorDrainSummary drain = DrainCollectorWorker(m_Limits.MaxDrainBatch);
		if (drain.Error != HookSubscriptionError::None)
		{
			return SubscriptionFailure(drain.Error, {
				{"collector_error", Runtime::ToString(drain.CollectorError)}
			});
		}

		std::vector<HookSubscription> subscriptions;
		std::uint64_t snapshotGeneration = 0;
		std::uint64_t unmatched = 0;
		std::uint64_t policyRejected = 0;
		std::uint64_t drainFailures = 0;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			subscriptions.reserve(m_Subscriptions.size());
			for (const auto& record : m_Subscriptions)
			{
				if (record)
					subscriptions.push_back(CopySubscriptionLocked(*record));
			}
			snapshotGeneration = m_EnabledSnapshotGeneration;
			unmatched = m_UnmatchedEventCount;
			policyRejected = m_PolicyRejectedEventCount;
			drainFailures = m_DrainFailureCount;
		}
		json items = json::array();
		items.get_ref<json::array_t&>().reserve(subscriptions.size());
		for (const HookSubscription& subscription : subscriptions)
			items.push_back(SerializeSubscription(subscription));
		return BoundedSuccess({
			{"hooks", std::move(items)},
			{"monitored_count", subscriptions.size()},
			{"enabled_snapshot_generation", snapshotGeneration},
			{"collector", {
				{"drained_count", drain.Count},
				{"more_available", drain.MoreAvailable},
				{"published_total", drain.PublishedTotal},
				{"drained_total", drain.DrainedTotal},
				{"dropped_overflow_total", drain.DroppedOverflowTotal},
				{"dropped_oversize_total", drain.DroppedOversizeTotal},
				{"dropped_contention_total", drain.DroppedContentionTotal},
				{"coalesced_overflow_total", drain.CoalescedOverflowTotal},
				{"unmatched_event_total", unmatched},
				{"policy_rejected_event_total", policyRejected},
				{"drain_failure_total", drainFailures}
			}}
		});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure("HOOK_SERIALIZATION_FAILED", "hook.list could not serialize state");
	}
}

HookCommandResult HookCommandService::Enable(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 2
			|| !data.contains("id") || !data.contains("enabled")
			|| !data.at("enabled").is_boolean())
		{
			return Failure(
				"INVALID_ARGUMENT",
				"hook.enable data must contain exactly id and enabled");
		}
		std::uint64_t id = 0;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id))
			return Failure("INVALID_ARGUMENT", "hook.enable id is invalid");
		const bool enabled = data.at("enabled").get<bool>();
		HookSubscription subscription;
		std::uint64_t snapshotGeneration = 0;
		HookSubscriptionError mutationError = HookSubscriptionError::None;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			SubscriptionRecord* record = FindLocked(id);
			if (!record)
				mutationError = HookSubscriptionError::NotFound;
			else if (record->Terminal)
				mutationError = HookSubscriptionError::Terminal;
			else if (record->Enabled != enabled)
			{
				if (enabled)
					mutationError = ValidateCurrentSpec(record->Spec);
				if (mutationError == HookSubscriptionError::None)
				{
					if (m_EnabledSnapshotGeneration >= kMaxProtocolInteger)
						mutationError = HookSubscriptionError::SnapshotGenerationExhausted;
					else
					{
						auto nextSnapshot = BuildEnabledSnapshotLocked(record, enabled);
						record->Enabled = enabled;
						PublishEnabledSnapshotLocked(std::move(nextSnapshot));
					}
				}
			}
			if (mutationError == HookSubscriptionError::None)
			{
				snapshotGeneration = m_EnabledSnapshotGeneration;
				subscription = CopySubscriptionLocked(*record);
			}
		}
		if (mutationError != HookSubscriptionError::None)
			return SubscriptionFailure(mutationError, {{"id", id}});
		return BoundedSuccess({
			{"subscription", SerializeSubscription(subscription)},
			{"enabled_snapshot_generation", snapshotGeneration}
		});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "hook.enable data could not be parsed");
	}
}

HookCommandResult HookCommandService::Remove(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 1 || !data.contains("id"))
			return Failure("INVALID_ARGUMENT", "hook.remove data must contain exactly id");
		std::uint64_t id = 0;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id))
			return Failure("INVALID_ARGUMENT", "hook.remove id is invalid");
		std::uint64_t snapshotGeneration = 0;
		HookSubscriptionError mutationError = HookSubscriptionError::None;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			const auto found = std::ranges::find_if(m_Subscriptions, [id](const auto& record) {
				return record && record->Id == id;
			});
			if (found == m_Subscriptions.end())
				mutationError = HookSubscriptionError::NotFound;
			else if (m_EnabledSnapshotGeneration >= kMaxProtocolInteger)
				mutationError = HookSubscriptionError::SnapshotGenerationExhausted;
			else
			{
				SubscriptionRecord* record = found->get();
				auto nextSnapshot = BuildEnabledSnapshotLocked(nullptr, false, record);
				m_Subscriptions.erase(found);
				PublishEnabledSnapshotLocked(std::move(nextSnapshot));
				snapshotGeneration = m_EnabledSnapshotGeneration;
			}
		}
		if (mutationError != HookSubscriptionError::None)
			return SubscriptionFailure(mutationError, {{"id", id}});
		return BoundedSuccess({
			{"id", id},
			{"removed", true},
			{"enabled_snapshot_generation", snapshotGeneration}
		});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "hook.remove data could not be parsed");
	}
}

HookCommandResult HookCommandService::Log(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.empty() || data.size() > 2
			|| !data.contains("id")
			|| (data.size() == 2 && !data.contains("limit")))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"hook.log data must contain id and optional limit only");
		}
		std::uint64_t id = 0;
		std::uint64_t limit = m_Limits.MaxLogEntriesPerSubscription;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id)
			|| (data.contains("limit")
				&& !TryUnsigned(
					data.at("limit"),
					1,
					m_Limits.MaxLogEntriesPerSubscription,
					limit)))
		{
			return Failure("INVALID_ARGUMENT", "hook.log id or limit is invalid");
		}
		bool subscriptionExists = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			subscriptionExists = FindLocked(id) != nullptr;
		}
		if (!subscriptionExists)
			return SubscriptionFailure(HookSubscriptionError::NotFound, {{"id", id}});
		const CollectorDrainSummary drain = DrainCollectorWorker(m_Limits.MaxDrainBatch);
		if (drain.Error != HookSubscriptionError::None)
		{
			return SubscriptionFailure(drain.Error, {
				{"collector_error", Runtime::ToString(drain.CollectorError)}
			});
		}

		HookSubscription subscription;
		std::vector<LogRecord> logs;
		bool removedDuringDrain = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			SubscriptionRecord* record = FindLocked(id);
			if (!record)
				removedDuringDrain = true;
			else
			{
				subscription = CopySubscriptionLocked(*record);
				const std::size_t count = (std::min)(
					static_cast<std::size_t>(limit),
					record->Logs.size());
				const std::size_t begin = record->Logs.size() - count;
				logs.reserve(count);
				for (std::size_t index = begin; index < record->Logs.size(); ++index)
					logs.push_back(record->Logs[index]);
			}
		}
		if (removedDuringDrain)
			return SubscriptionFailure(HookSubscriptionError::NotFound, {{"id", id}});

		json entries = json::array();
		entries.get_ref<json::array_t&>().reserve(logs.size());
		for (const LogRecord& log : logs)
		{
			entries.push_back({
				{"sequence", log.Sequence},
				{"configuration_generation", log.ConfigurationGeneration},
				{"kind", Runtime::ToString(log.Kind)},
				{"source", std::format("0x{:X}", log.Source)},
				{"subject", id},
				{"correlation", log.Correlation},
				{"coalesced_before", log.CoalescedBefore},
				{"drained_at_monotonic_us", log.DrainedAtMonotonicUs},
				{"function_path", subscription.Spec.FunctionPath},
				{"payload", {
					{"encoding", "hex"},
					{"size", log.Payload.size()},
					{"data", HexEncode(log.Payload)}
				}}
			});
		}
		return BoundedSuccess({
			{"subscription", SerializeSubscription(subscription)},
			{"entries", std::move(entries)},
			{"returned", logs.size()},
			{"total", subscription.LogCount},
			{"truncated", logs.size() < subscription.LogCount},
			{"collector_more_available", drain.MoreAvailable}
		});
	}
	catch (const std::bad_alloc&)
	{
		return SubscriptionFailure(HookSubscriptionError::AllocationFailed);
	}
	catch (...)
	{
		return Failure("HOOK_SERIALIZATION_FAILED", "hook.log could not serialize logs");
	}
}

HookMutationResult HookCommandService::MarkTerminal(
	const HookSubscriptionId id,
	std::string reasonCode,
	std::string reason) noexcept
{
	if (!IsBoundedText(reasonCode, kMaxReasonCodeBytes)
		|| !IsBoundedText(reason, kMaxReasonBytes))
	{
		return {.Error = HookSubscriptionError::InvalidSpec, .Id = id};
	}
	try
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		SubscriptionRecord* record = FindLocked(id);
		if (!record)
			return {.Error = HookSubscriptionError::NotFound, .Id = id};
		if (record->Terminal)
			return {.Error = HookSubscriptionError::Terminal, .Id = id};
		if (m_EnabledSnapshotGeneration >= kMaxProtocolInteger)
		{
			return {
				.Error = HookSubscriptionError::SnapshotGenerationExhausted,
				.Id = id
			};
		}
		auto nextSnapshot = BuildEnabledSnapshotLocked(nullptr, false, record);
		record->Enabled = false;
		record->Terminal = true;
		record->TerminalReasonCode = std::move(reasonCode);
		record->TerminalReason = std::move(reason);
		PublishEnabledSnapshotLocked(std::move(nextSnapshot));
		return {.Id = id};
	}
	catch (...)
	{
		return {.Error = HookSubscriptionError::AllocationFailed, .Id = id};
	}
}

} // namespace UExplorer::Services
