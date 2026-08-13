#include "WorldMutationCommandService.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
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
constexpr std::size_t kMaxFloatBytes = 128;
constexpr std::string_view kActorClassPath = "/Script/Engine.Actor";
constexpr std::string_view kSceneComponentClassPath = "/Script/Engine.SceneComponent";
constexpr std::string_view kVectorStructPath = "/Script/CoreUObject.Vector";
constexpr std::string_view kRotatorStructPath = "/Script/CoreUObject.Rotator";
constexpr std::string_view kActorScaleSetterPath =
	"/Script/Engine.Actor.SetActorScale3D";
constexpr std::string_view kRelativeScaleSetterPath =
	"/Script/Engine.SceneComponent.SetRelativeScale3D";
constexpr std::string_view kActorRotationSetterPath =
	"/Script/Engine.Actor.K2_SetActorRotation";

// UE 4.21-5.7 source exposes these three exact reflected signatures. Runtime
// admission still depends only on the current immutable descriptors and handles.
constexpr std::string_view kScaleParameterName = "NewScale3D";
constexpr std::string_view kRotationParameterName = "NewRotation";
constexpr std::string_view kTeleportPhysicsParameterName = "bTeleportPhysics";
constexpr std::string_view kReturnParameterName = "ReturnValue";

WorldMutationCommandError Error(
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

const char* ToString(const WorldMutationField field) noexcept
{
	switch (field)
	{
	case WorldMutationField::Location: return "location";
	case WorldMutationField::Rotation: return "rotation";
	case WorldMutationField::Scale: return "scale";
	}
	return "unknown";
}

const char* ToString(const WorldMutationSpace space) noexcept
{
	switch (space)
	{
	case WorldMutationSpace::World: return "world";
	case WorldMutationSpace::Relative: return "relative";
	}
	return "unknown";
}

const char* ToString(const WorldMutationExecutionPhase phase) noexcept
{
	switch (phase)
	{
	case WorldMutationExecutionPhase::BeforeInvoke: return "before_invoke";
	case WorldMutationExecutionPhase::Invoke: return "invoke";
	case WorldMutationExecutionPhase::AfterInvoke: return "after_invoke";
	}
	return "unknown";
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
	const auto converted = std::from_chars(
		encoded.data(),
		encoded.data() + encoded.size(),
		parsed,
		std::chars_format::general);
	return converted.ec == std::errc{}
		&& converted.ptr == encoded.data() + encoded.size()
		&& std::isfinite(parsed);
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

bool ValidateRootProperty(
	const Runtime::ReflectedProperty& property,
	const Runtime::ReflectedType& declaringType) noexcept
{
	return property.State == Runtime::ReflectedMemberState::Supported
		&& property.Kind == Runtime::PropertyKind::Object
		&& property.ArrayDim == 1
		&& property.Size == sizeof(std::uintptr_t)
		&& property.Descriptor
		&& property.Descriptor->Kind == Runtime::PropertyKind::Object
		&& property.Descriptor->TypeName == kSceneComponentClassPath
		&& property.Descriptor->Size == property.Size
		&& property.Offset <= declaringType.PropertiesSize
		&& property.Size <= declaringType.PropertiesSize - property.Offset;
}

const Runtime::ReflectedParameter* FindUniqueParameter(
	const Runtime::ReflectedFunction& function,
	const std::string_view name) noexcept
{
	const Runtime::ReflectedParameter* found = nullptr;
	for (const Runtime::ReflectedParameter& parameter : function.Parameters)
	{
		if (parameter.Property.Name != name)
			continue;
		if (found)
			return nullptr;
		found = &parameter;
	}
	return found;
}

bool ValidateParameter(
	const Runtime::ReflectedParameter& parameter,
	const Runtime::ReflectedParameterDirection direction,
	const Runtime::PropertyKind kind,
	const std::string_view typeName,
	const Runtime::PropertyCodec& codec,
	const std::uint32_t frameSize) noexcept
{
	const Runtime::ReflectedProperty& property = parameter.Property;
	return parameter.Direction == direction
		&& property.State == Runtime::ReflectedMemberState::Supported
		&& property.Kind == kind
		&& property.TypeName == typeName
		&& property.ArrayDim == 1
		&& property.Descriptor
		&& property.Descriptor->Kind == kind
		&& property.Descriptor->TypeName == typeName
		&& property.Descriptor->Size == property.Size
		&& property.Offset <= frameSize
		&& property.Size <= frameSize - property.Offset
		&& Runtime::ParamFrame::SupportsLifetime(*property.Descriptor)
		&& codec.Supports(kind);
}

bool ValidateCanonicalMathParameter(
	const Runtime::ReflectedParameter& parameter,
	const std::string_view typeName,
	const Runtime::CanonicalMathStructKind expectedKind,
	const Runtime::PropertyCodec& codec,
	const std::uint32_t frameSize) noexcept
{
	return ValidateParameter(
		parameter,
		Runtime::ReflectedParameterDirection::Input,
		Runtime::PropertyKind::Struct,
		typeName,
		codec,
		frameSize)
		&& Runtime::ClassifyCanonicalMathStruct(*parameter.Property.Descriptor)
			== expectedKind
		&& codec.SupportsInput(*parameter.Property.Descriptor);
}

bool ValidateBoolParameter(
	const Runtime::ReflectedParameter& parameter,
	const Runtime::ReflectedParameterDirection direction,
	const Runtime::PropertyCodec& codec,
	const std::uint32_t frameSize) noexcept
{
	return ValidateParameter(
		parameter,
		direction,
		Runtime::PropertyKind::Bool,
		"bool",
		codec,
		frameSize)
		&& parameter.Property.Descriptor->BoolMask != 0
		&& parameter.Property.Descriptor->BoolByteOffset < parameter.Property.Size
		&& (direction == Runtime::ReflectedParameterDirection::Return
			|| codec.SupportsInput(*parameter.Property.Descriptor));
}

bool ParseField(const json& update, WorldMutationField& field) noexcept
{
	if (!update.is_object() || !update.contains("field")
		|| !update.at("field").is_string())
	{
		return false;
	}
	const std::string& encoded = update.at("field").get_ref<const std::string&>();
	if (encoded == "location")
		field = WorldMutationField::Location;
	else if (encoded == "rotation")
		field = WorldMutationField::Rotation;
	else if (encoded == "scale")
		field = WorldMutationField::Scale;
	else
		return false;
	return true;
}

bool ParseSpace(const json& update, WorldMutationSpace& space) noexcept
{
	if (!update.contains("space") || !update.at("space").is_string())
		return false;
	const std::string& encoded = update.at("space").get_ref<const std::string&>();
	if (encoded == "world")
		space = WorldMutationSpace::World;
	else if (encoded == "relative")
		space = WorldMutationSpace::Relative;
	else
		return false;
	return true;
}

bool ParseComponents(
	const json& update,
	const WorldMutationField field,
	WorldMutationValue& value) noexcept
{
	if (!update.contains("value") || !update.at("value").is_object())
		return false;
	const json& encoded = update.at("value");
	const std::array<std::string_view, 3> names = field == WorldMutationField::Rotation
		? std::array<std::string_view, 3>{"pitch", "yaw", "roll"}
		: std::array<std::string_view, 3>{"x", "y", "z"};
	if (encoded.size() != names.size())
		return false;
	for (std::size_t index = 0; index < names.size(); ++index)
	{
		const std::string name(names[index]);
		if (!encoded.contains(name)
			|| !TryFiniteDouble(encoded.at(name), value.Components[index]))
		{
			return false;
		}
	}
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

json SerializeFunctionHandle(const Runtime::FunctionHandle& handle)
{
	return {
		{"function", SerializeHandle(handle.Function)},
		{"owner", SerializeHandle(handle.Owner)},
		{"full_path", handle.FullPath},
		{"signature_fingerprint", std::format("{:016X}", handle.SignatureFingerprint)}
	};
}

json SerializeObject(const Runtime::WorldSnapshotObject& object)
{
	return {
		{"handle", SerializeHandle(object.Handle)},
		{"index", object.Handle.Index},
		{"name", object.Name},
		{"class", object.ClassPath},
		{"full_path", object.FullPath},
		{"class_path", object.ClassPath},
		{"address", std::format("0x{:X}", object.Handle.Address)}
	};
}

} // namespace

WorldMutationUpdateWork::WorldMutationUpdateWork(
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	Runtime::GameThreadExecutor& gameThread,
	std::shared_ptr<const Runtime::EngineSnapshot> objects,
	std::shared_ptr<const Runtime::TypeSnapshot> types,
	std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection,
	std::shared_ptr<const Runtime::WorldSnapshot> world,
	Runtime::WorldSnapshotObject actor,
	Runtime::WorldSnapshotObject rootComponent,
	Runtime::ObjectHandle target,
	const Runtime::ReflectedProperty& actorRootProperty,
	const Runtime::ReflectedFunction& function,
	const Runtime::ReflectedProperty* returnProperty,
	const WorldMutationField field,
	const WorldMutationSpace space,
	WorldMutationValue value,
	Runtime::ParamFrame frame)
	: m_Lease(std::move(lease)),
	  m_Engine(engine),
	  m_GameThread(gameThread),
	  m_Objects(std::move(objects)),
	  m_Types(std::move(types)),
	  m_Reflection(std::move(reflection)),
	  m_World(std::move(world)),
	  m_Actor(std::move(actor)),
	  m_RootComponent(std::move(rootComponent)),
	  m_Target(std::move(target)),
	  m_ActorRootProperty(&actorRootProperty),
	  m_Function(&function),
	  m_ReturnProperty(returnProperty),
	  m_Field(field),
	  m_Space(space),
	  m_Value(std::move(value)),
	  m_Frame(std::move(frame))
{
}

const Runtime::FunctionHandle& WorldMutationUpdateWork::FunctionHandle() const noexcept
{
	return m_Function->Handle;
}

const std::string& WorldMutationUpdateWork::FunctionPath() const noexcept
{
	return m_Function->FullPath;
}

std::uint64_t WorldMutationUpdateWork::ContextGeneration() const noexcept
{
	return m_Lease.Context() ? m_Lease.Context()->Generation() : 0;
}

std::uint64_t WorldMutationUpdateWork::ObjectSnapshotGeneration() const noexcept
{
	return m_Objects ? m_Objects->Generation : 0;
}

std::uint64_t WorldMutationUpdateWork::TypeSnapshotGeneration() const noexcept
{
	return m_Types ? m_Types->Generation() : 0;
}

std::uint64_t WorldMutationUpdateWork::WorldSnapshotGeneration() const noexcept
{
	return m_World ? m_World->Generation : 0;
}

bool WorldMutationUpdateWork::ValidateLiveBindings(
	const WorldMutationExecutionPhase phase) noexcept
{
	m_Phase = phase;
	if (!m_Lease.Context()
		|| m_Engine.ContextGeneration() != m_Lease.Context()->Generation()
		|| m_Engine.Snapshots().Current() != m_Objects
		|| m_Engine.Types().Current() != m_Types
		|| m_Engine.Reflection() != m_Reflection
		|| m_Engine.Worlds().Current() != m_World
		|| !m_Objects || !m_Types || !m_Reflection || !m_Reflection->Properties
		|| !m_World || !m_ActorRootProperty || !m_Function)
	{
		m_Error = WorldMutationExecutionError::DependencyChanged;
		return false;
	}

	const Runtime::ObjectValidationResult actorValidation =
		m_Engine.ValidateObjectHandle(m_Actor.Handle);
	if (!actorValidation.Ok())
	{
		m_Error = WorldMutationExecutionError::ActorHandleStale;
		m_HandleError = actorValidation.Error;
		return false;
	}
	const Runtime::ObjectValidationResult rootValidation =
		m_Engine.ValidateObjectHandle(m_RootComponent.Handle);
	if (!rootValidation.Ok())
	{
		m_Error = WorldMutationExecutionError::RootComponentHandleStale;
		m_HandleError = rootValidation.Error;
		return false;
	}
	const Runtime::FunctionValidationResult functionValidation =
		m_Engine.ValidateFunctionHandle(m_Function->Handle);
	if (!functionValidation.Ok())
	{
		m_Error = WorldMutationExecutionError::FunctionHandleStale;
		m_HandleError = functionValidation.Error;
		return false;
	}

	const Runtime::EngineSnapshotObject* actorRecord =
		m_Objects->FindByIndex(m_Actor.Handle.Index);
	const Runtime::EngineSnapshotObject* rootRecord =
		m_Objects->FindByIndex(m_RootComponent.Handle.Index);
	const Runtime::EngineSnapshotObject* functionRecord =
		m_Objects->FindByIndex(m_Function->Handle.Function.Index);
	const Runtime::EngineSnapshotObject* ownerRecord =
		m_Objects->FindByIndex(m_Function->Handle.Owner.Index);
	const Runtime::WorldSnapshotActor* worldActor =
		m_World->FindActorByIndex(m_Actor.Handle.Index);
	const Runtime::ReflectedFunctionLookup liveFunction =
		m_Types->FindFunctionByFullPath(m_Function->FullPath);
	if (!actorRecord || !rootRecord || !functionRecord || !ownerRecord || !worldActor
		|| !liveFunction.Found() || liveFunction.Function != m_Function
		|| !SameFunctionHandle(liveFunction.Function->Handle, m_Function->Handle)
		|| !SameHandle(actorRecord->Handle, m_Actor.Handle)
		|| !SameHandle(rootRecord->Handle, m_RootComponent.Handle)
		|| !SameHandle(functionRecord->Handle, m_Function->Handle.Function)
		|| !SameHandle(ownerRecord->Handle, m_Function->Handle.Owner)
		|| !SameHandle(worldActor->Object.Handle, m_Actor.Handle)
		|| worldActor->RootComponent.State != Runtime::WorldReferenceState::Present
		|| !worldActor->RootComponent.Object
		|| !SameHandle(worldActor->RootComponent.Object->Handle, m_RootComponent.Handle))
	{
		m_Error = WorldMutationExecutionError::DependencyChanged;
		return false;
	}

	if (m_ActorRootProperty->Offset
		> (std::numeric_limits<std::uintptr_t>::max)() - m_Actor.Handle.Address)
	{
		m_Error = WorldMutationExecutionError::AddressOverflow;
		return false;
	}
	const std::uintptr_t rootFieldAddress =
		m_Actor.Handle.Address + m_ActorRootProperty->Offset;
	std::uintptr_t first = 0;
	std::uintptr_t second = 0;
	Runtime::MemoryResult memory = Runtime::ReadValue(rootFieldAddress, first);
	if (!memory.Ok())
	{
		m_Error = WorldMutationExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return false;
	}
	memory = Runtime::ReadValue(rootFieldAddress, second);
	if (!memory.Ok())
	{
		m_Error = WorldMutationExecutionError::MemoryReadFailed;
		m_MemoryError = memory.Error;
		return false;
	}
	if (first != second || second != m_RootComponent.Handle.Address)
	{
		m_Error = WorldMutationExecutionError::RootComponentChanged;
		return false;
	}
	return true;
}

bool WorldMutationUpdateWork::DecodeSetterResult() noexcept
{
	if (!m_ReturnProperty)
		return true;
	if (!m_ReturnProperty->Descriptor
		|| m_ReturnProperty->Kind != Runtime::PropertyKind::Bool
		|| m_ReturnProperty->Descriptor->Kind != Runtime::PropertyKind::Bool)
	{
		return false;
	}
	const std::uintptr_t address = m_Frame.ValueAddress(*m_ReturnProperty);
	const std::uint32_t byteOffset = m_ReturnProperty->Descriptor->BoolByteOffset;
	const std::uint8_t mask = m_ReturnProperty->Descriptor->BoolMask;
	if (address == 0 || byteOffset >= m_ReturnProperty->Size || mask == 0)
		return false;
	const auto* bytes = reinterpret_cast<const std::byte*>(address);
	m_SetterResult = (std::to_integer<std::uint8_t>(bytes[byteOffset]) & mask) != 0;
	return true;
}

bool WorldMutationUpdateWork::Execute()
{
	m_Phase = WorldMutationExecutionPhase::BeforeInvoke;
	if (!m_Engine.IsCurrentExecutionThreadValid())
	{
		m_Error = WorldMutationExecutionError::ExecutionThreadInvalid;
		return true;
	}
	if (!ValidateLiveBindings(WorldMutationExecutionPhase::BeforeInvoke))
		return true;

	m_Phase = WorldMutationExecutionPhase::Invoke;
	if (!m_GameThread.InvokeProcessEventFromCurrentTask(
		reinterpret_cast<void*>(m_Target.Address),
		reinterpret_cast<void*>(m_Function->Handle.Function.Address),
		m_Frame.Data()))
	{
		m_Error = WorldMutationExecutionError::ProcessEventUnavailable;
		return true;
	}
	m_Invoked = true;

	if (!ValidateLiveBindings(WorldMutationExecutionPhase::AfterInvoke))
		return true;
	if (!DecodeSetterResult())
	{
		m_Error = WorldMutationExecutionError::ReturnValueInvalid;
		m_Phase = WorldMutationExecutionPhase::AfterInvoke;
	}
	return true;
}

WorldMutationUpdatePreparation WorldMutationCommandService::PrepareUpdate(
	const json& data,
	Runtime::CoreRuntime::RequestLease lease,
	Runtime::EngineFacade& engine,
	Runtime::GameThreadExecutor& gameThread) noexcept
{
	try
	{
		if (!data.is_object() || data.size() != 7
			|| !data.contains("session_id")
			|| !data.contains("context_generation")
			|| !data.contains("object_snapshot_generation")
			|| !data.contains("type_snapshot_generation")
			|| !data.contains("world_snapshot_generation")
			|| !data.contains("actor")
			|| !data.contains("update")
			|| !data.at("session_id").is_string())
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_REQUEST_INVALID",
				"world.actor.transform.update requires exactly session_id, context_generation, object_snapshot_generation, type_snapshot_generation, world_snapshot_generation, actor, and update")};
		}

		const std::string sessionId = data.at("session_id").get<std::string>();
		Runtime::ObjectHandle actorHandle;
		std::uint64_t contextGeneration = 0;
		std::uint64_t objectGeneration = 0;
		std::uint64_t typeGeneration = 0;
		std::uint64_t worldGeneration = 0;
		if (!IsBoundedText(sessionId, kMaxSessionBytes)
			|| !TryParseObjectHandle(data.at("actor"), actorHandle)
			|| !TryUnsigned(data.at("context_generation"), 1, kMaxProtocolInteger, contextGeneration)
			|| !TryUnsigned(data.at("object_snapshot_generation"), 1, kMaxProtocolInteger, objectGeneration)
			|| !TryUnsigned(data.at("type_snapshot_generation"), 1, kMaxProtocolInteger, typeGeneration)
			|| !TryUnsigned(data.at("world_snapshot_generation"), 1, kMaxProtocolInteger, worldGeneration)
			|| actorHandle.SessionId != sessionId
			|| actorHandle.ContextGeneration != contextGeneration)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_REQUEST_INVALID",
				"The session, generations, or complete Actor handle is malformed or inconsistent")};
		}

		const json& update = data.at("update");
		WorldMutationField field{};
		WorldMutationSpace space{};
		WorldMutationValue value;
		if (!ParseField(update, field) || !ParseSpace(update, space))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_FIELD_INVALID",
				"update.field must be location, rotation, or scale and update.space must be world or relative")};
		}

		std::size_t expectedUpdateSize = 3;
		if (field == WorldMutationField::Location
			|| (field == WorldMutationField::Rotation
				&& space == WorldMutationSpace::Relative))
		{
			expectedUpdateSize = 5;
			if (update.size() != expectedUpdateSize
				|| !update.contains("sweep") || !update.at("sweep").is_boolean()
				|| !update.contains("teleport") || !update.at("teleport").is_boolean()
				|| !ParseComponents(update, field, value))
			{
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_VALUE_INVALID",
					"Location and relative rotation require exact value, sweep, and teleport fields")};
			}
			return {.Error = Error(
				field == WorldMutationField::Location
					? "WORLD_TRANSFORM_LOCATION_LIFETIME_UNAVAILABLE"
					: "WORLD_TRANSFORM_RELATIVE_ROTATION_LIFETIME_UNAVAILABLE",
				"The exact reflected setter contains an FHitResult output whose construction and destruction profile is not verified",
				{{"field", ToString(field)},
				 {"space", ToString(space)},
				 {"mutation_state", "not_invoked"},
				 {"required_lifetime", "/Script/Engine.HitResult"}})};
		}
		if (field == WorldMutationField::Rotation)
		{
			expectedUpdateSize = 4;
			if (!update.contains("teleport_physics")
				|| !update.at("teleport_physics").is_boolean())
			{
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_VALUE_INVALID",
					"World rotation requires an explicit teleport_physics boolean")};
			}
			value.TeleportPhysics = update.at("teleport_physics").get<bool>();
		}
		if (update.size() != expectedUpdateSize || !ParseComponents(update, field, value))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_VALUE_INVALID",
				"The single-field update contains an unexpected member or a non-finite, non-canonical component")};
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
			|| !reflection->IsPropertyCodecConfigured(contextGeneration)
			|| !world
			|| engine.SessionId() != sessionId
			|| engine.ContextGeneration() != contextGeneration
			|| lease.Context()->Generation() != contextGeneration
			|| objects->SessionId != sessionId
			|| objects->ContextGeneration != contextGeneration
			|| objects->Generation != objectGeneration
			|| types->SessionId() != sessionId
			|| types->ContextGeneration() != contextGeneration
			|| types->Generation() != typeGeneration
			|| types->ObjectSnapshotGeneration() != objectGeneration
			|| world->SessionId != sessionId
			|| world->ContextGeneration != contextGeneration
			|| world->Generation != worldGeneration
			|| world->ObjectSnapshotGeneration != objectGeneration
			|| world->TypeSnapshotGeneration != typeGeneration
			|| types->ReflectionLayoutFingerprint() != reflection->Layout->Fingerprint())
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_DEPENDENCY_MISMATCH",
				"The request is not bound to the exact current session/context/object/type/world/codec generation",
				{{"requested_object_generation", objectGeneration},
				 {"requested_type_generation", typeGeneration},
				 {"requested_world_generation", worldGeneration},
				 {"current_object_generation", objects ? json(objects->Generation) : json(nullptr)},
				 {"current_type_generation", types ? json(types->Generation()) : json(nullptr)},
				 {"current_world_generation", world ? json(world->Generation) : json(nullptr)}})};
		}

		const Runtime::WorldSnapshotActor* actor = world->FindActorByIndex(actorHandle.Index);
		if (!actor || !SameHandle(actor->Object.Handle, actorHandle))
		{
			return {.Error = Error(
				"WORLD_ACTOR_HANDLE_STALE",
				"The Actor handle is absent from the exact requested WorldSnapshot")};
		}
		if (actor->RootComponent.State != Runtime::WorldReferenceState::Present
			|| !actor->RootComponent.Object)
		{
			return {.Error = Error(
				actor->RootComponent.ReasonCode.empty()
					? "WORLD_ROOT_COMPONENT_UNAVAILABLE"
					: actor->RootComponent.ReasonCode,
				actor->RootComponent.Reason.empty()
					? "The Actor has no exact current RootComponent"
					: actor->RootComponent.Reason)};
		}
		const Runtime::WorldSnapshotObject& root = *actor->RootComponent.Object;
		const Runtime::EngineSnapshotObject* actorRecord =
			objects->FindByIndex(actorHandle.Index);
		const Runtime::EngineSnapshotObject* rootRecord =
			objects->FindByIndex(root.Handle.Index);
		if (!actorRecord || !rootRecord
			|| !SameHandle(actorRecord->Handle, actorHandle)
			|| !SameHandle(rootRecord->Handle, root.Handle))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_OBJECT_SNAPSHOT_MISMATCH",
				"The Actor or RootComponent is absent from the exact ObjectSnapshot")};
		}

		const Runtime::ReflectedType* actorType = FindExactAncestor(
			*types, actorRecord->ClassPath, kActorClassPath);
		const Runtime::ReflectedType* sceneComponentType = FindExactAncestor(
			*types, rootRecord->ClassPath, kSceneComponentClassPath);
		if (!actorType || !sceneComponentType)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_TYPE_HIERARCHY_MISMATCH",
				"The exact Actor or SceneComponent class hierarchy is unavailable")};
		}

		bool ambiguous = false;
		const Runtime::ReflectedProperty* rootProperty =
			FindUniqueDirectProperty(*actorType, "RootComponent", ambiguous);
		if (ambiguous || !rootProperty || !ValidateRootProperty(*rootProperty, *actorType))
		{
			return {.Error = Error(
				ambiguous
					? "WORLD_TRANSFORM_UPDATE_ROOT_PROPERTY_AMBIGUOUS"
					: "WORLD_TRANSFORM_UPDATE_ROOT_PROPERTY_UNAVAILABLE",
				"The exact direct AActor.RootComponent descriptor is unavailable")};
		}

		std::string_view functionPath;
		const Runtime::ReflectedType* expectedOwner = nullptr;
		Runtime::ObjectHandle target;
		if (field == WorldMutationField::Rotation)
		{
			functionPath = kActorRotationSetterPath;
			expectedOwner = actorType;
			target = actorHandle;
		}
		else if (space == WorldMutationSpace::World)
		{
			functionPath = kActorScaleSetterPath;
			expectedOwner = actorType;
			target = actorHandle;
		}
		else
		{
			functionPath = kRelativeScaleSetterPath;
			expectedOwner = sceneComponentType;
			target = root.Handle;
		}

		const Runtime::ReflectedFunctionLookup lookup =
			types->FindFunctionByFullPath(functionPath);
		if (!lookup.Found()
			|| lookup.DeclaringType != expectedOwner
			|| lookup.DeclaringType->FullPath != expectedOwner->FullPath
			|| !SameHandle(lookup.Function->Handle.Owner, expectedOwner->Handle)
			|| lookup.Function->FullPath != functionPath
			|| lookup.Function->Implementation
				== Runtime::ReflectedFunctionImplementation::Unavailable)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_SETTER_UNAVAILABLE",
				"The exact reflected setter owner, path, implementation, or handle is unavailable",
				{{"function_path", functionPath}})};
		}
		const Runtime::EngineSnapshotObject* functionRecord =
			objects->FindByIndex(lookup.Function->Handle.Function.Index);
		const Runtime::EngineSnapshotObject* ownerRecord =
			objects->FindByIndex(lookup.Function->Handle.Owner.Index);
		if (!functionRecord || !ownerRecord
			|| !SameHandle(functionRecord->Handle, lookup.Function->Handle.Function)
			|| !SameHandle(ownerRecord->Handle, lookup.Function->Handle.Owner))
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_SETTER_SNAPSHOT_MISMATCH",
				"The reflected setter is not an exact member of the requested ObjectSnapshot",
				{{"function_path", functionPath}})};
		}
		if (lookup.Function->ParameterSize == 0
			|| lookup.Function->ParameterSize > kMaxParameterBytes)
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_FRAME_SIZE_INVALID",
				"The exact reflected setter frame is empty or exceeds 64 KiB")};
		}

		const Runtime::ReflectedParameter* valueParameter = nullptr;
		const Runtime::ReflectedParameter* teleportParameter = nullptr;
		const Runtime::ReflectedParameter* returnParameter = nullptr;
		if (field == WorldMutationField::Rotation)
		{
			valueParameter = FindUniqueParameter(*lookup.Function, kRotationParameterName);
			teleportParameter = FindUniqueParameter(
				*lookup.Function, kTeleportPhysicsParameterName);
			returnParameter = FindUniqueParameter(*lookup.Function, kReturnParameterName);
			if (lookup.Function->Parameters.size() != 3
				|| !valueParameter || !teleportParameter || !returnParameter
				|| !ValidateCanonicalMathParameter(
					*valueParameter,
					kRotatorStructPath,
					Runtime::CanonicalMathStructKind::Rotator,
					*reflection->Properties,
					lookup.Function->ParameterSize)
				|| !ValidateBoolParameter(
					*teleportParameter,
					Runtime::ReflectedParameterDirection::Input,
					*reflection->Properties,
					lookup.Function->ParameterSize)
				|| !ValidateBoolParameter(
					*returnParameter,
					Runtime::ReflectedParameterDirection::Return,
					*reflection->Properties,
					lookup.Function->ParameterSize))
			{
				return {.Error = Error(
					"WORLD_TRANSFORM_ROTATION_SIGNATURE_UNAVAILABLE",
					"K2_SetActorRotation must contain exactly canonical NewRotation, bool bTeleportPhysics, and bool ReturnValue",
					{{"function_path", functionPath}})};
			}
		}
		else
		{
			valueParameter = FindUniqueParameter(*lookup.Function, kScaleParameterName);
			if (lookup.Function->Parameters.size() != 1
				|| !valueParameter
				|| !ValidateCanonicalMathParameter(
					*valueParameter,
					kVectorStructPath,
					Runtime::CanonicalMathStructKind::Vector,
					*reflection->Properties,
					lookup.Function->ParameterSize))
			{
				return {.Error = Error(
					"WORLD_TRANSFORM_SCALE_SIGNATURE_UNAVAILABLE",
					"The scale setter must contain exactly one canonical FVector NewScale3D input and no output",
					{{"function_path", functionPath}})};
			}
		}

		Runtime::ParamFrame frame;
		const Runtime::ParamFrameResult created = Runtime::ParamFrame::Create(
			lookup.Function->ParameterSize, frame);
		if (!created.Ok())
		{
			return {.Error = Error(
				Runtime::ToString(created.Error),
				created.Message,
				{{"function_path", functionPath}})};
		}
		const Runtime::PropertyMathStructInput mathInput{
			.TypeName = field == WorldMutationField::Rotation
				? std::string(kRotatorStructPath)
				: std::string(kVectorStructPath),
			.Components = value.Components};
		Runtime::ParamFrameResult encoded = frame.SetInput(
			valueParameter->Property,
			mathInput,
			*reflection->Properties);
		if (!encoded.Ok())
		{
			return {.Error = Error(
				encoded.EncodeError == Runtime::PropertyEncodeError::None
					? Runtime::ToString(encoded.Error)
					: Runtime::ToString(encoded.EncodeError),
				encoded.Message,
				{{"parameter", valueParameter->Property.Name}})};
		}
		if (teleportParameter)
		{
			encoded = frame.SetInput(
				teleportParameter->Property,
				value.TeleportPhysics,
				*reflection->Properties);
			if (!encoded.Ok())
			{
				return {.Error = Error(
					encoded.EncodeError == Runtime::PropertyEncodeError::None
						? Runtime::ToString(encoded.Error)
						: Runtime::ToString(encoded.EncodeError),
					encoded.Message,
					{{"parameter", teleportParameter->Property.Name}})};
			}
		}

		auto work = std::shared_ptr<WorldMutationUpdateWork>(
			new WorldMutationUpdateWork(
				std::move(lease),
				engine,
				gameThread,
				objects,
				types,
				reflection,
				world,
				actor->Object,
				root,
				std::move(target),
				*rootProperty,
				*lookup.Function,
				returnParameter ? &returnParameter->Property : nullptr,
				field,
				space,
				std::move(value),
				std::move(frame)));
		return {.Work = std::move(work)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_UPDATE_ALLOCATION_FAILED",
			"The bounded transform update could not allocate owned state")};
	}
	catch (...)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_UPDATE_PREPARATION_FAILED",
			"The transform update could not validate its immutable inputs")};
	}
}

WorldMutationCommandResult WorldMutationCommandService::CompleteUpdate(
	const WorldMutationUpdateWork& work) noexcept
{
	try
	{
		if (work.ExecutionError() != WorldMutationExecutionError::None)
		{
			json details = {
				{"phase", ToString(work.ExecutionPhase())},
				{"invoked", work.Invoked()},
				{"mutation_state", work.Invoked()
					? "unknown_after_invoke"
					: "not_invoked"},
				{"function_path", work.FunctionPath()}
			};
			switch (work.ExecutionError())
			{
			case WorldMutationExecutionError::DependencyChanged:
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_DEPENDENCY_CHANGED",
					"An immutable session/context/object/type/world/codec dependency changed around execution",
					std::move(details))};
			case WorldMutationExecutionError::ExecutionThreadInvalid:
				return {.Error = Error(
					"GAME_THREAD_IDENTITY_INVALID",
					"The transform setter did not execute on the witnessed game thread",
					std::move(details))};
			case WorldMutationExecutionError::ActorHandleStale:
				details["handle_error"] = Runtime::ToString(work.HandleError());
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_ACTOR_STALE",
					"The Actor identity failed live validation around ProcessEvent",
					std::move(details))};
			case WorldMutationExecutionError::RootComponentHandleStale:
				details["handle_error"] = Runtime::ToString(work.HandleError());
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_ROOT_COMPONENT_STALE",
					"The RootComponent identity failed live validation around ProcessEvent",
					std::move(details))};
			case WorldMutationExecutionError::RootComponentChanged:
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_ROOT_COMPONENT_CHANGED",
					"AActor.RootComponent changed or was unstable around ProcessEvent",
					std::move(details))};
			case WorldMutationExecutionError::FunctionHandleStale:
				details["handle_error"] = Runtime::ToString(work.HandleError());
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_SETTER_STALE",
					"The exact reflected setter identity failed live validation around ProcessEvent",
					std::move(details))};
			case WorldMutationExecutionError::AddressOverflow:
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_ADDRESS_OVERFLOW",
					"The witnessed AActor.RootComponent address overflowed",
					std::move(details))};
			case WorldMutationExecutionError::MemoryReadFailed:
				details["memory_error"] = Runtime::ToString(work.MemoryError());
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_MEMORY_READ_FAILED",
					"The witnessed AActor.RootComponent pointer could not be read coherently",
					std::move(details))};
			case WorldMutationExecutionError::ProcessEventUnavailable:
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_PROCESS_EVENT_UNAVAILABLE",
					"The exact setter could not enter ProcessEvent",
					std::move(details))};
			case WorldMutationExecutionError::ReturnValueInvalid:
				return {.Error = Error(
					"WORLD_TRANSFORM_UPDATE_RETURN_VALUE_INVALID",
					"The setter ran but its exact bool return slot could not be decoded",
					std::move(details))};
			case WorldMutationExecutionError::None:
				break;
			}
		}
		if (!work.Invoked())
		{
			return {.Error = Error(
				"WORLD_TRANSFORM_UPDATE_NOT_EXECUTED",
				"The transform update reached no terminal ProcessEvent invocation",
				{{"mutation_state", "not_invoked"}})};
		}

		json execution = {
			{"invoked", true},
			{"atomicity", "single_field_process_event"},
			{"post_identity_validated", true}
		};
		if (work.HasSetterResult())
		{
			execution["setter_result"] = work.SetterResult();
			execution["mutation_state"] = work.SetterResult()
				? "setter_reported_applied"
				: "setter_reported_not_applied";
		}
		else
		{
			execution["setter_result"] = nullptr;
			execution["mutation_state"] = "setter_returned_without_result";
		}

		json value;
		if (work.Field() == WorldMutationField::Rotation)
		{
			value = {
				{"pitch", work.Value().Components[0]},
				{"yaw", work.Value().Components[1]},
				{"roll", work.Value().Components[2]}
			};
		}
		else
		{
			value = {
				{"x", work.Value().Components[0]},
				{"y", work.Value().Components[1]},
				{"z", work.Value().Components[2]}
			};
		}
		json update = {
			{"field", ToString(work.Field())},
			{"space", ToString(work.Space())},
			{"value", std::move(value)}
		};
		if (work.Field() == WorldMutationField::Rotation)
			update["teleport_physics"] = work.Value().TeleportPhysics;

		return {.Data = {
			{"session_id", work.Actor().Handle.SessionId},
			{"context_generation", work.ContextGeneration()},
			{"object_snapshot_generation", work.ObjectSnapshotGeneration()},
			{"type_snapshot_generation", work.TypeSnapshotGeneration()},
			{"world_snapshot_generation", work.WorldSnapshotGeneration()},
			{"actor", SerializeObject(work.Actor())},
			{"root_component", SerializeObject(work.RootComponent())},
			{"target", SerializeHandle(work.Target())},
			{"setter", {
				{"function_path", work.FunctionPath()},
				{"handle", SerializeFunctionHandle(work.FunctionHandle())}
			}},
			{"update", std::move(update)},
			{"execution", std::move(execution)}
		}};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_UPDATE_ALLOCATION_FAILED",
			"The bounded transform result could not allocate serialized state")};
	}
	catch (...)
	{
		return {.Error = Error(
			"WORLD_TRANSFORM_UPDATE_RESULT_FAILED",
			"The transform update result could not be serialized")};
	}
}

} // namespace UExplorer::Services
