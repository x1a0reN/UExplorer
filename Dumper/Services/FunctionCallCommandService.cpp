#include "FunctionCallCommandService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <set>
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
constexpr std::size_t kMaxFloatBytes = 128;
constexpr std::size_t kMaxSerializedNodes = 65'536;

FunctionCallCommandError Error(
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

bool SameHandle(const Runtime::ObjectHandle& left, const Runtime::ObjectHandle& right) noexcept
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
	return SameHandle(left.Function, right.Function)
		&& SameHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
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

bool IsCanonicalSignedDecimal(const std::string_view encoded) noexcept
{
	if (encoded.empty())
		return false;
	std::size_t cursor = encoded.front() == '-' ? 1 : 0;
	if (cursor == encoded.size())
		return false;
	if (encoded[cursor] == '0')
		return cursor + 1 == encoded.size() && cursor == 0;
	if (encoded[cursor] < '1' || encoded[cursor] > '9')
		return false;
	for (++cursor; cursor < encoded.size(); ++cursor)
	{
		if (encoded[cursor] < '0' || encoded[cursor] > '9')
			return false;
	}
	return true;
}

bool IsCanonicalUnsignedDecimal(const std::string_view encoded) noexcept
{
	if (encoded.empty())
		return false;
	if (encoded.front() == '0')
		return encoded.size() == 1;
	if (encoded.front() < '1' || encoded.front() > '9')
		return false;
	return std::all_of(encoded.begin() + 1, encoded.end(), [](const char value) {
		return value >= '0' && value <= '9';
	});
}

bool TrySignedDecimal(const json& value, std::int64_t& parsed)
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalSignedDecimal(encoded))
		return false;
	const auto result = std::from_chars(
		encoded.data(), encoded.data() + encoded.size(), parsed, 10);
	return result.ec == std::errc{} && result.ptr == encoded.data() + encoded.size();
}

bool TryUnsignedDecimal(const json& value, std::uint64_t& parsed)
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalUnsignedDecimal(encoded))
		return false;
	const auto result = std::from_chars(
		encoded.data(), encoded.data() + encoded.size(), parsed, 10);
	return result.ec == std::errc{} && result.ptr == encoded.data() + encoded.size();
}

bool TryFiniteDouble(const json& value, double& parsed)
{
	parsed = 0.0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (encoded.empty() || encoded.size() > kMaxFloatBytes
		|| encoded.front() == '+'
		|| std::any_of(encoded.begin(), encoded.end(), [](const unsigned char character) {
			return character <= 0x20 || character == 0x7F;
		}))
	{
		return false;
	}
	const auto result = std::from_chars(
		encoded.data(),
		encoded.data() + encoded.size(),
		parsed,
		std::chars_format::general);
	return result.ec == std::errc{}
		&& result.ptr == encoded.data() + encoded.size()
		&& std::isfinite(parsed);
}

bool IsSignedKind(const Runtime::PropertyKind kind) noexcept
{
	return kind == Runtime::PropertyKind::Int8
		|| kind == Runtime::PropertyKind::Int16
		|| kind == Runtime::PropertyKind::Int32
		|| kind == Runtime::PropertyKind::Int64;
}

bool IsUnsignedKind(const Runtime::PropertyKind kind) noexcept
{
	return kind == Runtime::PropertyKind::UInt8
		|| kind == Runtime::PropertyKind::UInt16
		|| kind == Runtime::PropertyKind::UInt32
		|| kind == Runtime::PropertyKind::UInt64;
}

bool ParseInputValue(
	const json& encoded,
	const Runtime::ReflectedProperty& property,
	Runtime::PropertyInputValue& value,
	FunctionCallCommandError& error)
{
	value = {};
	if (!encoded.is_object()
		|| !encoded.contains("kind") || !encoded.contains("value")
		|| !encoded.at("kind").is_string()
		|| encoded.at("kind").get_ref<const std::string&>() != Runtime::ToString(property.Kind))
	{
		error = Error(
			"CALL_ARGUMENT_ENVELOPE_INVALID",
			"Each argument must contain exactly the matching reflected kind and value",
			{{"parameter", property.Name}, {"expected_kind", Runtime::ToString(property.Kind)}});
		return false;
	}
	const Runtime::CanonicalMathStructKind mathKind = property.Descriptor
		? Runtime::ClassifyCanonicalMathStruct(*property.Descriptor)
		: Runtime::CanonicalMathStructKind::None;
	if (mathKind == Runtime::CanonicalMathStructKind::None)
	{
		if (encoded.size() != 2)
		{
			error = Error(
				"CALL_ARGUMENT_ENVELOPE_INVALID",
				"Scalar arguments must contain exactly kind and value",
				{{"parameter", property.Name}});
			return false;
		}
	}
	else if (encoded.size() != 3
		|| !encoded.contains("type_name")
		|| !encoded.at("type_name").is_string()
		|| encoded.at("type_name").get_ref<const std::string&>() != property.TypeName
		|| property.Descriptor->TypeName != property.TypeName)
	{
		error = Error(
			"CALL_ARGUMENT_STRUCT_TYPE_MISMATCH",
			"Canonical struct arguments must contain the exact reflected type_name",
			{{"parameter", property.Name}, {"expected_type_name", property.TypeName}});
		return false;
	}
	const json& raw = encoded.at("value");
	if (property.Kind == Runtime::PropertyKind::Bool)
	{
		if (!raw.is_boolean())
		{
			error = Error("CALL_ARGUMENT_TYPE_MISMATCH", "The argument must be boolean", {{"parameter", property.Name}});
			return false;
		}
		value = raw.get<bool>();
		return true;
	}
	if (IsSignedKind(property.Kind))
	{
		std::int64_t parsed = 0;
		if (!TrySignedDecimal(raw, parsed))
		{
			error = Error("CALL_ARGUMENT_TYPE_MISMATCH", "The argument must be a canonical signed decimal string", {{"parameter", property.Name}});
			return false;
		}
		value = parsed;
		return true;
	}
	if (IsUnsignedKind(property.Kind))
	{
		std::uint64_t parsed = 0;
		if (!TryUnsignedDecimal(raw, parsed))
		{
			error = Error("CALL_ARGUMENT_TYPE_MISMATCH", "The argument must be a canonical unsigned decimal string", {{"parameter", property.Name}});
			return false;
		}
		value = parsed;
		return true;
	}
	if (property.Kind == Runtime::PropertyKind::Float
		|| property.Kind == Runtime::PropertyKind::Double)
	{
		double parsed = 0.0;
		if (!TryFiniteDouble(raw, parsed))
		{
			error = Error("CALL_ARGUMENT_TYPE_MISMATCH", "The argument must be a finite floating-point string", {{"parameter", property.Name}});
			return false;
		}
		value = parsed;
		return true;
	}
	if (property.Kind == Runtime::PropertyKind::Object)
	{
		if (raw.is_null())
		{
			value = std::monostate{};
			return true;
		}
		Runtime::ObjectHandle handle;
		if (!TryParseObjectHandle(raw, handle))
		{
			error = Error("CALL_ARGUMENT_TYPE_MISMATCH", "The object argument must be null or a complete stable handle", {{"parameter", property.Name}});
			return false;
		}
		value = Runtime::PropertyObjectReference{.Handle = std::move(handle)};
		return true;
	}
	if (mathKind != Runtime::CanonicalMathStructKind::None)
	{
		const std::array<std::string_view, 3> fields =
			mathKind == Runtime::CanonicalMathStructKind::Vector
			? std::array<std::string_view, 3>{"X", "Y", "Z"}
			: std::array<std::string_view, 3>{"Pitch", "Yaw", "Roll"};
		if (!raw.is_object() || raw.size() != fields.size())
		{
			error = Error(
				"CALL_ARGUMENT_STRUCT_VALUE_INVALID",
				"Canonical struct values must contain exactly three semantic components",
				{{"parameter", property.Name}, {"type_name", property.TypeName}});
			return false;
		}
		Runtime::PropertyMathStructInput input{.TypeName = property.TypeName};
		for (std::size_t index = 0; index < fields.size(); ++index)
		{
			const std::string field(fields[index]);
			double component = 0.0;
			if (!raw.contains(field) || !TryFiniteDouble(raw.at(field), component))
			{
				error = Error(
					"CALL_ARGUMENT_STRUCT_VALUE_INVALID",
					"Each canonical struct component must be a finite floating-point string",
					{{"parameter", property.Name}, {"component", field}});
				return false;
			}
			input.Components[index] = component;
		}
		value = std::move(input);
		return true;
	}
	error = Error(
		"CALL_PARAMETER_INPUT_UNAVAILABLE",
		"The reflected parameter kind has no safe owned-frame input codec",
		{{"parameter", property.Name}, {"kind", Runtime::ToString(property.Kind)}});
	return false;
}

bool IsInputDirection(const Runtime::ReflectedParameterDirection direction) noexcept
{
	return direction == Runtime::ReflectedParameterDirection::Input
		|| direction == Runtime::ReflectedParameterDirection::InOut;
}

bool IsOutputDirection(const Runtime::ReflectedParameterDirection direction) noexcept
{
	return direction == Runtime::ReflectedParameterDirection::Output
		|| direction == Runtime::ReflectedParameterDirection::InOut
		|| direction == Runtime::ReflectedParameterDirection::Return;
}

bool IsDeclaringTypeInTargetHierarchy(
	const Runtime::TypeSnapshot& types,
	const std::string_view targetClassPath,
	const std::string_view declaringTypePath) noexcept
{
	const Runtime::ReflectedType* type = types.FindByFullPath(targetClassPath);
	for (std::size_t depth = 0;
		type && depth <= Runtime::TypeSnapshotStore::kMaxHierarchyDepth;
		++depth)
	{
		if (type->FullPath == declaringTypePath)
			return type->Kind == Runtime::ReflectedTypeKind::Class;
		if (!type->Super)
			break;
		type = types.FindByObjectIndex(type->Super->Index);
	}
	return false;
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

	std::string_view SessionId() const noexcept override { return m_Engine.SessionId(); }
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
		return {.State = Runtime::PropertyValueState::Ok, .Handle = object.Handle};
	}

	Runtime::EngineFacade& m_Engine;
	const Runtime::EngineSnapshot& m_Snapshot;
};

} // namespace

FunctionCallWork::FunctionCallWork(
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	Runtime::GameThreadExecutor& gameThread,
	std::shared_ptr<const Runtime::EngineSnapshot> objects,
	std::shared_ptr<const Runtime::TypeSnapshot> types,
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
	Runtime::ObjectHandle target,
	const Runtime::ReflectedFunction& function,
	std::vector<FunctionCallInput> inputs,
	Runtime::ParamFrame frame,
	const std::size_t outputCount)
	: m_Lease(std::move(lease)),
	  m_Engine(engine),
	  m_GameThread(gameThread),
	  m_Objects(std::move(objects)),
	  m_Types(std::move(types)),
	  m_Reflection(std::move(reflection)),
	  m_Target(std::move(target)),
	  m_Function(&function),
	  m_Inputs(std::move(inputs)),
	  m_Frame(std::move(frame))
{
	m_Outputs.reserve(outputCount);
}

const Runtime::FunctionHandle& FunctionCallWork::FunctionHandle() const noexcept
{
	return m_Function->Handle;
}

std::uint64_t FunctionCallWork::TypeSnapshotGeneration() const noexcept
{
	return m_Types ? m_Types->Generation() : 0;
}

std::uint64_t FunctionCallWork::ObjectSnapshotGeneration() const noexcept
{
	return m_Objects ? m_Objects->Generation : 0;
}

bool FunctionCallWork::Execute()
{
	if (!m_Lease.Context()
		|| m_Engine.ContextGeneration() != m_Lease.Context()->Generation()
		|| m_Engine.Snapshots().Current() != m_Objects
		|| m_Engine.Types().Current() != m_Types
		|| m_Engine.Reflection() != m_Reflection
		|| !m_Objects || !m_Types || !m_Reflection || !m_Reflection->Properties
		|| !m_Function)
	{
		m_Error = FunctionCallExecutionError::DependencyChanged;
		return true;
	}
	if (!m_Engine.IsCurrentExecutionThreadValid())
	{
		m_Error = FunctionCallExecutionError::ExecutionThreadInvalid;
		return true;
	}
	const Runtime::ObjectValidationResult targetValidation =
		m_Engine.ValidateObjectHandle(m_Target);
	if (!targetValidation.Ok())
	{
		m_Error = FunctionCallExecutionError::TargetHandleStale;
		m_HandleError = targetValidation.Error;
		return true;
	}
	const Runtime::FunctionValidationResult functionValidation =
		m_Engine.ValidateFunctionHandle(m_Function->Handle);
	if (!functionValidation.Ok())
	{
		m_Error = FunctionCallExecutionError::FunctionHandleStale;
		m_HandleError = functionValidation.Error;
		return true;
	}
	const Runtime::EngineSnapshotObject* target = m_Objects->FindByIndex(m_Target.Index);
	const Runtime::EngineSnapshotObject* function =
		m_Objects->FindByIndex(m_Function->Handle.Function.Index);
	const Runtime::EngineSnapshotObject* owner =
		m_Objects->FindByIndex(m_Function->Handle.Owner.Index);
	if (!target || !function || !owner
		|| !SameHandle(target->Handle, m_Target)
		|| !SameHandle(function->Handle, m_Function->Handle.Function)
		|| !SameHandle(owner->Handle, m_Function->Handle.Owner))
	{
		m_Error = FunctionCallExecutionError::DependencyChanged;
		return true;
	}
	for (const FunctionCallInput& input : m_Inputs)
	{
		const auto* reference = std::get_if<Runtime::PropertyObjectReference>(&input.Value);
		if (!reference)
			continue;
		const Runtime::EngineSnapshotObject* record =
			m_Objects->FindByIndex(reference->Handle.Index);
		const Runtime::ObjectValidationResult validation =
			m_Engine.ValidateObjectHandle(reference->Handle);
		if (!record || !SameHandle(record->Handle, reference->Handle) || !validation.Ok())
		{
			m_Error = FunctionCallExecutionError::ArgumentHandleStale;
			m_HandleError = validation.Error;
			m_FailedArgument = input.Parameter ? input.Parameter->Property.Name : std::string{};
			return true;
		}
	}

	if (!m_GameThread.InvokeProcessEventFromCurrentTask(
		reinterpret_cast<void*>(m_Target.Address),
		reinterpret_cast<void*>(m_Function->Handle.Function.Address),
		m_Frame.Data()))
	{
		m_Error = FunctionCallExecutionError::ProcessEventUnavailable;
		return true;
	}
	m_Invoked = true;

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
	for (const Runtime::ReflectedParameter& parameter : m_Function->Parameters)
	{
		if (!IsOutputDirection(parameter.Direction))
			continue;
		const std::uintptr_t address = m_Frame.ValueAddress(parameter.Property);
		Runtime::PropertyValue value = address == 0
			? Runtime::PropertyValue{
				.State = Runtime::PropertyValueState::Error,
				.Kind = parameter.Property.Kind,
				.TypeName = parameter.Property.TypeName,
				.ErrorCode = "PARAM_FRAME_PARAMETER_OUT_OF_BOUNDS",
				.ErrorMessage = "The output parameter address is outside the owned frame"}
			: m_Reflection->Properties->Decode(
				address,
				*parameter.Property.Descriptor,
				options);
		m_Outputs.push_back(FunctionCallOutput{
			.Name = parameter.Direction == Runtime::ReflectedParameterDirection::Return
				? "return"
				: parameter.Property.Name,
			.Direction = parameter.Direction,
			.Value = std::move(value)
		});
	}
	return true;
}

FunctionCallPreparation FunctionCallCommandService::PrepareInvoke(
	const json& data,
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	Runtime::GameThreadExecutor& gameThread) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 5
			|| !data.contains("target")
			|| !data.contains("function")
			|| !data.contains("type_snapshot_generation")
			|| !data.contains("function_path")
			|| !data.contains("arguments")
			|| !data.at("function_path").is_string()
			|| !data.at("arguments").is_object())
		{
			return {.Error = Error(
				"INVALID_ARGUMENT",
				"Function call data must contain exactly target, function, type_snapshot_generation, function_path, and arguments")};
		}
		if (data.at("arguments").size() > kMaxParameters)
		{
			return {.Error = Error(
				"CALL_ARGUMENT_LIMIT_EXCEEDED",
				"The function call contains more than 128 arguments")};
		}

		Runtime::ObjectHandle targetHandle;
		Runtime::FunctionHandle functionHandle;
		std::uint64_t typeGeneration = 0;
		const std::string functionPath = data.at("function_path").get<std::string>();
		if (!TryParseObjectHandle(data.at("target"), targetHandle)
			|| !TryParseFunctionHandle(data.at("function"), functionHandle)
			|| !TryUnsigned(
				data.at("type_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				typeGeneration)
			|| !IsBoundedText(functionPath, kMaxPathBytes))
		{
			return {.Error = Error(
				"INVALID_ARGUMENT",
				"The target, function identity, generation, or display path is malformed")};
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
				"CALL_DEPENDENCY_MISMATCH",
				"The requested function metadata generation is not the current immutable object/type/codec generation",
				{{"requested_type_generation", typeGeneration},
				 {"current_type_generation", types ? json(types->Generation()) : json(nullptr)},
				 {"current_object_generation", objects ? json(objects->Generation) : json(nullptr)}})};
		}
		const Runtime::EngineSnapshotObject* target = objects->FindByIndex(targetHandle.Index);
		if (!target || !SameHandle(target->Handle, targetHandle))
		{
			return {.Error = Error(
				"CALL_TARGET_SNAPSHOT_MISMATCH",
				"The target handle is not an exact member of the current object generation",
				{{"index", targetHandle.Index}})};
		}
		const Runtime::ReflectedFunctionLookup lookup =
			types->FindFunctionByFullPath(functionPath);
		if (!lookup.Found()
			|| lookup.DeclaringType->Kind != Runtime::ReflectedTypeKind::Class)
		{
			return {.Error = Error(
				"CALL_FUNCTION_NOT_FOUND",
				"The exact function display path is absent from the current TypeSnapshot",
				{{"function_path", functionPath}})};
		}
		if (!SameFunctionHandle(lookup.Function->Handle, functionHandle)
			|| !SameHandle(lookup.DeclaringType->Handle, functionHandle.Owner))
		{
			return {.Error = Error(
				"CALL_FUNCTION_HANDLE_MISMATCH",
				"The function handle does not match the exact TypeSnapshot function identity",
				{{"function_path", functionPath}})};
		}
		if (!IsDeclaringTypeInTargetHierarchy(
			*types,
			target->ClassPath,
			lookup.DeclaringType->FullPath))
		{
			return {.Error = Error(
				"CALL_TARGET_TYPE_MISMATCH",
				"The target class hierarchy does not contain the function declaring type",
				{{"target_class_path", target->ClassPath},
				 {"declaring_type_path", lookup.DeclaringType->FullPath}})};
		}
		if (lookup.Function->Parameters.size() > kMaxParameters)
		{
			return {.Error = Error(
				"CALL_PARAMETER_LIMIT_EXCEEDED",
				"The reflected function contains more than 128 parameters")};
		}

		Runtime::ParamFrame frame;
		const Runtime::ParamFrameResult created = Runtime::ParamFrame::Create(
			lookup.Function->ParameterSize,
			frame);
		if (!created.Ok())
		{
			return {.Error = Error(
				Runtime::ToString(created.Error),
				created.Message,
				{{"parameter_size", lookup.Function->ParameterSize}})};
		}
		std::vector<FunctionCallInput> inputs;
		inputs.reserve(data.at("arguments").size());
		std::set<std::string> parameterNames;
		std::size_t outputCount = 0;
		for (const Runtime::ReflectedParameter& parameter : lookup.Function->Parameters)
		{
			const Runtime::ReflectedProperty& property = parameter.Property;
			if (!parameterNames.emplace(property.Name).second)
			{
				return {.Error = Error(
					"CALL_PARAMETER_IDENTITY_AMBIGUOUS",
					"The reflected function contains duplicate parameter names")};
			}
			if (property.State != Runtime::ReflectedMemberState::Supported
				|| !property.Descriptor
				|| property.ArrayDim != 1
				|| property.Descriptor->Kind != property.Kind
				|| property.Descriptor->TypeName != property.TypeName
				|| !Runtime::ParamFrame::SupportsLifetime(*property.Descriptor)
				|| !reflection->Properties->Supports(property.Kind))
			{
				return {.Error = Error(
					"CALL_PARAMETER_KIND_UNAVAILABLE",
					"The function contains a parameter without a safe trivial owned-frame codec",
					{{"parameter", property.Name},
					 {"kind", Runtime::ToString(property.Kind)},
					 {"state", Runtime::ToString(property.State)},
					 {"array_dim", property.ArrayDim}})};
			}
			if (property.Offset > frame.Size()
				|| property.Size > frame.Size() - property.Offset
				|| property.Descriptor->Size != property.Size)
			{
				return {.Error = Error(
					"CALL_PARAMETER_RANGE_INVALID",
					"The reflected parameter range is outside the exact parameter frame",
					{{"parameter", property.Name},
					 {"offset", property.Offset},
					 {"size", property.Size},
					 {"parameter_size", frame.Size()}})};
			}
			const bool hasArgument = data.at("arguments").contains(property.Name);
			if (IsInputDirection(parameter.Direction))
			{
				if (!hasArgument)
				{
					return {.Error = Error(
						"CALL_ARGUMENT_REQUIRED",
						"A required input or inout argument is missing",
						{{"parameter", property.Name}})};
				}
				if (!reflection->Properties->SupportsInput(*property.Descriptor))
				{
					return {.Error = Error(
						"CALL_PARAMETER_INPUT_UNAVAILABLE",
						"The input parameter kind has no safe owned-frame encoder",
						{{"parameter", property.Name}, {"kind", Runtime::ToString(property.Kind)}})};
				}
				Runtime::PropertyInputValue value;
				FunctionCallCommandError parseError;
				if (!ParseInputValue(
					data.at("arguments").at(property.Name),
					property,
					value,
					parseError))
				{
					return {.Error = std::move(parseError)};
				}
				if (const auto* reference =
					std::get_if<Runtime::PropertyObjectReference>(&value))
				{
					const Runtime::EngineSnapshotObject* argument =
						objects->FindByIndex(reference->Handle.Index);
					if (!argument || !SameHandle(argument->Handle, reference->Handle))
					{
						return {.Error = Error(
							"CALL_ARGUMENT_HANDLE_SNAPSHOT_MISMATCH",
							"An object argument handle is not an exact member of the current object generation",
							{{"parameter", property.Name}, {"index", reference->Handle.Index}})};
					}
				}
				const Runtime::ParamFrameResult encoded = frame.SetInput(
					property,
					value,
					*reflection->Properties);
				if (!encoded.Ok())
				{
					return {.Error = Error(
						encoded.EncodeError == Runtime::PropertyEncodeError::None
							? Runtime::ToString(encoded.Error)
							: Runtime::ToString(encoded.EncodeError),
						encoded.Message,
						{{"parameter", property.Name}})};
				}
				inputs.push_back(FunctionCallInput{
					.Parameter = &parameter,
					.Value = std::move(value)
				});
			}
			else if (hasArgument)
			{
				return {.Error = Error(
					"CALL_OUTPUT_ARGUMENT_FORBIDDEN",
					"Output and return parameters must not be supplied as input arguments",
					{{"parameter", property.Name}})};
			}
			if (IsOutputDirection(parameter.Direction))
				++outputCount;
		}
		if (parameterNames.size() < data.at("arguments").size())
		{
			for (const auto& [name, ignored] : data.at("arguments").items())
			{
				if (!parameterNames.contains(name))
				{
					return {.Error = Error(
						"CALL_ARGUMENT_UNKNOWN",
						"The arguments object contains a name absent from the exact function metadata",
						{{"parameter", name}})};
				}
			}
		}

		auto work = std::shared_ptr<FunctionCallWork>(new FunctionCallWork(
			std::move(lease),
			engine,
			gameThread,
			objects,
			types,
			reflection,
			std::move(targetHandle),
			*lookup.Function,
			std::move(inputs),
			std::move(frame),
			outputCount));
		return {.Work = std::move(work)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"CALL_ALLOCATION_FAILED",
			"The bounded function call could not allocate its owned state")};
	}
	catch (...)
	{
		return {.Error = Error(
			"CALL_PREPARATION_FAILED",
			"The function call could not validate its immutable inputs")};
	}
}

FunctionCallCommandResult FunctionCallCommandService::CompleteInvoke(
	const FunctionCallWork& work) noexcept
{
	try
	{
		if (work.ExecutionError() != FunctionCallExecutionError::None)
		{
			switch (work.ExecutionError())
			{
			case FunctionCallExecutionError::DependencyChanged:
				return {.Error = Error(
					"CALL_DEPENDENCY_CHANGED",
					"The immutable object/type/codec generation changed before game-thread execution")};
			case FunctionCallExecutionError::ExecutionThreadInvalid:
				return {.Error = Error(
					"GAME_THREAD_IDENTITY_INVALID",
					"The function call did not execute on the witnessed game thread")};
			case FunctionCallExecutionError::TargetHandleStale:
				return {.Error = Error(
					"CALL_TARGET_HANDLE_STALE",
					"The target object identity changed before ProcessEvent",
					{{"handle_error", Runtime::ToString(work.HandleError())}})};
			case FunctionCallExecutionError::FunctionHandleStale:
				return {.Error = Error(
					"CALL_FUNCTION_HANDLE_STALE",
					"The function or owner identity changed before ProcessEvent",
					{{"handle_error", Runtime::ToString(work.HandleError())}})};
			case FunctionCallExecutionError::ArgumentHandleStale:
				return {.Error = Error(
					"CALL_ARGUMENT_HANDLE_STALE",
					"An object argument identity changed before ProcessEvent",
					{{"parameter", work.FailedArgument()},
					 {"handle_error", Runtime::ToString(work.HandleError())}})};
			case FunctionCallExecutionError::ProcessEventUnavailable:
				return {.Error = Error(
					"PROCESS_EVENT_UNAVAILABLE",
					"The validated ProcessEvent entrypoint was unavailable on the executing task")};
			case FunctionCallExecutionError::None:
				break;
			}
		}
		if (!work.Invoked())
		{
			return {.Error = Error(
				"CALL_NOT_EXECUTED",
				"The function call reached no terminal ProcessEvent invocation")};
		}

		json outputs = json::array();
		outputs.get_ref<json::array_t&>().reserve(work.Outputs().size());
		std::size_t nodes = 0;
		for (const FunctionCallOutput& output : work.Outputs())
		{
			json value;
			if (!SerializeValue(output.Value, value, nodes, 0))
			{
				return {.Error = Error(
					"CALL_RESULT_LIMIT_EXCEEDED",
					"The decoded function outputs exceeded the serialized node or depth limit")};
			}
			outputs.push_back({
				{"name", output.Name},
				{"direction", Runtime::ToString(output.Direction)},
				{"value", std::move(value)}
			});
		}
		json data = {
			{"target", SerializeObjectHandle(work.Target())},
			{"function", SerializeFunctionHandle(work.FunctionHandle())},
			{"function_path", work.Function().FullPath},
			{"type_snapshot_generation", work.TypeSnapshotGeneration()},
			{"object_snapshot_generation", work.ObjectSnapshotGeneration()},
			{"invoked", true},
			{"outputs", std::move(outputs)}
		};
		if (data.dump().size() > kMaxSerializedDataBytes)
		{
			return {.Error = Error(
				"CALL_RESPONSE_TOO_LARGE",
				"The bounded function result exceeds the 4 MiB command-data ceiling")};
		}
		return {.Data = std::move(data)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"CALL_ALLOCATION_FAILED",
			"The bounded function response could not allocate serialization storage")};
	}
	catch (...)
	{
		return {.Error = Error(
			"CALL_SERIALIZATION_FAILED",
			"The function result could not be serialized")};
	}
}

} // namespace UExplorer::Services
