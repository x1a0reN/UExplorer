#include "ObjectPropertyCommandService.h"

#include "Runtime/SafeMemory.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxNameBytes = 1024;
constexpr std::size_t kMaxSerializedNodes = 65'536;

ObjectPropertyCommandError Error(
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
		|| !TryUnsigned(data.at("context_generation"), 1, kMaxProtocolInteger, contextGeneration)
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

bool SameHandle(const Runtime::ObjectHandle& left, const Runtime::ObjectHandle& right) noexcept
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

json SerializeScalar(const Runtime::PropertyScalar& scalar)
{
	if (std::holds_alternative<std::monostate>(scalar))
		return nullptr;
	if (const auto value = std::get_if<bool>(&scalar))
		return {{"kind", "bool"}, {"value", *value}};
	if (const auto value = std::get_if<std::int64_t>(&scalar))
		return {{"kind", "int64"}, {"value", std::to_string(*value)}};
	if (const auto value = std::get_if<std::uint64_t>(&scalar))
		return {{"kind", "uint64"}, {"value", std::to_string(*value)}};
	if (const auto value = std::get_if<double>(&scalar))
	{
		std::string encoded;
		if (std::isnan(*value)) encoded = "NaN";
		else if (std::isinf(*value)) encoded = std::signbit(*value) ? "-Infinity" : "Infinity";
		else encoded = std::format("{:.17g}", *value);
		return {{"kind", "float64"}, {"value", std::move(encoded)}};
	}
	if (const auto value = std::get_if<std::string>(&scalar))
		return {{"kind", "string"}, {"value", *value}};
	const auto& reference = std::get<Runtime::PropertyObjectReference>(scalar);
	return {{"kind", "object"}, {"value", SerializeObjectHandle(reference.Handle)}};
}

bool SerializeValue(
	const Runtime::PropertyValue& value,
	json& serialized,
	std::size_t& nodes,
	const std::size_t depth)
{
	if (depth > 64 || ++nodes > kMaxSerializedNodes)
		return false;
	json children = json::array();
	children.get_ref<json::array_t&>().reserve(value.Children.size());
	for (const Runtime::PropertyValue& child : value.Children)
	{
		json encoded;
		if (!SerializeValue(child, encoded, nodes, depth + 1))
			return false;
		children.push_back(std::move(encoded));
	}
	serialized = {
		{"state", Runtime::ToString(value.State)},
		{"kind", Runtime::ToString(value.Kind)},
		{"label", value.Label.empty() ? json(nullptr) : json(value.Label)},
		{"type_name", value.TypeName},
		{"scalar", SerializeScalar(value.Scalar)},
		{"display_name", value.DisplayName.empty() ? json(nullptr) : json(value.DisplayName)},
		{"children", std::move(children)},
		{"total_count", value.TotalCount},
		{"truncated", value.Truncated},
		{"error_code", value.ErrorCode.empty() ? json(nullptr) : json(value.ErrorCode)},
		{"error", value.ErrorMessage.empty() ? json(nullptr) : json(value.ErrorMessage)}
	};
	return true;
}

class SnapshotReferenceResolver final : public Runtime::IPropertyReferenceResolver
{
public:
	SnapshotReferenceResolver(
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

} // namespace

ObjectPropertyReadWork::ObjectPropertyReadWork(
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	std::shared_ptr<const Runtime::EngineSnapshot> objects,
	std::shared_ptr<const Runtime::TypeSnapshot> types,
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
	Runtime::ObjectHandle object,
	const Runtime::ReflectedProperty& property,
	std::string declaringTypePath,
	const std::uint32_t arrayIndex)
	: m_Lease(std::move(lease)),
	  m_Engine(engine),
	  m_Objects(std::move(objects)),
	  m_Types(std::move(types)),
	  m_Reflection(std::move(reflection)),
	  m_Object(std::move(object)),
	  m_Property(&property),
	  m_DeclaringTypePath(std::move(declaringTypePath)),
	  m_PropertyName(property.Name),
	  m_ArrayIndex(arrayIndex)
{
}

bool ObjectPropertyReadWork::Execute()
{
	if (!m_Lease.Context()
		|| m_Engine.ContextGeneration() != m_Lease.Context()->Generation()
		|| m_Engine.Snapshots().Current() != m_Objects
		|| m_Engine.Types().Current() != m_Types
		|| m_Engine.Reflection() != m_Reflection
		|| !m_Objects || !m_Types || !m_Reflection || !m_Reflection->Properties
		|| !m_Property || !m_Property->Descriptor)
	{
		m_Error = ObjectPropertyExecutionError::DependencyChanged;
		return true;
	}
	if (!m_Engine.IsCurrentExecutionThreadValid())
	{
		m_Error = ObjectPropertyExecutionError::ExecutionThreadInvalid;
		return true;
	}
	const Runtime::ObjectValidationResult validation =
		m_Engine.ValidateObjectHandle(m_Object);
	if (!validation.Ok())
	{
		m_Error = ObjectPropertyExecutionError::ObjectHandleStale;
		m_HandleError = validation.Error;
		return true;
	}
	const Runtime::EngineSnapshotObject* exact = m_Objects->FindByIndex(m_Object.Index);
	if (!exact || !SameHandle(exact->Handle, m_Object))
	{
		m_Error = ObjectPropertyExecutionError::DependencyChanged;
		return true;
	}

	const std::uint64_t elementOffset = static_cast<std::uint64_t>(m_Property->Offset)
		+ static_cast<std::uint64_t>(m_ArrayIndex) * m_Property->Size;
	if (elementOffset > (std::numeric_limits<std::uintptr_t>::max)() - m_Object.Address)
	{
		m_Error = ObjectPropertyExecutionError::AddressOverflow;
		return true;
	}
	const std::uintptr_t valueAddress = m_Object.Address
		+ static_cast<std::uintptr_t>(elementOffset);
	std::uintptr_t rangeEnd = 0;
	if (!Runtime::CheckedAddressRange(valueAddress, m_Property->Size, rangeEnd))
	{
		m_Error = ObjectPropertyExecutionError::AddressOverflow;
		return true;
	}

	SnapshotReferenceResolver references(m_Engine, *m_Objects);
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
	m_Value = m_Reflection->Properties->Decode(
		valueAddress,
		*m_Property->Descriptor,
		std::move(options));
	return true;
}

std::uint64_t ObjectPropertyReadWork::TypeSnapshotGeneration() const noexcept
{
	return m_Types ? m_Types->Generation() : 0;
}

std::uint64_t ObjectPropertyReadWork::ObjectSnapshotGeneration() const noexcept
{
	return m_Objects ? m_Objects->Generation : 0;
}

std::uint32_t ObjectPropertyReadWork::PropertyOffset() const noexcept
{
	return m_Property ? m_Property->Offset : 0;
}

std::uint32_t ObjectPropertyReadWork::PropertySize() const noexcept
{
	return m_Property ? m_Property->Size : 0;
}

ObjectPropertyReadPreparation ObjectPropertyCommandService::PrepareRead(
	const json& data,
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 5
			|| !data.contains("object")
			|| !data.contains("type_snapshot_generation")
			|| !data.contains("declaring_type_path")
			|| !data.contains("property_name")
			|| !data.contains("array_index")
			|| !data.at("declaring_type_path").is_string()
			|| !data.at("property_name").is_string())
		{
			return {.Error = Error(
				"INVALID_ARGUMENT",
				"Property read data must contain exactly object, type_snapshot_generation, declaring_type_path, property_name, and array_index")};
		}

		Runtime::ObjectHandle object;
		std::uint64_t typeGeneration = 0;
		std::uint64_t arrayIndex = 0;
		const std::string declaringPath = data.at("declaring_type_path").get<std::string>();
		const std::string propertyName = data.at("property_name").get<std::string>();
		if (!TryParseObjectHandle(data.at("object"), object)
			|| !TryUnsigned(
				data.at("type_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				typeGeneration)
			|| !TryUnsigned(data.at("array_index"), 0, 1023, arrayIndex)
			|| !IsBoundedText(declaringPath, kMaxPathBytes)
			|| !IsBoundedText(propertyName, kMaxNameBytes))
		{
			return {.Error = Error(
				"INVALID_ARGUMENT",
				"Property read identity contains a malformed handle, generation, path, name, or array index")};
		}

		const std::shared_ptr<const Runtime::EngineSnapshot> objects =
			engine.Snapshots().Current();
		const std::shared_ptr<const Runtime::TypeSnapshot> types =
			engine.Types().Current();
		const std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection =
			engine.Reflection();
		if (!lease.Context() || !objects || !types || !reflection
			|| !reflection->IsPropertyCodecConfigured(lease.Context()->Generation())
			|| types->Generation() != typeGeneration
			|| types->ObjectSnapshotGeneration() != objects->Generation
			|| types->ReflectionLayoutFingerprint() != reflection->Layout->Fingerprint())
		{
			return {.Error = Error(
				"PROPERTY_DEPENDENCY_MISMATCH",
				"The requested property metadata generation is not the current immutable object/type/codec generation",
				{{"requested_type_generation", typeGeneration},
				 {"current_type_generation", types ? json(types->Generation()) : json(nullptr)},
				 {"current_object_generation", objects ? json(objects->Generation) : json(nullptr)}})};
		}
		const Runtime::EngineSnapshotObject* record = objects->FindByIndex(object.Index);
		if (!record || !SameHandle(record->Handle, object))
		{
			return {.Error = Error(
				"OBJECT_HANDLE_SNAPSHOT_MISMATCH",
				"The object handle is not an exact member of the current immutable object generation",
				{{"index", object.Index}})};
		}

		const Runtime::ReflectedType* type = types->FindByFullPath(record->ClassPath);
		const Runtime::ReflectedType* declaringType = nullptr;
		for (std::size_t depth = 0; type && depth <= Runtime::TypeSnapshotStore::kMaxHierarchyDepth; ++depth)
		{
			if (type->FullPath == declaringPath)
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
			return {.Error = Error(
				"PROPERTY_DECLARING_TYPE_MISMATCH",
				"The declaring type is not in the exact runtime class hierarchy",
				{{"object_class_path", record->ClassPath}, {"declaring_type_path", declaringPath}})};
		}
		const Runtime::ReflectedProperty* property = nullptr;
		for (const Runtime::ReflectedProperty& candidate : declaringType->DirectProperties)
		{
			if (candidate.Name == propertyName)
			{
				if (property)
				{
					return {.Error = Error(
						"PROPERTY_IDENTITY_AMBIGUOUS",
						"The declaring type contains duplicate direct property names")};
				}
				property = &candidate;
			}
		}
		if (!property)
		{
			return {.Error = Error(
				"PROPERTY_NOT_FOUND",
				"The declaring type has no direct property with the requested exact name",
				{{"declaring_type_path", declaringPath}, {"property_name", propertyName}})};
		}
		if (arrayIndex >= property->ArrayDim)
		{
			return {.Error = Error(
				"PROPERTY_ARRAY_INDEX_OUT_OF_RANGE",
				"array_index is outside the witnessed fixed-array dimension",
				{{"array_index", arrayIndex}, {"array_dim", property->ArrayDim}})};
		}
		if (property->State != Runtime::ReflectedMemberState::Supported
			|| !property->Descriptor)
		{
			return {.Error = Error(
				property->State == Runtime::ReflectedMemberState::Unsupported
					? "PROPERTY_KIND_UNSUPPORTED"
					: "PROPERTY_DESCRIPTOR_UNAVAILABLE",
				property->Reason.empty()
					? "No immutable descriptor is available for the requested property"
					: property->Reason,
				 {{"state", Runtime::ToString(property->State)},
				  {"reason_code", property->ReasonCode.empty() ? json(nullptr) : json(property->ReasonCode)}})};
		}
		if (!reflection->Properties->Supports(property->Kind))
		{
			return {.Error = Error(
				"PROPERTY_CODEC_KIND_UNAVAILABLE",
				"The current immutable codec profile does not witness the requested property kind",
				{{"kind", Runtime::ToString(property->Kind)},
				 {"profile_source", reflection->Properties->Profile().Source}})};
		}

		auto work = std::shared_ptr<ObjectPropertyReadWork>(new ObjectPropertyReadWork(
			std::move(lease),
			engine,
			objects,
			types,
			reflection,
			std::move(object),
			*property,
			declaringPath,
			static_cast<std::uint32_t>(arrayIndex)));
		return {.Work = std::move(work)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"PROPERTY_ALLOCATION_FAILED",
			"The bounded property read command could not allocate its owned state")};
	}
	catch (...)
	{
		return {.Error = Error(
			"PROPERTY_PREPARATION_FAILED",
			"The property read command could not validate its immutable inputs")};
	}
}

ObjectPropertyCommandResult ObjectPropertyCommandService::CompleteRead(
	const ObjectPropertyReadWork& work) noexcept
{
	try
	{
		if (work.ExecutionError() != ObjectPropertyExecutionError::None)
		{
			switch (work.ExecutionError())
			{
			case ObjectPropertyExecutionError::DependencyChanged:
				return {.Error = Error(
					"PROPERTY_DEPENDENCY_CHANGED",
					"The immutable object/type/codec generation changed before game-thread execution")};
			case ObjectPropertyExecutionError::ExecutionThreadInvalid:
				return {.Error = Error(
					"GAME_THREAD_IDENTITY_INVALID",
					"The property read did not execute on the witnessed game thread")};
			case ObjectPropertyExecutionError::ObjectHandleStale:
				return {.Error = Error(
					"OBJECT_HANDLE_STALE",
					"The object identity changed before the property read executed",
					{{"handle_error", Runtime::ToString(work.HandleError())}})};
			case ObjectPropertyExecutionError::AddressOverflow:
				return {.Error = Error(
					"PROPERTY_ADDRESS_OVERFLOW",
					"The witnessed property offset and fixed-array index overflowed the object address")};
			case ObjectPropertyExecutionError::None:
				break;
			}
		}

		json value;
		std::size_t nodes = 0;
		if (!SerializeValue(work.Value(), value, nodes, 0))
		{
			return {.Error = Error(
				"PROPERTY_VALUE_LIMIT_EXCEEDED",
				"The decoded property value exceeded the serialized node or depth limit")};
		}
		json data = {
			{"object", SerializeObjectHandle(work.Object())},
			{"type_snapshot_generation", work.TypeSnapshotGeneration()},
			{"object_snapshot_generation", work.ObjectSnapshotGeneration()},
			{"declaring_type_path", work.DeclaringTypePath()},
			{"property_name", work.PropertyName()},
			{"array_index", work.ArrayIndex()},
			{"offset", work.PropertyOffset()},
			{"size", work.PropertySize()},
			{"value", std::move(value)}
		};
		if (data.dump().size() > kMaxSerializedDataBytes)
		{
			return {.Error = Error(
				"PROPERTY_RESPONSE_TOO_LARGE",
				"The bounded property result exceeds the 4 MiB command-data ceiling")};
		}
		return {.Data = std::move(data)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"PROPERTY_ALLOCATION_FAILED",
			"The bounded property response could not allocate serialization storage")};
	}
	catch (...)
	{
		return {.Error = Error(
			"PROPERTY_SERIALIZATION_FAILED",
			"The decoded property value could not be serialized")};
	}
}

} // namespace UExplorer::Services
