#include "ObjectSnapshotTypeCandidateSource.h"

#include "EngineFacade.h"
#include "SafeMemory.h"
#include "TypeMetadataContext.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::string_view kSourceName =
	"object_snapshot_reflection_type_metadata_v1";
constexpr std::size_t kMaximumFieldChainDepth = 65'536;
constexpr std::size_t kSeenFieldTableCapacity = 131'072;
static_assert(std::has_single_bit(kSeenFieldTableCapacity));

constexpr std::uint64_t kCastInt8Property = 0x0000000000000002ULL;
constexpr std::uint64_t kCastByteProperty = 0x0000000000000040ULL;
constexpr std::uint64_t kCastIntProperty = 0x0000000000000080ULL;
constexpr std::uint64_t kCastFloatProperty = 0x0000000000000100ULL;
constexpr std::uint64_t kCastUInt64Property = 0x0000000000000200ULL;
constexpr std::uint64_t kCastClassProperty = 0x0000000000000400ULL;
constexpr std::uint64_t kCastUInt32Property = 0x0000000000000800ULL;
constexpr std::uint64_t kCastInterfaceProperty = 0x0000000000001000ULL;
constexpr std::uint64_t kCastNameProperty = 0x0000000000002000ULL;
constexpr std::uint64_t kCastStrProperty = 0x0000000000004000ULL;
constexpr std::uint64_t kCastProperty = 0x0000000000008000ULL;
constexpr std::uint64_t kCastObjectProperty = 0x0000000000010000ULL;
constexpr std::uint64_t kCastBoolProperty = 0x0000000000020000ULL;
constexpr std::uint64_t kCastUInt16Property = 0x0000000000040000ULL;
constexpr std::uint64_t kCastFunction = 0x0000000000080000ULL;
constexpr std::uint64_t kCastStructProperty = 0x0000000000100000ULL;
constexpr std::uint64_t kCastArrayProperty = 0x0000000000200000ULL;
constexpr std::uint64_t kCastInt64Property = 0x0000000000400000ULL;
constexpr std::uint64_t kCastDelegateProperty = 0x0000000000800000ULL;
constexpr std::uint64_t kCastMulticastDelegateProperty = 0x0000000002000000ULL;
constexpr std::uint64_t kCastWeakObjectProperty = 0x0000000008000000ULL;
constexpr std::uint64_t kCastSoftObjectProperty = 0x0000000020000000ULL;
constexpr std::uint64_t kCastTextProperty = 0x0000000040000000ULL;
constexpr std::uint64_t kCastInt16Property = 0x0000000080000000ULL;
constexpr std::uint64_t kCastDoubleProperty = 0x0000000100000000ULL;
constexpr std::uint64_t kCastSoftClassProperty = 0x0000000200000000ULL;
constexpr std::uint64_t kCastMapProperty = 0x0000400000000000ULL;
constexpr std::uint64_t kCastSetProperty = 0x0000800000000000ULL;
constexpr std::uint64_t kCastEnumProperty = 0x0001000000000000ULL;
constexpr std::uint64_t kCastMulticastInlineDelegateProperty =
	0x0004000000000000ULL;
constexpr std::uint64_t kCastMulticastSparseDelegateProperty =
	0x0008000000000000ULL;

constexpr std::uint64_t kPropertyFlagParm = 0x0000000000000080ULL;
constexpr std::uint64_t kPropertyFlagOutParm = 0x0000000000000100ULL;
constexpr std::uint64_t kPropertyFlagReturnParm = 0x0000000000000400ULL;
constexpr std::uint64_t kPropertyFlagReferenceParm = 0x0000000008000000ULL;
constexpr std::uint32_t kFunctionFlagNative = 0x00000400U;

std::uint32_t ExactScalarSize(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8:
	case PropertyKind::UInt8: return 1;
	case PropertyKind::Int16:
	case PropertyKind::UInt16: return 2;
	case PropertyKind::Int32:
	case PropertyKind::UInt32:
	case PropertyKind::Float: return 4;
	case PropertyKind::Int64:
	case PropertyKind::UInt64:
	case PropertyKind::Double: return 8;
	default: return 0;
	}
}

bool IsFlatDescriptorKind(const PropertyKind kind) noexcept
{
	return ExactScalarSize(kind) != 0
		|| kind == PropertyKind::Bool
		|| kind == PropertyKind::Name
		|| kind == PropertyKind::String
		|| kind == PropertyKind::Text
		|| kind == PropertyKind::Object
		|| kind == PropertyKind::WeakObject
		|| kind == PropertyKind::SoftObject;
}

bool HasFlag(const std::uint64_t value, const std::uint64_t flag) noexcept
{
	return (value & flag) != 0;
}

bool SameHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameFunctionHandle(
	const FunctionHandle& left,
	const FunctionHandle& right) noexcept
{
	return SameHandle(left.Function, right.Function)
		&& SameHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
}

bool TryAddAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	const std::size_t size,
	std::uintptr_t& address) noexcept
{
	address = 0;
	if (base == 0 || offset < 0)
		return false;
	const auto unsignedOffset = static_cast<std::uintptr_t>(offset);
	if (unsignedOffset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	address = base + unsignedOffset;
	std::uintptr_t ignored = 0;
	return CheckedAddressRange(address, size, ignored);
}

template<typename T>
bool ReadStable(
	const std::uintptr_t base,
	const std::int32_t offset,
	T& value) noexcept
{
	static_assert(std::is_trivially_copyable_v<T>);
	std::uintptr_t address = 0;
	if (!TryAddAddress(base, offset, sizeof(T), address))
		return false;
	T first{};
	T second{};
	if (!ReadValue(address, first).Ok() || !ReadValue(address, second).Ok()
		|| std::memcmp(&first, &second, sizeof(T)) != 0)
	{
		return false;
	}
	value = first;
	return true;
}

bool IsTypeObject(const EngineSnapshotObject& object) noexcept
{
	return object.Kind == EngineObjectKind::Class
		|| object.Kind == EngineObjectKind::Struct
		|| object.Kind == EngineObjectKind::Enum;
}

ReflectedTypeKind ReflectedKind(const EngineObjectKind kind) noexcept
{
	switch (kind)
	{
	case EngineObjectKind::Class: return ReflectedTypeKind::Class;
	case EngineObjectKind::Enum: return ReflectedTypeKind::Enum;
	default: return ReflectedTypeKind::Struct;
	}
}

PropertyKind ClassifyProperty(
	const std::uint64_t castFlags,
	const std::uintptr_t referencedType) noexcept
{
	if (HasFlag(castFlags, kCastBoolProperty)) return PropertyKind::Bool;
	if (HasFlag(castFlags, kCastInt8Property)) return PropertyKind::Int8;
	if (HasFlag(castFlags, kCastInt16Property)) return PropertyKind::Int16;
	if (HasFlag(castFlags, kCastIntProperty)) return PropertyKind::Int32;
	if (HasFlag(castFlags, kCastInt64Property)) return PropertyKind::Int64;
	if (HasFlag(castFlags, kCastByteProperty))
		return referencedType == 0 ? PropertyKind::UInt8 : PropertyKind::Enum;
	if (HasFlag(castFlags, kCastUInt16Property)) return PropertyKind::UInt16;
	if (HasFlag(castFlags, kCastUInt32Property)) return PropertyKind::UInt32;
	if (HasFlag(castFlags, kCastUInt64Property)) return PropertyKind::UInt64;
	if (HasFlag(castFlags, kCastFloatProperty)) return PropertyKind::Float;
	if (HasFlag(castFlags, kCastDoubleProperty)) return PropertyKind::Double;
	if (HasFlag(castFlags, kCastNameProperty)) return PropertyKind::Name;
	if (HasFlag(castFlags, kCastStrProperty)) return PropertyKind::String;
	if (HasFlag(castFlags, kCastTextProperty)) return PropertyKind::Text;
	if (HasFlag(castFlags, kCastWeakObjectProperty)) return PropertyKind::WeakObject;
	if (HasFlag(castFlags, kCastSoftObjectProperty)
		|| HasFlag(castFlags, kCastSoftClassProperty))
	{
		return PropertyKind::SoftObject;
	}
	if (HasFlag(castFlags, kCastObjectProperty)
		|| HasFlag(castFlags, kCastClassProperty)
		|| HasFlag(castFlags, kCastInterfaceProperty))
	{
		return PropertyKind::Object;
	}
	if (HasFlag(castFlags, kCastEnumProperty)) return PropertyKind::Enum;
	if (HasFlag(castFlags, kCastStructProperty)) return PropertyKind::Struct;
	if (HasFlag(castFlags, kCastArrayProperty)) return PropertyKind::Array;
	if (HasFlag(castFlags, kCastMapProperty)) return PropertyKind::Map;
	if (HasFlag(castFlags, kCastSetProperty)) return PropertyKind::Set;
	if (HasFlag(castFlags, kCastDelegateProperty)
		|| HasFlag(castFlags, kCastMulticastDelegateProperty)
		|| HasFlag(castFlags, kCastMulticastInlineDelegateProperty)
		|| HasFlag(castFlags, kCastMulticastSparseDelegateProperty))
	{
		return PropertyKind::Delegate;
	}
	return PropertyKind::Unknown;
}

bool TryParameterDirection(
	const std::uint64_t flags,
	ReflectedParameterDirection& direction) noexcept
{
	if (!HasFlag(flags, kPropertyFlagParm))
		return false;
	if (HasFlag(flags, kPropertyFlagReturnParm))
	{
		direction = ReflectedParameterDirection::Return;
		return true;
	}
	if (!HasFlag(flags, kPropertyFlagOutParm))
	{
		direction = ReflectedParameterDirection::Input;
		return true;
	}
	direction = HasFlag(flags, kPropertyFlagReferenceParm)
		? ReflectedParameterDirection::InOut
		: ReflectedParameterDirection::Output;
	return true;
}

} // namespace

const char* ToString(const TypeCandidatePreparationError error) noexcept
{
	switch (error)
	{
	case TypeCandidatePreparationError::None: return "NONE";
	case TypeCandidatePreparationError::Busy: return "TYPE_PREPARE_BUSY";
	case TypeCandidatePreparationError::InvalidConfiguration:
		return "TYPE_PREPARE_INVALID";
	case TypeCandidatePreparationError::SnapshotUnavailable:
		return "TYPE_PREPARE_SNAPSHOT_UNAVAILABLE";
	case TypeCandidatePreparationError::SnapshotInvalid:
		return "TYPE_PREPARE_SNAPSHOT_INVALID";
	case TypeCandidatePreparationError::ReflectionUnavailable:
		return "TYPE_PREPARE_REFLECTION_UNAVAILABLE";
	case TypeCandidatePreparationError::ReflectionInvalid:
		return "TYPE_PREPARE_REFLECTION_INVALID";
	case TypeCandidatePreparationError::TypeCoverageExceeded:
		return "TYPE_PREPARE_COVERAGE_EXCEEDED";
	case TypeCandidatePreparationError::FunctionOwnerMissing:
		return "TYPE_PREPARE_FUNCTION_OWNER_MISSING";
	case TypeCandidatePreparationError::FunctionOwnerAmbiguous:
		return "TYPE_PREPARE_FUNCTION_OWNER_AMBIGUOUS";
	case TypeCandidatePreparationError::AllocationFailed:
		return "TYPE_PREPARE_ALLOCATION_FAILED";
	}
	return "TYPE_PREPARE_UNKNOWN";
}

class ObjectSnapshotTypeCandidateSource::Impl final
{
public:
	Impl(std::shared_ptr<const EngineContext> context, EngineFacade& engine)
		: Context(std::move(context)),
		  Engine(engine),
		  Metadata(Context ? CaptureTypeMetadataContext(*Context) : TypeMetadataContext{})
	{
	}

	struct PreparedPlan final
	{
		std::shared_ptr<const EngineSnapshot> Snapshot;
		std::shared_ptr<const ReflectionRuntimeSnapshot> Reflection;
		std::vector<const EngineSnapshotObject*> Types;
		std::vector<std::vector<const EngineSnapshotObject*>> FunctionsByType;
		std::unordered_map<std::uintptr_t, const EngineSnapshotObject*> ByAddress;
		std::unordered_map<std::uintptr_t, std::size_t> TypeIndexByAddress;
		std::size_t FunctionCount = 0;
	};

	struct TypeEvidence final
	{
		const EngineSnapshotObject* Object = nullptr;
		std::int32_t PropertiesSize = 0;
		std::int32_t MinAlignment = 0;
		std::uintptr_t Super = 0;
		std::uintptr_t ChildHead = 0;
		std::uintptr_t DefaultObject = 0;
		bool IsEnum = false;
	};

	struct NestedPropertyEvidence final
	{
		const EngineSnapshotObject* Object = nullptr;
		std::uintptr_t Address = 0;
		std::uintptr_t ClassAddress = 0;
		std::uint64_t CastFlags = 0;
		std::int32_t ArrayDim = 0;
		std::int32_t ElementSize = 0;
		std::int32_t Offset = 0;
		std::uint64_t Flags = 0;
		std::uintptr_t ReferencedType = 0;
		std::uint32_t BoolByteOffset = 0;
		std::uint8_t BoolMask = 0;
		std::string Name;
	};

	struct FieldEvidence final
	{
		const EngineSnapshotObject* Owner = nullptr;
		const EngineSnapshotObject* Object = nullptr;
		std::uintptr_t Address = 0;
		std::uintptr_t ClassAddress = 0;
		std::uintptr_t Next = 0;
		std::uint64_t CastFlags = 0;
		std::int32_t ArrayDim = 0;
		std::int32_t ElementSize = 0;
		std::int32_t Offset = 0;
		std::uint64_t Flags = 0;
		std::uintptr_t ReferencedType = 0;
		std::uint32_t BoolByteOffset = 0;
		std::uint8_t BoolMask = 0;
		std::optional<NestedPropertyEvidence> Element;
		std::uint32_t OwnerSize = 0;
		std::string Name;
		bool IsProperty = false;
		bool IsFunction = false;
	};

	struct FunctionEvidence final
	{
		const EngineSnapshotObject* Object = nullptr;
		const EngineSnapshotObject* Owner = nullptr;
		FunctionHandle Handle;
		std::uint32_t Flags = 0;
		std::int32_t ParameterSize = 0;
		std::uintptr_t NativeAddress = 0;
		std::uintptr_t ChildHead = 0;
		bool NativeAddressWitnessed = false;
	};

	using Evidence = std::variant<TypeEvidence, FieldEvidence, FunctionEvidence>;

	enum class CapturePhase : std::size_t
	{
		TypeBegin,
		Fields,
		FunctionBegin,
		FunctionFields,
		FunctionEnd,
		TypeEnd,
		Complete
	};

	struct ActiveState final
	{
		std::shared_ptr<const PreparedPlan> Plan;
		CapturePhase Phase = CapturePhase::TypeBegin;
		std::size_t TypeIndex = 0;
		std::size_t FunctionIndex = 0;
		std::uintptr_t CurrentField = 0;
		std::uint32_t CurrentOwnerSize = 0;
		std::uint32_t SeenEpoch = 0;
		std::size_t ChainDepth = 0;
		std::array<std::uintptr_t, kSeenFieldTableCapacity> SeenKeys{};
		std::array<std::uint32_t, kSeenFieldTableCapacity> SeenEpochs{};
		std::vector<Evidence> EvidenceRecords;
		std::size_t ValidationCursor = 0;
		bool EmptyValidationPending = false;
		bool ValidationBegun = false;
		std::size_t SourceSteps = 0;
		std::size_t ValidationSteps = 0;

		void BeginFieldChain() noexcept
		{
			++SeenEpoch;
			if (SeenEpoch == 0)
			{
				SeenEpochs.fill(0);
				SeenEpoch = 1;
			}
			ChainDepth = 0;
		}

		bool VisitField(const std::uintptr_t address) noexcept
		{
			if (address == 0 || ChainDepth >= kMaximumFieldChainDepth)
				return false;
			const std::size_t mask = kSeenFieldTableCapacity - 1;
			std::size_t slot = static_cast<std::size_t>(
				(address >> 4U) * 11400714819323198485ULL) & mask;
			for (std::size_t probe = 0; probe < kSeenFieldTableCapacity; ++probe)
			{
				if (SeenEpochs[slot] != SeenEpoch)
				{
					SeenEpochs[slot] = SeenEpoch;
					SeenKeys[slot] = address;
					++ChainDepth;
					return true;
				}
				if (SeenKeys[slot] == address)
					return false;
				slot = (slot + 1) & mask;
			}
			return false;
		}
	};

	std::shared_ptr<const EngineContext> Context;
	EngineFacade& Engine;
	TypeMetadataContext Metadata;
	std::atomic<std::shared_ptr<const PreparedPlan>> Prepared;
	std::mutex OwnershipMutex;
	std::unique_ptr<ActiveState> PreparedActive;
	std::unique_ptr<ActiveState> Active;
	std::atomic<bool> ActiveFlag{false};
	std::atomic<TypeCandidatePreparationError> PreparationError{
		TypeCandidatePreparationError::None};
	std::atomic<TypeSnapshotSourceError> SourceError{TypeSnapshotSourceError::None};
	std::atomic<std::uint64_t> PreparedGeneration{0};
	std::atomic<std::uint64_t> PreparedFingerprint{0};
	std::atomic<std::size_t> PreparedTypeCount{0};
	std::atomic<std::size_t> PreparedFunctionCount{0};
	std::atomic<std::size_t> DiagnosticPhase{0};
	std::atomic<std::size_t> DiagnosticSourceSteps{0};
	std::atomic<std::size_t> DiagnosticValidationSteps{0};
	std::atomic<std::size_t> DiagnosticEvidence{0};

	bool BaseConfigured() const noexcept
	{
		return Context
			&& Context.get() == Engine.Context().get()
			&& Context->Generation() != 0
			&& Engine.ContextGeneration() == Context->Generation()
			&& Engine.IsConfigured()
			&& Engine.Names().IsConfigured()
			&& Metadata.ContextGeneration == Context->Generation()
			&& Metadata.IsConfigured();
	}

	bool DependenciesCurrent(const PreparedPlan& plan) const noexcept
	{
		return BaseConfigured()
			&& Engine.Snapshots().Current() == plan.Snapshot
			&& Engine.Reflection() == plan.Reflection
			&& plan.Snapshot
			&& plan.Reflection
			&& plan.Reflection->Layout
			&& plan.Reflection->IsLayoutConfigured(Context->Generation());
	}

	const ReflectionFieldReport* Field(const ReflectionField field) const noexcept
	{
		if (!Active || !Active->Plan || !Active->Plan->Reflection
			|| !Active->Plan->Reflection->Layout)
		{
			return nullptr;
		}
		const ReflectionFieldReport* report =
			Active->Plan->Reflection->Layout->Find(field);
		return report && report->Validated ? report : nullptr;
	}

	bool DecodeFNameStable(
		std::uintptr_t base,
		std::int32_t offset,
		std::string& name) const;
	TypeSnapshotSourceError ReadTypeEvidence(
		const EngineSnapshotObject& object,
		TypeEvidence& evidence);
	TypeSnapshotSourceError ReadFieldEvidence(
		std::uintptr_t address,
		const EngineSnapshotObject& owner,
		std::uint32_t ownerSize,
		FieldEvidence& evidence);
	TypeSnapshotSourceError ReadNestedPropertyEvidence(
		std::uintptr_t address,
		NestedPropertyEvidence& evidence);
	TypeSnapshotSourceError ReadFunctionEvidence(
		const EngineSnapshotObject& object,
		const EngineSnapshotObject& owner,
		FunctionEvidence& evidence);
	TypeSnapshotSourceError BuildFlatDescriptor(
		std::int32_t elementSize,
		std::uint32_t boolByteOffset,
		std::uint8_t boolMask,
		std::uintptr_t referencedType,
		PropertyKind kind,
		bool requireExactType,
		std::shared_ptr<const PropertyDescriptor>& descriptor) const;
	TypeSnapshotSourceError BuildArrayDescriptor(
		const FieldEvidence& evidence,
		std::shared_ptr<const PropertyDescriptor>& descriptor) const;
	TypeSnapshotSourceError CaptureType(TypeSnapshotSourceStepResult& result);
	TypeSnapshotSourceError CaptureField(
		bool directTypeProperty,
		TypeSnapshotSourceStepResult& result);
	TypeSnapshotSourceError CaptureFunction(TypeSnapshotSourceStepResult& result);
	TypeSnapshotSourceError ValidateEvidence(const Evidence& evidence);

	const EngineSnapshotObject* CurrentType() const noexcept
	{
		return Active && Active->Plan && Active->TypeIndex < Active->Plan->Types.size()
			? Active->Plan->Types[Active->TypeIndex]
			: nullptr;
	}

	const EngineSnapshotObject* CurrentFunction() const noexcept
	{
		if (!Active || !Active->Plan
			|| Active->TypeIndex >= Active->Plan->FunctionsByType.size())
		{
			return nullptr;
		}
		const auto& functions = Active->Plan->FunctionsByType[Active->TypeIndex];
		return Active->FunctionIndex < functions.size()
			? functions[Active->FunctionIndex]
			: nullptr;
	}
};

bool ObjectSnapshotTypeCandidateSource::Impl::DecodeFNameStable(
	const std::uintptr_t base,
	const std::int32_t offset,
	std::string& name) const
{
	name.clear();
	if (!Context || Context->NameProfile().FNameSize <= 0
		|| Context->NameProfile().FNameSize > 64)
	{
		return false;
	}
	const std::size_t size = static_cast<std::size_t>(Context->NameProfile().FNameSize);
	std::uintptr_t address = 0;
	if (!TryAddAddress(base, offset, size, address))
		return false;
	std::array<std::byte, 64> first{};
	std::array<std::byte, 64> second{};
	if (!ReadMemory(address, std::span<std::byte>(first).first(size)).Ok())
		return false;
	const EngineNameResult decoded = Engine.Names().DecodeFName(address);
	if (!decoded.Ok() || decoded.Value.empty()
		|| decoded.Value.size() > EngineSnapshotStore::kMaxNameBytes)
	{
		return false;
	}
	if (!ReadMemory(address, std::span<std::byte>(second).first(size)).Ok()
		|| !std::equal(first.begin(), first.begin() + size, second.begin()))
	{
		return false;
	}
	name = decoded.Value;
	return true;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::ReadTypeEvidence(
	const EngineSnapshotObject& object,
	TypeEvidence& evidence)
{
	evidence = {.Object = &object, .IsEnum = object.Kind == EngineObjectKind::Enum};
	if (!Engine.ValidateObjectHandle(object.Handle).Ok())
		return TypeSnapshotSourceError::DependencyChanged;
	if (evidence.IsEnum)
		return TypeSnapshotSourceError::None;

	const ReflectionFieldReport* propertiesSize = Field(ReflectionField::StructPropertiesSize);
	const ReflectionFieldReport* minAlignment = Field(ReflectionField::StructMinAlignment);
	const ReflectionFieldReport* super = Field(ReflectionField::StructSuper);
	const ReflectionField childField = Active->Plan->Reflection->Layout->PropertySystem()
		== ReflectionPropertySystem::FProperty
		? ReflectionField::StructChildProperties
		: ReflectionField::StructChildren;
	const ReflectionFieldReport* children = Field(childField);
	if (!propertiesSize || !minAlignment || !super || !children
		|| !ReadStable(object.Handle.Address, propertiesSize->Offset, evidence.PropertiesSize)
		|| !ReadStable(object.Handle.Address, minAlignment->Offset, evidence.MinAlignment)
		|| !ReadStable(object.Handle.Address, super->Offset, evidence.Super)
		|| !ReadStable(object.Handle.Address, children->Offset, evidence.ChildHead))
	{
		return TypeSnapshotSourceError::MemoryUnavailable;
	}
	if (evidence.PropertiesSize < 0
		|| static_cast<std::uint64_t>(evidence.PropertiesSize)
			> TypeSnapshotStore::kMaxValueSize
		|| evidence.MinAlignment <= 0
		|| evidence.MinAlignment > 4096
		|| !std::has_single_bit(static_cast<std::uint32_t>(evidence.MinAlignment)))
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	if (evidence.Super != 0)
	{
		const auto found = Active->Plan->TypeIndexByAddress.find(evidence.Super);
		if (found == Active->Plan->TypeIndexByAddress.end())
			return TypeSnapshotSourceError::EvidenceUnavailable;
		const EngineSnapshotObject* parent = Active->Plan->Types[found->second];
		if (!parent || parent->Kind != object.Kind
			|| !Engine.ValidateObjectHandle(parent->Handle).Ok())
		{
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		}
	}
	if (object.Kind == EngineObjectKind::Class)
	{
		if (!ReadStable(
			object.Handle.Address,
			Metadata.ClassDefaultObject,
			evidence.DefaultObject))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		if (evidence.DefaultObject != 0)
		{
			const auto found = Active->Plan->ByAddress.find(evidence.DefaultObject);
			if (found != Active->Plan->ByAddress.end())
			{
				const EngineSnapshotObject* defaultObject = found->second;
				if (!defaultObject || defaultObject->ClassPath != object.FullPath
					|| !Engine.ValidateObjectHandle(defaultObject->Handle).Ok())
				{
					return TypeSnapshotSourceError::EvidenceAmbiguous;
				}
			}
		}
	}
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::ReadNestedPropertyEvidence(
	const std::uintptr_t address,
	NestedPropertyEvidence& evidence)
{
	evidence = {.Address = address};
	if (address == 0 || !Active || !Active->Plan || !Active->Plan->Reflection)
		return TypeSnapshotSourceError::InvalidConfiguration;
	const ReflectionLayout& layout = *Active->Plan->Reflection->Layout;
	if (layout.PropertySystem() == ReflectionPropertySystem::FProperty)
	{
		const ReflectionFieldReport* fieldClass = Field(ReflectionField::FFieldClass);
		const ReflectionFieldReport* fieldName = Field(ReflectionField::FFieldName);
		const ReflectionFieldReport* classFlags =
			Field(ReflectionField::FFieldClassCastFlags);
		if (!fieldClass || !fieldName || !classFlags
			|| !ReadStable(address, fieldClass->Offset, evidence.ClassAddress)
			|| evidence.ClassAddress == 0
			|| !ReadStable(
				evidence.ClassAddress,
				classFlags->Offset,
				evidence.CastFlags)
			|| !DecodeFNameStable(address, fieldName->Offset, evidence.Name))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else
	{
		const auto object = Active->Plan->ByAddress.find(address);
		if (object == Active->Plan->ByAddress.end() || !object->second
			|| !Engine.ValidateObjectHandle(object->second->Handle).Ok()
			|| !ReadStable(address, Metadata.ObjectClass, evidence.ClassAddress)
			|| evidence.ClassAddress == 0
			|| !ReadStable(
				evidence.ClassAddress,
				Metadata.ClassCastFlags,
				evidence.CastFlags))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		evidence.Object = object->second;
		evidence.Name = evidence.Object->Name;
	}

	const ReflectionFieldReport* arrayDim = Field(ReflectionField::PropertyArrayDim);
	const ReflectionFieldReport* elementSize = Field(ReflectionField::PropertyElementSize);
	const ReflectionFieldReport* flags = Field(ReflectionField::PropertyFlags);
	const ReflectionFieldReport* offset = Field(ReflectionField::PropertyOffset);
	if (!arrayDim || !elementSize || !flags || !offset
		|| !ReadStable(address, arrayDim->Offset, evidence.ArrayDim)
		|| !ReadStable(address, elementSize->Offset, evidence.ElementSize)
		|| !ReadStable(address, flags->Offset, evidence.Flags)
		|| !ReadStable(address, offset->Offset, evidence.Offset))
	{
		return TypeSnapshotSourceError::MemoryUnavailable;
	}
	if (evidence.ArrayDim != 1 || evidence.ElementSize <= 0
		|| static_cast<std::uint64_t>(evidence.ElementSize)
			> TypeSnapshotStore::kMaxValueSize
		|| evidence.Offset != 0)
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}

	if (HasFlag(evidence.CastFlags, kCastByteProperty))
	{
		const ReflectionFieldReport* enumField = Field(ReflectionField::BytePropertyEnum);
		if (!enumField
			|| !ReadStable(address, enumField->Offset, evidence.ReferencedType))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastObjectProperty)
		|| HasFlag(evidence.CastFlags, kCastClassProperty)
		|| HasFlag(evidence.CastFlags, kCastInterfaceProperty))
	{
		const ReflectionFieldReport* objectClass = Field(ReflectionField::ObjectPropertyClass);
		if (!objectClass
			|| !ReadStable(address, objectClass->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastStructProperty))
	{
		const ReflectionFieldReport* structure = Field(ReflectionField::StructPropertyStruct);
		if (!structure
			|| !ReadStable(address, structure->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastEnumProperty))
	{
		const ReflectionFieldReport* enumField = Field(ReflectionField::EnumPropertyEnum);
		if (!enumField
			|| !ReadStable(address, enumField->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	if (HasFlag(evidence.CastFlags, kCastBoolProperty))
	{
		const ReflectionFieldReport* byteOffset = Field(ReflectionField::BoolByteOffset);
		const ReflectionFieldReport* fieldMask = Field(ReflectionField::BoolFieldMask);
		std::uint8_t witnessedByteOffset = 0;
		if (!byteOffset || !fieldMask
			|| !ReadStable(address, byteOffset->Offset, witnessedByteOffset)
			|| !ReadStable(address, fieldMask->Offset, evidence.BoolMask)
			|| evidence.BoolMask == 0
			|| witnessedByteOffset >= evidence.ElementSize)
		{
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		}
		evidence.BoolByteOffset = witnessedByteOffset;
	}
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::ReadFieldEvidence(
	const std::uintptr_t address,
	const EngineSnapshotObject& owner,
	const std::uint32_t ownerSize,
	FieldEvidence& evidence)
{
	evidence = {
		.Owner = &owner,
		.Address = address,
		.OwnerSize = ownerSize
	};
	const ReflectionLayout& layout = *Active->Plan->Reflection->Layout;
	if (layout.PropertySystem() == ReflectionPropertySystem::FProperty)
	{
		const ReflectionFieldReport* fieldClass = Field(ReflectionField::FFieldClass);
		const ReflectionFieldReport* fieldNext = Field(ReflectionField::FFieldNext);
		const ReflectionFieldReport* fieldName = Field(ReflectionField::FFieldName);
		const ReflectionFieldReport* classFlags =
			Field(ReflectionField::FFieldClassCastFlags);
		if (!fieldClass || !fieldNext || !fieldName || !classFlags
			|| !ReadStable(address, fieldClass->Offset, evidence.ClassAddress)
			|| evidence.ClassAddress == 0
			|| !ReadStable(
				evidence.ClassAddress,
				classFlags->Offset,
				evidence.CastFlags)
			|| !ReadStable(address, fieldNext->Offset, evidence.Next)
			|| !DecodeFNameStable(address, fieldName->Offset, evidence.Name))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		evidence.IsProperty = HasFlag(evidence.CastFlags, kCastProperty);
		evidence.IsFunction = HasFlag(evidence.CastFlags, kCastFunction);
		if (!evidence.IsProperty || evidence.IsFunction)
			return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	else
	{
		const ReflectionFieldReport* fieldNext = Field(ReflectionField::UFieldNext);
		const auto object = Active->Plan->ByAddress.find(address);
		if (!fieldNext || object == Active->Plan->ByAddress.end() || !object->second
			|| !Engine.ValidateObjectHandle(object->second->Handle).Ok()
			|| !ReadStable(address, Metadata.ObjectClass, evidence.ClassAddress)
			|| evidence.ClassAddress == 0
			|| !ReadStable(
				evidence.ClassAddress,
				Metadata.ClassCastFlags,
				evidence.CastFlags)
			|| !ReadStable(address, fieldNext->Offset, evidence.Next))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		evidence.Object = object->second;
		evidence.Name = evidence.Object->Name;
		evidence.IsProperty = HasFlag(evidence.CastFlags, kCastProperty);
		evidence.IsFunction = HasFlag(evidence.CastFlags, kCastFunction);
		if (evidence.IsProperty && evidence.IsFunction)
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		if (evidence.IsFunction && evidence.Object->Kind != EngineObjectKind::Function)
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		if (evidence.IsFunction
			&& evidence.Object->FullPath != owner.FullPath + "." + evidence.Name)
		{
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		}
	}

	if (!evidence.IsProperty)
		return TypeSnapshotSourceError::None;
	const std::string expectedPath = owner.FullPath + "." + evidence.Name;
	if (evidence.Object && evidence.Object->FullPath != expectedPath)
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	const ReflectionFieldReport* arrayDim = Field(ReflectionField::PropertyArrayDim);
	const ReflectionFieldReport* elementSize = Field(ReflectionField::PropertyElementSize);
	const ReflectionFieldReport* flags = Field(ReflectionField::PropertyFlags);
	const ReflectionFieldReport* offset = Field(ReflectionField::PropertyOffset);
	if (!arrayDim || !elementSize || !flags || !offset
		|| !ReadStable(address, arrayDim->Offset, evidence.ArrayDim)
		|| !ReadStable(address, elementSize->Offset, evidence.ElementSize)
		|| !ReadStable(address, flags->Offset, evidence.Flags)
		|| !ReadStable(address, offset->Offset, evidence.Offset))
	{
		return TypeSnapshotSourceError::MemoryUnavailable;
	}
	if (evidence.ArrayDim <= 0 || evidence.ArrayDim > 1024
		|| evidence.ElementSize <= 0
		|| static_cast<std::uint64_t>(evidence.ElementSize)
			> TypeSnapshotStore::kMaxValueSize
		|| evidence.Offset < 0)
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	const std::uint64_t total = static_cast<std::uint64_t>(evidence.ElementSize)
		* static_cast<std::uint64_t>(evidence.ArrayDim);
	if (static_cast<std::uint64_t>(evidence.Offset) > ownerSize
		|| total > static_cast<std::uint64_t>(ownerSize)
			- static_cast<std::uint64_t>(evidence.Offset))
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	if (HasFlag(evidence.CastFlags, kCastByteProperty))
	{
		const ReflectionFieldReport* enumField = Field(ReflectionField::BytePropertyEnum);
		if (!enumField
			|| !ReadStable(address, enumField->Offset, evidence.ReferencedType))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastEnumProperty))
	{
		const ReflectionFieldReport* enumField = Field(ReflectionField::EnumPropertyEnum);
		if (!enumField
			|| !ReadStable(address, enumField->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastObjectProperty)
		|| HasFlag(evidence.CastFlags, kCastClassProperty)
		|| HasFlag(evidence.CastFlags, kCastInterfaceProperty))
	{
		const ReflectionFieldReport* objectClass = Field(ReflectionField::ObjectPropertyClass);
		if (!objectClass
			|| !ReadStable(address, objectClass->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastStructProperty))
	{
		const ReflectionFieldReport* structure = Field(ReflectionField::StructPropertyStruct);
		if (!structure
			|| !ReadStable(address, structure->Offset, evidence.ReferencedType)
			|| evidence.ReferencedType == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
	}
	else if (HasFlag(evidence.CastFlags, kCastArrayProperty))
	{
		const ReflectionFieldReport* inner = Field(ReflectionField::ArrayPropertyInner);
		std::uintptr_t innerAddress = 0;
		if (!inner
			|| !ReadStable(address, inner->Offset, innerAddress)
			|| innerAddress == 0)
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		NestedPropertyEvidence element;
		const TypeSnapshotSourceError nested = ReadNestedPropertyEvidence(
			innerAddress,
			element);
		if (nested != TypeSnapshotSourceError::None)
			return nested;
		evidence.ReferencedType = innerAddress;
		evidence.Element.emplace(std::move(element));
	}
	if (HasFlag(evidence.CastFlags, kCastBoolProperty))
	{
		const ReflectionFieldReport* byteOffset = Field(ReflectionField::BoolByteOffset);
		const ReflectionFieldReport* fieldMask = Field(ReflectionField::BoolFieldMask);
		std::uint8_t witnessedByteOffset = 0;
		if (!byteOffset || !fieldMask
			|| !ReadStable(address, byteOffset->Offset, witnessedByteOffset)
			|| !ReadStable(address, fieldMask->Offset, evidence.BoolMask)
			|| evidence.BoolMask == 0
			|| witnessedByteOffset >= evidence.ElementSize)
		{
			return TypeSnapshotSourceError::EvidenceAmbiguous;
		}
		evidence.BoolByteOffset = witnessedByteOffset;
	}
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::BuildFlatDescriptor(
	const std::int32_t elementSize,
	const std::uint32_t boolByteOffset,
	const std::uint8_t boolMask,
	const std::uintptr_t referencedType,
	const PropertyKind kind,
	const bool requireExactType,
	std::shared_ptr<const PropertyDescriptor>& descriptor) const
{
	descriptor.reset();
	if (!IsFlatDescriptorKind(kind) || elementSize <= 0)
		return TypeSnapshotSourceError::None;
	const auto size = static_cast<std::uint32_t>(elementSize);
	const std::uint32_t scalarSize = ExactScalarSize(kind);
	if ((scalarSize != 0 && size != scalarSize)
		|| (kind == PropertyKind::Bool
			&& (boolMask == 0 || boolByteOffset >= size))
		|| (kind == PropertyKind::Name
			&& (Context->NameProfile().FNameSize <= 0
				|| size != static_cast<std::uint32_t>(Context->NameProfile().FNameSize)))
		|| (kind == PropertyKind::String && size < sizeof(std::uintptr_t) + 8)
		|| (kind == PropertyKind::Text && size < sizeof(std::uintptr_t))
		|| (kind == PropertyKind::Object && size != sizeof(std::uintptr_t))
		|| (kind == PropertyKind::WeakObject && size < 8)
		|| (kind == PropertyKind::SoftObject
			&& (Context->NameProfile().FNameSize <= 0
				|| size < static_cast<std::uint32_t>(Context->NameProfile().FNameSize))))
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	auto mutableDescriptor = std::make_shared<PropertyDescriptor>();
	mutableDescriptor->Kind = kind;
	mutableDescriptor->TypeName = ToString(kind);
	mutableDescriptor->Size = size;
	if (kind == PropertyKind::Object && referencedType != 0)
	{
		const auto referenced = Active->Plan->ByAddress.find(referencedType);
		if (referenced == Active->Plan->ByAddress.end() || !referenced->second
			|| referenced->second->Kind != EngineObjectKind::Class)
		{
			if (requireExactType)
				return TypeSnapshotSourceError::EvidenceUnavailable;
		}
		else
		{
			mutableDescriptor->TypeName = referenced->second->FullPath;
		}
	}
	else if (kind == PropertyKind::Object && requireExactType)
	{
		return TypeSnapshotSourceError::EvidenceUnavailable;
	}
	if (kind == PropertyKind::Bool)
	{
		mutableDescriptor->BoolByteOffset = boolByteOffset;
		mutableDescriptor->BoolMask = boolMask;
	}
	descriptor = std::shared_ptr<const PropertyDescriptor>(std::move(mutableDescriptor));
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::BuildArrayDescriptor(
	const FieldEvidence& evidence,
	std::shared_ptr<const PropertyDescriptor>& descriptor) const
{
	descriptor.reset();
	const DynamicArrayLayout layout = WindowsX64ScriptArrayLayout();
	if (!evidence.Element
		|| evidence.ArrayDim != 1
		|| evidence.ElementSize != layout.HeaderSize)
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	const NestedPropertyEvidence& element = *evidence.Element;
	const PropertyKind elementKind = ClassifyProperty(
		element.CastFlags,
		element.ReferencedType);
	if (!IsFlatDescriptorKind(elementKind)
		|| HasFlag(element.CastFlags, kCastInterfaceProperty))
	{
		return TypeSnapshotSourceError::None;
	}
	std::shared_ptr<const PropertyDescriptor> elementDescriptor;
	const TypeSnapshotSourceError elementResult = BuildFlatDescriptor(
		element.ElementSize,
		element.BoolByteOffset,
		element.BoolMask,
		element.ReferencedType,
		elementKind,
		elementKind == PropertyKind::Object,
		elementDescriptor);
	if (elementResult != TypeSnapshotSourceError::None || !elementDescriptor)
		return elementResult;
	auto mutableDescriptor = std::make_shared<PropertyDescriptor>();
	mutableDescriptor->Kind = PropertyKind::Array;
	mutableDescriptor->TypeName = "array<" + elementDescriptor->TypeName + ">";
	mutableDescriptor->Size = static_cast<std::uint32_t>(evidence.ElementSize);
	mutableDescriptor->ElementStride = static_cast<std::uint32_t>(element.ElementSize);
	mutableDescriptor->Element = std::move(elementDescriptor);
	descriptor = std::shared_ptr<const PropertyDescriptor>(std::move(mutableDescriptor));
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError
ObjectSnapshotTypeCandidateSource::Impl::ReadFunctionEvidence(
	const EngineSnapshotObject& object,
	const EngineSnapshotObject& owner,
	FunctionEvidence& evidence)
{
	evidence = {.Object = &object, .Owner = &owner};
	const FunctionHandleResult issued = Engine.IssueFunctionHandle(object.Handle.Index);
	if (!issued.Ok() || !SameHandle(issued.Value.Function, object.Handle)
		|| !SameHandle(issued.Value.Owner, owner.Handle))
	{
		return TypeSnapshotSourceError::DependencyChanged;
	}
	evidence.Handle = issued.Value;
	const ReflectionFieldReport* parameterSize = Field(ReflectionField::StructPropertiesSize);
	const ReflectionField childField = Active->Plan->Reflection->Layout->PropertySystem()
		== ReflectionPropertySystem::FProperty
		? ReflectionField::StructChildProperties
		: ReflectionField::StructChildren;
	const ReflectionFieldReport* children = Field(childField);
	if (!parameterSize || !children
		|| !ReadStable(object.Handle.Address, Metadata.FunctionFlags, evidence.Flags)
		|| !ReadStable(
			object.Handle.Address,
			parameterSize->Offset,
			evidence.ParameterSize)
		|| !ReadStable(object.Handle.Address, children->Offset, evidence.ChildHead))
	{
		return TypeSnapshotSourceError::MemoryUnavailable;
	}
	if (evidence.ParameterSize < 0
		|| static_cast<std::uint64_t>(evidence.ParameterSize)
			> TypeSnapshotStore::kMaxParameterSize)
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	if (Metadata.FunctionExec > 0)
	{
		if (!ReadStable(
			object.Handle.Address,
			Metadata.FunctionExec,
			evidence.NativeAddress))
		{
			return TypeSnapshotSourceError::MemoryUnavailable;
		}
		evidence.NativeAddressWitnessed = true;
	}
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError ObjectSnapshotTypeCandidateSource::Impl::CaptureType(
	TypeSnapshotSourceStepResult& result)
{
	const EngineSnapshotObject* object = CurrentType();
	if (!object)
		return TypeSnapshotSourceError::ContractViolation;
	TypeEvidence evidence;
	const TypeSnapshotSourceError read = ReadTypeEvidence(*object, evidence);
	if (read != TypeSnapshotSourceError::None)
		return read;
	ReflectedType type{
		.Handle = object->Handle,
		.Kind = ReflectedKind(object->Kind),
		.Name = object->Name,
		.FullPath = object->FullPath,
		.PackagePath = object->PackagePath
	};
	if (object->Kind == EngineObjectKind::Enum)
	{
		type.EnumState = ReflectedMemberState::Unavailable;
		type.EnumReasonCode = "ENUM_LAYOUT_NOT_CAPTURED";
		type.EnumReason =
			"No witnessed immutable UEnum entry layout is available";
	}
	else
	{
		type.PropertiesSize = static_cast<std::uint32_t>(evidence.PropertiesSize);
		type.MinAlignment = static_cast<std::uint32_t>(evidence.MinAlignment);
		if (evidence.Super != 0)
		{
			const auto found = Active->Plan->TypeIndexByAddress.find(evidence.Super);
			type.Super = Active->Plan->Types[found->second]->Handle;
		}
		if (object->Kind == EngineObjectKind::Class)
		{
			if (evidence.DefaultObject == 0)
			{
				type.DefaultObjectState = ClassDefaultObjectState::NotConstructed;
			}
			else
			{
				const auto found = Active->Plan->ByAddress.find(evidence.DefaultObject);
				if (found == Active->Plan->ByAddress.end())
				{
					type.DefaultObjectState = ClassDefaultObjectState::Unavailable;
					type.DefaultObjectReasonCode = "TYPE_CDO_NOT_IN_OBJECT_SNAPSHOT";
					type.DefaultObjectReason =
						"The witnessed class default object is absent from the exact object generation";
				}
				else
				{
					type.DefaultObjectState = ClassDefaultObjectState::Present;
					type.DefaultObject = found->second->Handle;
				}
			}
		}
	}
	Active->EvidenceRecords.emplace_back(evidence);
	Active->CurrentField = evidence.ChildHead;
	Active->CurrentOwnerSize = object->Kind == EngineObjectKind::Enum
		? 0
		: static_cast<std::uint32_t>(evidence.PropertiesSize);
	Active->BeginFieldChain();
	Active->Phase = object->Kind == EngineObjectKind::Enum
		? CapturePhase::TypeEnd
		: CapturePhase::Fields;
	result.Record.emplace(TypeSnapshotTypeBegin{std::move(type)});
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError ObjectSnapshotTypeCandidateSource::Impl::CaptureField(
	const bool directTypeProperty,
	TypeSnapshotSourceStepResult& result)
{
	const EngineSnapshotObject* type = CurrentType();
	const EngineSnapshotObject* function = directTypeProperty ? nullptr : CurrentFunction();
	const EngineSnapshotObject* owner = directTypeProperty ? type : function;
	if (!type || !owner || Active->CurrentField == 0
		|| !Active->VisitField(Active->CurrentField))
	{
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	}
	const std::uint32_t ownerSize = Active->CurrentOwnerSize;
	FieldEvidence evidence;
	const TypeSnapshotSourceError read = ReadFieldEvidence(
		Active->CurrentField,
		*owner,
		ownerSize,
		evidence);
	if (read != TypeSnapshotSourceError::None)
		return read;
	Active->CurrentField = evidence.Next;
	Active->EvidenceRecords.emplace_back(evidence);
	if (!evidence.IsProperty)
		return TypeSnapshotSourceError::None;
	const bool isParameter = HasFlag(evidence.Flags, kPropertyFlagParm);
	if (directTypeProperty && isParameter)
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	if (!directTypeProperty && !isParameter)
		return TypeSnapshotSourceError::None;

	const PropertyKind kind = ClassifyProperty(
		evidence.CastFlags,
		evidence.ReferencedType);
	ReflectedProperty property{
		.Name = evidence.Name,
		.TypeName = ToString(kind),
		.Kind = kind,
		.Offset = static_cast<std::uint32_t>(evidence.Offset),
		.Size = static_cast<std::uint32_t>(evidence.ElementSize),
		.ArrayDim = static_cast<std::uint32_t>(evidence.ArrayDim),
		.Flags = evidence.Flags
	};
	if (kind == PropertyKind::Unknown || kind == PropertyKind::Delegate)
	{
		property.State = ReflectedMemberState::Unsupported;
		property.ReasonCode = "PROPERTY_KIND_UNSUPPORTED";
		property.Reason =
			"The witnessed property class has no supported immutable descriptor kind";
	}
	else if ((IsFlatDescriptorKind(kind)
			&& !HasFlag(evidence.CastFlags, kCastInterfaceProperty))
		|| kind == PropertyKind::Array)
	{
		const TypeSnapshotSourceError descriptorResult = kind == PropertyKind::Array
			? BuildArrayDescriptor(evidence, property.Descriptor)
			: BuildFlatDescriptor(
				evidence.ElementSize,
				evidence.BoolByteOffset,
				evidence.BoolMask,
				evidence.ReferencedType,
				kind,
				false,
				property.Descriptor);
		if (descriptorResult != TypeSnapshotSourceError::None)
			return descriptorResult;
		if (property.Descriptor)
		{
			property.TypeName = property.Descriptor->TypeName;
			property.State = ReflectedMemberState::Supported;
		}
		else
		{
			property.State = ReflectedMemberState::Unavailable;
			property.ReasonCode = "PROPERTY_DESCRIPTOR_NOT_CAPTURED";
			property.Reason =
				"The container element metadata is witnessed, but its immutable descriptor kind is not captured";
		}
	}
	else
	{
		property.State = ReflectedMemberState::Unavailable;
		property.ReasonCode = "PROPERTY_DESCRIPTOR_NOT_CAPTURED";
		property.Reason =
			"Structural metadata is witnessed, but no immutable descriptor graph has been captured";
	}
	if (directTypeProperty)
	{
		result.Record.emplace(TypeSnapshotPropertyRecord{std::move(property)});
		return TypeSnapshotSourceError::None;
	}
	ReflectedParameterDirection direction{};
	if (!TryParameterDirection(evidence.Flags, direction))
		return TypeSnapshotSourceError::EvidenceAmbiguous;
	result.Record.emplace(TypeSnapshotParameterRecord{
		ReflectedParameter{.Direction = direction, .Property = std::move(property)}});
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError ObjectSnapshotTypeCandidateSource::Impl::CaptureFunction(
	TypeSnapshotSourceStepResult& result)
{
	const EngineSnapshotObject* type = CurrentType();
	const EngineSnapshotObject* function = CurrentFunction();
	if (!type || !function)
		return TypeSnapshotSourceError::ContractViolation;
	FunctionEvidence evidence;
	const TypeSnapshotSourceError read = ReadFunctionEvidence(
		*function,
		*type,
		evidence);
	if (read != TypeSnapshotSourceError::None)
		return read;
	ReflectedFunction reflected{
		.Handle = evidence.Handle,
		.Name = function->Name,
		.FullPath = function->FullPath,
		.Flags = evidence.Flags,
		.ParameterSize = static_cast<std::uint32_t>(evidence.ParameterSize)
	};
	if ((evidence.Flags & kFunctionFlagNative) != 0
		&& evidence.NativeAddressWitnessed
		&& evidence.NativeAddress != 0)
	{
		reflected.NativeAddress = evidence.NativeAddress;
		reflected.Implementation = ReflectedFunctionImplementation::Native;
	}
	else
	{
		reflected.Implementation = ReflectedFunctionImplementation::Unavailable;
		if (!evidence.NativeAddressWitnessed)
		{
			reflected.ReasonCode = "FUNCTION_EXEC_OFFSET_UNAVAILABLE";
			reflected.Reason =
				"No validated native implementation pointer offset is available";
		}
		else if ((evidence.Flags & kFunctionFlagNative) == 0)
		{
			reflected.ReasonCode = "FUNCTION_BYTECODE_NOT_CAPTURED";
			reflected.Reason =
				"A non-native function requires a witnessed bounded bytecode layout";
		}
		else
		{
			reflected.ReasonCode = "FUNCTION_IMPLEMENTATION_NOT_WITNESSED";
			reflected.Reason =
				"A native flag without a native pointer does not prove an implementation";
		}
	}
	Active->EvidenceRecords.emplace_back(evidence);
	Active->CurrentField = evidence.ChildHead;
	Active->CurrentOwnerSize = static_cast<std::uint32_t>(evidence.ParameterSize);
	Active->BeginFieldChain();
	Active->Phase = CapturePhase::FunctionFields;
	result.Record.emplace(TypeSnapshotFunctionBegin{std::move(reflected)});
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceError ObjectSnapshotTypeCandidateSource::Impl::ValidateEvidence(
	const Evidence& evidence)
{
	if (const auto* expected = std::get_if<TypeEvidence>(&evidence))
	{
		if (!expected->Object)
			return TypeSnapshotSourceError::ContractViolation;
		TypeEvidence current;
		const TypeSnapshotSourceError read = ReadTypeEvidence(*expected->Object, current);
		if (read != TypeSnapshotSourceError::None)
			return read;
		return current.Object == expected->Object
			&& current.PropertiesSize == expected->PropertiesSize
			&& current.MinAlignment == expected->MinAlignment
			&& current.Super == expected->Super
			&& current.ChildHead == expected->ChildHead
			&& current.DefaultObject == expected->DefaultObject
			&& current.IsEnum == expected->IsEnum
			? TypeSnapshotSourceError::None
			: TypeSnapshotSourceError::DependencyChanged;
	}
	if (const auto* expected = std::get_if<FieldEvidence>(&evidence))
	{
		if (!expected->Owner)
			return TypeSnapshotSourceError::ContractViolation;
		FieldEvidence current;
		const TypeSnapshotSourceError read = ReadFieldEvidence(
			expected->Address,
			*expected->Owner,
			expected->OwnerSize,
			current);
		if (read != TypeSnapshotSourceError::None)
			return read;
		const auto sameElement = [](
			const std::optional<NestedPropertyEvidence>& left,
			const std::optional<NestedPropertyEvidence>& right) {
			if (left.has_value() != right.has_value())
				return false;
			if (!left)
				return true;
			return left->Object == right->Object
				&& left->Address == right->Address
				&& left->ClassAddress == right->ClassAddress
				&& left->CastFlags == right->CastFlags
				&& left->ArrayDim == right->ArrayDim
				&& left->ElementSize == right->ElementSize
				&& left->Offset == right->Offset
				&& left->Flags == right->Flags
				&& left->ReferencedType == right->ReferencedType
				&& left->BoolByteOffset == right->BoolByteOffset
				&& left->BoolMask == right->BoolMask
				&& left->Name == right->Name;
		};
		return current.Owner == expected->Owner
			&& current.Object == expected->Object
			&& current.Address == expected->Address
			&& current.ClassAddress == expected->ClassAddress
			&& current.Next == expected->Next
			&& current.CastFlags == expected->CastFlags
			&& current.ArrayDim == expected->ArrayDim
			&& current.ElementSize == expected->ElementSize
			&& current.Offset == expected->Offset
			&& current.Flags == expected->Flags
			&& current.ReferencedType == expected->ReferencedType
			&& current.BoolByteOffset == expected->BoolByteOffset
			&& current.BoolMask == expected->BoolMask
			&& sameElement(current.Element, expected->Element)
			&& current.OwnerSize == expected->OwnerSize
			&& current.Name == expected->Name
			&& current.IsProperty == expected->IsProperty
			&& current.IsFunction == expected->IsFunction
			? TypeSnapshotSourceError::None
			: TypeSnapshotSourceError::DependencyChanged;
	}
	const auto* expected = std::get_if<FunctionEvidence>(&evidence);
	if (!expected || !expected->Object || !expected->Owner)
		return TypeSnapshotSourceError::ContractViolation;
	FunctionEvidence current;
	const TypeSnapshotSourceError read = ReadFunctionEvidence(
		*expected->Object,
		*expected->Owner,
		current);
	if (read != TypeSnapshotSourceError::None)
		return read;
	return current.Object == expected->Object
		&& current.Owner == expected->Owner
		&& SameFunctionHandle(current.Handle, expected->Handle)
		&& current.Flags == expected->Flags
		&& current.ParameterSize == expected->ParameterSize
		&& current.NativeAddress == expected->NativeAddress
		&& current.ChildHead == expected->ChildHead
		&& current.NativeAddressWitnessed == expected->NativeAddressWitnessed
		? TypeSnapshotSourceError::None
		: TypeSnapshotSourceError::DependencyChanged;
}

ObjectSnapshotTypeCandidateSource::ObjectSnapshotTypeCandidateSource(
	std::shared_ptr<const EngineContext> context,
	EngineFacade& engine)
	: m_Impl(std::make_unique<Impl>(std::move(context), engine))
{
}

ObjectSnapshotTypeCandidateSource::~ObjectSnapshotTypeCandidateSource() = default;

TypeCandidatePreparationResult ObjectSnapshotTypeCandidateSource::Prepare() noexcept
{
	if (!m_Impl || !m_Impl->BaseConfigured())
	{
		if (m_Impl)
		{
			m_Impl->PreparationError.store(
				TypeCandidatePreparationError::InvalidConfiguration,
				std::memory_order_release);
		}
		return {.Error = TypeCandidatePreparationError::InvalidConfiguration};
	}
	try
	{
		{
			std::lock_guard lock(m_Impl->OwnershipMutex);
			if (m_Impl->ActiveFlag.load(std::memory_order_acquire)
				|| m_Impl->Active || m_Impl->PreparedActive
				|| m_Impl->Prepared.load(std::memory_order_acquire))
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::Busy,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::Busy};
			}
		}
		const std::shared_ptr<const EngineSnapshot> snapshot =
			m_Impl->Engine.Snapshots().Current();
		if (!snapshot)
		{
			m_Impl->PreparationError.store(
				TypeCandidatePreparationError::SnapshotUnavailable,
				std::memory_order_release);
			return {.Error = TypeCandidatePreparationError::SnapshotUnavailable};
		}
		if (snapshot->SessionId != m_Impl->Engine.SessionId()
			|| snapshot->ContextGeneration != m_Impl->Context->Generation()
			|| snapshot->Generation == 0)
		{
			m_Impl->PreparationError.store(
				TypeCandidatePreparationError::SnapshotInvalid,
				std::memory_order_release);
			return {.Error = TypeCandidatePreparationError::SnapshotInvalid};
		}
		const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection =
			m_Impl->Engine.Reflection();
		if (!reflection)
		{
			m_Impl->PreparationError.store(
				TypeCandidatePreparationError::ReflectionUnavailable,
				std::memory_order_release);
			return {.Error = TypeCandidatePreparationError::ReflectionUnavailable};
		}
		if (!reflection->Layout
			|| !reflection->IsLayoutConfigured(m_Impl->Context->Generation()))
		{
			m_Impl->PreparationError.store(
				TypeCandidatePreparationError::ReflectionInvalid,
				std::memory_order_release);
			return {.Error = TypeCandidatePreparationError::ReflectionInvalid};
		}

		auto plan = std::make_shared<Impl::PreparedPlan>();
		plan->Snapshot = snapshot;
		plan->Reflection = reflection;
		plan->ByAddress.reserve(snapshot->Objects.size());
		plan->TypeIndexByAddress.reserve(snapshot->Objects.size());
		std::map<std::string_view, std::size_t, std::less<>> typesByPath;
		for (const EngineSnapshotObject& object : snapshot->Objects)
		{
			if (!plan->ByAddress.emplace(object.Handle.Address, &object).second)
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::SnapshotInvalid,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::SnapshotInvalid};
			}
			if (!IsTypeObject(object))
				continue;
			if (plan->Types.size() >= TypeSnapshotStore::kMaxTypeRecords)
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::TypeCoverageExceeded,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::TypeCoverageExceeded};
			}
			const std::size_t typeIndex = plan->Types.size();
			if (!typesByPath.emplace(object.FullPath, typeIndex).second
				|| !plan->TypeIndexByAddress.emplace(
					object.Handle.Address,
					typeIndex).second)
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::FunctionOwnerAmbiguous,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::FunctionOwnerAmbiguous};
			}
			plan->Types.push_back(&object);
		}
		plan->FunctionsByType.resize(plan->Types.size());
		for (const EngineSnapshotObject& object : snapshot->Objects)
		{
			if (object.Kind != EngineObjectKind::Function)
				continue;
			if (plan->FunctionCount >= TypeSnapshotStore::kMaxTotalMembers
				|| object.Name.empty()
				|| object.FullPath.size() <= object.Name.size()
				|| !object.FullPath.ends_with(object.Name))
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::TypeCoverageExceeded,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::TypeCoverageExceeded};
			}
			const std::size_t separator = object.FullPath.size() - object.Name.size() - 1;
			if (object.FullPath[separator] != '.')
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::FunctionOwnerMissing,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::FunctionOwnerMissing};
			}
			const std::string_view ownerPath(object.FullPath.data(), separator);
			const auto owner = typesByPath.find(ownerPath);
			if (owner == typesByPath.end()
				|| plan->Types[owner->second]->Kind == EngineObjectKind::Enum)
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::FunctionOwnerMissing,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::FunctionOwnerMissing};
			}
			plan->FunctionsByType[owner->second].push_back(&object);
			++plan->FunctionCount;
		}
		std::shared_ptr<const Impl::PreparedPlan> immutable = std::move(plan);
		auto active = std::make_unique<Impl::ActiveState>();
		active->Plan = immutable;
		const std::size_t reserveHint = snapshot->Objects.size()
			<= TypeSnapshotStore::kMaxTotalMembers - immutable->Types.size()
				? snapshot->Objects.size() + immutable->Types.size()
				: TypeSnapshotStore::kMaxTotalMembers;
		active->EvidenceRecords.reserve(reserveHint);
		{
			std::lock_guard lock(m_Impl->OwnershipMutex);
			if (m_Impl->ActiveFlag.load(std::memory_order_acquire)
				|| m_Impl->Active || m_Impl->PreparedActive
				|| m_Impl->Prepared.load(std::memory_order_acquire)
				|| m_Impl->Engine.Snapshots().Current() != snapshot
				|| m_Impl->Engine.Reflection() != reflection)
			{
				m_Impl->PreparationError.store(
					TypeCandidatePreparationError::Busy,
					std::memory_order_release);
				return {.Error = TypeCandidatePreparationError::Busy};
			}
			m_Impl->PreparedActive = std::move(active);
			m_Impl->Prepared.store(immutable, std::memory_order_release);
		}
		m_Impl->PreparationError.store(
			TypeCandidatePreparationError::None,
			std::memory_order_release);
		m_Impl->SourceError.store(TypeSnapshotSourceError::None, std::memory_order_release);
		m_Impl->PreparedGeneration.store(snapshot->Generation, std::memory_order_release);
		m_Impl->PreparedFingerprint.store(
			reflection->Layout->Fingerprint(),
			std::memory_order_release);
		m_Impl->PreparedTypeCount.store(immutable->Types.size(), std::memory_order_release);
		m_Impl->PreparedFunctionCount.store(
			immutable->FunctionCount,
			std::memory_order_release);
		return {
			.SnapshotGeneration = snapshot->Generation,
			.ReflectionLayoutFingerprint = reflection->Layout->Fingerprint(),
			.TypeCount = immutable->Types.size(),
			.FunctionCount = immutable->FunctionCount
		};
	}
	catch (const std::bad_alloc&)
	{
		m_Impl->PreparationError.store(
			TypeCandidatePreparationError::AllocationFailed,
			std::memory_order_release);
		return {.Error = TypeCandidatePreparationError::AllocationFailed};
	}
	catch (...)
	{
		m_Impl->PreparationError.store(
			TypeCandidatePreparationError::SnapshotInvalid,
			std::memory_order_release);
		return {.Error = TypeCandidatePreparationError::SnapshotInvalid};
	}
}

bool ObjectSnapshotTypeCandidateSource::ReleasePreparedPlan() noexcept
{
	if (!m_Impl || m_Impl->ActiveFlag.load(std::memory_order_acquire))
		return false;
	std::unique_ptr<Impl::ActiveState> active;
	std::unique_ptr<Impl::ActiveState> preparedActive;
	std::shared_ptr<const Impl::PreparedPlan> prepared;
	{
		std::lock_guard lock(m_Impl->OwnershipMutex);
		if (m_Impl->ActiveFlag.load(std::memory_order_acquire))
			return false;
		active = std::move(m_Impl->Active);
		preparedActive = std::move(m_Impl->PreparedActive);
		prepared = m_Impl->Prepared.exchange({}, std::memory_order_acq_rel);
	}
	return true;
}

ObjectSnapshotTypeSourceDiagnostics
ObjectSnapshotTypeCandidateSource::Diagnostics() const noexcept
{
	if (!m_Impl)
		return {};
	return {
		.PreparationError = m_Impl->PreparationError.load(std::memory_order_acquire),
		.SourceError = m_Impl->SourceError.load(std::memory_order_acquire),
		.PreparedSnapshotGeneration =
			m_Impl->PreparedGeneration.load(std::memory_order_acquire),
		.ReflectionLayoutFingerprint =
			m_Impl->PreparedFingerprint.load(std::memory_order_acquire),
		.PreparedTypes = m_Impl->PreparedTypeCount.load(std::memory_order_acquire),
		.PreparedFunctions =
			m_Impl->PreparedFunctionCount.load(std::memory_order_acquire),
		.CapturePhase = m_Impl->DiagnosticPhase.load(std::memory_order_acquire),
		.SourceSteps = m_Impl->DiagnosticSourceSteps.load(std::memory_order_acquire),
		.ValidationSteps =
			m_Impl->DiagnosticValidationSteps.load(std::memory_order_acquire),
		.CapturedEvidence = m_Impl->DiagnosticEvidence.load(std::memory_order_acquire),
		.Active = m_Impl->ActiveFlag.load(std::memory_order_acquire)
	};
}

std::uint64_t ObjectSnapshotTypeCandidateSource::ContextGeneration() const noexcept
{
	return m_Impl && m_Impl->Context ? m_Impl->Context->Generation() : 0;
}

bool ObjectSnapshotTypeCandidateSource::IsConfigured() const noexcept
{
	return m_Impl && m_Impl->BaseConfigured()
		&& static_cast<bool>(m_Impl->Prepared.load(std::memory_order_acquire));
}

bool ObjectSnapshotTypeCandidateSource::IsCurrentExecutionThreadValid() const noexcept
{
	return IsConfigured() && m_Impl->Engine.IsCurrentExecutionThreadValid();
}

TypeSnapshotSourceBeginResult ObjectSnapshotTypeCandidateSource::Begin() noexcept
{
	if (!m_Impl || !IsConfigured())
		return {.Error = TypeSnapshotSourceError::InvalidConfiguration};
	bool expected = false;
	if (!m_Impl->ActiveFlag.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return {.Error = TypeSnapshotSourceError::ContractViolation};
	}
	try
	{
		std::shared_ptr<const Impl::PreparedPlan> plan =
			m_Impl->Prepared.load(std::memory_order_acquire);
		{
			std::lock_guard lock(m_Impl->OwnershipMutex);
			if (m_Impl->Active || !m_Impl->PreparedActive
				|| m_Impl->PreparedActive->Plan != plan)
			{
				m_Impl->ActiveFlag.store(false, std::memory_order_release);
				return {.Error = TypeSnapshotSourceError::ContractViolation};
			}
			m_Impl->Active = std::move(m_Impl->PreparedActive);
		}
		if (!plan || !m_Impl->DependenciesCurrent(*plan)
			|| !IsCurrentExecutionThreadValid())
		{
			m_Impl->ActiveFlag.store(false, std::memory_order_release);
			m_Impl->SourceError.store(
				TypeSnapshotSourceError::DependencyChanged,
				std::memory_order_release);
			return {.Error = TypeSnapshotSourceError::DependencyChanged};
		}
		m_Impl->DiagnosticPhase.store(0, std::memory_order_release);
		m_Impl->DiagnosticSourceSteps.store(0, std::memory_order_release);
		m_Impl->DiagnosticValidationSteps.store(0, std::memory_order_release);
		m_Impl->DiagnosticEvidence.store(0, std::memory_order_release);
		m_Impl->SourceError.store(TypeSnapshotSourceError::None, std::memory_order_release);
		return {
			.Source = std::string(kSourceName),
			.ObjectSnapshot = plan->Snapshot,
			.Reflection = plan->Reflection,
			.ExpectedTypeCount = plan->Types.size(),
			.ExpectedFunctionCount = plan->FunctionCount
		};
	}
	catch (const std::bad_alloc&)
	{
		m_Impl->ActiveFlag.store(false, std::memory_order_release);
		return {.Error = TypeSnapshotSourceError::AllocationFailed};
	}
	catch (...)
	{
		m_Impl->ActiveFlag.store(false, std::memory_order_release);
		return {.Error = TypeSnapshotSourceError::UnexpectedException};
	}
}

TypeSnapshotSourceStepResult ObjectSnapshotTypeCandidateSource::CaptureNext() noexcept
{
	if (!m_Impl || !m_Impl->Active
		|| !m_Impl->ActiveFlag.load(std::memory_order_acquire))
	{
		return {.Error = TypeSnapshotSourceError::InvalidConfiguration};
	}
	TypeSnapshotSourceStepResult result{.Progressed = true};
	try
	{
		if (!m_Impl->DependenciesCurrent(*m_Impl->Active->Plan))
		{
			m_Impl->SourceError.store(
				TypeSnapshotSourceError::DependencyChanged,
				std::memory_order_release);
			return {.Error = TypeSnapshotSourceError::DependencyChanged, .Progressed = true};
		}
		++m_Impl->Active->SourceSteps;
		m_Impl->DiagnosticSourceSteps.store(
			m_Impl->Active->SourceSteps,
			std::memory_order_release);
		m_Impl->DiagnosticPhase.store(
			static_cast<std::size_t>(m_Impl->Active->Phase),
			std::memory_order_release);
		TypeSnapshotSourceError error = TypeSnapshotSourceError::None;
		switch (m_Impl->Active->Phase)
		{
		case Impl::CapturePhase::TypeBegin:
			if (m_Impl->Active->TypeIndex >= m_Impl->Active->Plan->Types.size())
			{
				m_Impl->Active->Phase = Impl::CapturePhase::Complete;
				result.Complete = true;
			}
			else
			{
				error = m_Impl->CaptureType(result);
			}
			break;
		case Impl::CapturePhase::Fields:
			if (m_Impl->Active->CurrentField == 0)
			{
				m_Impl->Active->FunctionIndex = 0;
				m_Impl->Active->Phase = Impl::CapturePhase::FunctionBegin;
			}
			else
			{
				error = m_Impl->CaptureField(true, result);
			}
			break;
		case Impl::CapturePhase::FunctionBegin:
			if (!m_Impl->CurrentFunction())
				m_Impl->Active->Phase = Impl::CapturePhase::TypeEnd;
			else
				error = m_Impl->CaptureFunction(result);
			break;
		case Impl::CapturePhase::FunctionFields:
			if (m_Impl->Active->CurrentField == 0)
				m_Impl->Active->Phase = Impl::CapturePhase::FunctionEnd;
			else
				error = m_Impl->CaptureField(false, result);
			break;
		case Impl::CapturePhase::FunctionEnd:
			result.Record.emplace(TypeSnapshotFunctionEnd{});
			++m_Impl->Active->FunctionIndex;
			m_Impl->Active->Phase = Impl::CapturePhase::FunctionBegin;
			break;
		case Impl::CapturePhase::TypeEnd:
			result.Record.emplace(TypeSnapshotTypeEnd{});
			++m_Impl->Active->TypeIndex;
			m_Impl->Active->FunctionIndex = 0;
			if (m_Impl->Active->TypeIndex >= m_Impl->Active->Plan->Types.size())
			{
				m_Impl->Active->Phase = Impl::CapturePhase::Complete;
				result.Complete = true;
			}
			else
			{
				m_Impl->Active->Phase = Impl::CapturePhase::TypeBegin;
			}
			break;
		case Impl::CapturePhase::Complete:
			result.Complete = true;
			break;
		}
		m_Impl->DiagnosticEvidence.store(
			m_Impl->Active->EvidenceRecords.size(),
			std::memory_order_release);
		if (error != TypeSnapshotSourceError::None)
		{
			m_Impl->SourceError.store(error, std::memory_order_release);
			result.Error = error;
		}
		return result;
	}
	catch (const std::bad_alloc&)
	{
		m_Impl->SourceError.store(
			TypeSnapshotSourceError::AllocationFailed,
			std::memory_order_release);
		return {.Error = TypeSnapshotSourceError::AllocationFailed, .Progressed = true};
	}
	catch (...)
	{
		m_Impl->SourceError.store(
			TypeSnapshotSourceError::UnexpectedException,
			std::memory_order_release);
		return {.Error = TypeSnapshotSourceError::UnexpectedException, .Progressed = true};
	}
}

TypeSnapshotSourceError ObjectSnapshotTypeCandidateSource::BeginValidation() noexcept
{
	if (!m_Impl || !m_Impl->Active
		|| !m_Impl->ActiveFlag.load(std::memory_order_acquire)
		|| m_Impl->Active->Phase != Impl::CapturePhase::Complete
		|| m_Impl->Active->ValidationBegun)
	{
		return TypeSnapshotSourceError::ContractViolation;
	}
	if (!m_Impl->DependenciesCurrent(*m_Impl->Active->Plan))
	{
		m_Impl->SourceError.store(
			TypeSnapshotSourceError::DependencyChanged,
			std::memory_order_release);
		return TypeSnapshotSourceError::DependencyChanged;
	}
	m_Impl->Active->ValidationBegun = true;
	m_Impl->Active->ValidationCursor = 0;
	m_Impl->Active->EmptyValidationPending = m_Impl->Active->EvidenceRecords.empty();
	return TypeSnapshotSourceError::None;
}

TypeSnapshotSourceValidationResult
ObjectSnapshotTypeCandidateSource::ValidateNext() noexcept
{
	if (!m_Impl || !m_Impl->Active || !m_Impl->Active->ValidationBegun
		|| !m_Impl->ActiveFlag.load(std::memory_order_acquire))
	{
		return {.Error = TypeSnapshotSourceError::ContractViolation};
	}
	try
	{
		++m_Impl->Active->ValidationSteps;
		m_Impl->DiagnosticValidationSteps.store(
			m_Impl->Active->ValidationSteps,
			std::memory_order_release);
		if (!m_Impl->DependenciesCurrent(*m_Impl->Active->Plan))
		{
			m_Impl->SourceError.store(
				TypeSnapshotSourceError::DependencyChanged,
				std::memory_order_release);
			return {
				.Error = TypeSnapshotSourceError::DependencyChanged,
				.Progressed = true
			};
		}
		if (m_Impl->Active->EmptyValidationPending)
		{
			m_Impl->Active->EmptyValidationPending = false;
			return {.Progressed = true, .Complete = true};
		}
		if (m_Impl->Active->ValidationCursor
			>= m_Impl->Active->EvidenceRecords.size())
		{
			return {.Error = TypeSnapshotSourceError::ContractViolation, .Progressed = true};
		}
		const TypeSnapshotSourceError error = m_Impl->ValidateEvidence(
			m_Impl->Active->EvidenceRecords[m_Impl->Active->ValidationCursor]);
		if (error != TypeSnapshotSourceError::None)
		{
			m_Impl->SourceError.store(error, std::memory_order_release);
			return {.Error = error, .Progressed = true};
		}
		++m_Impl->Active->ValidationCursor;
		return {
			.Progressed = true,
			.Complete = m_Impl->Active->ValidationCursor
				== m_Impl->Active->EvidenceRecords.size()
		};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = TypeSnapshotSourceError::AllocationFailed, .Progressed = true};
	}
	catch (...)
	{
		return {.Error = TypeSnapshotSourceError::UnexpectedException, .Progressed = true};
	}
}

bool ObjectSnapshotTypeCandidateSource::ValidateDependencies() noexcept
{
	if (!m_Impl || !m_Impl->Active
		|| !m_Impl->ActiveFlag.load(std::memory_order_acquire))
	{
		return false;
	}
	const bool current = m_Impl->DependenciesCurrent(*m_Impl->Active->Plan);
	if (!current)
	{
		m_Impl->SourceError.store(
			TypeSnapshotSourceError::DependencyChanged,
			std::memory_order_release);
	}
	return current;
}

void ObjectSnapshotTypeCandidateSource::Cancel() noexcept
{
	if (m_Impl)
		m_Impl->ActiveFlag.store(false, std::memory_order_release);
}

} // namespace UExplorer::Runtime
