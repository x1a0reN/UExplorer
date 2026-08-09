#include "ObjectSnapshotReflectionCandidateSource.h"

#include "EngineFacade.h"
#include "SafeMemory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::string_view kSourceName =
	"object_snapshot_safe_memory_semantic_scan_v1";
constexpr std::int32_t kMaximumScanBytes = 0x200;
// One outer offset per source step keeps nested name/class semantic checks
// preemptible at the scheduler boundary.
constexpr std::size_t kOffsetCandidatesPerStep = 1;
constexpr std::size_t kFieldNodesPerStep = 8;
constexpr std::size_t kMaximumFieldChainDepth = 512;

constexpr std::string_view kStructClass = "/Script/CoreUObject.Struct";
constexpr std::string_view kFieldClass = "/Script/CoreUObject.Field";
constexpr std::string_view kClassClass = "/Script/CoreUObject.Class";
constexpr std::string_view kGuid = "/Script/CoreUObject.Guid";
constexpr std::string_view kColor = "/Script/CoreUObject.Color";
constexpr std::string_view kVector = "/Script/CoreUObject.Vector";
constexpr std::string_view kTwoVectors = "/Script/CoreUObject.TwoVectors";
constexpr std::string_view kEngine = "/Script/Engine.Engine";
constexpr std::string_view kPlayerController = "/Script/Engine.PlayerController";
constexpr std::string_view kCollisionResponseContainer =
	"/Script/Engine.CollisionResponseContainer";
constexpr std::string_view kController = "/Script/Engine.Controller";
constexpr std::string_view kPlayerState = "/Script/Engine.PlayerState";
constexpr std::string_view kPawn = "/Script/Engine.Pawn";
constexpr std::string_view kGameViewportClient = "/Script/Engine.GameViewportClient";
constexpr std::string_view kUserDefinedEnum = "/Script/Engine.UserDefinedEnum";
constexpr std::string_view kLevelCollection = "/Script/Engine.LevelCollection";
constexpr std::string_view kActorComponent = "/Script/Engine.ActorComponent";
constexpr std::string_view kDebugDisplayProperty =
	"/Script/Engine.DebugDisplayProperty";
constexpr std::string_view kLevel = "/Script/Engine.Level";
constexpr std::string_view kCollisionResponseEnum =
	"/Script/Engine.ECollisionResponse";
constexpr std::string_view kComponentCreationMethodEnum =
	"/Script/Engine.EComponentCreationMethod";
constexpr std::string_view kAutoPossessAiEnum = "/Script/Engine.EAutoPossessAI";

struct RequiredObjectSpec
{
	std::string_view Path;
	EngineObjectKind Kind = EngineObjectKind::Object;
};

constexpr std::array kRequiredObjects{
	RequiredObjectSpec{kStructClass, EngineObjectKind::Class},
	RequiredObjectSpec{kFieldClass, EngineObjectKind::Class},
	RequiredObjectSpec{kClassClass, EngineObjectKind::Class},
	RequiredObjectSpec{kGuid, EngineObjectKind::Struct},
	RequiredObjectSpec{kColor, EngineObjectKind::Struct},
	RequiredObjectSpec{kVector, EngineObjectKind::Struct},
	RequiredObjectSpec{kTwoVectors, EngineObjectKind::Struct},
	RequiredObjectSpec{kEngine, EngineObjectKind::Class},
	RequiredObjectSpec{kPlayerController, EngineObjectKind::Class},
	RequiredObjectSpec{kCollisionResponseContainer, EngineObjectKind::Struct},
	RequiredObjectSpec{kController, EngineObjectKind::Class},
	RequiredObjectSpec{kPlayerState, EngineObjectKind::Class},
	RequiredObjectSpec{kPawn, EngineObjectKind::Class},
	RequiredObjectSpec{kGameViewportClient, EngineObjectKind::Class},
	RequiredObjectSpec{kUserDefinedEnum, EngineObjectKind::Class},
	RequiredObjectSpec{kLevelCollection, EngineObjectKind::Struct},
	RequiredObjectSpec{kActorComponent, EngineObjectKind::Class},
	RequiredObjectSpec{kDebugDisplayProperty, EngineObjectKind::Struct},
	RequiredObjectSpec{kLevel, EngineObjectKind::Class},
	RequiredObjectSpec{kCollisionResponseEnum, EngineObjectKind::Enum},
	RequiredObjectSpec{kComponentCreationMethodEnum, EngineObjectKind::Enum},
	RequiredObjectSpec{kAutoPossessAiEnum, EngineObjectKind::Enum}
};

enum class PropertyKey : std::size_t
{
	GuidA,
	GuidC,
	GuidD,
	ColorR,
	ColorB,
	ColorG,
	EngineNativeBool,
	PlayerControllerNativeBool,
	GameTraceChannel1,
	GameTraceChannel2,
	ControllerPlayerState,
	ControllerPawn,
	TwoVectorsV1,
	TwoVectorsV2,
	DebugProperties,
	DisplayNameMap,
	LevelCollectionLevels,
	CreationMethod,
	AutoPossessAi,
	Count
};

constexpr std::size_t kPropertyCount = static_cast<std::size_t>(PropertyKey::Count);

struct TargetPropertySpec
{
	PropertyKey Key = PropertyKey::GuidA;
	std::string_view Name;
	std::string_view AlternateName;
	std::uint64_t RequiredCastFlag = 0;
};

struct OwnerPropertySpec
{
	std::string_view OwnerPath;
	std::span<const TargetPropertySpec> Targets;
};

enum class ClassCastFlag : std::uint64_t
{
	Field = 0x0000000000000001,
	ByteProperty = 0x0000000000000040,
	IntProperty = 0x0000000000000080,
	NameProperty = 0x0000000000002000,
	Property = 0x0000000000008000,
	ObjectProperty = 0x0000000000010000,
	BoolProperty = 0x0000000000020000,
	StructProperty = 0x0000000000100000,
	ArrayProperty = 0x0000000000200000,
	NumericProperty = 0x0000000001000000,
	TextProperty = 0x0000000040000000,
	MapProperty = 0x0000400000000000,
	SetProperty = 0x0000800000000000,
	EnumProperty = 0x0001000000000000
};

enum class PropertyFlag : std::uint64_t
{
	Edit = 0x0000000000000001,
	BlueprintVisible = 0x0000000000000004,
	ZeroConstructor = 0x0000000000000200,
	SaveGame = 0x0000000001000000,
	IsPlainOldData = 0x0000000040000000,
	NoDestructor = 0x0000001000000000,
	HasGetValueTypeHash = 0x0008000000000000,
	NativeAccessSpecifierPublic = 0x0010000000000000
};

template<typename... Flags>
constexpr std::uint64_t Mask(const Flags... flags) noexcept
{
	return (std::uint64_t{0} | ... | static_cast<std::uint64_t>(flags));
}

constexpr std::array kGuidTargets{
	TargetPropertySpec{PropertyKey::GuidA, "A", {}, Mask(ClassCastFlag::IntProperty)},
	TargetPropertySpec{PropertyKey::GuidC, "C", {}, Mask(ClassCastFlag::IntProperty)},
	TargetPropertySpec{PropertyKey::GuidD, "D", {}, Mask(ClassCastFlag::IntProperty)}
};
constexpr std::array kColorTargets{
	TargetPropertySpec{PropertyKey::ColorR, "R", "r", Mask(ClassCastFlag::ByteProperty)},
	TargetPropertySpec{PropertyKey::ColorB, "B", "b", Mask(ClassCastFlag::ByteProperty)},
	TargetPropertySpec{PropertyKey::ColorG, "G", "g", Mask(ClassCastFlag::ByteProperty)}
};
constexpr std::array kEngineTargets{
	TargetPropertySpec{PropertyKey::EngineNativeBool, "bIsOverridingSelectedColor", {}, Mask(ClassCastFlag::BoolProperty)}
};
constexpr std::array kPlayerControllerTargets{
	TargetPropertySpec{PropertyKey::PlayerControllerNativeBool, "bAutoManageActiveCameraTarget", {}, Mask(ClassCastFlag::BoolProperty)}
};
constexpr std::array kCollisionTargets{
	TargetPropertySpec{PropertyKey::GameTraceChannel1, "GameTraceChannel1", {}, Mask(ClassCastFlag::ByteProperty)},
	TargetPropertySpec{PropertyKey::GameTraceChannel2, "GameTraceChannel2", {}, Mask(ClassCastFlag::ByteProperty)}
};
constexpr std::array kControllerTargets{
	TargetPropertySpec{PropertyKey::ControllerPlayerState, "PlayerState", {}, Mask(ClassCastFlag::ObjectProperty)},
	TargetPropertySpec{PropertyKey::ControllerPawn, "Pawn", {}, Mask(ClassCastFlag::ObjectProperty)}
};
constexpr std::array kTwoVectorsTargets{
	TargetPropertySpec{PropertyKey::TwoVectorsV1, "v1", {}, Mask(ClassCastFlag::StructProperty)},
	TargetPropertySpec{PropertyKey::TwoVectorsV2, "v2", {}, Mask(ClassCastFlag::StructProperty)}
};
constexpr std::array kViewportTargets{
	TargetPropertySpec{PropertyKey::DebugProperties, "DebugProperties", {}, Mask(ClassCastFlag::ArrayProperty)}
};
constexpr std::array kUserDefinedEnumTargets{
	TargetPropertySpec{PropertyKey::DisplayNameMap, "DisplayNameMap", {}, Mask(ClassCastFlag::MapProperty)}
};
constexpr std::array kLevelCollectionTargets{
	TargetPropertySpec{PropertyKey::LevelCollectionLevels, "Levels", {}, Mask(ClassCastFlag::SetProperty)}
};
constexpr std::array kActorComponentTargets{
	TargetPropertySpec{PropertyKey::CreationMethod, "CreationMethod", {}, Mask(ClassCastFlag::EnumProperty)}
};
constexpr std::array kPawnTargets{
	TargetPropertySpec{PropertyKey::AutoPossessAi, "AutoPossessAI", {}, Mask(ClassCastFlag::EnumProperty)}
};

constexpr std::array kOwnerProperties{
	OwnerPropertySpec{kGuid, kGuidTargets},
	OwnerPropertySpec{kColor, kColorTargets},
	OwnerPropertySpec{kEngine, kEngineTargets},
	OwnerPropertySpec{kPlayerController, kPlayerControllerTargets},
	OwnerPropertySpec{kCollisionResponseContainer, kCollisionTargets},
	OwnerPropertySpec{kController, kControllerTargets},
	OwnerPropertySpec{kTwoVectors, kTwoVectorsTargets},
	OwnerPropertySpec{kGameViewportClient, kViewportTargets},
	OwnerPropertySpec{kUserDefinedEnum, kUserDefinedEnumTargets},
	OwnerPropertySpec{kLevelCollection, kLevelCollectionTargets},
	OwnerPropertySpec{kActorComponent, kActorComponentTargets},
	OwnerPropertySpec{kPawn, kPawnTargets}
};

constexpr std::array kFPropertyFields{
	ReflectionField::StructSuper,
	ReflectionField::StructChildProperties,
	ReflectionField::StructPropertiesSize,
	ReflectionField::StructMinAlignment,
	ReflectionField::FFieldClass,
	ReflectionField::FFieldNext,
	ReflectionField::FFieldName,
	ReflectionField::FFieldClassCastFlags,
	ReflectionField::PropertyArrayDim,
	ReflectionField::PropertyElementSize,
	ReflectionField::PropertyFlags,
	ReflectionField::PropertyOffset,
	ReflectionField::BoolFieldSize,
	ReflectionField::BoolByteOffset,
	ReflectionField::BoolByteMask,
	ReflectionField::BoolFieldMask,
	ReflectionField::BytePropertyEnum,
	ReflectionField::ObjectPropertyClass,
	ReflectionField::StructPropertyStruct,
	ReflectionField::ArrayPropertyInner,
	ReflectionField::MapPropertyKey,
	ReflectionField::MapPropertyValue,
	ReflectionField::SetPropertyElement,
	ReflectionField::EnumPropertyUnderlying,
	ReflectionField::EnumPropertyEnum
};

constexpr std::array kUPropertyFields{
	ReflectionField::StructSuper,
	ReflectionField::StructChildren,
	ReflectionField::StructPropertiesSize,
	ReflectionField::StructMinAlignment,
	ReflectionField::UFieldNext,
	ReflectionField::PropertyArrayDim,
	ReflectionField::PropertyElementSize,
	ReflectionField::PropertyFlags,
	ReflectionField::PropertyOffset,
	ReflectionField::BoolFieldSize,
	ReflectionField::BoolByteOffset,
	ReflectionField::BoolByteMask,
	ReflectionField::BoolFieldMask,
	ReflectionField::BytePropertyEnum,
	ReflectionField::ObjectPropertyClass,
	ReflectionField::StructPropertyStruct,
	ReflectionField::ArrayPropertyInner,
	ReflectionField::MapPropertyKey,
	ReflectionField::MapPropertyValue,
	ReflectionField::SetPropertyElement,
	ReflectionField::EnumPropertyUnderlying,
	ReflectionField::EnumPropertyEnum
};

std::span<const ReflectionField> RequiredFields(
	const ReflectionPropertySystem system) noexcept
{
	return system == ReflectionPropertySystem::FProperty
		? std::span<const ReflectionField>(kFPropertyFields)
		: std::span<const ReflectionField>(kUPropertyFields);
}

std::string MemberPath(
	const std::string_view owner,
	const std::string_view member)
{
	std::string path(owner);
	path.push_back('.');
	path.append(member);
	return path;
}

bool TryAddAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	const std::size_t size,
	std::uintptr_t& address) noexcept
{
	address = 0;
	if (base == 0 || offset < 0
		|| static_cast<std::uintptr_t>(offset)
			> (std::numeric_limits<std::uintptr_t>::max)() - base)
	{
		return false;
	}
	address = base + static_cast<std::uintptr_t>(offset);
	std::uintptr_t ignored = 0;
	return CheckedAddressRange(address, size, ignored);
}

template<typename T>
bool ReadStable(
	const std::uintptr_t base,
	const std::int32_t offset,
	T& output) noexcept
{
	std::uintptr_t address = 0;
	if (!TryAddAddress(base, offset, sizeof(T), address))
		return false;
	T first{};
	T second{};
	return ReadValue(address, first).Ok()
		&& ReadValue(address, second).Ok()
		&& std::memcmp(&first, &second, sizeof(T)) == 0
		&& (output = first, true);
}

bool HasCastFlag(const std::uint64_t flags, const std::uint64_t required) noexcept
{
	return required != 0
		&& (flags & Mask(ClassCastFlag::Property)) != 0
		&& (flags & required) == required;
}

enum class ScanOutcome : std::uint8_t
{
	Progress,
	Found,
	Missing,
	Ambiguous
};

struct OffsetScanState
{
	std::int32_t Next = 0;
	std::int32_t End = 0;
	std::int32_t Alignment = 1;
	std::int32_t Selected = -1;
	std::size_t Matches = 0;
	bool Initialized = false;

	void Reset() noexcept
	{
		*this = {};
	}
};

template<typename Predicate>
ScanOutcome AdvanceOffsetScan(
	OffsetScanState& state,
	const std::int32_t begin,
	const std::int32_t end,
	const std::int32_t alignment,
	Predicate&& predicate)
{
	if (!state.Initialized)
	{
		state.Next = begin;
		state.End = end;
		state.Alignment = alignment;
		state.Initialized = true;
	}
	std::size_t probed = 0;
	while (state.Next <= state.End && probed < kOffsetCandidatesPerStep)
	{
		const std::int32_t offset = state.Next;
		state.Next += state.Alignment;
		++probed;
		if (predicate(offset))
		{
			state.Selected = offset;
			++state.Matches;
		}
	}
	if (state.Next <= state.End)
		return ScanOutcome::Progress;
	if (state.Matches == 0)
		return ScanOutcome::Missing;
	return state.Matches == 1 ? ScanOutcome::Found : ScanOutcome::Ambiguous;
}

ReflectionCandidateSourceError ScanError(const ScanOutcome outcome) noexcept
{
	return outcome == ScanOutcome::Ambiguous
		? ReflectionCandidateSourceError::EvidenceAmbiguous
		: ReflectionCandidateSourceError::EvidenceUnavailable;
}

std::int32_t AlignUp(const std::int32_t value, const std::int32_t alignment) noexcept
{
	if (value < 0 || alignment <= 0)
		return -1;
	const std::int64_t result =
		(static_cast<std::int64_t>(value) + alignment - 1) / alignment * alignment;
	return result > (std::numeric_limits<std::int32_t>::max)()
		? -1
		: static_cast<std::int32_t>(result);
}

} // namespace

const char* ToString(const ReflectionCandidatePreparationError error) noexcept
{
	switch (error)
	{
	case ReflectionCandidatePreparationError::None: return "NONE";
	case ReflectionCandidatePreparationError::Busy: return "REFLECTION_PREPARE_BUSY";
	case ReflectionCandidatePreparationError::InvalidConfiguration:
		return "REFLECTION_PREPARE_INVALID";
	case ReflectionCandidatePreparationError::SnapshotUnavailable:
		return "REFLECTION_PREPARE_SNAPSHOT_UNAVAILABLE";
	case ReflectionCandidatePreparationError::SnapshotInvalid:
		return "REFLECTION_PREPARE_SNAPSHOT_INVALID";
	case ReflectionCandidatePreparationError::RequiredObjectMissing:
		return "REFLECTION_PREPARE_OBJECT_MISSING";
	case ReflectionCandidatePreparationError::RequiredObjectAmbiguous:
		return "REFLECTION_PREPARE_OBJECT_AMBIGUOUS";
	case ReflectionCandidatePreparationError::PropertySystemAmbiguous:
		return "REFLECTION_PREPARE_PROPERTY_SYSTEM_AMBIGUOUS";
	case ReflectionCandidatePreparationError::PropertySystemMismatch:
		return "REFLECTION_PREPARE_PROPERTY_SYSTEM_MISMATCH";
	case ReflectionCandidatePreparationError::AllocationFailed:
		return "REFLECTION_PREPARE_ALLOCATION_FAILED";
	}
	return "REFLECTION_PREPARE_UNKNOWN";
}

class ObjectSnapshotReflectionCandidateSource::Impl final
{
public:
	Impl(std::shared_ptr<const EngineContext> context, EngineFacade& engine)
		: Context(std::move(context)), Engine(engine)
	{
		ObjectIndexOffset = RequiredOffset("uobject.index");
		ObjectClassOffset = RequiredOffset("uobject.class");
		ClassCastFlagsOffset = RequiredOffset("uclass.cast_flags");
	}

	struct PreparedPlan final
	{
		std::shared_ptr<const EngineSnapshot> Snapshot;
		ReflectionPropertySystem PropertySystem = ReflectionPropertySystem::Unavailable;
		std::map<std::string, EngineSnapshotObject, std::less<>> Objects;
		std::array<std::optional<EngineSnapshotObject>, kPropertyCount> UProperties;
	};

	enum class DiscoveryPhase : std::size_t
	{
		StructPropertiesSize,
		StructMinAlignment,
		StructSuper,
		StructChildren,
		FieldClass,
		FieldNext,
		ResolveProperties,
		PropertyArrayDim,
		PropertyElementSize,
		PropertyFlags,
		PropertyOffset,
		BoolLayout,
		ByteProperty,
		ObjectProperty,
		StructProperty,
		ArrayProperty,
		MapPropertyKey,
		MapPropertyValue,
		SetProperty,
		EnumPropertyUnderlying,
		EnumPropertyEnum,
		ValidateRelations,
		Emit
	};

	struct ActiveState final
	{
		std::shared_ptr<const PreparedPlan> Plan;
		DiscoveryPhase Phase = DiscoveryPhase::StructPropertiesSize;
		OffsetScanState Scan;
		std::size_t SourceSteps = 0;
		std::size_t EmissionIndex = 0;

		std::int32_t StructPropertiesSize = -1;
		std::int32_t StructMinAlignment = -1;
		std::int32_t StructSuper = -1;
		std::int32_t StructChildren = -1;
		std::int32_t StructContainerSize = -1;
		std::uintptr_t GuidHead = 0;
		std::uintptr_t ColorHead = 0;
		std::string GuidHeadName;
		std::string ColorHeadName;

		std::int32_t FieldClass = -1;
		std::int32_t FieldNext = -1;
		std::int32_t FieldName = -1;
		std::int32_t FieldClassCastFlags = -1;
		std::uintptr_t GuidFieldClass = 0;
		std::uintptr_t ColorFieldClass = 0;
		std::uintptr_t GuidNext = 0;
		std::uintptr_t ColorNext = 0;
		std::string GuidNextName;
		std::string ColorNextName;

		std::array<std::uintptr_t, kPropertyCount> Properties{};
		std::size_t ResolutionOwner = 0;
		std::uintptr_t ResolutionCurrent = 0;
		std::size_t ResolutionDepth = 0;
		std::array<std::uintptr_t, kMaximumFieldChainDepth> ResolutionSeen{};

		std::int32_t PropertyArrayDim = -1;
		std::int32_t PropertyElementSize = -1;
		std::int32_t PropertyFlags = -1;
		std::int32_t PropertyOffset = -1;
		std::array<std::uint64_t, 2> PropertyFlagValues{};
		std::int32_t PropertyContainerSize = -1;
		std::int32_t FieldContainerSize = -1;

		std::int32_t BoolBase = -1;
		std::int32_t BytePropertyEnum = -1;
		std::int32_t ObjectPropertyClass = -1;
		std::int32_t StructPropertyStruct = -1;
		std::int32_t ArrayPropertyInner = -1;
		std::int32_t MapPropertyKey = -1;
		std::int32_t MapPropertyValue = -1;
		std::int32_t SetPropertyElement = -1;
		std::int32_t EnumPropertyUnderlying = -1;
		std::int32_t EnumPropertyEnum = -1;
		std::uintptr_t ArrayInner = 0;
		std::uintptr_t MapKey = 0;
		std::uintptr_t MapValue = 0;
		std::uintptr_t SetElement = 0;
		std::array<std::uintptr_t, 2> EnumUnderlying{};
	};

	std::shared_ptr<const EngineContext> Context;
	EngineFacade& Engine;
	std::atomic<std::shared_ptr<const PreparedPlan>> Prepared;
	std::unique_ptr<ActiveState> Active;
	std::atomic<bool> ActiveFlag{false};
	std::atomic<ReflectionCandidatePreparationError> PreparationError{
		ReflectionCandidatePreparationError::None};
	std::atomic<ReflectionCandidateSourceError> SourceError{
		ReflectionCandidateSourceError::None};
	std::atomic<std::uint64_t> PreparedGeneration{0};
	std::atomic<ReflectionPropertySystem> PreparedSystem{
		ReflectionPropertySystem::Unavailable};
	std::atomic<std::size_t> DiagnosticPhase{0};
	std::atomic<std::size_t> DiagnosticSourceSteps{0};
	std::atomic<std::size_t> EmittedFields{0};
	std::int32_t ObjectIndexOffset = -1;
	std::int32_t ObjectClassOffset = -1;
	std::int32_t ClassCastFlagsOffset = -1;

	std::int32_t RequiredOffset(const char* name) const noexcept
	{
		if (!Context)
			return -1;
		const OffsetReport* report = Context->FindOffset(name);
		return report && report->IsValidated()
			&& report->Value > 0
			&& report->Value <= (std::numeric_limits<std::int32_t>::max)()
			? static_cast<std::int32_t>(report->Value)
			: -1;
	}

	bool Configured() const noexcept
	{
		return Context
			&& Context.get() == Engine.Context().get()
			&& Context->Generation() != 0
			&& Engine.ContextGeneration() == Context->Generation()
			&& Engine.IsConfigured()
			&& Engine.Names().IsConfigured()
			&& ObjectIndexOffset > 0
			&& ObjectClassOffset > 0
			&& ClassCastFlagsOffset > 0;
	}

	const EngineSnapshotObject* FindObject(
		const PreparedPlan& plan,
		const std::string_view path) const noexcept
	{
		const auto it = plan.Objects.find(path);
		return it == plan.Objects.end() ? nullptr : &it->second;
	}

	bool ValidateObject(const EngineSnapshotObject& object) noexcept
	{
		return Engine.ValidateObjectHandle(object.Handle).Ok();
	}

	bool ValidateObject(
		const PreparedPlan& plan,
		const std::string_view path) noexcept
	{
		const EngineSnapshotObject* object = FindObject(plan, path);
		return object && ValidateObject(*object);
	}

	bool TryPropertyCastFlags(
		const std::uintptr_t address,
		std::uint64_t& flags) noexcept;
	bool TryDecodeFieldName(
		std::uintptr_t address,
		std::string& name);
	bool TryReadNextField(std::uintptr_t address, std::uintptr_t& next) noexcept;
	bool OwnerTargetsResolved(const OwnerPropertySpec& owner) const noexcept;
	ReflectionCandidateSourceError AdvancePropertyResolution();
	ReflectionCandidateSourceStepResult AdvanceDiscovery();
	ReflectionCandidateSourceStepResult AdvanceEmission();
	ReflectionCandidateEvidence BuildEvidence(ReflectionField field) const;
	ReflectionCandidateSourceError ValidateRelations() noexcept;

	template<typename Predicate, typename OnFound>
	ReflectionCandidateSourceStepResult ScanPhase(
		const std::int32_t begin,
		const std::int32_t end,
		const std::int32_t alignment,
		Predicate&& predicate,
		OnFound&& onFound)
	{
		const ScanOutcome outcome = AdvanceOffsetScan(
			Active->Scan,
			begin,
			end,
			alignment,
			std::forward<Predicate>(predicate));
		if (outcome == ScanOutcome::Progress)
			return {.Progressed = true};
		if (outcome != ScanOutcome::Found)
		{
			return {
				.Error = ScanError(outcome),
				.Progressed = true
			};
		}
		const std::int32_t selected = Active->Scan.Selected;
		Active->Scan.Reset();
		if (!std::forward<OnFound>(onFound)(selected))
		{
			return {
				.Error = ReflectionCandidateSourceError::MemoryUnavailable,
				.Progressed = true
			};
		}
		Active->Phase = static_cast<DiscoveryPhase>(
			static_cast<std::size_t>(Active->Phase) + 1);
		return {.Progressed = true};
	}
};

bool ObjectSnapshotReflectionCandidateSource::Impl::TryPropertyCastFlags(
	const std::uintptr_t address,
	std::uint64_t& flags) noexcept
{
	flags = 0;
	if (!Active || !Active->Plan || address == 0)
		return false;
	if (Active->Plan->PropertySystem == ReflectionPropertySystem::FProperty)
	{
		std::uintptr_t fieldClass = 0;
		return Active->FieldClass >= 0
			&& Active->FieldClassCastFlags >= 0
			&& ReadStable(address, Active->FieldClass, fieldClass)
			&& fieldClass != 0
			&& ReadStable(fieldClass, Active->FieldClassCastFlags, flags)
			&& HasCastFlag(flags, Mask(ClassCastFlag::Property));
	}

	std::int32_t objectIndex = -1;
	std::uintptr_t objectClass = 0;
	if (!ReadStable(address, ObjectIndexOffset, objectIndex)
		|| objectIndex < 0
		|| !ReadStable(address, ObjectClassOffset, objectClass)
		|| objectClass == 0)
	{
		return false;
	}
	const ObjectHandleResult issued = Engine.IssueObjectHandle(objectIndex);
	return issued.Ok()
		&& issued.Value.Address == address
		&& ReadStable(objectClass, ClassCastFlagsOffset, flags)
		&& HasCastFlag(flags, Mask(ClassCastFlag::Property));
}

bool ObjectSnapshotReflectionCandidateSource::Impl::TryDecodeFieldName(
	const std::uintptr_t address,
	std::string& name)
{
	name.clear();
	if (!Active
		|| !Active->Plan
		|| Active->Plan->PropertySystem != ReflectionPropertySystem::FProperty
		|| Active->FieldName < 0)
	{
		return false;
	}
	std::uintptr_t nameAddress = 0;
	if (!TryAddAddress(
		address,
		Active->FieldName,
		static_cast<std::size_t>(Context->NameProfile().FNameSize),
		nameAddress))
	{
		return false;
	}
	const EngineNameResult decoded = Engine.Names().DecodeFName(nameAddress);
	if (!decoded.Ok() || decoded.Value.empty()
		|| decoded.Value.size() > EngineSnapshotStore::kMaxNameBytes)
	{
		return false;
	}
	name = decoded.Value;
	return true;
}

bool ObjectSnapshotReflectionCandidateSource::Impl::TryReadNextField(
	const std::uintptr_t address,
	std::uintptr_t& next) noexcept
{
	next = 0;
	return Active && Active->FieldNext >= 0
		&& ReadStable(address, Active->FieldNext, next);
}

bool ObjectSnapshotReflectionCandidateSource::Impl::OwnerTargetsResolved(
	const OwnerPropertySpec& owner) const noexcept
{
	if (!Active)
		return false;
	for (const TargetPropertySpec& target : owner.Targets)
	{
		if (Active->Properties[static_cast<std::size_t>(target.Key)] == 0)
			return false;
	}
	return true;
}

ReflectionCandidateSourceError
ObjectSnapshotReflectionCandidateSource::Impl::AdvancePropertyResolution()
{
	if (!Active || !Active->Plan)
		return ReflectionCandidateSourceError::InvalidConfiguration;
	if (Active->ResolutionOwner >= kOwnerProperties.size())
	{
		Active->Phase = DiscoveryPhase::PropertyArrayDim;
		return ReflectionCandidateSourceError::None;
	}

	const OwnerPropertySpec& owner = kOwnerProperties[Active->ResolutionOwner];
	if (Active->Plan->PropertySystem == ReflectionPropertySystem::UProperty)
	{
		for (const TargetPropertySpec& target : owner.Targets)
		{
			const std::size_t key = static_cast<std::size_t>(target.Key);
			const std::optional<EngineSnapshotObject>& property =
				Active->Plan->UProperties[key];
			std::uint64_t flags = 0;
			if (!property
				|| !ValidateObject(*property)
				|| !TryPropertyCastFlags(property->Handle.Address, flags)
				|| !HasCastFlag(flags, target.RequiredCastFlag))
			{
				return ReflectionCandidateSourceError::EvidenceUnavailable;
			}
			Active->Properties[key] = property->Handle.Address;
		}
		++Active->ResolutionOwner;
		if (Active->ResolutionOwner == kOwnerProperties.size())
			Active->Phase = DiscoveryPhase::PropertyArrayDim;
		return ReflectionCandidateSourceError::None;
	}

	if (Active->ResolutionCurrent == 0 && Active->ResolutionDepth == 0)
	{
		const EngineSnapshotObject* ownerObject = FindObject(*Active->Plan, owner.OwnerPath);
		if (!ownerObject || !ValidateObject(*ownerObject)
			|| !ReadStable(
				ownerObject->Handle.Address,
				Active->StructChildren,
				Active->ResolutionCurrent)
			|| Active->ResolutionCurrent == 0)
		{
			return ReflectionCandidateSourceError::EvidenceUnavailable;
		}
	}

	std::size_t processed = 0;
	while (Active->ResolutionCurrent != 0 && processed < kFieldNodesPerStep)
	{
		if (Active->ResolutionDepth >= kMaximumFieldChainDepth)
			return ReflectionCandidateSourceError::EvidenceUnavailable;
		for (std::size_t index = 0; index < Active->ResolutionDepth; ++index)
		{
			if (Active->ResolutionSeen[index] == Active->ResolutionCurrent)
				return ReflectionCandidateSourceError::EvidenceAmbiguous;
		}
		Active->ResolutionSeen[Active->ResolutionDepth++] = Active->ResolutionCurrent;

		std::string fieldName;
		std::uint64_t fieldFlags = 0;
		if (!TryDecodeFieldName(Active->ResolutionCurrent, fieldName)
			|| !TryPropertyCastFlags(Active->ResolutionCurrent, fieldFlags))
		{
			return ReflectionCandidateSourceError::MemoryUnavailable;
		}
		for (const TargetPropertySpec& target : owner.Targets)
		{
			const bool nameMatches = fieldName == target.Name
				|| (!target.AlternateName.empty() && fieldName == target.AlternateName);
			if (!nameMatches)
				continue;
			const std::size_t key = static_cast<std::size_t>(target.Key);
			if (!HasCastFlag(fieldFlags, target.RequiredCastFlag)
				|| (Active->Properties[key] != 0
					&& Active->Properties[key] != Active->ResolutionCurrent))
			{
				return ReflectionCandidateSourceError::EvidenceAmbiguous;
			}
			Active->Properties[key] = Active->ResolutionCurrent;
		}

		std::uintptr_t next = 0;
		if (!TryReadNextField(Active->ResolutionCurrent, next))
			return ReflectionCandidateSourceError::MemoryUnavailable;
		Active->ResolutionCurrent = next;
		++processed;
		if (OwnerTargetsResolved(owner))
			break;
	}

	if (!OwnerTargetsResolved(owner))
	{
		return Active->ResolutionCurrent == 0
			? ReflectionCandidateSourceError::EvidenceUnavailable
			: ReflectionCandidateSourceError::None;
	}
	++Active->ResolutionOwner;
	Active->ResolutionCurrent = 0;
	Active->ResolutionDepth = 0;
	Active->ResolutionSeen.fill(0);
	if (Active->ResolutionOwner == kOwnerProperties.size())
		Active->Phase = DiscoveryPhase::PropertyArrayDim;
	return ReflectionCandidateSourceError::None;
}

ObjectSnapshotReflectionCandidateSource::ObjectSnapshotReflectionCandidateSource(
	std::shared_ptr<const EngineContext> context,
	EngineFacade& engine)
	: m_Impl(std::make_unique<Impl>(std::move(context), engine))
{
}

ObjectSnapshotReflectionCandidateSource::~ObjectSnapshotReflectionCandidateSource() = default;

ReflectionCandidatePreparationResult
ObjectSnapshotReflectionCandidateSource::Prepare() noexcept
{
	if (!m_Impl || !m_Impl->Configured())
	{
		if (m_Impl)
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::InvalidConfiguration,
				std::memory_order_release);
		}
		return {.Error = ReflectionCandidatePreparationError::InvalidConfiguration};
	}
	if (m_Impl->ActiveFlag.load(std::memory_order_acquire))
	{
		m_Impl->PreparationError.store(
			ReflectionCandidatePreparationError::Busy,
			std::memory_order_release);
		return {.Error = ReflectionCandidatePreparationError::Busy};
	}
	try
	{
		const std::shared_ptr<const EngineSnapshot> snapshot =
			m_Impl->Engine.Snapshots().Current();
		if (!snapshot)
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::SnapshotUnavailable,
				std::memory_order_release);
			return {.Error = ReflectionCandidatePreparationError::SnapshotUnavailable};
		}
		if (snapshot->ContextGeneration != m_Impl->Context->Generation()
			|| snapshot->Generation == 0
			|| snapshot->SessionId != m_Impl->Engine.SessionId())
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::SnapshotInvalid,
				std::memory_order_release);
			return {.Error = ReflectionCandidatePreparationError::SnapshotInvalid};
		}

		auto plan = std::make_shared<Impl::PreparedPlan>();
		plan->Snapshot = snapshot;
		for (const RequiredObjectSpec& required : kRequiredObjects)
		{
			const EngineSnapshotObject* selected = nullptr;
			for (const EngineSnapshotObject& object : snapshot->Objects)
			{
				if (object.FullPath != required.Path)
					continue;
				if (selected)
				{
					m_Impl->PreparationError.store(
						ReflectionCandidatePreparationError::RequiredObjectAmbiguous,
						std::memory_order_release);
					return {
						.Error = ReflectionCandidatePreparationError::RequiredObjectAmbiguous,
						.SnapshotGeneration = snapshot->Generation,
						.EvidencePath = std::string(required.Path)
					};
				}
				selected = &object;
			}
			if (!selected || selected->Kind != required.Kind)
			{
				m_Impl->PreparationError.store(
					ReflectionCandidatePreparationError::RequiredObjectMissing,
					std::memory_order_release);
				return {
					.Error = ReflectionCandidatePreparationError::RequiredObjectMissing,
					.SnapshotGeneration = snapshot->Generation,
					.EvidencePath = std::string(required.Path)
				};
			}
			plan->Objects.emplace(std::string(required.Path), *selected);
		}

		const auto findMember = [&](const TargetPropertySpec& target,
			const std::string_view ownerPath,
			std::optional<EngineSnapshotObject>& selected) {
			selected.reset();
			std::size_t matches = 0;
			for (const EngineSnapshotObject& object : snapshot->Objects)
			{
				const bool primary = object.FullPath == MemberPath(ownerPath, target.Name);
				const bool alternate = !target.AlternateName.empty()
					&& object.FullPath == MemberPath(ownerPath, target.AlternateName);
				if (!primary && !alternate)
					continue;
				selected = object;
				++matches;
			}
			return matches;
		};

		std::size_t sentinelPresent = 0;
		for (const auto [ownerIndex, targetIndex] : std::array{
			std::pair<std::size_t, std::size_t>{0, 0},
			std::pair<std::size_t, std::size_t>{0, 1},
			std::pair<std::size_t, std::size_t>{0, 2},
			std::pair<std::size_t, std::size_t>{1, 0}})
		{
			std::optional<EngineSnapshotObject> property;
			const TargetPropertySpec& target =
				kOwnerProperties[ownerIndex].Targets[targetIndex];
			const std::size_t matches = findMember(
				target,
				kOwnerProperties[ownerIndex].OwnerPath,
				property);
			if (matches > 1)
			{
				m_Impl->PreparationError.store(
					ReflectionCandidatePreparationError::RequiredObjectAmbiguous,
					std::memory_order_release);
				return {
					.Error = ReflectionCandidatePreparationError::RequiredObjectAmbiguous,
					.SnapshotGeneration = snapshot->Generation,
					.EvidencePath = MemberPath(
						kOwnerProperties[ownerIndex].OwnerPath,
						target.Name)
				};
			}
			if (matches == 1)
				++sentinelPresent;
		}
		if (sentinelPresent != 0 && sentinelPresent != 4)
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::PropertySystemAmbiguous,
				std::memory_order_release);
			return {
				.Error = ReflectionCandidatePreparationError::PropertySystemAmbiguous,
				.SnapshotGeneration = snapshot->Generation
			};
		}
		plan->PropertySystem = sentinelPresent == 4
			? ReflectionPropertySystem::UProperty
			: ReflectionPropertySystem::FProperty;
		const ReflectionPropertySystem expectedSystem =
			m_Impl->Context->Profile().UsesFProperty
				? ReflectionPropertySystem::FProperty
				: ReflectionPropertySystem::UProperty;
		if (plan->PropertySystem != expectedSystem)
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::PropertySystemMismatch,
				std::memory_order_release);
			return {
				.Error = ReflectionCandidatePreparationError::PropertySystemMismatch,
				.SnapshotGeneration = snapshot->Generation,
				.PropertySystem = plan->PropertySystem
			};
		}

		if (plan->PropertySystem == ReflectionPropertySystem::UProperty)
		{
			for (const OwnerPropertySpec& owner : kOwnerProperties)
			{
				for (const TargetPropertySpec& target : owner.Targets)
				{
					std::optional<EngineSnapshotObject> property;
					const std::size_t matches = findMember(target, owner.OwnerPath, property);
					if (matches != 1)
					{
						const ReflectionCandidatePreparationError error = matches == 0
							? ReflectionCandidatePreparationError::RequiredObjectMissing
							: ReflectionCandidatePreparationError::RequiredObjectAmbiguous;
						m_Impl->PreparationError.store(error, std::memory_order_release);
						return {
							.Error = error,
							.SnapshotGeneration = snapshot->Generation,
							.PropertySystem = plan->PropertySystem,
							.EvidencePath = MemberPath(owner.OwnerPath, target.Name)
						};
					}
					plan->UProperties[static_cast<std::size_t>(target.Key)] =
						std::move(property);
				}
			}
		}

		if (m_Impl->ActiveFlag.load(std::memory_order_acquire))
		{
			m_Impl->PreparationError.store(
				ReflectionCandidatePreparationError::Busy,
				std::memory_order_release);
			return {.Error = ReflectionCandidatePreparationError::Busy};
		}
		const ReflectionPropertySystem propertySystem = plan->PropertySystem;
		std::shared_ptr<const Impl::PreparedPlan> immutable = std::move(plan);
		m_Impl->Prepared.store(std::move(immutable), std::memory_order_release);
		m_Impl->PreparationError.store(
			ReflectionCandidatePreparationError::None,
			std::memory_order_release);
		m_Impl->SourceError.store(
			ReflectionCandidateSourceError::None,
			std::memory_order_release);
		m_Impl->PreparedGeneration.store(snapshot->Generation, std::memory_order_release);
		m_Impl->PreparedSystem.store(propertySystem, std::memory_order_release);
		return {
			.SnapshotGeneration = snapshot->Generation,
			.PropertySystem = propertySystem
		};
	}
	catch (const std::bad_alloc&)
	{
		m_Impl->PreparationError.store(
			ReflectionCandidatePreparationError::AllocationFailed,
			std::memory_order_release);
		return {.Error = ReflectionCandidatePreparationError::AllocationFailed};
	}
	catch (...)
	{
		m_Impl->PreparationError.store(
			ReflectionCandidatePreparationError::SnapshotInvalid,
			std::memory_order_release);
		return {.Error = ReflectionCandidatePreparationError::SnapshotInvalid};
	}
}

ObjectSnapshotReflectionSourceDiagnostics
ObjectSnapshotReflectionCandidateSource::Diagnostics() const noexcept
{
	if (!m_Impl)
		return {};
	return {
		.PreparationError = m_Impl->PreparationError.load(std::memory_order_acquire),
		.SourceError = m_Impl->SourceError.load(std::memory_order_acquire),
		.PreparedSnapshotGeneration =
			m_Impl->PreparedGeneration.load(std::memory_order_acquire),
		.PropertySystem = m_Impl->PreparedSystem.load(std::memory_order_acquire),
		.DiscoveryPhase = m_Impl->DiagnosticPhase.load(std::memory_order_acquire),
		.SourceSteps = m_Impl->DiagnosticSourceSteps.load(std::memory_order_acquire),
		.EmittedFields = m_Impl->EmittedFields.load(std::memory_order_acquire),
		.Active = m_Impl->ActiveFlag.load(std::memory_order_acquire)
	};
}

bool ObjectSnapshotReflectionCandidateSource::ReleasePreparedPlan() noexcept
{
	if (!m_Impl || m_Impl->ActiveFlag.load(std::memory_order_acquire))
		return false;
	m_Impl->Prepared.store({}, std::memory_order_release);
	return true;
}

std::uint64_t ObjectSnapshotReflectionCandidateSource::ContextGeneration() const noexcept
{
	return m_Impl && m_Impl->Context ? m_Impl->Context->Generation() : 0;
}

bool ObjectSnapshotReflectionCandidateSource::IsConfigured() const noexcept
{
	return m_Impl && m_Impl->Configured()
		&& static_cast<bool>(m_Impl->Prepared.load(std::memory_order_acquire));
}

bool ObjectSnapshotReflectionCandidateSource::IsCurrentExecutionThreadValid() const noexcept
{
	return m_Impl && IsConfigured() && m_Impl->Engine.IsCurrentExecutionThreadValid();
}

ReflectionCandidateSourceBeginResult
ObjectSnapshotReflectionCandidateSource::Begin() noexcept
{
	if (!m_Impl || !IsConfigured())
	{
		if (m_Impl)
		{
			m_Impl->SourceError.store(
				ReflectionCandidateSourceError::InvalidConfiguration,
				std::memory_order_release);
		}
		return {.Error = ReflectionCandidateSourceError::InvalidConfiguration};
	}
	bool expected = false;
	if (!m_Impl->ActiveFlag.compare_exchange_strong(
		expected,
		true,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		m_Impl->SourceError.store(
			ReflectionCandidateSourceError::ContractViolation,
			std::memory_order_release);
		return {.Error = ReflectionCandidateSourceError::ContractViolation};
	}
	try
	{
		const std::shared_ptr<const Impl::PreparedPlan> plan =
			m_Impl->Prepared.load(std::memory_order_acquire);
		if (!plan || m_Impl->Engine.Snapshots().Current() != plan->Snapshot
			|| !IsCurrentExecutionThreadValid())
		{
			m_Impl->ActiveFlag.store(false, std::memory_order_release);
			m_Impl->SourceError.store(
				ReflectionCandidateSourceError::DependencyChanged,
				std::memory_order_release);
			return {.Error = ReflectionCandidateSourceError::DependencyChanged};
		}
		m_Impl->Active = std::make_unique<Impl::ActiveState>();
		m_Impl->Active->Plan = plan;
		m_Impl->SourceError.store(
			ReflectionCandidateSourceError::None,
			std::memory_order_release);
		m_Impl->DiagnosticPhase.store(0, std::memory_order_release);
		m_Impl->DiagnosticSourceSteps.store(0, std::memory_order_release);
		m_Impl->EmittedFields.store(0, std::memory_order_release);
		return {
			.PropertySystem = plan->PropertySystem,
			.Source = std::string(kSourceName)
		};
	}
	catch (...)
	{
		m_Impl->Active.reset();
		m_Impl->ActiveFlag.store(false, std::memory_order_release);
		m_Impl->SourceError.store(
			ReflectionCandidateSourceError::UnexpectedException,
			std::memory_order_release);
		return {.Error = ReflectionCandidateSourceError::UnexpectedException};
	}
}

ReflectionCandidateSourceStepResult
ObjectSnapshotReflectionCandidateSource::CaptureNext() noexcept
{
	if (!m_Impl || !m_Impl->Active || !m_Impl->ActiveFlag.load(std::memory_order_acquire))
	{
		if (m_Impl)
		{
			m_Impl->SourceError.store(
				ReflectionCandidateSourceError::InvalidConfiguration,
				std::memory_order_release);
		}
		return {.Error = ReflectionCandidateSourceError::InvalidConfiguration};
	}
	try
	{
		++m_Impl->Active->SourceSteps;
		m_Impl->DiagnosticSourceSteps.store(
			m_Impl->Active->SourceSteps,
			std::memory_order_release);
		m_Impl->DiagnosticPhase.store(
			static_cast<std::size_t>(m_Impl->Active->Phase),
			std::memory_order_release);
		ReflectionCandidateSourceStepResult result =
			m_Impl->Active->Phase == Impl::DiscoveryPhase::Emit
				? m_Impl->AdvanceEmission()
				: m_Impl->AdvanceDiscovery();
		if (!result.Ok())
			m_Impl->SourceError.store(result.Error, std::memory_order_release);
		return result;
	}
	catch (...)
	{
		m_Impl->SourceError.store(
			ReflectionCandidateSourceError::UnexpectedException,
			std::memory_order_release);
		return {
			.Error = ReflectionCandidateSourceError::UnexpectedException,
			.Progressed = true
		};
	}
}

bool ObjectSnapshotReflectionCandidateSource::ValidateDependencies() noexcept
{
	if (!m_Impl || !m_Impl->Active || !IsCurrentExecutionThreadValid())
		return false;
	const std::shared_ptr<const Impl::PreparedPlan>& plan = m_Impl->Active->Plan;
	if (!plan || m_Impl->Engine.Snapshots().Current() != plan->Snapshot)
		return false;
	for (const auto& [path, object] : plan->Objects)
	{
		(void)path;
		if (!m_Impl->ValidateObject(object))
			return false;
	}
	if (plan->PropertySystem == ReflectionPropertySystem::UProperty)
	{
		for (const std::optional<EngineSnapshotObject>& property : plan->UProperties)
		{
			if (!property || !m_Impl->ValidateObject(*property))
				return false;
		}
	}
	return true;
}

void ObjectSnapshotReflectionCandidateSource::Cancel() noexcept
{
	if (!m_Impl)
		return;
	m_Impl->Active.reset();
	m_Impl->ActiveFlag.store(false, std::memory_order_release);
}

ReflectionCandidateSourceStepResult
ObjectSnapshotReflectionCandidateSource::Impl::AdvanceDiscovery()
{
	if (!Active || !Active->Plan)
		return {.Error = ReflectionCandidateSourceError::InvalidConfiguration};
	const PreparedPlan& plan = *Active->Plan;
	const auto objectAddress = [&](const std::string_view path) -> std::uintptr_t {
		const EngineSnapshotObject* object = FindObject(plan, path);
		return object && ValidateObject(*object) ? object->Handle.Address : 0;
	};
	const std::uintptr_t guid = objectAddress(kGuid);
	const std::uintptr_t color = objectAddress(kColor);
	if (guid == 0 || color == 0)
		return {.Error = ReflectionCandidateSourceError::DependencyChanged, .Progressed = true};

	switch (Active->Phase)
	{
	case DiscoveryPhase::StructPropertiesSize:
		return ScanPhase(
			0x20,
			0x180,
			4,
			[&](const std::int32_t offset) {
				std::int32_t guidSize = -1;
				std::int32_t colorSize = -1;
				return ReadStable(guid, offset, guidSize)
					&& ReadStable(color, offset, colorSize)
					&& guidSize == 0x10
					&& colorSize == 0x04;
			},
			[&](const std::int32_t offset) {
				const std::uintptr_t structClass = objectAddress(kStructClass);
				std::int32_t containerSize = -1;
				if (structClass == 0
					|| !ReadStable(structClass, offset, containerSize)
					|| containerSize <= offset + static_cast<std::int32_t>(sizeof(std::int32_t))
					|| containerSize > ReflectionLayoutLimits::MaxRecordSize)
				{
					return false;
				}
				Active->StructPropertiesSize = offset;
				Active->StructContainerSize = containerSize;
				return true;
			});

	case DiscoveryPhase::StructMinAlignment:
		return ScanPhase(
			0x20,
			Active->StructContainerSize - static_cast<std::int32_t>(sizeof(std::int32_t)),
			4,
			[&](const std::int32_t offset) {
				std::int32_t guidAlignment = -1;
				std::int32_t colorAlignment = -1;
				return ReadStable(guid, offset, guidAlignment)
					&& ReadStable(color, offset, colorAlignment)
					&& guidAlignment == 4
					&& colorAlignment == 1;
			},
			[&](const std::int32_t offset) {
				Active->StructMinAlignment = offset;
				return true;
			});

	case DiscoveryPhase::StructSuper:
	{
		const std::uintptr_t structClass = objectAddress(kStructClass);
		const std::uintptr_t fieldClass = objectAddress(kFieldClass);
		const std::uintptr_t classClass = objectAddress(kClassClass);
		if (structClass == 0 || fieldClass == 0 || classClass == 0)
			return {.Error = ReflectionCandidateSourceError::DependencyChanged, .Progressed = true};
		return ScanPhase(
			0x20,
			Active->StructContainerSize - static_cast<std::int32_t>(sizeof(std::uintptr_t)),
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				std::uintptr_t structSuper = 0;
				std::uintptr_t classSuper = 0;
				return ReadStable(structClass, offset, structSuper)
					&& ReadStable(classClass, offset, classSuper)
					&& structSuper == fieldClass
					&& classSuper == structClass;
			},
			[&](const std::int32_t offset) {
				Active->StructSuper = offset;
				return true;
			});
	}

	case DiscoveryPhase::StructChildren:
	{
		const auto isGuidName = [](const std::string_view name) {
			return name == "A" || name == "B" || name == "C" || name == "D";
		};
		const auto isColorName = [](const std::string_view name) {
			return name == "R" || name == "G" || name == "B" || name == "A"
				|| name == "r" || name == "g" || name == "b" || name == "a";
		};
		const auto matchFPropertyRoots = [&](
			const std::int32_t structOffset,
			std::uintptr_t& guidHead,
			std::uintptr_t& colorHead,
			std::int32_t& nameOffset,
			std::string& guidName,
			std::string& colorName) {
			guidHead = 0;
			colorHead = 0;
			nameOffset = -1;
			guidName.clear();
			colorName.clear();
			if (!ReadStable(guid, structOffset, guidHead)
				|| !ReadStable(color, structOffset, colorHead)
				|| guidHead == 0 || colorHead == 0
				|| !ValidateReadableMemory(guidHead, 1).Ok()
				|| !ValidateReadableMemory(colorHead, 1).Ok())
			{
				return false;
			}
			std::size_t matches = 0;
			for (std::int32_t candidate = 0; candidate <= 0x60; candidate += 4)
			{
				std::uintptr_t guidNameAddress = 0;
				std::uintptr_t colorNameAddress = 0;
				if (!TryAddAddress(
					guidHead,
					candidate,
					static_cast<std::size_t>(Context->NameProfile().FNameSize),
					guidNameAddress)
					|| !TryAddAddress(
						colorHead,
						candidate,
						static_cast<std::size_t>(Context->NameProfile().FNameSize),
						colorNameAddress))
				{
					continue;
				}
				const EngineNameResult decodedGuid = Engine.Names().DecodeFName(guidNameAddress);
				const EngineNameResult decodedColor = Engine.Names().DecodeFName(colorNameAddress);
				if (!decodedGuid.Ok() || !decodedColor.Ok()
					|| !isGuidName(decodedGuid.Value)
					|| !isColorName(decodedColor.Value))
				{
					continue;
				}
				++matches;
				nameOffset = candidate;
				guidName = decodedGuid.Value;
				colorName = decodedColor.Value;
			}
			return matches == 1;
		};

		return ScanPhase(
			0x20,
			Active->StructContainerSize - static_cast<std::int32_t>(sizeof(std::uintptr_t)),
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				if (plan.PropertySystem == ReflectionPropertySystem::FProperty)
				{
					std::uintptr_t ignoredGuid = 0;
					std::uintptr_t ignoredColor = 0;
					std::int32_t ignoredNameOffset = -1;
					std::string ignoredGuidName;
					std::string ignoredColorName;
					return matchFPropertyRoots(
						offset,
						ignoredGuid,
						ignoredColor,
						ignoredNameOffset,
						ignoredGuidName,
						ignoredColorName);
				}
				std::uintptr_t guidHead = 0;
				std::uintptr_t colorHead = 0;
				if (!ReadStable(guid, offset, guidHead)
					|| !ReadStable(color, offset, colorHead))
				{
					return false;
				}
				const auto isOneOf = [&](
					const std::uintptr_t address,
					const std::span<const TargetPropertySpec> targets) {
					for (const TargetPropertySpec& target : targets)
					{
						const auto& property = plan.UProperties[
							static_cast<std::size_t>(target.Key)];
						if (property && property->Handle.Address == address)
							return true;
					}
					return false;
				};
				return isOneOf(guidHead, kGuidTargets)
					&& isOneOf(colorHead, kColorTargets);
			},
			[&](const std::int32_t offset) {
				Active->StructChildren = offset;
				if (!ReadStable(guid, offset, Active->GuidHead)
					|| !ReadStable(color, offset, Active->ColorHead))
				{
					return false;
				}
				if (plan.PropertySystem == ReflectionPropertySystem::FProperty)
				{
					return matchFPropertyRoots(
						offset,
						Active->GuidHead,
						Active->ColorHead,
						Active->FieldName,
						Active->GuidHeadName,
						Active->ColorHeadName);
				}
				for (const TargetPropertySpec& target : kGuidTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == Active->GuidHead)
						Active->GuidHeadName = property->Name;
				}
				for (const TargetPropertySpec& target : kColorTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == Active->ColorHead)
						Active->ColorHeadName = property->Name;
				}
				return !Active->GuidHeadName.empty() && !Active->ColorHeadName.empty();
			});
	}

	case DiscoveryPhase::FieldClass:
		if (plan.PropertySystem == ReflectionPropertySystem::UProperty)
		{
			Active->Phase = DiscoveryPhase::FieldNext;
			return {.Progressed = true};
		}
		return ScanPhase(
			0,
			0x60,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t fieldClassOffset) {
				std::uintptr_t guidClass = 0;
				std::uintptr_t colorClass = 0;
				if (!ReadStable(Active->GuidHead, fieldClassOffset, guidClass)
					|| !ReadStable(Active->ColorHead, fieldClassOffset, colorClass)
					|| guidClass == 0 || colorClass == 0)
				{
					return false;
				}
				const std::uint64_t intFlags = Mask(
					ClassCastFlag::Field,
					ClassCastFlag::Property,
					ClassCastFlag::NumericProperty,
					ClassCastFlag::IntProperty);
				const std::uint64_t byteFlags = Mask(
					ClassCastFlag::Field,
					ClassCastFlag::Property,
					ClassCastFlag::NumericProperty,
					ClassCastFlag::ByteProperty);
				std::size_t castMatches = 0;
				for (std::int32_t castOffset = 0; castOffset <= 0x38; castOffset += 4)
				{
					std::uint64_t guidFlags = 0;
					std::uint64_t colorFlags = 0;
					if (ReadStable(guidClass, castOffset, guidFlags)
						&& ReadStable(colorClass, castOffset, colorFlags)
						&& guidFlags == intFlags
						&& colorFlags == byteFlags)
					{
						++castMatches;
					}
				}
				return castMatches == 1;
			},
			[&](const std::int32_t fieldClassOffset) {
				if (!ReadStable(Active->GuidHead, fieldClassOffset, Active->GuidFieldClass)
					|| !ReadStable(Active->ColorHead, fieldClassOffset, Active->ColorFieldClass))
				{
					return false;
				}
				const std::uint64_t intFlags = Mask(
					ClassCastFlag::Field,
					ClassCastFlag::Property,
					ClassCastFlag::NumericProperty,
					ClassCastFlag::IntProperty);
				const std::uint64_t byteFlags = Mask(
					ClassCastFlag::Field,
					ClassCastFlag::Property,
					ClassCastFlag::NumericProperty,
					ClassCastFlag::ByteProperty);
				std::int32_t selectedCast = -1;
				std::size_t matches = 0;
				for (std::int32_t castOffset = 0; castOffset <= 0x38; castOffset += 4)
				{
					std::uint64_t guidFlags = 0;
					std::uint64_t colorFlags = 0;
					if (ReadStable(Active->GuidFieldClass, castOffset, guidFlags)
						&& ReadStable(Active->ColorFieldClass, castOffset, colorFlags)
						&& guidFlags == intFlags && colorFlags == byteFlags)
					{
						selectedCast = castOffset;
						++matches;
					}
				}
				if (matches != 1)
					return false;
				Active->FieldClass = fieldClassOffset;
				Active->FieldClassCastFlags = selectedCast;
				return true;
			});

	case DiscoveryPhase::FieldNext:
	{
		const auto guidTarget = [&](const std::uintptr_t address) {
			if (address == 0 || address == Active->GuidHead)
				return false;
			if (plan.PropertySystem == ReflectionPropertySystem::UProperty)
			{
				for (const TargetPropertySpec& target : kGuidTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == address)
						return true;
				}
				return false;
			}
			std::string name;
			std::uint64_t flags = 0;
			return TryDecodeFieldName(address, name)
				&& name != Active->GuidHeadName
				&& (name == "A" || name == "B" || name == "C" || name == "D")
				&& TryPropertyCastFlags(address, flags)
				&& HasCastFlag(flags, Mask(ClassCastFlag::IntProperty));
		};
		const auto colorTarget = [&](const std::uintptr_t address) {
			if (address == 0 || address == Active->ColorHead)
				return false;
			if (plan.PropertySystem == ReflectionPropertySystem::UProperty)
			{
				for (const TargetPropertySpec& target : kColorTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == address)
						return true;
				}
				return false;
			}
			std::string name;
			std::uint64_t flags = 0;
			return TryDecodeFieldName(address, name)
				&& name != Active->ColorHeadName
				&& (name == "R" || name == "G" || name == "B" || name == "A"
					|| name == "r" || name == "g" || name == "b" || name == "a")
				&& TryPropertyCastFlags(address, flags)
				&& HasCastFlag(flags, Mask(ClassCastFlag::ByteProperty));
		};
		return ScanPhase(
			0,
			0x80,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				std::uintptr_t guidNext = 0;
				std::uintptr_t colorNext = 0;
				return ReadStable(Active->GuidHead, offset, guidNext)
					&& ReadStable(Active->ColorHead, offset, colorNext)
					&& guidTarget(guidNext)
					&& colorTarget(colorNext);
			},
			[&](const std::int32_t offset) {
				if (!ReadStable(Active->GuidHead, offset, Active->GuidNext)
					|| !ReadStable(Active->ColorHead, offset, Active->ColorNext))
				{
					return false;
				}
				Active->FieldNext = offset;
				if (plan.PropertySystem == ReflectionPropertySystem::FProperty)
				{
					return TryDecodeFieldName(Active->GuidNext, Active->GuidNextName)
						&& TryDecodeFieldName(Active->ColorNext, Active->ColorNextName);
				}
				for (const TargetPropertySpec& target : kGuidTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == Active->GuidNext)
						Active->GuidNextName = property->Name;
				}
				for (const TargetPropertySpec& target : kColorTargets)
				{
					const auto& property = plan.UProperties[
						static_cast<std::size_t>(target.Key)];
					if (property && property->Handle.Address == Active->ColorNext)
						Active->ColorNextName = property->Name;
				}
				return !Active->GuidNextName.empty() && !Active->ColorNextName.empty();
			});
	}

	case DiscoveryPhase::ResolveProperties:
	{
		const ReflectionCandidateSourceError error = AdvancePropertyResolution();
		return {
			.Error = error,
			.Progressed = true
		};
	}

	case DiscoveryPhase::PropertyArrayDim:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidA)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidC)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidD)]
		};
		return ScanPhase(
			0,
			0x180,
			4,
			[&](const std::int32_t offset) {
				for (const std::uintptr_t base : bases)
				{
					std::int32_t value = 0;
					if (!ReadStable(base, offset, value) || value != 1)
						return false;
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->PropertyArrayDim = offset;
				return true;
			});
	}

	case DiscoveryPhase::PropertyElementSize:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidA)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidC)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidD)]
		};
		return ScanPhase(
			0,
			0x180,
			4,
			[&](const std::int32_t offset) {
				for (const std::uintptr_t base : bases)
				{
					std::int32_t value = 0;
					if (!ReadStable(base, offset, value) || value != 4)
						return false;
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->PropertyElementSize = offset;
				return true;
			});
	}

	case DiscoveryPhase::PropertyFlags:
	{
		const std::uintptr_t guidA =
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidA)];
		const std::uintptr_t colorR =
			Active->Properties[static_cast<std::size_t>(PropertyKey::ColorR)];
		const std::uint64_t publicFlag = Mask(
			PropertyFlag::NativeAccessSpecifierPublic);
		const std::uint64_t guidFlags = Mask(
			PropertyFlag::Edit,
			PropertyFlag::ZeroConstructor,
			PropertyFlag::SaveGame,
			PropertyFlag::IsPlainOldData,
			PropertyFlag::NoDestructor,
			PropertyFlag::HasGetValueTypeHash);
		const std::uint64_t colorFlags = Mask(
			PropertyFlag::Edit,
			PropertyFlag::BlueprintVisible,
			PropertyFlag::ZeroConstructor,
			PropertyFlag::SaveGame,
			PropertyFlag::IsPlainOldData,
			PropertyFlag::NoDestructor,
			PropertyFlag::HasGetValueTypeHash);
		return ScanPhase(
			0,
			0x180,
			4,
			[&](const std::int32_t offset) {
				std::uint64_t observedGuid = 0;
				std::uint64_t observedColor = 0;
				return ReadStable(guidA, offset, observedGuid)
					&& ReadStable(colorR, offset, observedColor)
					&& (observedGuid == guidFlags || observedGuid == (guidFlags | publicFlag))
					&& (observedColor == colorFlags || observedColor == (colorFlags | publicFlag));
			},
			[&](const std::int32_t offset) {
				Active->PropertyFlags = offset;
				return ReadStable(guidA, offset, Active->PropertyFlagValues[0])
					&& ReadStable(colorR, offset, Active->PropertyFlagValues[1]);
			});
	}

	case DiscoveryPhase::PropertyOffset:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidA)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GuidC)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::ColorB)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::ColorG)]
		};
		constexpr std::array expected{0, 8, 0, 1};
		return ScanPhase(
			0,
			0x180,
			4,
			[&](const std::int32_t offset) {
				for (std::size_t index = 0; index < bases.size(); ++index)
				{
					std::int32_t value = -1;
					if (!ReadStable(bases[index], offset, value) || value != expected[index])
						return false;
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->PropertyOffset = offset;
				return true;
			});
	}

	case DiscoveryPhase::BoolLayout:
	{
		const std::uintptr_t first = Active->Properties[
			static_cast<std::size_t>(PropertyKey::EngineNativeBool)];
		const std::uintptr_t second = Active->Properties[
			static_cast<std::size_t>(PropertyKey::PlayerControllerNativeBool)];
		return ScanPhase(
			0,
			0x1FC,
			1,
			[&](const std::int32_t offset) {
				std::array<std::uint8_t, 4> left{};
				std::array<std::uint8_t, 4> right{};
				return ReadStable(first, offset, left)
					&& ReadStable(second, offset, right)
					&& left == std::array<std::uint8_t, 4>{1, 0, 1, 0xFF}
					&& right == left;
			},
			[&](const std::int32_t offset) {
				Active->BoolBase = offset;
				return true;
			});
	}

	case DiscoveryPhase::ByteProperty:
	{
		const std::uintptr_t expected = objectAddress(kCollisionResponseEnum);
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::GameTraceChannel1)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::GameTraceChannel2)]
		};
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				for (const std::uintptr_t base : bases)
				{
					std::uintptr_t value = 0;
					if (!ReadStable(base, offset, value) || value != expected)
						return false;
				}
				return expected != 0;
			},
			[&](const std::int32_t offset) {
				Active->BytePropertyEnum = offset;
				return true;
			});
	}

	case DiscoveryPhase::ObjectProperty:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::ControllerPlayerState)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::ControllerPawn)]
		};
		const std::array expected{objectAddress(kPlayerState), objectAddress(kPawn)};
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				for (std::size_t index = 0; index < bases.size(); ++index)
				{
					std::uintptr_t value = 0;
					if (expected[index] == 0
						|| !ReadStable(bases[index], offset, value)
						|| value != expected[index])
					{
						return false;
					}
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->ObjectPropertyClass = offset;
				return true;
			});
	}

	case DiscoveryPhase::StructProperty:
	{
		const std::uintptr_t expected = objectAddress(kVector);
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::TwoVectorsV1)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::TwoVectorsV2)]
		};
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				for (const std::uintptr_t base : bases)
				{
					std::uintptr_t value = 0;
					if (!ReadStable(base, offset, value) || value != expected)
						return false;
				}
				return expected != 0;
			},
			[&](const std::int32_t offset) {
				Active->StructPropertyStruct = offset;
				return true;
			});
	}

	case DiscoveryPhase::ArrayProperty:
	{
		const std::uintptr_t base = Active->Properties[
			static_cast<std::size_t>(PropertyKey::DebugProperties)];
		const std::uintptr_t expectedStruct = objectAddress(kDebugDisplayProperty);
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				std::uintptr_t inner = 0;
				std::uintptr_t innerStruct = 0;
				std::uint64_t flags = 0;
				return ReadStable(base, offset, inner)
					&& inner != 0
					&& TryPropertyCastFlags(inner, flags)
					&& HasCastFlag(flags, Mask(ClassCastFlag::StructProperty))
					&& ReadStable(inner, Active->StructPropertyStruct, innerStruct)
					&& innerStruct == expectedStruct;
			},
			[&](const std::int32_t offset) {
				Active->ArrayPropertyInner = offset;
				return ReadStable(base, offset, Active->ArrayInner);
			});
	}

	case DiscoveryPhase::MapPropertyKey:
	case DiscoveryPhase::MapPropertyValue:
	{
		const bool keyPhase = Active->Phase == DiscoveryPhase::MapPropertyKey;
		const std::uintptr_t base = Active->Properties[
			static_cast<std::size_t>(PropertyKey::DisplayNameMap)];
		const std::uint64_t required = keyPhase
			? Mask(ClassCastFlag::NameProperty)
			: Mask(ClassCastFlag::TextProperty);
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				std::uintptr_t property = 0;
				std::uint64_t flags = 0;
				return ReadStable(base, offset, property)
					&& property != 0
					&& TryPropertyCastFlags(property, flags)
					&& HasCastFlag(flags, required);
			},
			[&](const std::int32_t offset) {
				std::uintptr_t property = 0;
				if (!ReadStable(base, offset, property))
					return false;
				if (keyPhase)
				{
					Active->MapPropertyKey = offset;
					Active->MapKey = property;
				}
				else
				{
					Active->MapPropertyValue = offset;
					Active->MapValue = property;
				}
				return true;
			});
	}

	case DiscoveryPhase::SetProperty:
	{
		const std::uintptr_t base = Active->Properties[
			static_cast<std::size_t>(PropertyKey::LevelCollectionLevels)];
		const std::uintptr_t expectedClass = objectAddress(kLevel);
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				std::uintptr_t element = 0;
				std::uintptr_t propertyClass = 0;
				std::uint64_t flags = 0;
				return ReadStable(base, offset, element)
					&& element != 0
					&& TryPropertyCastFlags(element, flags)
					&& HasCastFlag(flags, Mask(ClassCastFlag::ObjectProperty))
					&& ReadStable(element, Active->ObjectPropertyClass, propertyClass)
					&& propertyClass == expectedClass;
			},
			[&](const std::int32_t offset) {
				Active->SetPropertyElement = offset;
				return ReadStable(base, offset, Active->SetElement);
			});
	}

	case DiscoveryPhase::EnumPropertyUnderlying:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::CreationMethod)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::AutoPossessAi)]
		};
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				for (const std::uintptr_t base : bases)
				{
					std::uintptr_t underlying = 0;
					std::uint64_t flags = 0;
					if (!ReadStable(base, offset, underlying)
						|| underlying == 0
						|| !TryPropertyCastFlags(underlying, flags)
						|| !HasCastFlag(flags, Mask(ClassCastFlag::ByteProperty)))
					{
						return false;
					}
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->EnumPropertyUnderlying = offset;
				return ReadStable(bases[0], offset, Active->EnumUnderlying[0])
					&& ReadStable(bases[1], offset, Active->EnumUnderlying[1]);
			});
	}

	case DiscoveryPhase::EnumPropertyEnum:
	{
		const std::array bases{
			Active->Properties[static_cast<std::size_t>(PropertyKey::CreationMethod)],
			Active->Properties[static_cast<std::size_t>(PropertyKey::AutoPossessAi)]
		};
		const std::array expected{
			objectAddress(kComponentCreationMethodEnum),
			objectAddress(kAutoPossessAiEnum)
		};
		return ScanPhase(
			0,
			0x1F8,
			static_cast<std::int32_t>(alignof(std::uintptr_t)),
			[&](const std::int32_t offset) {
				for (std::size_t index = 0; index < bases.size(); ++index)
				{
					std::uintptr_t value = 0;
					if (expected[index] == 0
						|| !ReadStable(bases[index], offset, value)
						|| value != expected[index])
					{
						return false;
					}
				}
				return true;
			},
			[&](const std::int32_t offset) {
				Active->EnumPropertyEnum = offset;
				return true;
			});
	}

	case DiscoveryPhase::ValidateRelations:
	{
		const ReflectionCandidateSourceError error = ValidateRelations();
		if (error == ReflectionCandidateSourceError::None)
			Active->Phase = DiscoveryPhase::Emit;
		return {.Error = error, .Progressed = true};
	}

	case DiscoveryPhase::Emit:
		return AdvanceEmission();
	}
	return {.Error = ReflectionCandidateSourceError::UnexpectedException, .Progressed = true};
}

ReflectionCandidateSourceError
ObjectSnapshotReflectionCandidateSource::Impl::ValidateRelations() noexcept
{
	if (!Active || !Active->Plan)
		return ReflectionCandidateSourceError::InvalidConfiguration;
	const std::array derivedBases{
		Active->BoolBase,
		Active->BytePropertyEnum,
		Active->ObjectPropertyClass,
		Active->StructPropertyStruct,
		Active->ArrayPropertyInner,
		Active->MapPropertyKey,
		Active->SetPropertyElement,
		Active->EnumPropertyUnderlying
	};
	const std::int32_t propertySize = derivedBases.front();
	if (propertySize <= 0 || propertySize > ReflectionLayoutLimits::MaxRecordSize)
		return ReflectionCandidateSourceError::EvidenceUnavailable;
	for (const std::int32_t base : derivedBases)
	{
		if (base != propertySize)
			return ReflectionCandidateSourceError::EvidenceAmbiguous;
	}
	if (Active->MapPropertyValue != propertySize + static_cast<std::int32_t>(sizeof(void*))
		|| Active->EnumPropertyEnum != propertySize + static_cast<std::int32_t>(sizeof(void*)))
	{
		return ReflectionCandidateSourceError::EvidenceAmbiguous;
	}

	const std::array genericFields{
		std::pair{Active->PropertyArrayDim, static_cast<std::int32_t>(sizeof(std::int32_t))},
		std::pair{Active->PropertyElementSize, static_cast<std::int32_t>(sizeof(std::int32_t))},
		std::pair{Active->PropertyFlags, static_cast<std::int32_t>(sizeof(std::uint64_t))},
		std::pair{Active->PropertyOffset, static_cast<std::int32_t>(sizeof(std::int32_t))}
	};
	std::int32_t firstPropertyField = propertySize;
	for (const auto [offset, width] : genericFields)
	{
		if (offset < 0 || offset > propertySize - width)
			return ReflectionCandidateSourceError::EvidenceUnavailable;
		firstPropertyField = (std::min)(firstPropertyField, offset);
	}
	if (firstPropertyField <= 0)
		return ReflectionCandidateSourceError::EvidenceUnavailable;

	if (Active->Plan->PropertySystem == ReflectionPropertySystem::FProperty)
	{
		const std::int32_t nameWidth = Context->NameProfile().FNameSize;
		if (Active->FieldClass < 0
			|| Active->FieldNext < 0
			|| Active->FieldName < 0
			|| Active->FieldClassCastFlags < 0
			|| Active->FieldClass > firstPropertyField - static_cast<std::int32_t>(sizeof(void*))
			|| Active->FieldNext > firstPropertyField - static_cast<std::int32_t>(sizeof(void*))
			|| nameWidth <= 0
			|| Active->FieldName > firstPropertyField - nameWidth)
		{
			return ReflectionCandidateSourceError::EvidenceUnavailable;
		}
	}
	else if (Active->FieldNext < 0
		|| Active->FieldNext > firstPropertyField - static_cast<std::int32_t>(sizeof(void*)))
	{
		return ReflectionCandidateSourceError::EvidenceUnavailable;
	}

	Active->PropertyContainerSize = propertySize;
	Active->FieldContainerSize = firstPropertyField;
	return ReflectionCandidateSourceError::None;
}

ReflectionCandidateEvidence
ObjectSnapshotReflectionCandidateSource::Impl::BuildEvidence(
	const ReflectionField field) const
{
	if (!Active || !Active->Plan)
		throw std::runtime_error("reflection evidence requested without an active plan");
	const PreparedPlan& plan = *Active->Plan;
	const auto address = [&](const std::string_view path) {
		const EngineSnapshotObject* object = FindObject(plan, path);
		if (!object)
			throw std::runtime_error("reflection evidence object is unavailable");
		return object->Handle.Address;
	};
	const auto property = [&](const PropertyKey key) {
		const std::uintptr_t value = Active->Properties[static_cast<std::size_t>(key)];
		if (value == 0)
			throw std::runtime_error("reflection evidence property is unavailable");
		return value;
	};
	const auto scalarWitness = [&](
		const std::string_view id,
		const std::uintptr_t base,
		const std::uint64_t expected) {
		return ReflectionFieldWitness{
			.Id = std::string(id),
			.Field = field,
			.BaseAddress = base,
			.Expected = expected
		};
	};
	const auto nameWitness = [&](
		const std::string_view id,
		const std::uintptr_t base,
		const std::string& expected) {
		return ReflectionFieldWitness{
			.Id = std::string(id),
			.Field = field,
			.BaseAddress = base,
			.Expected = expected
		};
	};
	const auto candidate = [&](const std::int32_t offset, const std::int32_t size) {
		return ReflectionFieldCandidate{
			.Field = field,
			.Offset = offset,
			.ContainerSize = size,
			.Source = std::string(kSourceName)
		};
	};

	ReflectionCandidateEvidence evidence;
	switch (field)
	{
	case ReflectionField::StructSuper:
		evidence.Field = candidate(Active->StructSuper, Active->StructContainerSize);
		evidence.Witnesses = {
			scalarWitness("Struct.super=Field", address(kStructClass), address(kFieldClass)),
			scalarWitness("Class.super=Struct", address(kClassClass), address(kStructClass))
		};
		break;
	case ReflectionField::StructChildren:
	case ReflectionField::StructChildProperties:
		evidence.Field = candidate(Active->StructChildren, Active->StructContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.child_head", address(kGuid), Active->GuidHead),
			scalarWitness("Color.child_head", address(kColor), Active->ColorHead)
		};
		break;
	case ReflectionField::StructPropertiesSize:
		evidence.Field = candidate(Active->StructPropertiesSize, Active->StructContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.properties_size=16", address(kGuid), 16),
			scalarWitness("Color.properties_size=4", address(kColor), 4)
		};
		break;
	case ReflectionField::StructMinAlignment:
		evidence.Field = candidate(Active->StructMinAlignment, Active->StructContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.min_alignment=4", address(kGuid), 4),
			scalarWitness("Color.min_alignment=1", address(kColor), 1)
		};
		break;
	case ReflectionField::UFieldNext:
		evidence.Field = candidate(Active->FieldNext, Active->FieldContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.ufield_next", Active->GuidHead, Active->GuidNext),
			scalarWitness("Color.ufield_next", Active->ColorHead, Active->ColorNext)
		};
		break;
	case ReflectionField::FFieldClass:
		evidence.Field = candidate(Active->FieldClass, Active->FieldContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.ffield_class", Active->GuidHead, Active->GuidFieldClass),
			scalarWitness("Color.ffield_class", Active->ColorHead, Active->ColorFieldClass)
		};
		break;
	case ReflectionField::FFieldNext:
		evidence.Field = candidate(Active->FieldNext, Active->FieldContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.ffield_next", Active->GuidHead, Active->GuidNext),
			scalarWitness("Color.ffield_next", Active->ColorHead, Active->ColorNext)
		};
		break;
	case ReflectionField::FFieldName:
		evidence.Field = candidate(Active->FieldName, Active->FieldContainerSize);
		evidence.Witnesses = {
			nameWitness("Guid.ffield_name", Active->GuidHead, Active->GuidHeadName),
			nameWitness("Color.ffield_name", Active->ColorHead, Active->ColorHeadName)
		};
		break;
	case ReflectionField::FFieldClassCastFlags:
	{
		const std::uint64_t intFlags = Mask(
			ClassCastFlag::Field,
			ClassCastFlag::Property,
			ClassCastFlag::NumericProperty,
			ClassCastFlag::IntProperty);
		const std::uint64_t byteFlags = Mask(
			ClassCastFlag::Field,
			ClassCastFlag::Property,
			ClassCastFlag::NumericProperty,
			ClassCastFlag::ByteProperty);
		const std::int32_t size = AlignUp(
			Active->FieldClassCastFlags + static_cast<std::int32_t>(sizeof(std::uint64_t)),
			static_cast<std::int32_t>(alignof(std::uintptr_t)));
		evidence.Field = candidate(Active->FieldClassCastFlags, size);
		evidence.Witnesses = {
			scalarWitness("IntProperty.cast_flags", Active->GuidFieldClass, intFlags),
			scalarWitness("ByteProperty.cast_flags", Active->ColorFieldClass, byteFlags)
		};
		break;
	}
	case ReflectionField::PropertyArrayDim:
		evidence.Field = candidate(Active->PropertyArrayDim, Active->PropertyContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.A.array_dim=1", property(PropertyKey::GuidA), 1),
			scalarWitness("Guid.C.array_dim=1", property(PropertyKey::GuidC), 1)
		};
		break;
	case ReflectionField::PropertyElementSize:
		evidence.Field = candidate(Active->PropertyElementSize, Active->PropertyContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.A.element_size=4", property(PropertyKey::GuidA), 4),
			scalarWitness("Guid.C.element_size=4", property(PropertyKey::GuidC), 4)
		};
		break;
	case ReflectionField::PropertyFlags:
		evidence.Field = candidate(Active->PropertyFlags, Active->PropertyContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.A.property_flags", property(PropertyKey::GuidA), Active->PropertyFlagValues[0]),
			scalarWitness("Color.R.property_flags", property(PropertyKey::ColorR), Active->PropertyFlagValues[1])
		};
		break;
	case ReflectionField::PropertyOffset:
		evidence.Field = candidate(Active->PropertyOffset, Active->PropertyContainerSize);
		evidence.Witnesses = {
			scalarWitness("Guid.C.offset=8", property(PropertyKey::GuidC), 8),
			scalarWitness("Color.G.offset=1", property(PropertyKey::ColorG), 1)
		};
		break;
	case ReflectionField::BoolFieldSize:
	case ReflectionField::BoolByteOffset:
	case ReflectionField::BoolByteMask:
	case ReflectionField::BoolFieldMask:
	{
		const std::size_t delta = field == ReflectionField::BoolFieldSize ? 0
			: (field == ReflectionField::BoolByteOffset ? 1
				: (field == ReflectionField::BoolByteMask ? 2 : 3));
		constexpr std::array<std::uint64_t, 4> expected{1, 0, 1, 0xFF};
		constexpr std::array<std::string_view, 4> engineIds{
			"Engine.native_bool.field_size",
			"Engine.native_bool.byte_offset",
			"Engine.native_bool.byte_mask",
			"Engine.native_bool.field_mask"
		};
		constexpr std::array<std::string_view, 4> controllerIds{
			"PlayerController.native_bool.field_size",
			"PlayerController.native_bool.byte_offset",
			"PlayerController.native_bool.byte_mask",
			"PlayerController.native_bool.field_mask"
		};
		evidence.Field = candidate(
			Active->BoolBase + static_cast<std::int32_t>(delta),
			Active->BoolBase + 4);
		evidence.Witnesses = {
			scalarWitness(engineIds[delta],
				property(PropertyKey::EngineNativeBool), expected[delta]),
			scalarWitness(controllerIds[delta],
				property(PropertyKey::PlayerControllerNativeBool), expected[delta])
		};
		break;
	}
	case ReflectionField::BytePropertyEnum:
		evidence.Field = candidate(Active->BytePropertyEnum, Active->BytePropertyEnum + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("GameTraceChannel1.enum", property(PropertyKey::GameTraceChannel1), address(kCollisionResponseEnum)),
			scalarWitness("GameTraceChannel2.enum", property(PropertyKey::GameTraceChannel2), address(kCollisionResponseEnum))
		};
		break;
	case ReflectionField::ObjectPropertyClass:
		evidence.Field = candidate(Active->ObjectPropertyClass, Active->ObjectPropertyClass + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("Controller.PlayerState.class", property(PropertyKey::ControllerPlayerState), address(kPlayerState)),
			scalarWitness("Controller.Pawn.class", property(PropertyKey::ControllerPawn), address(kPawn))
		};
		break;
	case ReflectionField::StructPropertyStruct:
		evidence.Field = candidate(Active->StructPropertyStruct, Active->StructPropertyStruct + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("TwoVectors.v1.struct", property(PropertyKey::TwoVectorsV1), address(kVector)),
			scalarWitness("TwoVectors.v2.struct", property(PropertyKey::TwoVectorsV2), address(kVector))
		};
		break;
	case ReflectionField::ArrayPropertyInner:
		evidence.Field = candidate(Active->ArrayPropertyInner, Active->ArrayPropertyInner + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("GameViewportClient.DebugProperties.inner", property(PropertyKey::DebugProperties), Active->ArrayInner)
		};
		break;
	case ReflectionField::MapPropertyKey:
		evidence.Field = candidate(Active->MapPropertyKey, Active->MapPropertyValue + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("UserDefinedEnum.DisplayNameMap.key", property(PropertyKey::DisplayNameMap), Active->MapKey)
		};
		break;
	case ReflectionField::MapPropertyValue:
		evidence.Field = candidate(Active->MapPropertyValue, Active->MapPropertyValue + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("UserDefinedEnum.DisplayNameMap.value", property(PropertyKey::DisplayNameMap), Active->MapValue)
		};
		break;
	case ReflectionField::SetPropertyElement:
		evidence.Field = candidate(Active->SetPropertyElement, Active->SetPropertyElement + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("LevelCollection.Levels.element", property(PropertyKey::LevelCollectionLevels), Active->SetElement)
		};
		break;
	case ReflectionField::EnumPropertyUnderlying:
		evidence.Field = candidate(Active->EnumPropertyUnderlying, Active->EnumPropertyEnum + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("ActorComponent.CreationMethod.underlying", property(PropertyKey::CreationMethod), Active->EnumUnderlying[0]),
			scalarWitness("Pawn.AutoPossessAI.underlying", property(PropertyKey::AutoPossessAi), Active->EnumUnderlying[1])
		};
		break;
	case ReflectionField::EnumPropertyEnum:
		evidence.Field = candidate(Active->EnumPropertyEnum, Active->EnumPropertyEnum + static_cast<std::int32_t>(sizeof(void*)));
		evidence.Witnesses = {
			scalarWitness("ActorComponent.CreationMethod.enum", property(PropertyKey::CreationMethod), address(kComponentCreationMethodEnum)),
			scalarWitness("Pawn.AutoPossessAI.enum", property(PropertyKey::AutoPossessAi), address(kAutoPossessAiEnum))
		};
		break;
	}
	return evidence;
}

ReflectionCandidateSourceStepResult
ObjectSnapshotReflectionCandidateSource::Impl::AdvanceEmission()
{
	if (!Active || !Active->Plan)
		return {.Error = ReflectionCandidateSourceError::InvalidConfiguration};
	const std::span<const ReflectionField> required =
		RequiredFields(Active->Plan->PropertySystem);
	if (Active->EmissionIndex >= required.size())
		return {.Error = ReflectionCandidateSourceError::ContractViolation};
	ReflectionCandidateEvidence evidence = BuildEvidence(required[Active->EmissionIndex]);
	++Active->EmissionIndex;
	EmittedFields.store(Active->EmissionIndex, std::memory_order_release);
	return {
		.Progressed = true,
		.Complete = Active->EmissionIndex == required.size(),
		.Evidence = std::move(evidence)
	};
}

} // namespace UExplorer::Runtime
