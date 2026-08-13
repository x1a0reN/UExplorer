#include "WatchCommandService.h"

#include "Runtime/SafeMemory.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace UExplorer::Services
{
namespace
{

constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxNameBytes = 1024;
constexpr std::uint64_t kMaxArrayIndex = 1023;
constexpr std::size_t kMaxCanonicalNodes = 2048;
constexpr std::size_t kMaxCanonicalDepth = 16;
constexpr std::size_t kMaxDisplayBytes = 1024;

WatchCommandError Error(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Code = std::move(code),
		.Message = std::move(message),
		.Details = std::move(details)
	};
}

WatchCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {.Error = Error(
		std::move(code),
		std::move(message),
		std::move(details))};
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
		digits.data(),
		digits.data() + digits.size(),
		parsed,
		16);
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
			kMaxProtocolInteger,
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
		|| !TryCanonicalHex(
			data.at("address").get_ref<const std::string&>(),
			true,
			false,
			address)
		|| address == 0
		|| address > (std::numeric_limits<std::uintptr_t>::max)()
		|| !TryCanonicalHex(
			data.at("class_fingerprint").get_ref<const std::string&>(),
			false,
			true,
			classFingerprint)
		|| classFingerprint == 0)
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

bool SameHandle(
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

class CanonicalWriter final
{
public:
	CanonicalWriter(std::vector<std::byte>& output, const std::size_t limit) noexcept
		: m_Output(output), m_Limit(limit)
	{
	}

	bool Ok() const noexcept { return m_Ok; }

	void Byte(const std::uint8_t value)
	{
		Append(std::as_bytes(std::span(&value, 1)));
	}

	void U32(const std::uint32_t value)
	{
		std::byte bytes[4];
		for (std::size_t index = 0; index < std::size(bytes); ++index)
			bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFU);
		Append(bytes);
	}

	void U64(const std::uint64_t value)
	{
		std::byte bytes[8];
		for (std::size_t index = 0; index < std::size(bytes); ++index)
			bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFU);
		Append(bytes);
	}

	void Text(const std::string_view value)
	{
		if (value.size() > (std::numeric_limits<std::uint32_t>::max)())
		{
			m_Ok = false;
			return;
		}
		U32(static_cast<std::uint32_t>(value.size()));
		Append(std::as_bytes(std::span(value.data(), value.size())));
	}

	void RawText(const std::string_view value)
	{
		Append(std::as_bytes(std::span(value.data(), value.size())));
	}

private:
	void Append(const std::span<const std::byte> bytes)
	{
		if (!m_Ok || bytes.size() > m_Limit
			|| m_Output.size() > m_Limit - bytes.size())
		{
			m_Ok = false;
			return;
		}
		m_Output.insert(m_Output.end(), bytes.begin(), bytes.end());
	}

	std::vector<std::byte>& m_Output;
	std::size_t m_Limit = 0;
	bool m_Ok = true;
};

void EncodeHandle(CanonicalWriter& writer, const Runtime::ObjectHandle& handle)
{
	writer.Text(handle.SessionId);
	writer.U64(handle.ContextGeneration);
	writer.U32(static_cast<std::uint32_t>(handle.Index));
	writer.U32(static_cast<std::uint32_t>(handle.SerialNumber));
	writer.U64(static_cast<std::uint64_t>(handle.Address));
	writer.U64(handle.ClassFingerprint);
}

bool EncodePropertyValue(
	CanonicalWriter& writer,
	const Runtime::PropertyValue& value,
	const std::size_t depth,
	std::size_t& nodes)
{
	if (!writer.Ok() || depth > kMaxCanonicalDepth || ++nodes > kMaxCanonicalNodes)
		return false;

	writer.Byte(static_cast<std::uint8_t>(value.State));
	writer.Byte(static_cast<std::uint8_t>(value.Kind));
	writer.Text(value.Label);
	writer.Text(value.TypeName);
	writer.Byte(static_cast<std::uint8_t>(value.Scalar.index()));
	if (const auto booleanValue = std::get_if<bool>(&value.Scalar))
	{
		writer.Byte(*booleanValue ? 1U : 0U);
	}
	else if (const auto signedValue = std::get_if<std::int64_t>(&value.Scalar))
	{
		writer.U64(std::bit_cast<std::uint64_t>(*signedValue));
	}
	else if (const auto unsignedValue = std::get_if<std::uint64_t>(&value.Scalar))
	{
		writer.U64(*unsignedValue);
	}
	else if (const auto floatingValue = std::get_if<double>(&value.Scalar))
	{
		writer.U64(std::bit_cast<std::uint64_t>(*floatingValue));
	}
	else if (const auto stringValue = std::get_if<std::string>(&value.Scalar))
	{
		writer.Text(*stringValue);
	}
	else if (const auto objectValue = std::get_if<Runtime::PropertyObjectReference>(&value.Scalar))
	{
		EncodeHandle(writer, objectValue->Handle);
	}

	writer.Text(value.DisplayName);
	if (value.Children.size() > (std::numeric_limits<std::uint32_t>::max)())
		return false;
	writer.U32(static_cast<std::uint32_t>(value.Children.size()));
	for (const Runtime::PropertyValue& child : value.Children)
	{
		if (!EncodePropertyValue(writer, child, depth + 1, nodes))
			return false;
	}
	writer.U32(value.TotalCount);
	writer.Byte(value.Truncated ? 1U : 0U);
	writer.Text(value.ErrorCode);
	writer.Text(value.ErrorMessage);
	return writer.Ok();
}

std::string PropertyDisplayValue(const Runtime::PropertyValue& value)
{
	std::string display;
	if (!value.DisplayName.empty())
		display = value.DisplayName;
	else if (const auto booleanValue = std::get_if<bool>(&value.Scalar))
		display = *booleanValue ? "true" : "false";
	else if (const auto signedValue = std::get_if<std::int64_t>(&value.Scalar))
		display = std::to_string(*signedValue);
	else if (const auto unsignedValue = std::get_if<std::uint64_t>(&value.Scalar))
		display = std::to_string(*unsignedValue);
	else if (const auto floatingValue = std::get_if<double>(&value.Scalar))
	{
		if (std::isnan(*floatingValue)) display = "NaN";
		else if (std::isinf(*floatingValue)) display = std::signbit(*floatingValue) ? "-Infinity" : "Infinity";
		else display = std::format("{:.17g}", *floatingValue);
	}
	else if (const auto stringValue = std::get_if<std::string>(&value.Scalar))
		display = *stringValue;
	else if (const auto objectValue = std::get_if<Runtime::PropertyObjectReference>(&value.Scalar))
		display = std::format("0x{:X}", objectValue->Handle.Address);
	else if (!value.Children.empty() || value.TotalCount != 0)
		display = std::format("{}:{} items", Runtime::ToString(value.Kind), value.TotalCount);
	else
		display = Runtime::ToString(value.State);

	if (display.size() > kMaxDisplayBytes)
		display.resize(kMaxDisplayBytes);
	return display;
}

Runtime::WatchSampleResult SampleFailure(
	const Runtime::WatchSampleStatus status,
	std::string code,
	std::string reason)
{
	return {
		.Status = status,
		.ReasonCode = std::move(code),
		.Reason = std::move(reason)
	};
}

Runtime::WatchSampleResult ValueFailure(const Runtime::PropertyValue& value)
{
	const Runtime::WatchSampleStatus status =
		value.State == Runtime::PropertyValueState::Unsupported
			|| value.State == Runtime::PropertyValueState::Unavailable
		? Runtime::WatchSampleStatus::Unavailable
		: Runtime::WatchSampleStatus::Failed;
	return SampleFailure(
		status,
		value.ErrorCode.empty()
			? (status == Runtime::WatchSampleStatus::Unavailable
				? "WATCH_PROPERTY_VALUE_UNAVAILABLE"
				: "WATCH_PROPERTY_DECODE_FAILED")
			: value.ErrorCode,
		value.ErrorMessage.empty()
			? "The exact property codec did not produce a watchable value"
			: value.ErrorMessage);
}

class SnapshotWatchReferenceResolver final : public Runtime::IPropertyReferenceResolver
{
public:
	SnapshotWatchReferenceResolver(
		Runtime::EngineFacade& engine,
		const Runtime::EngineSnapshot& snapshot) noexcept
		: m_Engine(engine), m_Snapshot(snapshot)
	{
	}

	std::string_view SessionId() const noexcept override
	{
		return m_Engine.SessionId();
	}

	std::uint64_t ContextGeneration() const noexcept override
	{
		return m_Engine.ContextGeneration();
	}

	Runtime::PropertyReferenceResult ResolveAddress(const std::uintptr_t address) override
	{
		const Runtime::EngineSnapshotObject* object = m_Snapshot.FindByAddress(address);
		if (!object)
		{
			return {
				.State = Runtime::PropertyValueState::Unavailable,
				.ErrorCode = "PROPERTY_REFERENCE_NOT_IN_SNAPSHOT",
				.ErrorMessage = "The referenced address is absent from the exact object generation"
			};
		}
		return Validate(*object);
	}

	Runtime::PropertyReferenceResult ResolveWeak(
		const std::int32_t index,
		const std::int32_t serialNumber) override
	{
		const Runtime::EngineSnapshotObject* object = m_Snapshot.FindByIndex(index);
		if (!object || object->Handle.SerialNumber != serialNumber)
		{
			return {
				.State = Runtime::PropertyValueState::Unavailable,
				.ErrorCode = "PROPERTY_WEAK_REFERENCE_STALE",
				.ErrorMessage = "The weak identity is absent from the exact object generation"
			};
		}
		return Validate(*object);
	}

private:
	Runtime::PropertyReferenceResult Validate(const Runtime::EngineSnapshotObject& object)
	{
		const Runtime::ObjectValidationResult validation =
			m_Engine.ValidateObjectHandle(object.Handle);
		if (!validation.Ok())
		{
			return {
				.State = Runtime::PropertyValueState::Unavailable,
				.ErrorCode = "PROPERTY_REFERENCE_STALE",
				.ErrorMessage = Runtime::ToString(validation.Error)
			};
		}
		return {
			.State = Runtime::PropertyValueState::Ok,
			.Handle = object.Handle
		};
	}

	Runtime::EngineFacade& m_Engine;
	const Runtime::EngineSnapshot& m_Snapshot;
};

bool SameSubscriptionSpec(
	const Runtime::WatchSubscriptionSpec& left,
	const Runtime::WatchSubscriptionSpec& right) noexcept
{
	return SameHandle(left.Object, right.Object)
		&& left.ContextGeneration == right.ContextGeneration
		&& left.ObjectSnapshotGeneration == right.ObjectSnapshotGeneration
		&& left.TypeSnapshotGeneration == right.TypeSnapshotGeneration
		&& left.DeclaringTypePath == right.DeclaringTypePath
		&& left.PropertyName == right.PropertyName
		&& left.ArrayIndex == right.ArrayIndex
		&& left.IntervalMs == right.IntervalMs;
}

Runtime::WatchBindingResult BindFailure(std::string code, std::string reason)
{
	return {
		.ReasonCode = std::move(code),
		.Reason = std::move(reason)
	};
}

class ObjectPropertyWatchBinding final : public Runtime::IWatchSampleBinding
{
public:
	ObjectPropertyWatchBinding(
		const void* sourceIdentity,
		Runtime::WatchSubscriptionSpec spec,
		std::shared_ptr<const Runtime::EngineSnapshot> objects,
		std::shared_ptr<const Runtime::TypeSnapshot> types,
		std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
		const Runtime::ReflectedProperty* property,
		const std::uintptr_t valueAddress) noexcept
		: IWatchSampleBinding(sourceIdentity),
		  Spec(std::move(spec)),
		  Objects(std::move(objects)),
		  Types(std::move(types)),
		  Reflection(std::move(reflection)),
		  Property(property),
		  ValueAddress(valueAddress)
	{
	}

	Runtime::WatchSubscriptionSpec Spec;
	std::shared_ptr<const Runtime::EngineSnapshot> Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> Types;
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> Reflection;
	const Runtime::ReflectedProperty* Property = nullptr;
	std::uintptr_t ValueAddress = 0;
};

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
			? std::to_integer<std::uint8_t>(bytes[offset + 1])
			: 0;
		const std::uint32_t third = offset + 2 < bytes.size()
			? std::to_integer<std::uint8_t>(bytes[offset + 2])
			: 0;
		const std::uint32_t word = (first << 16) | (second << 8) | third;
		encoded.push_back(alphabet[(word >> 18) & 0x3F]);
		encoded.push_back(alphabet[(word >> 12) & 0x3F]);
		encoded.push_back(offset + 1 < bytes.size() ? alphabet[(word >> 6) & 0x3F] : '=');
		encoded.push_back(offset + 2 < bytes.size() ? alphabet[word & 0x3F] : '=');
	}
	return encoded;
}

json SerializeSpec(const Runtime::WatchSubscriptionSpec& spec)
{
	return {
		{"object", SerializeObjectHandle(spec.Object)},
		{"context_generation", spec.ContextGeneration},
		{"object_snapshot_generation", spec.ObjectSnapshotGeneration},
		{"type_snapshot_generation", spec.TypeSnapshotGeneration},
		{"declaring_type_path", spec.DeclaringTypePath},
		{"property_name", spec.PropertyName},
		{"array_index", spec.ArrayIndex},
		{"interval_ms", spec.IntervalMs}
	};
}

json SerializeSubscription(const Runtime::WatchSubscription& subscription)
{
	return {
		{"id", subscription.Id},
		{"state", Runtime::ToString(subscription.State)},
		{"spec", SerializeSpec(subscription.Spec)},
		{"created_at_monotonic_us", subscription.CreatedAtMonotonicUs},
		{"next_due_monotonic_us", subscription.NextDueMonotonicUs},
		{"last_sampled_at_monotonic_us", subscription.LastSampledAtMonotonicUs},
		{"last_change_sequence", subscription.LastChangeSequence},
		{"sample_count", subscription.SampleCount},
		{"failure_count", subscription.FailureCount},
		{"history_count", subscription.HistoryCount},
		{"history_bytes", subscription.HistoryBytes},
		{"history_drop_count", subscription.HistoryDropCount},
		{"terminal_reason_code", subscription.TerminalReasonCode.empty()
			? json(nullptr) : json(subscription.TerminalReasonCode)},
		{"terminal_reason", subscription.TerminalReason.empty()
			? json(nullptr) : json(subscription.TerminalReason)}
	};
}

json SerializePayload(const std::shared_ptr<const Runtime::WatchSamplePayload>& payload)
{
	if (!payload)
		return nullptr;
	return {
		{"encoding", "uexplorer.property-value.v1.base64"},
		{"type_name", payload->TypeName},
		{"canonical_value", Base64(payload->CanonicalValue)},
		{"display_value", payload->DisplayValue}
	};
}

json SerializeSchedulerSnapshot(const Runtime::WatchSchedulerSnapshot& snapshot)
{
	return {
		{"configured", snapshot.Configured},
		{"stopping", snapshot.Stopping},
		{"stopped", snapshot.Stopped},
		{"pumping", snapshot.Pumping},
		{"enabled_snapshot_generation", snapshot.EnabledSnapshotGeneration},
		{"subscription_count", snapshot.SubscriptionCount},
		{"enabled_count", snapshot.EnabledCount},
		{"pending_event_count", snapshot.PendingEventCount},
		{"pending_event_bytes", snapshot.PendingEventBytes},
		{"dropped_events", snapshot.DroppedEvents},
		{"coalesced_events", snapshot.CoalescedEvents},
		{"pending_push_event_count", snapshot.PendingPushEventCount},
		{"pending_push_event_bytes", snapshot.PendingPushEventBytes},
		{"dropped_push_events", snapshot.DroppedPushEvents},
		{"coalesced_push_events", snapshot.CoalescedPushEvents},
		{"last_event_sequence", snapshot.LastEventSequence},
		{"pump_count", snapshot.PumpCount},
		{"last_pump_duration_us", snapshot.LastPumpDurationUs},
		{"last_pump_items", snapshot.LastPumpItems},
		{"last_pump_bytes", snapshot.LastPumpBytes},
		{"item_budget_exhaustions", snapshot.ItemBudgetExhaustions},
		{"byte_budget_exhaustions", snapshot.ByteBudgetExhaustions},
		{"time_budget_exhaustions", snapshot.TimeBudgetExhaustions},
		{"concurrent_pump_rejections", snapshot.ConcurrentPumpRejections}
	};
}

const char* WatchErrorMessage(const Runtime::WatchError error) noexcept
{
	switch (error)
	{
	case Runtime::WatchError::InvalidConfiguration:
		return "The watch scheduler is not configured for the current session";
	case Runtime::WatchError::Stopped:
		return "The watch scheduler is stopping or stopped";
	case Runtime::WatchError::InvalidSpec:
		return "The watch subscription identity is invalid";
	case Runtime::WatchError::SessionMismatch:
		return "The watch object handle belongs to another session";
	case Runtime::WatchError::ContextGenerationMismatch:
		return "The watch identity belongs to another engine-context generation";
	case Runtime::WatchError::SnapshotGenerationInvalid:
		return "The watch snapshot generation is invalid";
	case Runtime::WatchError::IntervalOutOfRange:
		return "The watch interval is outside the configured scheduler bounds";
	case Runtime::WatchError::CapacityExceeded:
		return "The watch subscription capacity is exhausted";
	case Runtime::WatchError::IdExhausted:
		return "The watch identifier space is exhausted";
	case Runtime::WatchError::Duplicate:
		return "An exact watch subscription for this object, generation, property, array index, and interval already exists";
	case Runtime::WatchError::NotFound:
		return "The exact watch identifier was not found";
	case Runtime::WatchError::Terminal:
		return "The watch is terminal and cannot be re-enabled";
	case Runtime::WatchError::InvalidLimit:
		return "The requested watch result limit is invalid";
	case Runtime::WatchError::AllocationFailed:
		return "The bounded watch operation could not allocate owned storage";
	case Runtime::WatchError::BindingFailed:
		return "The watch sample source could not bind the exact immutable dependencies";
	case Runtime::WatchError::DrainTimedOut:
		return "Watch sampling did not drain before its shutdown deadline";
	case Runtime::WatchError::ConcurrentPumpRejected:
		return "A concurrent watch pump was rejected";
	case Runtime::WatchError::None:
		return "";
	}
	return "The watch scheduler returned an unknown error";
}

WatchCommandResult SchedulerFailure(
	const Runtime::WatchError error,
	json details = json::object())
{
	return Failure(Runtime::ToString(error), WatchErrorMessage(error), std::move(details));
}

WatchCommandResult BoundedSuccess(json data)
{
	if (data.dump().size() > WatchCommandService::kMaxSerializedDataBytes)
	{
		return Failure(
			"WATCH_RESPONSE_TOO_LARGE",
			"The bounded watch response exceeds the 4 MiB command-data ceiling");
	}
	return {.Data = std::move(data)};
}

} // namespace

ObjectPropertyWatchSampleSource::ObjectPropertyWatchSampleSource(
	Runtime::CoreRuntime& runtime,
	Runtime::EngineFacade& engine) noexcept
	: m_Runtime(runtime), m_Engine(engine)
{
}

std::string_view ObjectPropertyWatchSampleSource::SessionId() const noexcept
{
	return m_Engine.SessionId();
}

std::uint64_t ObjectPropertyWatchSampleSource::ContextGeneration() const noexcept
{
	return m_Engine.ContextGeneration();
}

bool ObjectPropertyWatchSampleSource::IsCurrentExecutionThreadValid() const noexcept
{
	return m_Engine.IsCurrentExecutionThreadValid();
}

Runtime::WatchBindingResult ObjectPropertyWatchSampleSource::Bind(
	const Runtime::WatchSubscriptionSpec& spec)
{
	if (spec.Object.SessionId != m_Engine.SessionId()
		|| spec.ContextGeneration != m_Engine.ContextGeneration()
		|| spec.Object.ContextGeneration != m_Engine.ContextGeneration())
	{
		return BindFailure(
			"WATCH_IDENTITY_GENERATION_STALE",
			"The watched session or engine-context generation is no longer current");
	}

	const std::shared_ptr<const Runtime::EngineSnapshot> objects =
		m_Engine.Snapshots().Current();
	const std::shared_ptr<const Runtime::TypeSnapshot> types =
		m_Engine.Types().Current();
	const std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection =
		m_Engine.Reflection();
	if (!objects || !types || !reflection
		|| !reflection->IsPropertyCodecConfigured(spec.ContextGeneration)
		|| objects->Generation != spec.ObjectSnapshotGeneration
		|| types->Generation() != spec.TypeSnapshotGeneration
		|| types->ObjectSnapshotGeneration() != objects->Generation
		|| types->ReflectionLayoutFingerprint() != reflection->Layout->Fingerprint())
	{
		return BindFailure(
			"WATCH_PROPERTY_GENERATION_STALE",
			"The requested object/type/property-codec generation is not the current coherent generation");
	}

	const Runtime::EngineSnapshotObject* record = objects->FindByIndex(spec.Object.Index);
	if (!record || !SameHandle(record->Handle, spec.Object))
	{
		return BindFailure(
			"WATCH_OBJECT_SNAPSHOT_STALE",
			"The exact object handle is absent from the requested immutable object generation");
	}

	const Runtime::ReflectedType* type = types->FindByFullPath(record->ClassPath);
	const Runtime::ReflectedType* declaringType = nullptr;
	for (std::size_t depth = 0;
		type && depth <= Runtime::TypeSnapshotStore::kMaxHierarchyDepth;
		++depth)
	{
		if (type->FullPath == spec.DeclaringTypePath)
		{
			declaringType = type;
			break;
		}
		if (!type->Super)
			break;
		type = types->FindByObjectIndex(type->Super->Index);
	}
	if (!declaringType || declaringType->Kind != Runtime::ReflectedTypeKind::Class)
	{
		return BindFailure(
			"WATCH_PROPERTY_DECLARING_TYPE_STALE",
			"The exact declaring type is absent from the object's immutable class hierarchy");
	}

	const Runtime::ReflectedProperty* property = nullptr;
	for (const Runtime::ReflectedProperty& candidate : declaringType->DirectProperties)
	{
		if (candidate.Name != spec.PropertyName)
			continue;
		if (property)
		{
			return BindFailure(
				"WATCH_PROPERTY_IDENTITY_AMBIGUOUS",
				"The exact declaring type contains duplicate direct property names");
		}
		property = &candidate;
	}
	if (!property || spec.ArrayIndex >= property->ArrayDim)
	{
		return BindFailure(
			"WATCH_PROPERTY_IDENTITY_STALE",
			"The exact property name or fixed-array element is absent from the requested type generation");
	}
	if (property->State != Runtime::ReflectedMemberState::Supported
		|| !property->Descriptor)
	{
		return BindFailure(
			property->ReasonCode.empty()
				? "WATCH_PROPERTY_DESCRIPTOR_UNAVAILABLE" : property->ReasonCode,
			property->Reason.empty()
				? "No immutable descriptor is available for the watched property"
				: property->Reason);
	}
	if (!reflection->Properties->Supports(property->Kind))
	{
		return BindFailure(
			"WATCH_PROPERTY_CODEC_KIND_UNAVAILABLE",
			"The immutable property codec does not witness the watched property kind");
	}

	const std::uint64_t elementOffset = static_cast<std::uint64_t>(property->Offset)
		+ static_cast<std::uint64_t>(spec.ArrayIndex) * property->Size;
	if (elementOffset > (std::numeric_limits<std::uintptr_t>::max)() - spec.Object.Address)
	{
		return BindFailure(
			"WATCH_PROPERTY_ADDRESS_OVERFLOW",
			"The witnessed property offset overflows the object address");
	}
	const std::uintptr_t valueAddress = spec.Object.Address
		+ static_cast<std::uintptr_t>(elementOffset);
	std::uintptr_t rangeEnd = 0;
	if (!Runtime::CheckedAddressRange(valueAddress, property->Size, rangeEnd))
	{
		return BindFailure(
			"WATCH_PROPERTY_ADDRESS_OVERFLOW",
			"The witnessed property range overflows the target address space");
	}

	if (m_Engine.Snapshots().Current() != objects
		|| m_Engine.Types().Current() != types
		|| m_Engine.Reflection() != reflection)
	{
		return BindFailure(
			"WATCH_BIND_DEPENDENCY_CHANGED",
			"The immutable object/type/property-codec generation changed while binding");
	}

	auto binding = std::make_shared<const ObjectPropertyWatchBinding>(
		static_cast<const Runtime::IWatchSampleSource*>(this),
		spec,
		objects,
		types,
		reflection,
		property,
		valueAddress);
	return {.Binding = std::move(binding)};
}

Runtime::WatchSampleResult ObjectPropertyWatchSampleSource::Sample(
	const Runtime::WatchSubscriptionSpec& spec,
	const std::shared_ptr<const Runtime::IWatchSampleBinding>& binding,
	const std::size_t maxValueBytes)
{
	const void* sourceIdentity =
		static_cast<const Runtime::IWatchSampleSource*>(this);
	if (!binding || binding->SourceIdentity() != sourceIdentity)
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_BINDING_SOURCE_MISMATCH",
			"The owned watch binding does not belong to this sample source");
	}
	const auto& pinned = static_cast<const ObjectPropertyWatchBinding&>(*binding);
	if (!SameSubscriptionSpec(pinned.Spec, spec))
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_BINDING_SPEC_MISMATCH",
			"The owned watch binding does not match the exact subscription specification");
	}
	if (!pinned.Objects || !pinned.Types || !pinned.Reflection || !pinned.Property
		|| !pinned.Property->Descriptor
		|| pinned.Objects->Generation != spec.ObjectSnapshotGeneration
		|| pinned.Types->Generation() != spec.TypeSnapshotGeneration
		|| pinned.Types->ObjectSnapshotGeneration() != pinned.Objects->Generation
		|| !pinned.Reflection->IsPropertyCodecConfigured(spec.ContextGeneration)
		|| pinned.Types->ReflectionLayoutFingerprint()
			!= pinned.Reflection->Layout->Fingerprint())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_BINDING_DEPENDENCY_STALE",
			"The owned immutable watch binding is internally inconsistent");
	}
	if (spec.Object.SessionId != m_Engine.SessionId()
		|| spec.ContextGeneration != m_Engine.ContextGeneration()
		|| spec.Object.ContextGeneration != m_Engine.ContextGeneration())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_IDENTITY_GENERATION_STALE",
			"The watched session or engine-context generation is no longer current");
	}
	if (!m_Engine.IsCurrentExecutionThreadValid())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Failed,
			"WATCH_EXECUTION_THREAD_INVALID",
			"Property watch sampling must run on the witnessed game thread");
	}
	if (maxValueBytes == 0)
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Unavailable,
			"WATCH_SAMPLE_BUDGET_EMPTY",
			"No byte budget remains for the property sample");
	}

	std::string admissionError;
	auto lease = m_Runtime.TryAcquireRequest(&admissionError);
	if (!lease)
	{
		return SampleFailure(
			admissionError == "CORE_STOPPING"
				? Runtime::WatchSampleStatus::Stale
				: Runtime::WatchSampleStatus::Unavailable,
			admissionError.empty() ? "WATCH_CORE_ADMISSION_UNAVAILABLE" : admissionError,
			"The Core request lease is unavailable for watch sampling");
	}
	if (!lease->Context()
		|| lease->Context()->Generation() != spec.ContextGeneration)
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_CONTEXT_GENERATION_STALE",
			"The active Core context no longer matches the watched generation");
	}

	const Runtime::EngineSnapshotObject* record =
		pinned.Objects->FindByIndex(spec.Object.Index);
	if (!record || !SameHandle(record->Handle, spec.Object))
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_BINDING_OBJECT_MISMATCH",
			"The pinned object generation no longer matches the subscription identity");
	}

	const Runtime::ObjectValidationResult validation =
		m_Engine.ValidateObjectHandle(spec.Object);
	if (!validation.Ok())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_OBJECT_HANDLE_STALE",
			Runtime::ToString(validation.Error));
	}

	SnapshotWatchReferenceResolver references(m_Engine, *pinned.Objects);
	Runtime::PropertyDecodeOptions options{
		.Limits = {
			.MaxDepth = 8,
			.MaxContainerElements = 128,
			.MaxTotalNodes = 1024,
			.MaxStringCodeUnits = 16 * 1024,
			.MaxReadableContainerBytes = 16 * 1024 * 1024
		},
		.ReferenceResolver = &references
	};
	Runtime::PropertyValue value = pinned.Reflection->Properties->Decode(
		pinned.ValueAddress,
		*pinned.Property->Descriptor,
		std::move(options));

	const Runtime::ObjectValidationResult validationAfter =
		m_Engine.ValidateObjectHandle(spec.Object);
	if (!validationAfter.Ok())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Stale,
			"WATCH_OBJECT_HANDLE_CHANGED_DURING_SAMPLE",
			Runtime::ToString(validationAfter.Error));
	}
	if (!value.Ok())
		return ValueFailure(value);

	Runtime::WatchSamplePayload payload;
	payload.TypeName = value.TypeName.empty()
		? pinned.Property->Descriptor->TypeName
		: value.TypeName;
	payload.CanonicalValue.reserve((std::min)(maxValueBytes, std::size_t{4096}));
	CanonicalWriter writer(payload.CanonicalValue, maxValueBytes);
	writer.RawText("UEPV1");
	std::size_t nodes = 0;
	if (!EncodePropertyValue(writer, value, 0, nodes) || !writer.Ok())
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Unavailable,
			"WATCH_SAMPLE_VALUE_TOO_LARGE",
			"The canonical property value exceeds the per-sample byte or node budget");
	}
	payload.DisplayValue = PropertyDisplayValue(value);
	if (payload.TypeName.size() + payload.CanonicalValue.size()
		+ payload.DisplayValue.size() > maxValueBytes)
	{
		return SampleFailure(
			Runtime::WatchSampleStatus::Unavailable,
			"WATCH_SAMPLE_VALUE_TOO_LARGE",
			"The encoded property payload exceeds the remaining frame byte budget");
	}
	return {
		.Status = Runtime::WatchSampleStatus::Value,
		.Payload = std::move(payload)
	};
}

WatchCommandService::WatchCommandService(
	Runtime::WatchScheduler& scheduler,
	Runtime::CoreRuntime* const runtime,
	Runtime::EngineFacade* const engine) noexcept
	: m_Scheduler(scheduler),
	  m_Runtime(runtime),
	  m_Engine(engine)
{
}

WatchCommandResult WatchCommandService::Execute(
	const std::string_view operation,
	const json& data) noexcept
{
	try
	{
		if (operation == "watch.add") return Add(data);
		if (operation == "watch.list") return List(data);
		if (operation == "watch.enable") return Enable(data);
		if (operation == "watch.remove") return Remove(data);
		if (operation == "watch.snapshot") return Snapshot(data);
		if (operation == "watch.events.drain") return DrainEvents(data);
		return Failure(
			"OPERATION_NOT_SUPPORTED",
			"The watch command service does not implement the requested operation",
			{{"operation", operation}});
	}
	catch (const std::bad_alloc&)
	{
		return Failure(
			"WATCH_ALLOCATION_FAILED",
			"The bounded watch command could not allocate owned state");
	}
	catch (...)
	{
		return Failure(
			"WATCH_COMMAND_FAILED",
			"The watch command could not validate or serialize its bounded data");
	}
}

WatchCommandResult WatchCommandService::Add(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 8
			|| !data.contains("object")
			|| !data.contains("object_snapshot_generation")
			|| !data.contains("type_snapshot_generation")
			|| !data.contains("declaring_type_path")
			|| !data.contains("property_name")
			|| !data.contains("array_index")
			|| !data.contains("interval_ms")
			|| !data.contains("enabled")
			|| !data.at("declaring_type_path").is_string()
			|| !data.at("property_name").is_string()
			|| !data.at("enabled").is_boolean())
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.add data must contain exactly object, object_snapshot_generation, type_snapshot_generation, declaring_type_path, property_name, array_index, interval_ms, and enabled");
		}

		Runtime::WatchSubscriptionSpec spec;
		std::uint64_t objectGeneration = 0;
		std::uint64_t typeGeneration = 0;
		std::uint64_t arrayIndex = 0;
		std::uint64_t intervalMs = 0;
		if (!TryParseObjectHandle(data.at("object"), spec.Object)
			|| !TryUnsigned(
				data.at("object_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				objectGeneration)
			|| !TryUnsigned(
				data.at("type_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				typeGeneration)
			|| !TryUnsigned(data.at("array_index"), 0, kMaxArrayIndex, arrayIndex)
			|| !TryUnsigned(
				data.at("interval_ms"),
				1,
				(std::numeric_limits<std::uint32_t>::max)(),
				intervalMs))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.add contains a malformed handle, generation, array index, or interval");
		}
		spec.DeclaringTypePath = data.at("declaring_type_path").get<std::string>();
		spec.PropertyName = data.at("property_name").get<std::string>();
		if (!IsBoundedText(spec.DeclaringTypePath, kMaxPathBytes)
			|| !IsBoundedText(spec.PropertyName, kMaxNameBytes))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.add declaring_type_path or property_name is empty, oversized, or contains control characters");
		}

		spec.ContextGeneration = spec.Object.ContextGeneration;
		spec.ObjectSnapshotGeneration = objectGeneration;
		spec.TypeSnapshotGeneration = typeGeneration;
		spec.ArrayIndex = static_cast<std::uint32_t>(arrayIndex);
		spec.IntervalMs = static_cast<std::uint32_t>(intervalMs);
		if (m_Runtime && m_Engine)
		{
			const std::shared_ptr<const Runtime::EngineSnapshot> objects =
				m_Engine->Snapshots().Current();
			const std::shared_ptr<const Runtime::TypeSnapshot> types =
				m_Engine->Types().Current();
			const std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection =
				m_Engine->Reflection();
			if (!objects || !types || !reflection
				|| !reflection->IsPropertyCodecConfigured(spec.ContextGeneration)
				|| m_Engine->SessionId() != spec.Object.SessionId
				|| m_Engine->ContextGeneration() != spec.ContextGeneration
				|| objects->Generation != objectGeneration
				|| types->Generation() != typeGeneration
				|| types->ObjectSnapshotGeneration() != objectGeneration
				|| types->ReflectionLayoutFingerprint() != reflection->Layout->Fingerprint())
			{
				return Failure(
					"WATCH_PROPERTY_GENERATION_STALE",
					"watch.add does not match the current immutable object/type/property-codec generation");
			}
			const Runtime::EngineSnapshotObject* record =
				objects->FindByIndex(spec.Object.Index);
			if (!record || !SameHandle(record->Handle, spec.Object))
			{
				return Failure(
					"WATCH_OBJECT_SNAPSHOT_STALE",
					"watch.add object handle is absent from the current immutable snapshot");
			}
			const Runtime::ReflectedType* type = types->FindByFullPath(record->ClassPath);
			const Runtime::ReflectedType* declaringType = nullptr;
			for (std::size_t depth = 0;
				type && depth <= Runtime::TypeSnapshotStore::kMaxHierarchyDepth;
				++depth)
			{
				if (type->FullPath == spec.DeclaringTypePath)
				{
					declaringType = type;
					break;
				}
				if (!type->Super)
					break;
				type = types->FindByObjectIndex(type->Super->Index);
			}
			const Runtime::ReflectedProperty* property = nullptr;
			if (declaringType && declaringType->Kind == Runtime::ReflectedTypeKind::Class)
			{
				for (const Runtime::ReflectedProperty& candidate : declaringType->DirectProperties)
				{
					if (candidate.Name != spec.PropertyName)
						continue;
					if (property)
					{
						return Failure(
							"WATCH_PROPERTY_IDENTITY_AMBIGUOUS",
							"watch.add direct property identity is ambiguous");
					}
					property = &candidate;
				}
			}
			if (!property || spec.ArrayIndex >= property->ArrayDim
				|| property->State != Runtime::ReflectedMemberState::Supported
				|| !property->Descriptor
				|| !reflection->Properties->Supports(property->Kind))
			{
				return Failure(
					"WATCH_PROPERTY_IDENTITY_STALE",
					"watch.add property identity or codec support is absent from the current type generation");
			}
		}
		const bool enabled = data.at("enabled").get<bool>();
		const Runtime::WatchAddResult result = m_Scheduler.Add(spec, enabled);
		if (!result.Ok())
		{
			if (result.Error == Runtime::WatchError::BindingFailed)
			{
				return Failure(
					result.ReasonCode.empty()
						? Runtime::ToString(result.Error) : result.ReasonCode,
					result.Reason.empty()
						? WatchErrorMessage(result.Error) : result.Reason);
			}
			return SchedulerFailure(result.Error);
		}
		return BoundedSuccess({
			{"id", result.Id},
			{"state", enabled ? "enabled" : "disabled"},
			{"spec", SerializeSpec(spec)}
		});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.add could not allocate owned state");
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "watch.add data could not be parsed");
	}
}

WatchCommandResult WatchCommandService::List(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || !data.empty())
			return Failure("INVALID_ARGUMENT", "watch.list data must be an empty object");
		const Runtime::WatchListResult result = m_Scheduler.List();
		if (!result.Ok())
			return SchedulerFailure(result.Error);
		json subscriptions = json::array();
		subscriptions.get_ref<json::array_t&>().reserve(result.Subscriptions.size());
		for (const Runtime::WatchSubscription& subscription : result.Subscriptions)
			subscriptions.push_back(SerializeSubscription(subscription));
		return BoundedSuccess({
			{"enabled_snapshot_generation", result.EnabledSnapshotGeneration},
			{"subscriptions", std::move(subscriptions)},
			{"scheduler", SerializeSchedulerSnapshot(m_Scheduler.Snapshot())}
		});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.list could not allocate serialization storage");
	}
	catch (...)
	{
		return Failure("WATCH_SERIALIZATION_FAILED", "watch.list could not serialize scheduler state");
	}
}

WatchCommandResult WatchCommandService::Enable(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 2
			|| !data.contains("id") || !data.contains("enabled")
			|| !data.at("enabled").is_boolean())
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.enable data must contain exactly id and enabled");
		}
		std::uint64_t id = 0;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id))
			return Failure("INVALID_ARGUMENT", "watch.enable id is invalid");
		const bool enabled = data.at("enabled").get<bool>();
		const Runtime::WatchMutationResult result = m_Scheduler.Enable(id, enabled);
		if (!result.Ok())
			return SchedulerFailure(result.Error, {{"id", id}});
		return BoundedSuccess({{"id", result.Id}, {"enabled", enabled}});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.enable could not allocate response storage");
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "watch.enable data could not be parsed");
	}
}

WatchCommandResult WatchCommandService::Remove(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 1 || !data.contains("id"))
			return Failure("INVALID_ARGUMENT", "watch.remove data must contain exactly id");
		std::uint64_t id = 0;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id))
			return Failure("INVALID_ARGUMENT", "watch.remove id is invalid");
		const Runtime::WatchMutationResult result = m_Scheduler.Remove(id);
		if (!result.Ok())
			return SchedulerFailure(result.Error, {{"id", id}});
		return BoundedSuccess({{"id", result.Id}, {"removed", true}});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.remove could not allocate response storage");
	}
	catch (...)
	{
		return Failure("INVALID_ARGUMENT", "watch.remove data could not be parsed");
	}
}

WatchCommandResult WatchCommandService::Snapshot(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() < 1 || data.size() > 2
			|| !data.contains("id")
			|| (data.size() == 2 && !data.contains("history_limit")))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.snapshot data must contain id and optional history_limit only");
		}
		std::uint64_t id = 0;
		std::uint64_t historyLimit = kMaxSnapshotHistory;
		if (!TryUnsigned(data.at("id"), 1, kMaxProtocolInteger, id)
			|| (data.contains("history_limit")
				&& !TryUnsigned(
					data.at("history_limit"),
					1,
					kMaxSnapshotHistory,
					historyLimit)))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.snapshot id or history_limit is invalid");
		}

		const Runtime::WatchSnapshotResult result = m_Scheduler.Snapshot(id);
		if (!result.Ok())
			return SchedulerFailure(result.Error, {{"id", id}});
		const std::size_t begin = result.History.size() > historyLimit
			? result.History.size() - static_cast<std::size_t>(historyLimit)
			: 0;
		json history = json::array();
		history.get_ref<json::array_t&>().reserve(result.History.size() - begin);
		for (std::size_t index = begin; index < result.History.size(); ++index)
		{
			const Runtime::WatchHistoryEntry& entry = result.History[index];
			history.push_back({
				{"sequence", entry.Sequence},
				{"captured_at_monotonic_us", entry.CapturedAtMonotonicUs},
				{"value", SerializePayload(entry.Value)}
			});
		}
		return BoundedSuccess({
			{"subscription", SerializeSubscription(result.Subscription)},
			{"last_value", SerializePayload(result.LastValue)},
			{"history", std::move(history)},
			{"history_returned", result.History.size() - begin},
			{"history_total", result.History.size()},
			{"history_truncated", begin != 0}
		});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.snapshot could not allocate serialization storage");
	}
	catch (...)
	{
		return Failure("WATCH_SERIALIZATION_FAILED", "watch.snapshot could not serialize watch state");
	}
}

WatchCommandResult WatchCommandService::DrainEvents(const json& data) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 1 || !data.contains("limit"))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.events.drain data must contain exactly limit");
		}
		std::uint64_t limit = 0;
		if (!TryUnsigned(data.at("limit"), 1, kMaxDrainEvents, limit))
		{
			return Failure(
				"INVALID_ARGUMENT",
				"watch.events.drain limit must be between 1 and 32");
		}
		const Runtime::WatchDrainResult result =
			m_Scheduler.DrainEvents(static_cast<std::size_t>(limit));
		if (!result.Ok())
			return SchedulerFailure(result.Error);
		json events = json::array();
		events.get_ref<json::array_t&>().reserve(result.Events.size());
		for (const Runtime::WatchEvent& event : result.Events)
		{
			events.push_back({
				{"sequence", event.Sequence},
				{"id", event.Id},
				{"kind", Runtime::ToString(event.Kind)},
				{"captured_at_monotonic_us", event.CapturedAtMonotonicUs},
				{"value", SerializePayload(event.Value)},
				{"reason_code", event.ReasonCode.empty()
					? json(nullptr) : json(event.ReasonCode)},
				{"reason", event.Reason.empty() ? json(nullptr) : json(event.Reason)},
				{"drop_count", event.DropCount},
				{"coalesce_count", event.CoalesceCount}
			});
		}
		return BoundedSuccess({
			{"events", std::move(events)},
			{"count", result.Events.size()},
			{"dropped_total", result.DroppedTotal},
			{"coalesced_total", result.CoalescedTotal},
			{"more_available", result.MoreAvailable}
		});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("WATCH_ALLOCATION_FAILED", "watch.events.drain could not allocate serialization storage");
	}
	catch (...)
	{
		return Failure("WATCH_SERIALIZATION_FAILED", "watch.events.drain could not serialize events");
	}
}

} // namespace UExplorer::Services
