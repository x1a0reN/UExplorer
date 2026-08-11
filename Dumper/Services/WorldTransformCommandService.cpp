#include "WorldTransformCommandService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::string_view kActorClassPath = "/Script/Engine.Actor";
constexpr std::string_view kSceneComponentClassPath = "/Script/Engine.SceneComponent";
constexpr std::string_view kVectorStructPath = "/Script/CoreUObject.Vector";
constexpr std::string_view kRotatorStructPath = "/Script/CoreUObject.Rotator";

WorldTransformCommandError Error(
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

const Runtime::ReflectedType* FindExactAncestor(
	const Runtime::TypeSnapshot& types,
	const std::string_view concretePath,
	const std::string_view ancestorPath) noexcept
{
	const Runtime::ReflectedType* current = types.FindByFullPath(concretePath);
	for (std::size_t depth = 0;
		current && depth <= Runtime::TypeSnapshotStore::kMaxHierarchyDepth;
		++depth)
	{
		if (current->Kind != Runtime::ReflectedTypeKind::Class)
			return nullptr;
		if (current->FullPath == ancestorPath)
			return current;
		if (!current->Super)
			return nullptr;
		const Runtime::ReflectedType* parent =
			types.FindByObjectIndex(current->Super->Index);
		if (!parent || !SameHandle(*current->Super, parent->Handle))
			return nullptr;
		current = parent;
	}
	return nullptr;
}

const Runtime::ReflectedProperty* FindUniqueDirectProperty(
	const Runtime::ReflectedType& type,
	const std::string_view name,
	bool& ambiguous) noexcept
{
	ambiguous = false;
	const Runtime::ReflectedProperty* found = nullptr;
	for (const Runtime::ReflectedProperty& property : type.DirectProperties)
	{
		if (property.Name != name)
			continue;
		if (found)
		{
			ambiguous = true;
			return nullptr;
		}
		found = &property;
	}
	return found;
}

bool IsPropertyRangeValid(
	const Runtime::ReflectedProperty& property,
	const Runtime::ReflectedType& declaringType) noexcept
{
	return property.Offset <= declaringType.PropertiesSize
		&& property.Size <= declaringType.PropertiesSize - property.Offset;
}

bool ValidateRootProperty(
	const Runtime::ReflectedProperty& property,
	const Runtime::ReflectedType& declaringType,
	const Runtime::PropertyCodec& codec) noexcept
{
	return property.State == Runtime::ReflectedMemberState::Supported
		&& property.Kind == Runtime::PropertyKind::Object
		&& property.ArrayDim == 1
		&& property.Size == sizeof(std::uintptr_t)
		&& property.Descriptor
		&& property.Descriptor->Kind == Runtime::PropertyKind::Object
		&& property.Descriptor->TypeName == kSceneComponentClassPath
		&& property.Descriptor->Size == property.Size
		&& IsPropertyRangeValid(property, declaringType)
		&& codec.Supports(property.Kind);
}

bool ValidateMathProperty(
	const Runtime::ReflectedProperty& property,
	const Runtime::ReflectedType& declaringType,
	const Runtime::PropertyCodec& codec,
	const std::string_view typePath,
	const std::array<std::string_view, 3>& fieldNames,
	WorldTransformPrecision& precision) noexcept
{
	if (property.State != Runtime::ReflectedMemberState::Supported
		|| property.Kind != Runtime::PropertyKind::Struct
		|| property.TypeName != typePath
		|| property.ArrayDim != 1
		|| !property.Descriptor
		|| property.Descriptor->Kind != Runtime::PropertyKind::Struct
		|| property.Descriptor->TypeName != typePath
		|| property.Descriptor->Size != property.Size
		|| property.Descriptor->Fields.size() != fieldNames.size()
		|| !IsPropertyRangeValid(property, declaringType)
		|| !codec.Supports(property.Kind))
	{
		return false;
	}

	Runtime::PropertyKind scalarKind = Runtime::PropertyKind::Unknown;
	for (std::size_t index = 0; index < fieldNames.size(); ++index)
	{
		const Runtime::PropertyFieldDescriptor& field = property.Descriptor->Fields[index];
		if (field.Name != fieldNames[index]
			|| !field.Descriptor
			|| (field.Descriptor->Kind != Runtime::PropertyKind::Float
				&& field.Descriptor->Kind != Runtime::PropertyKind::Double))
		{
			return false;
		}
		if (index == 0)
			scalarKind = field.Descriptor->Kind;
		if (field.Descriptor->Kind != scalarKind)
			return false;
	}
	precision = scalarKind == Runtime::PropertyKind::Double
		? WorldTransformPrecision::Float64
		: WorldTransformPrecision::Float32;
	return true;
}

bool ValidateBoolProperty(
	const Runtime::ReflectedProperty& property,
	const Runtime::ReflectedType& declaringType,
	const Runtime::PropertyCodec& codec) noexcept
{
	return property.State == Runtime::ReflectedMemberState::Supported
		&& property.Kind == Runtime::PropertyKind::Bool
		&& property.ArrayDim == 1
		&& property.Descriptor
		&& property.Descriptor->Kind == Runtime::PropertyKind::Bool
		&& property.Descriptor->Size == property.Size
		&& property.Descriptor->BoolMask != 0
		&& property.Descriptor->BoolByteOffset < property.Size
		&& IsPropertyRangeValid(property, declaringType)
		&& codec.Supports(property.Kind);
}

bool ExtractMathValue(
	const Runtime::PropertyValue& value,
	const std::string_view typePath,
	const std::array<std::string_view, 3>& fieldNames,
	std::array<double, 3>& output,
	std::string& errorCode,
	std::string& error) noexcept
{
	if (!value.Ok())
	{
		errorCode = value.ErrorCode.empty()
			? "WORLD_TRANSFORM_VALUE_DECODE_FAILED"
			: value.ErrorCode;
		error = value.ErrorMessage.empty()
			? "A canonical transform struct could not be decoded"
			: value.ErrorMessage;
		return false;
	}
	if (value.Kind != Runtime::PropertyKind::Struct
		|| value.TypeName != typePath
		|| value.Children.size() != fieldNames.size())
	{
		errorCode = "WORLD_TRANSFORM_VALUE_SHAPE_INVALID";
		error = "A canonical transform struct returned an unexpected value shape";
		return false;
	}
	for (std::size_t index = 0; index < fieldNames.size(); ++index)
	{
		const Runtime::PropertyValue& field = value.Children[index];
		const double* scalar = std::get_if<double>(&field.Scalar);
		if (!field.Ok() || field.Label != fieldNames[index] || !scalar
			|| !std::isfinite(*scalar))
		{
			errorCode = "WORLD_TRANSFORM_VALUE_SHAPE_INVALID";
			error = "A canonical transform field was missing, mislabeled, or non-finite";
			return false;
		}
		output[index] = *scalar;
	}
	return true;
}

bool ExtractBoolValue(
	const Runtime::PropertyValue& value,
	bool& output,
	std::string& errorCode,
	std::string& error) noexcept
{
	if (!value.Ok())
	{
		errorCode = value.ErrorCode.empty()
			? "WORLD_TRANSFORM_VALUE_DECODE_FAILED"
			: value.ErrorCode;
		error = value.ErrorMessage.empty()
			? "An absolute-space flag could not be decoded"
			: value.ErrorMessage;
		return false;
	}
	const bool* scalar = std::get_if<bool>(&value.Scalar);
	if (value.Kind != Runtime::PropertyKind::Bool || !scalar)
	{
		errorCode = "WORLD_TRANSFORM_VALUE_SHAPE_INVALID";
		error = "An absolute-space flag returned an unexpected value shape";
		return false;
	}
	output = *scalar;
	return true;
}

json SerializeHandle(const Runtime::ObjectHandle& handle)
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

json SerializeObject(const Runtime::WorldSnapshotObject& object)
{
	return {
		{"handle", SerializeHandle(object.Handle)},
		{"index", object.Handle.Index},
		{"name", object.Name},
		{"full_path", object.FullPath},
		{"class_path", object.ClassPath},
		{"class", object.ClassPath},
		{"address", std::format("0x{:X}", object.Handle.Address)}
	};
}

} // namespace

WorldTransformReadWork::WorldTransformReadWork(
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	std::shared_ptr<const Runtime::EngineSnapshot> objects,
	std::shared_ptr<const Runtime::TypeSnapshot> types,
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
	std::shared_ptr<const Runtime::WorldSnapshot> world,
	Runtime::WorldSnapshotObject actor,
	Runtime::WorldSnapshotObject rootComponent,
	const Runtime::ReflectedProperty& actorRootProperty,
	std::array<const Runtime::ReflectedProperty*, 3> transformProperties,
	std::array<const Runtime::ReflectedProperty*, 3> absoluteProperties,
	const WorldTransformPrecision precision,
	const std::uint32_t spanOffset,
	const std::uint32_t spanSize)
	: m_Lease(std::move(lease)),
	  m_Engine(engine),
	  m_Objects(std::move(objects)),
	  m_Types(std::move(types)),
	  m_Reflection(std::move(reflection)),
	  m_World(std::move(world)),
	  m_Actor(std::move(actor)),
	  m_RootComponent(std::move(rootComponent)),
	  m_ActorRootProperty(&actorRootProperty),
	  m_TransformProperties(transformProperties),
	  m_AbsoluteProperties(absoluteProperties),
	  m_SpanOffset(spanOffset),
	  m_FirstBytes(spanSize),
	  m_StableBytes(spanSize),
	  m_FinalBytes(spanSize)
{
	m_Value.Precision = precision;
}

bool WorldTransformReadWork::Execute()
{
	if (!m_Lease.Context()
		|| m_Engine.ContextGeneration() != m_Lease.Context()->Generation()
		|| m_Engine.Snapshots().Current() != m_Objects
		|| m_Engine.Types().Current() != m_Types
		|| m_Engine.Reflection() != m_Reflection
		|| m_Engine.Worlds().Current() != m_World
		|| !m_Objects || !m_Types || !m_Reflection || !m_Reflection->Properties
		|| !m_World || !m_ActorRootProperty
		|| std::ranges::any_of(m_TransformProperties, [](const auto* value) { return !value; })
		|| std::ranges::any_of(m_AbsoluteProperties, [](const auto* value) { return !value; }))
	{
		m_Error = WorldTransformExecutionError::DependencyChanged;
		return true;
	}
	if (!m_Engine.IsCurrentExecutionThreadValid())
	{
		m_Error = WorldTransformExecutionError::ExecutionThreadInvalid;
		return true;
	}

	const Runtime::ObjectValidationResult actorValidation =
		m_Engine.ValidateObjectHandle(m_Actor.Handle);
	if (!actorValidation.Ok())
	{
		m_Error = WorldTransformExecutionError::ActorHandleStale;
		m_HandleError = actorValidation.Error;
		return true;
	}
	const Runtime::ObjectValidationResult rootValidation =
		m_Engine.ValidateObjectHandle(m_RootComponent.Handle);
	if (!rootValidation.Ok())
	{
		m_Error = WorldTransformExecutionError::RootComponentHandleStale;
		m_HandleError = rootValidation.Error;
		return true;
	}
	const Runtime::EngineSnapshotObject* actorRecord =
		m_Objects->FindByIndex(m_Actor.Handle.Index);
	const Runtime::EngineSnapshotObject* rootRecord =
		m_Objects->FindByIndex(m_RootComponent.Handle.Index);
	const Runtime::WorldSnapshotActor* worldActor =
		m_World->FindActorByIndex(m_Actor.Handle.Index);
	if (!actorRecord || !rootRecord || !worldActor
		|| !SameHandle(actorRecord->Handle, m_Actor.Handle)
		|| !SameHandle(rootRecord->Handle, m_RootComponent.Handle)
		|| !SameHandle(worldActor->Object.Handle, m_Actor.Handle)
		|| worldActor->RootComponent.State != Runtime::WorldReferenceState::Present
		|| !worldActor->RootComponent.Object
		|| !SameHandle(worldActor->RootComponent.Object->Handle, m_RootComponent.Handle))
	{
		m_Error = WorldTransformExecutionError::DependencyChanged;
		return true;
	}

	std::uintptr_t rootFieldAddress = 0;
	if (m_ActorRootProperty->Offset
		> (std::numeric_limits<std::uintptr_t>::max)() - m_Actor.Handle.Address)
	{
		m_Error = WorldTransformExecutionError::AddressOverflow;
		return true;
	}
	rootFieldAddress = m_Actor.Handle.Address + m_ActorRootProperty->Offset;
	std::uintptr_t rootFirst = 0;
	std::uintptr_t rootSecond = 0;
	Runtime::MemoryResult memory = Runtime::ReadValue(rootFieldAddress, rootFirst);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	memory = Runtime::ReadValue(rootFieldAddress, rootSecond);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	if (rootFirst != rootSecond || rootSecond != m_RootComponent.Handle.Address)
	{
		m_Error = WorldTransformExecutionError::RootComponentChanged;
		return true;
	}

	if (m_SpanOffset > (std::numeric_limits<std::uintptr_t>::max)()
		- m_RootComponent.Handle.Address)
	{
		m_Error = WorldTransformExecutionError::AddressOverflow;
		return true;
	}
	const std::uintptr_t liveSpanAddress =
		m_RootComponent.Handle.Address + m_SpanOffset;
	std::uintptr_t liveSpanEnd = 0;
	if (!Runtime::CheckedAddressRange(
		liveSpanAddress,
		m_StableBytes.size(),
		liveSpanEnd))
	{
		m_Error = WorldTransformExecutionError::AddressOverflow;
		return true;
	}

	memory = Runtime::ReadMemory(liveSpanAddress, m_FirstBytes);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	memory = Runtime::ReadMemory(liveSpanAddress, m_StableBytes);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	if (m_FirstBytes != m_StableBytes)
	{
		m_Error = WorldTransformExecutionError::ValueChangedDuringRead;
		return true;
	}

	std::array<Runtime::PropertyValue, 3> transforms;
	std::array<Runtime::PropertyValue, 3> absolutes;
	const auto stableBase = reinterpret_cast<std::uintptr_t>(m_StableBytes.data());
	for (std::size_t index = 0; index < m_TransformProperties.size(); ++index)
	{
		const Runtime::ReflectedProperty& property = *m_TransformProperties[index];
		const std::uintptr_t address = stableBase + property.Offset - m_SpanOffset;
		transforms[index] = m_Reflection->Properties->Decode(
			address,
			*property.Descriptor,
			{.Limits = {
				.MaxDepth = 3,
				.MaxContainerElements = 8,
				.MaxTotalNodes = 32,
				.MaxStringCodeUnits = 0,
				.MaxReadableContainerBytes = WorldTransformCommandService::kMaxAggregateBytes}});
	}
	for (std::size_t index = 0; index < m_AbsoluteProperties.size(); ++index)
	{
		const Runtime::ReflectedProperty& property = *m_AbsoluteProperties[index];
		const std::uintptr_t address = stableBase + property.Offset - m_SpanOffset;
		absolutes[index] = m_Reflection->Properties->Decode(
			address,
			*property.Descriptor,
			{.Limits = {
				.MaxDepth = 1,
				.MaxContainerElements = 1,
				.MaxTotalNodes = 4,
				.MaxStringCodeUnits = 0,
				.MaxReadableContainerBytes = WorldTransformCommandService::kMaxAggregateBytes}});
	}

	std::array<double, 3> location{};
	std::array<double, 3> rotation{};
	std::array<double, 3> scale{};
	std::array<bool, 3> absolute{};
	if (!ExtractMathValue(
		transforms[0],
		kVectorStructPath,
		{"X", "Y", "Z"},
		location,
		m_DecodeErrorCode,
		m_DecodeError)
		|| !ExtractMathValue(
			transforms[1],
			kRotatorStructPath,
			{"Pitch", "Yaw", "Roll"},
			rotation,
			m_DecodeErrorCode,
			m_DecodeError)
		|| !ExtractMathValue(
			transforms[2],
			kVectorStructPath,
			{"X", "Y", "Z"},
			scale,
			m_DecodeErrorCode,
			m_DecodeError))
	{
		m_Error = m_DecodeErrorCode == "WORLD_TRANSFORM_VALUE_SHAPE_INVALID"
			? WorldTransformExecutionError::ValueShapeInvalid
			: WorldTransformExecutionError::ValueDecodeFailed;
		return true;
	}
	for (std::size_t index = 0; index < absolutes.size(); ++index)
	{
		if (!ExtractBoolValue(
			absolutes[index],
			absolute[index],
			m_DecodeErrorCode,
			m_DecodeError))
		{
			m_Error = m_DecodeErrorCode == "WORLD_TRANSFORM_VALUE_SHAPE_INVALID"
				? WorldTransformExecutionError::ValueShapeInvalid
				: WorldTransformExecutionError::ValueDecodeFailed;
			return true;
		}
	}

	memory = Runtime::ReadMemory(liveSpanAddress, m_FinalBytes);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	std::uintptr_t rootFinal = 0;
	memory = Runtime::ReadValue(rootFieldAddress, rootFinal);
	if (!memory.Ok())
	{
		m_Error = WorldTransformExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return true;
	}
	if (m_FinalBytes != m_StableBytes)
	{
		m_Error = WorldTransformExecutionError::ValueChangedDuringRead;
		return true;
	}
	if (rootFinal != rootSecond)
	{
		m_Error = WorldTransformExecutionError::RootComponentChanged;
		return true;
	}
	if (m_Engine.Snapshots().Current() != m_Objects
		|| m_Engine.Types().Current() != m_Types
		|| m_Engine.Reflection() != m_Reflection
		|| m_Engine.Worlds().Current() != m_World)
	{
		m_Error = WorldTransformExecutionError::DependencyChanged;
		return true;
	}

	m_Value.Location = {location[0], location[1], location[2]};
	m_Value.Rotation = {rotation[0], rotation[1], rotation[2]};
	m_Value.Scale = {scale[0], scale[1], scale[2]};
	m_Value.AbsoluteLocation = absolute[0];
	m_Value.AbsoluteRotation = absolute[1];
	m_Value.AbsoluteScale = absolute[2];
	return true;
}

std::uint64_t WorldTransformReadWork::WorldSnapshotGeneration() const noexcept
{
	return m_World ? m_World->Generation : 0;
}

std::uint64_t WorldTransformReadWork::ObjectSnapshotGeneration() const noexcept
{
	return m_Objects ? m_Objects->Generation : 0;
}

std::uint64_t WorldTransformReadWork::TypeSnapshotGeneration() const noexcept
{
	return m_Types ? m_Types->Generation() : 0;
}

WorldTransformReadPreparation WorldTransformCommandService::PrepareRead(
	const json& data,
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 2
			|| !data.contains("actor")
			|| !data.contains("world_snapshot_generation"))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_REQUEST_INVALID",
				"world.actor.transform.get requires exactly actor and world_snapshot_generation")};
		}
		Runtime::ObjectHandle actorHandle;
		std::uint64_t requestedWorldGeneration = 0;
		if (!TryParseObjectHandle(data.at("actor"), actorHandle)
			|| !TryUnsigned(
				data.at("world_snapshot_generation"),
				1,
				kMaxProtocolInteger,
				requestedWorldGeneration))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_REQUEST_INVALID",
				"The actor handle or world snapshot generation is invalid")};
		}

		const std::shared_ptr<const Runtime::EngineSnapshot> objects =
			engine.Snapshots().Current();
		const std::shared_ptr<const Runtime::TypeSnapshot> types =
			engine.Types().Current();
		const std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection =
			engine.Reflection();
		const std::shared_ptr<const Runtime::WorldSnapshot> world =
			engine.Worlds().Current();
		if (!lease.Context() || !objects || !types || !reflection
			|| !reflection->IsPropertyCodecConfigured(lease.Context()->Generation())
			|| !world
			|| world->Generation != requestedWorldGeneration
			|| world->SessionId != engine.SessionId()
			|| world->ContextGeneration != lease.Context()->Generation()
			|| world->ObjectSnapshotGeneration != objects->Generation
			|| world->TypeSnapshotGeneration != types->Generation()
			|| types->ObjectSnapshotGeneration() != objects->Generation
			|| types->ReflectionLayoutFingerprint() != reflection->Layout->Fingerprint())
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_DEPENDENCY_MISMATCH",
				"The requested transform does not match the current immutable world/object/type/codec generation",
				{{"requested_world_generation", requestedWorldGeneration},
				 {"current_world_generation", world ? json(world->Generation) : json(nullptr)},
				 {"current_object_generation", objects ? json(objects->Generation) : json(nullptr)},
				 {"current_type_generation", types ? json(types->Generation()) : json(nullptr)}})};
		}

		const Runtime::WorldSnapshotActor* actor = world->FindActorByIndex(actorHandle.Index);
		if (!actor || !SameHandle(actor->Object.Handle, actorHandle))
		{
			return {.Error = Error(
				"WORLD_ACTOR_HANDLE_STALE",
				"The actor identity is absent from the exact current WorldSnapshot",
				{{"index", actorHandle.Index}})};
		}
		if (actor->RootComponent.State != Runtime::WorldReferenceState::Present
			|| !actor->RootComponent.Object)
		{
			return {.Error = Error(
				actor->RootComponent.ReasonCode.empty()
					? "WORLD_ROOT_COMPONENT_UNAVAILABLE"
					: actor->RootComponent.ReasonCode,
				actor->RootComponent.Reason.empty()
					? "The actor has no exact current root SceneComponent"
					: actor->RootComponent.Reason)};
		}
		const Runtime::WorldSnapshotObject& root = *actor->RootComponent.Object;
		const Runtime::EngineSnapshotObject* actorRecord = objects->FindByIndex(actorHandle.Index);
		const Runtime::EngineSnapshotObject* rootRecord = objects->FindByIndex(root.Handle.Index);
		if (!actorRecord || !rootRecord
			|| !SameHandle(actorRecord->Handle, actorHandle)
			|| !SameHandle(rootRecord->Handle, root.Handle))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_OBJECT_SNAPSHOT_MISMATCH",
				"The Actor or RootComponent is absent from the exact current ObjectSnapshot")};
		}

		const Runtime::ReflectedType* actorType = FindExactAncestor(
			*types,
			actorRecord->ClassPath,
			kActorClassPath);
		const Runtime::ReflectedType* sceneComponentType = FindExactAncestor(
			*types,
			rootRecord->ClassPath,
			kSceneComponentClassPath);
		if (!actorType || !sceneComponentType)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_TYPE_HIERARCHY_MISMATCH",
				"The exact Actor or SceneComponent class hierarchy is unavailable",
				{{"actor_class_path", actorRecord->ClassPath},
				 {"root_component_class_path", rootRecord->ClassPath}})};
		}

		bool ambiguous = false;
		const Runtime::ReflectedProperty* actorRootProperty =
			FindUniqueDirectProperty(*actorType, "RootComponent", ambiguous);
		if (ambiguous || !actorRootProperty
			|| !ValidateRootProperty(
				*actorRootProperty,
				*actorType,
				*reflection->Properties))
		{
			return {.Error = Error(
				ambiguous
					? "WORLD_TRANSFORM_PROPERTY_AMBIGUOUS"
					: "WORLD_TRANSFORM_ROOT_PROPERTY_UNAVAILABLE",
				"The exact AActor.RootComponent object descriptor is unavailable")};
		}

		constexpr std::array transformNames{
			std::string_view("RelativeLocation"),
			std::string_view("RelativeRotation"),
			std::string_view("RelativeScale3D")};
		constexpr std::array absoluteNames{
			std::string_view("bAbsoluteLocation"),
			std::string_view("bAbsoluteRotation"),
			std::string_view("bAbsoluteScale")};
		std::array<const Runtime::ReflectedProperty*, 3> transformProperties{};
		std::array<const Runtime::ReflectedProperty*, 3> absoluteProperties{};
		for (std::size_t index = 0; index < transformNames.size(); ++index)
		{
			transformProperties[index] = FindUniqueDirectProperty(
				*sceneComponentType,
				transformNames[index],
				ambiguous);
			if (ambiguous || !transformProperties[index])
			{
				return {.Error = Error(
					ambiguous
						? "WORLD_TRANSFORM_PROPERTY_AMBIGUOUS"
						: "WORLD_TRANSFORM_PROPERTY_UNAVAILABLE",
					"An exact direct USceneComponent transform property is unavailable",
					{{"property", transformNames[index]}})};
			}
			absoluteProperties[index] = FindUniqueDirectProperty(
				*sceneComponentType,
				absoluteNames[index],
				ambiguous);
			if (ambiguous || !absoluteProperties[index])
			{
				return {.Error = Error(
					ambiguous
						? "WORLD_TRANSFORM_PROPERTY_AMBIGUOUS"
						: "WORLD_TRANSFORM_PROPERTY_UNAVAILABLE",
					"An exact direct USceneComponent absolute-space flag is unavailable",
					{{"property", absoluteNames[index]}})};
			}
		}

		WorldTransformPrecision locationPrecision{};
		WorldTransformPrecision rotationPrecision{};
		WorldTransformPrecision scalePrecision{};
		if (!ValidateMathProperty(
			*transformProperties[0],
			*sceneComponentType,
			*reflection->Properties,
			kVectorStructPath,
			{"X", "Y", "Z"},
			locationPrecision)
			|| !ValidateMathProperty(
				*transformProperties[1],
				*sceneComponentType,
				*reflection->Properties,
				kRotatorStructPath,
				{"Pitch", "Yaw", "Roll"},
				rotationPrecision)
			|| !ValidateMathProperty(
				*transformProperties[2],
				*sceneComponentType,
				*reflection->Properties,
				kVectorStructPath,
				{"X", "Y", "Z"},
				scalePrecision)
			|| locationPrecision != rotationPrecision
			|| locationPrecision != scalePrecision)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_CODEC_UNAVAILABLE",
				"The exact FVector/FRotator descriptors are missing or use inconsistent scalar precision")};
		}
		for (const Runtime::ReflectedProperty* property : absoluteProperties)
		{
			if (!ValidateBoolProperty(
				*property,
				*sceneComponentType,
				*reflection->Properties))
			{
				return {.Error = Error(
					"WORLD_TRANSFORM_ABSOLUTE_FLAGS_UNAVAILABLE",
					"A witnessed bAbsoluteLocation/Rotation/Scale bool descriptor is unavailable",
					{{"property", property->Name}})};
			}
		}

		std::uint64_t spanBegin = (std::numeric_limits<std::uint64_t>::max)();
		std::uint64_t spanEnd = 0;
		const auto includeProperty = [&spanBegin, &spanEnd](
			const Runtime::ReflectedProperty* property) {
			spanBegin = (std::min)(spanBegin, static_cast<std::uint64_t>(property->Offset));
			spanEnd = (std::max)(
				spanEnd,
				static_cast<std::uint64_t>(property->Offset) + property->Size);
		};
		for (const Runtime::ReflectedProperty* property : transformProperties)
			includeProperty(property);
		for (const Runtime::ReflectedProperty* property : absoluteProperties)
			includeProperty(property);
		if (spanBegin >= spanEnd
			|| spanEnd > sceneComponentType->PropertiesSize
			|| spanEnd - spanBegin > kMaxAggregateBytes
			|| spanEnd > (std::numeric_limits<std::uint32_t>::max)())
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_RANGE_INVALID",
				"The witnessed SceneComponent transform property span is invalid or exceeds 64 KiB")};
		}

		auto work = std::shared_ptr<WorldTransformReadWork>(new WorldTransformReadWork(
			std::move(lease),
			engine,
			objects,
			types,
			reflection,
			world,
			actor->Object,
			root,
			*actorRootProperty,
			transformProperties,
			absoluteProperties,
			locationPrecision,
			static_cast<std::uint32_t>(spanBegin),
			static_cast<std::uint32_t>(spanEnd - spanBegin)));
		return {.Work = std::move(work)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_ALLOCATION_FAILED",
			"The bounded transform command could not allocate owned state")};
	}
	catch (...)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_PREPARATION_FAILED",
			"The transform command could not validate its immutable inputs")};
	}
}

WorldTransformCommandResult WorldTransformCommandService::CompleteRead(
	const WorldTransformReadWork& work) noexcept
{
	try
	{
		if (work.ExecutionError() != WorldTransformExecutionError::None)
		{
			switch (work.ExecutionError())
			{
			case WorldTransformExecutionError::DependencyChanged:
				return {.Error = Error(
					"WORLD_TRANSFORM_DEPENDENCY_CHANGED",
					"The immutable world/object/type/codec generation changed before game-thread execution")};
			case WorldTransformExecutionError::ExecutionThreadInvalid:
				return {.Error = Error(
					"GAME_THREAD_IDENTITY_INVALID",
					"The transform read did not execute on the witnessed game thread")};
			case WorldTransformExecutionError::ActorHandleStale:
				return {.Error = Error(
					"WORLD_ACTOR_HANDLE_STALE",
					"The Actor identity changed before the transform read executed",
					{{"handle_error", Runtime::ToString(work.HandleError())}})};
			case WorldTransformExecutionError::RootComponentHandleStale:
				return {.Error = Error(
					"WORLD_ROOT_COMPONENT_HANDLE_STALE",
					"The RootComponent identity changed before the transform read executed",
					{{"handle_error", Runtime::ToString(work.HandleError())}})};
			case WorldTransformExecutionError::RootComponentChanged:
				return {.Error = Error(
					"WORLD_ROOT_COMPONENT_CHANGED",
					"AActor.RootComponent changed during the same-frame transform read")};
			case WorldTransformExecutionError::AddressOverflow:
				return {.Error = Error(
					"WORLD_TRANSFORM_ADDRESS_OVERFLOW",
					"A witnessed transform property range overflowed its object address")};
			case WorldTransformExecutionError::MemoryReadFailed:
				return {.Error = Error(
					"WORLD_TRANSFORM_MEMORY_READ_FAILED",
					"A witnessed transform property range could not be read",
					{{"memory_error", Runtime::ToString(work.MemoryError())}})};
			case WorldTransformExecutionError::ValueChangedDuringRead:
				return {.Error = Error(
					"WORLD_TRANSFORM_CHANGED_DURING_READ",
					"The transform property span changed before one coherent value could be published")};
			case WorldTransformExecutionError::ValueDecodeFailed:
			case WorldTransformExecutionError::ValueShapeInvalid:
				return {.Error = Error(
					work.DecodeErrorCode().empty()
						? "WORLD_TRANSFORM_VALUE_DECODE_FAILED"
						: work.DecodeErrorCode(),
					work.DecodeError().empty()
						? "The witnessed transform bytes did not decode as canonical math values"
						: work.DecodeError())};
			case WorldTransformExecutionError::None:
				break;
			}
		}

		const WorldTransformValue& value = work.Value();
		const char* precision = value.Precision == WorldTransformPrecision::Float64
			? "float64"
			: "float32";
		return {.Data = {
			{"generation", work.WorldSnapshotGeneration()},
			{"context_generation", work.Actor().Handle.ContextGeneration},
			{"object_snapshot_generation", work.ObjectSnapshotGeneration()},
			{"type_snapshot_generation", work.TypeSnapshotGeneration()},
			{"actor", SerializeObject(work.Actor())},
			{"root_component", SerializeObject(work.RootComponent())},
			{"transform", {
				{"source", "scene_component_stored_relative"},
				{"computed_world", false},
				{"precision", precision},
				{"location", {
					{"x", value.Location.X},
					{"y", value.Location.Y},
					{"z", value.Location.Z}}},
				{"rotation", {
					{"pitch", value.Rotation.Pitch},
					{"yaw", value.Rotation.Yaw},
					{"roll", value.Rotation.Roll}}},
				{"scale", {
					{"x", value.Scale.X},
					{"y", value.Scale.Y},
					{"z", value.Scale.Z}}},
				{"absolute", {
					{"location", value.AbsoluteLocation},
					{"rotation", value.AbsoluteRotation},
					{"scale", value.AbsoluteScale}}},
				{"space", {
					{"location", value.AbsoluteLocation ? "world" : "relative"},
					{"rotation", value.AbsoluteRotation ? "world" : "relative"},
					{"scale", value.AbsoluteScale ? "world" : "relative"}}}
			}}
		}};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_ALLOCATION_FAILED",
			"The transform response could not allocate serialization storage")};
	}
	catch (...)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_SERIALIZATION_FAILED",
			"The transform response could not be serialized")};
	}
}

} // namespace UExplorer::Services
