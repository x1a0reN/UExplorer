#include "BlueprintBytecodeRuntime.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <new>
#include <ranges>
#include <string_view>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

bool SameBinding(
	const BlueprintEvidenceBinding& left,
	const BlueprintEvidenceBinding& right) noexcept
{
	return SameBlueprintEvidenceBinding(left, right);
}

bool IsBoundedProfileId(const std::string_view value) noexcept
{
	return !value.empty()
		&& value.size() <= BlueprintBytecodeEvidenceStore::kMaxProfileIdBytes
		&& std::ranges::none_of(value, [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

BlueprintEvidenceBinding BindingFor(
	const EngineSnapshot& objects,
	const TypeSnapshot& types)
{
	return {
		.SessionId = objects.SessionId,
		.ContextGeneration = objects.ContextGeneration,
		.ObjectSnapshotGeneration = objects.Generation,
		.TypeSnapshotGeneration = types.Generation()
	};
}

struct SourceProfile final
{
	std::string_view MarkerVersion;
	std::string_view SourceVersion;
	std::string_view Family;
	std::uint8_t ComponentWidth;
	bool UsesFProperty;
	EngineNameStorageKind NameStorage;
	bool SupportsOutlineNumber;
	bool HasPropertyConst;
	bool HasLocalCalls;
	bool HasSparseData;
	bool HasFieldPath;
	bool HasDoubleAndVector3f;
	bool HasDebugAndAutoRtfm;
};

constexpr std::array kSourceProfiles{
	SourceProfile{
		.MarkerVersion = "4.21", .SourceVersion = "4.21.2", .Family = "ue4.21",
		.ComponentWidth = 4, .UsesFProperty = false,
		.NameStorage = EngineNameStorageKind::ChunkedArray},
	SourceProfile{
		.MarkerVersion = "4.24", .SourceVersion = "4.24.3", .Family = "ue4.24",
		.ComponentWidth = 4, .UsesFProperty = false,
		.NameStorage = EngineNameStorageKind::NamePool,
		.HasLocalCalls = true, .HasSparseData = true},
	SourceProfile{
		.MarkerVersion = "4.25", .SourceVersion = "4.25.4", .Family = "ue4.25",
		.ComponentWidth = 4, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool,
		.HasLocalCalls = true, .HasSparseData = true, .HasFieldPath = true},
	SourceProfile{
		.MarkerVersion = "4.26", .SourceVersion = "4.26.2", .Family = "ue4.26",
		.ComponentWidth = 4, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true},
	SourceProfile{
		.MarkerVersion = "4.27", .SourceVersion = "4.27.2", .Family = "ue4.27",
		.ComponentWidth = 4, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true},
	SourceProfile{
		.MarkerVersion = "5.0", .SourceVersion = "5.0.3", .Family = "ue5.0",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true},
	SourceProfile{
		.MarkerVersion = "5.1", .SourceVersion = "5.1.1", .Family = "ue5.1",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true},
	SourceProfile{
		.MarkerVersion = "5.2", .SourceVersion = "5.2.1", .Family = "ue5.2",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true},
	SourceProfile{
		.MarkerVersion = "5.3", .SourceVersion = "5.3.2", .Family = "ue5.3",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true,
		.HasDebugAndAutoRtfm = true},
	SourceProfile{
		.MarkerVersion = "5.4", .SourceVersion = "5.4.4", .Family = "ue5.4",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true,
		.HasDebugAndAutoRtfm = true},
	SourceProfile{
		.MarkerVersion = "5.6", .SourceVersion = "5.6.1", .Family = "ue5.6",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true,
		.HasDebugAndAutoRtfm = true},
	SourceProfile{
		.MarkerVersion = "5.7", .SourceVersion = "5.7.4", .Family = "ue5.7",
		.ComponentWidth = 8, .UsesFProperty = true,
		.NameStorage = EngineNameStorageKind::NamePool, .SupportsOutlineNumber = true,
		.HasPropertyConst = true, .HasLocalCalls = true, .HasSparseData = true,
		.HasFieldPath = true, .HasDoubleAndVector3f = true,
		.HasDebugAndAutoRtfm = true},
};

bool IsCatalogVersion(const std::string_view version) noexcept
{
	if (version.empty() || version.size() > 16
		|| version.front() == '.' || version.back() == '.')
		return false;
	std::size_t componentCount = 1;
	std::size_t componentLength = 0;
	for (const char value : version)
	{
		if (value == '.')
		{
			if (componentLength == 0 || componentLength > 3)
				return false;
			++componentCount;
			componentLength = 0;
			continue;
		}
		if (value < '0' || value > '9' || ++componentLength > 3)
			return false;
	}
	return componentLength != 0 && componentLength <= 3
		&& componentCount >= 2 && componentCount <= 4
		&& (version.front() == '4' || version.front() == '5');
}

const SourceProfile* FindSourceProfile(const std::string_view version) noexcept
{
	if (!IsCatalogVersion(version))
		return nullptr;
	std::size_t secondDot = std::string_view::npos;
	const std::size_t firstDot = version.find('.');
	if (firstDot != std::string_view::npos)
		secondDot = version.find('.', firstDot + 1);
	const std::string_view releaseMarker = secondDot == std::string_view::npos
		? version
		: version.substr(0, secondDot);
	const auto found = std::ranges::find(
		kSourceProfiles,
		releaseMarker,
		&SourceProfile::MarkerVersion);
	return found == kSourceProfiles.end() ? nullptr : &*found;
}

std::string MakeProfileId(
	const SourceProfile& source,
	const bool usesOutlineNumber)
{
	return std::format(
		"ue-marker-{}-source-{}-windows-x64-fscriptname-{}-v1",
		source.MarkerVersion,
		source.SourceVersion,
		usesOutlineNumber ? "outline" : "inline");
}

void MapCommonOpcodes(BlueprintDecompiler::BytecodeProfile& profile)
{
	using Token = EExprToken;
	const auto map = [&profile](const std::uint8_t raw, const Token token) {
		profile.MapOpcode(raw, token);
	};
	map(0x00, Token::EX_LocalVariable);
	map(0x01, Token::EX_InstanceVariable);
	map(0x02, Token::EX_DefaultVariable);
	map(0x04, Token::EX_Return);
	map(0x06, Token::EX_Jump);
	map(0x07, Token::EX_JumpIfNot);
	map(0x09, Token::EX_Assert);
	map(0x0B, Token::EX_Nothing);
	map(0x0F, Token::EX_Let);
	map(0x12, Token::EX_ClassContext);
	map(0x13, Token::EX_MetaCast);
	map(0x14, Token::EX_LetBool);
	map(0x15, Token::EX_EndParmValue);
	map(0x16, Token::EX_EndFunctionParms);
	map(0x17, Token::EX_Self);
	map(0x18, Token::EX_Skip);
	map(0x19, Token::EX_Context);
	map(0x1A, Token::EX_Context_FailSilent);
	map(0x1B, Token::EX_VirtualFunction);
	map(0x1C, Token::EX_FinalFunction);
	map(0x1D, Token::EX_IntConst);
	map(0x1E, Token::EX_FloatConst);
	map(0x1F, Token::EX_StringConst);
	map(0x20, Token::EX_ObjectConst);
	map(0x21, Token::EX_NameConst);
	map(0x22, Token::EX_RotationConst);
	map(0x23, Token::EX_VectorConst);
	map(0x24, Token::EX_ByteConst);
	map(0x25, Token::EX_IntZero);
	map(0x26, Token::EX_IntOne);
	map(0x27, Token::EX_True);
	map(0x28, Token::EX_False);
	map(0x29, Token::EX_TextConst);
	map(0x2A, Token::EX_NoObject);
	map(0x2B, Token::EX_TransformConst);
	map(0x2C, Token::EX_IntConstByte);
	map(0x2D, Token::EX_NoInterface);
	map(0x2E, Token::EX_DynamicCast);
	map(0x2F, Token::EX_StructConst);
	map(0x30, Token::EX_EndStructConst);
	map(0x31, Token::EX_SetArray);
	map(0x32, Token::EX_EndArray);
	map(0x34, Token::EX_UnicodeStringConst);
	map(0x35, Token::EX_Int64Const);
	map(0x36, Token::EX_UInt64Const);
	profile.MapPrimitiveCast(0x38);
	map(0x39, Token::EX_SetSet);
	map(0x3A, Token::EX_EndSet);
	map(0x3B, Token::EX_SetMap);
	map(0x3C, Token::EX_EndMap);
	map(0x3D, Token::EX_SetConst);
	map(0x3E, Token::EX_EndSetConst);
	map(0x3F, Token::EX_MapConst);
	map(0x40, Token::EX_EndMapConst);
	map(0x42, Token::EX_StructMemberContext);
	map(0x43, Token::EX_LetMulticastDelegate);
	map(0x44, Token::EX_LetDelegate);
	map(0x48, Token::EX_LocalOutVariable);
	map(0x4A, Token::EX_DeprecatedOp4A);
	map(0x4B, Token::EX_InstanceDelegate);
	map(0x4C, Token::EX_PushExecutionFlow);
	map(0x4D, Token::EX_PopExecutionFlow);
	map(0x4E, Token::EX_ComputedJump);
	map(0x4F, Token::EX_PopExecutionFlowIfNot);
	map(0x50, Token::EX_Breakpoint);
	map(0x51, Token::EX_InterfaceContext);
	map(0x52, Token::EX_ObjToInterfaceCast);
	map(0x53, Token::EX_EndOfScript);
	map(0x54, Token::EX_CrossInterfaceCast);
	map(0x55, Token::EX_InterfaceToObjCast);
	map(0x5A, Token::EX_WireTracepoint);
	map(0x5B, Token::EX_SkipOffsetConst);
	map(0x5C, Token::EX_AddMulticastDelegate);
	map(0x5D, Token::EX_ClearMulticastDelegate);
	map(0x5E, Token::EX_Tracepoint);
	map(0x5F, Token::EX_LetObj);
	map(0x60, Token::EX_LetWeakObjPtr);
	map(0x61, Token::EX_BindDelegate);
	map(0x62, Token::EX_RemoveMulticastDelegate);
	map(0x63, Token::EX_CallMulticastDelegate);
	map(0x64, Token::EX_LetValueOnPersistentFrame);
	map(0x65, Token::EX_ArrayConst);
	map(0x66, Token::EX_EndArrayConst);
	map(0x67, Token::EX_SoftObjectConst);
	map(0x68, Token::EX_CallMath);
	map(0x69, Token::EX_SwitchValue);
	map(0x6A, Token::EX_InstrumentationEvent);
	map(0x6B, Token::EX_ArrayGetByRef);
}

std::uint8_t CanonicalComponentWidth(
	const TypeSnapshot& types,
	const std::string_view path,
	const CanonicalMathStructKind expected) noexcept
{
	const ReflectedType* type = types.FindByFullPath(path);
	if (!type
		|| type->Kind != ReflectedTypeKind::Struct
		|| type->DirectProperties.size() != 3)
		return 0;
	const std::array<std::string_view, 3> fieldNames =
		expected == CanonicalMathStructKind::Vector
			? std::array<std::string_view, 3>{"X", "Y", "Z"}
			: std::array<std::string_view, 3>{"Pitch", "Yaw", "Roll"};
	PropertyKind scalarKind = PropertyKind::Unknown;
	std::uint32_t scalarWidth = 0;
	for (std::size_t index = 0; index < fieldNames.size(); ++index)
	{
		const auto property = std::ranges::find(
			type->DirectProperties,
			fieldNames[index],
			&ReflectedProperty::Name);
		if (property == type->DirectProperties.end()
			|| property->State != ReflectedMemberState::Supported
			|| !property->Descriptor
			|| property->ArrayDim != 1
			|| (property->Kind != PropertyKind::Float
				&& property->Kind != PropertyKind::Double)
			|| property->Descriptor->Kind != property->Kind
			|| property->Descriptor->Size != property->Size)
		{
			return 0;
		}
		if (index == 0)
		{
			scalarKind = property->Kind;
			scalarWidth = property->Size;
		}
		if (property->Kind != scalarKind
			|| property->Size != scalarWidth
			|| property->Offset != index * scalarWidth)
		{
			return 0;
		}
	}
	return scalarWidth != 0
		&& type->PropertiesSize == scalarWidth * 3U
		&& type->MinAlignment == scalarWidth
		? static_cast<std::uint8_t>(scalarWidth)
		: 0;
}

} // namespace

struct BlueprintBytecodeRuntimeSource::GenerationState
{
	GenerationState(
		EngineFacade& engine,
		GameThreadExecutor& gameThread,
		BlueprintEvidenceBinding binding,
		BlueprintScriptArrayLayoutWitness layout)
		: Binding(std::move(binding)),
		  Evidence(Binding),
		  Source(engine, gameThread, Evidence, std::move(layout))
	{
	}

	BlueprintEvidenceBinding Binding;
	BlueprintBytecodeEvidenceStore Evidence;
	BlueprintBytecodeCaptureSource Source;
};

BlueprintBytecodeRuntimeSource::BlueprintBytecodeRuntimeSource(
	std::shared_ptr<const EngineContext> context,
	EngineFacade& engine,
	GameThreadExecutor& gameThread,
	EngineVersionProbeResult versionProbe)
	: m_Context(std::move(context)),
	  m_Engine(engine),
	  m_GameThread(gameThread),
	  m_VersionProbe(std::move(versionProbe))
{
}

BlueprintBytecodeRuntimeSource::~BlueprintBytecodeRuntimeSource()
{
	(void)StopAndDrain(std::chrono::milliseconds(5000));
}

bool BlueprintBytecodeRuntimeSource::IsScriptLayoutReportValid() const noexcept
{
	const OffsetReport* report = m_Context
		? m_Context->FindOffset("ufunction.script")
		: nullptr;
	return report
		&& report->IsValidated()
		&& report->Value > 0
		&& report->Value <= static_cast<std::int64_t>(
			BlueprintBytecodeEvidenceStore::kMaxScriptFieldOffset)
		&& report->Source == "scored_runtime_blueprint_script_validation_v2"
		&& report->Confidence == "high";
}

bool BlueprintBytecodeRuntimeSource::IsConfigured() const noexcept
{
	return sizeof(std::uintptr_t) == 8
		&& m_Context
		&& m_Context->Generation() != 0
		&& m_Engine.IsConfigured()
		&& m_Engine.Context() == m_Context
		&& m_Engine.ContextGeneration() == m_Context->Generation()
		&& IsScriptLayoutReportValid()
		&& !m_Barrier.IsStopping();
}

bool BlueprintBytecodeRuntimeSource::IsCaptureConfigured() const noexcept
{
	if (!IsConfigured() || !m_GameThread.IsEnabled())
		return false;
	const std::shared_ptr<const EngineSnapshot> objects =
		m_Engine.Snapshots().Current();
	const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
	return objects
		&& types
		&& objects->SessionId == m_Engine.SessionId()
		&& objects->ContextGeneration == m_Engine.ContextGeneration()
		&& types->SessionId() == objects->SessionId
		&& types->ContextGeneration() == objects->ContextGeneration
		&& types->ObjectSnapshotGeneration() == objects->Generation;
}

bool BlueprintBytecodeRuntimeSource::IsProfileConfigured() const noexcept
{
	try
	{
		if (!IsCaptureConfigured()
			|| !m_VersionProbe.Ok()
			|| m_VersionProbe.Version != m_Context->GameVersion()
			|| !FindSourceProfile(m_VersionProbe.Version)
			|| !m_Context->NameProfile().Validated)
		{
			return false;
		}
		const std::shared_ptr<const EngineSnapshot> objects = m_Engine.Snapshots().Current();
		const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
		if (!objects || !types)
			return false;
		const BlueprintEvidenceBinding binding = BindingFor(*objects, *types);
		return ValidateProfileDependencies(binding, *types)
			&& m_Engine.Snapshots().Current() == objects
			&& m_Engine.Types().Current() == types;
	}
	catch (...)
	{
		return false;
	}
}

std::string BlueprintBytecodeRuntimeSource::CurrentProfileId() const
{
	const SourceProfile* source = FindSourceProfile(m_VersionProbe.Version);
	return source && IsProfileConfigured()
		? MakeProfileId(*source, m_Context->NameProfile().UsesOutlineNumber)
		: std::string{};
}

std::string BlueprintBytecodeRuntimeSource::SourceProfileIdForVersion(
	const std::string_view version,
	const bool usesOutlineNumber)
{
	const SourceProfile* source = FindSourceProfile(version);
	return source && (!usesOutlineNumber || source->SupportsOutlineNumber)
		? MakeProfileId(*source, usesOutlineNumber)
		: std::string{};
}

std::optional<BlueprintDecompiler::BytecodeProfile>
BlueprintBytecodeRuntimeSource::SourceProfileDefinitionForVersion(
	const std::string_view version,
	const bool usesOutlineNumber)
{
	const SourceProfile* source = FindSourceProfile(version);
	if (!source || (usesOutlineNumber && !source->SupportsOutlineNumber))
		return std::nullopt;

	BlueprintDecompiler::BytecodeProfile definition;
	definition.Id = MakeProfileId(*source, usesOutlineNumber);
	definition.PointerWidth = 8;
	definition.CodeSkipWidth = 4;
	definition.VectorComponentWidth = source->ComponentWidth;
	definition.RotationComponentWidth = source->ComponentWidth;
	definition.TransformComponentWidth = source->ComponentWidth;
	definition.NameLayout = {
		.ByteWidth = 12,
		.ComparisonIndexOffset = 0,
		.ComparisonIndexWidth = 4,
		.DisplayIndexOffset = 4,
		.DisplayIndexWidth = 4,
		.NumberOffset = 8,
		.NumberWidth = 4,
		.NumberEncodedInComparisonIndex = usesOutlineNumber
	};
	definition.Limits = {
		.MaxInputBytes = BlueprintBytecodeEvidenceStore::kMaxScriptBytes,
		.MaxBytesConsumed = BlueprintBytecodeEvidenceStore::kMaxScriptBytes,
		.MaxInstructions = BlueprintBytecodeEvidenceStore::kMaxInstructions,
		.MaxStringCodeUnits = BlueprintBytecodeEvidenceStore::kMaxStringCodeUnits,
		.MaxRecursionDepth = BlueprintBytecodeEvidenceStore::kMaxRecursionDepth
	};
	MapCommonOpcodes(definition);
	if (source->HasPropertyConst)
		definition.MapOpcode(0x33, EExprToken::EX_PropertyConst);
	if (source->HasLocalCalls)
	{
		definition.MapOpcode(0x45, EExprToken::EX_LocalVirtualFunction);
		definition.MapOpcode(0x46, EExprToken::EX_LocalFinalFunction);
	}
	if (source->HasSparseData)
		definition.MapOpcode(0x6C, EExprToken::EX_ClassSparseDataVariable);
	if (source->HasFieldPath)
		definition.MapOpcode(0x6D, EExprToken::EX_FieldPathConst);
	if (source->HasDoubleAndVector3f)
	{
		definition.MapOpcode(0x37, EExprToken::EX_DoubleConst);
		definition.MapOpcode(0x41, EExprToken::EX_Vector3fConst);
	}
	if (source->HasDebugAndAutoRtfm)
	{
		definition.MapOpcode(0x0C, EExprToken::EX_NothingInt32);
		definition.MapOpcode(0x11, EExprToken::EX_BitFieldConst);
		definition.MapOpcode(0x70, EExprToken::EX_AutoRtfmTransact);
		definition.MapOpcode(0x71, EExprToken::EX_AutoRtfmStopTransact);
		definition.MapOpcode(0x72, EExprToken::EX_AutoRtfmAbortIfNot);
	}
	return definition;
}

bool BlueprintBytecodeRuntimeSource::ValidateProfileDependencies(
	const BlueprintEvidenceBinding& binding,
	const TypeSnapshot& types) const noexcept
{
	const SourceProfile* source = FindSourceProfile(m_VersionProbe.Version);
	if (!source
		|| !m_Context
		|| sizeof(std::uintptr_t) != 8
		|| !m_VersionProbe.Ok()
		|| m_Context->GameVersion() != m_VersionProbe.Version
		|| binding.SessionId != m_Engine.SessionId()
		|| binding.ContextGeneration != m_Context->Generation()
		|| binding.TypeSnapshotGeneration != types.Generation()
		|| binding.SessionId != types.SessionId()
		|| binding.ContextGeneration != types.ContextGeneration()
		|| binding.ObjectSnapshotGeneration != types.ObjectSnapshotGeneration()
		|| !m_Context->NameProfile().Validated
		|| m_Context->NameProfile().ComparisonIndexOffset != 0
		|| m_Context->NameProfile().Storage != source->NameStorage
		|| m_Context->Profile().UsesNamePool
			!= (source->NameStorage == EngineNameStorageKind::NamePool)
		|| m_Context->Profile().UsesFProperty != source->UsesFProperty
		|| (m_Context->NameProfile().UsesOutlineNumber
			&& !source->SupportsOutlineNumber)
		|| m_Context->Profile().UsesLargeWorldCoordinates
			!= (source->ComponentWidth == 8))
	{
		return false;
	}
	const std::uint8_t vectorWidth = CanonicalComponentWidth(
		types,
		"/Script/CoreUObject.Vector",
		CanonicalMathStructKind::Vector);
	const std::uint8_t rotationWidth = CanonicalComponentWidth(
		types,
		"/Script/CoreUObject.Rotator",
		CanonicalMathStructKind::Rotator);
	return vectorWidth == source->ComponentWidth
		&& rotationWidth == source->ComponentWidth;
}

std::shared_ptr<const BlueprintBytecodeProfileRecord>
BlueprintBytecodeRuntimeSource::BuildProfile(
	const BlueprintEvidenceBinding& binding,
	const TypeSnapshot& types) const
{
	const SourceProfile* source = FindSourceProfile(m_VersionProbe.Version);
	if (!source || !ValidateProfileDependencies(binding, types))
		return nullptr;
	std::optional<BlueprintDecompiler::BytecodeProfile> definition =
		SourceProfileDefinitionForVersion(
			m_VersionProbe.Version,
			m_Context->NameProfile().UsesOutlineNumber);
	if (!definition)
		return nullptr;

	BlueprintBytecodeProfileRecord record;
	record.Binding = binding;
	record.Source = std::format(
		"local_ue_source_catalog:family={};marker={};source={}:Build.version+Script.h+ScriptSerialization.h+NameTypes.h;runtime:engine_version_marker+validated_name_storage+fproperty_mode+fscriptname_number_mode+canonical_vector_rotator",
		source->Family,
		source->MarkerVersion,
		source->SourceVersion);
	record.Definition = std::move(*definition);
	record.EvidenceFingerprint = ComputeBlueprintBytecodeProfileFingerprint(record);
	return std::make_shared<const BlueprintBytecodeProfileRecord>(std::move(record));
}

BlueprintBytecodeProfileResult BlueprintBytecodeRuntimeSource::ResolveProfile(
	const BlueprintBytecodeProfileRequest& request) const
{
	auto lease = m_Barrier.Enter();
	if (!lease.OwnedWorkAllowed())
		return {.Error = BlueprintEvidenceSourceError::Stopped};
	try
	{
		if (!IsBoundedProfileId(request.ProfileId))
			return {.Error = BlueprintEvidenceSourceError::InvalidRequest};
		if (!IsCaptureConfigured())
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		const std::shared_ptr<const EngineSnapshot> objects = m_Engine.Snapshots().Current();
		const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
		if (!objects || !types)
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		const BlueprintEvidenceBinding current = BindingFor(*objects, *types);
		if (!SameBinding(request.Binding, current))
			return {.Error = BlueprintEvidenceSourceError::DependencyChanged};
		std::shared_ptr<const BlueprintBytecodeProfileRecord> profile =
			BuildProfile(current, *types);
		if (!profile)
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		if (m_Engine.Snapshots().Current() != objects
			|| m_Engine.Types().Current() != types)
		{
			return {.Error = BlueprintEvidenceSourceError::DependencyChanged};
		}
		if (request.ProfileId != profile->Definition.Id)
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		return {
			.Error = BlueprintEvidenceSourceError::None,
			.Profile = std::move(profile)
		};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = BlueprintEvidenceSourceError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = BlueprintEvidenceSourceError::InternalError};
	}
}

std::size_t BlueprintBytecodeRuntimeSource::RetainedGenerationCount() const noexcept
{
	std::lock_guard lock(m_Mutex);
	return m_States.size();
}

bool BlueprintBytecodeRuntimeSource::BuildLayoutWitness(
	const BlueprintEvidenceBinding& binding,
	BlueprintScriptArrayLayoutWitness& witness) const noexcept
{
	witness = {};
	if (!IsScriptLayoutReportValid())
		return false;
	const OffsetReport* report = m_Context->FindOffset("ufunction.script");
	if (!report
		|| report->Value > (std::numeric_limits<std::uint32_t>::max)())
	{
		return false;
	}
	witness.Binding = binding;
	witness.Source =
		"engine_context.scored_runtime_blueprint_script_validation_v2+windows_x64_tarray_u8";
	witness.Validated = true;
	witness.ScriptFieldOffset = static_cast<std::uint32_t>(report->Value);
	witness.HeaderByteWidth = 16;
	witness.DataPointerOffset = 0;
	witness.DataPointerWidth = 8;
	witness.NumOffset = 8;
	witness.NumWidth = 4;
	witness.MaxOffset = 12;
	witness.MaxWidth = 4;
	witness.ElementWidth = 1;
	witness.CountsSigned = true;
	witness.ByteOrder = BlueprintScriptArrayByteOrder::LittleEndian;
	witness.EvidenceFingerprint =
		ComputeBlueprintScriptArrayLayoutWitnessFingerprint(witness);
	return IsBlueprintScriptArrayLayoutWitnessValid(witness);
}

void BlueprintBytecodeRuntimeSource::PruneRetiredStatesLocked(
	const BlueprintEvidenceBinding& currentBinding) noexcept
{
	for (auto iterator = m_States.begin(); iterator != m_States.end();)
	{
		const std::shared_ptr<GenerationState>& state = *iterator;
		if (!state
			|| (!SameBinding(state->Binding, currentBinding)
				&& state->Source.InFlight() == 0
				&& state->Source.StopAndDrain(std::chrono::milliseconds(0))))
		{
			if (state)
				state->Evidence.Stop();
			iterator = m_States.erase(iterator);
			continue;
		}
		++iterator;
	}
}

std::shared_ptr<BlueprintBytecodeRuntimeSource::GenerationState>
BlueprintBytecodeRuntimeSource::AcquireGenerationState(
	const BlueprintEvidenceBinding& binding,
	BlueprintEvidenceSourceError& error) noexcept
{
	error = BlueprintEvidenceSourceError::Unavailable;
	try
	{
		if (!IsCaptureConfigured())
			return nullptr;
		const std::shared_ptr<const EngineSnapshot> objects =
			m_Engine.Snapshots().Current();
		const std::shared_ptr<const TypeSnapshot> types = m_Engine.Types().Current();
		if (!objects || !types)
			return nullptr;
		const BlueprintEvidenceBinding current = BindingFor(*objects, *types);
		if (!SameBinding(binding, current))
		{
			error = BlueprintEvidenceSourceError::DependencyChanged;
			return nullptr;
		}

		std::lock_guard lock(m_Mutex);
		if (m_Barrier.IsStopping() || m_Drained)
		{
			error = BlueprintEvidenceSourceError::Stopped;
			return nullptr;
		}
		PruneRetiredStatesLocked(current);
		const auto existing = std::find_if(
			m_States.begin(),
			m_States.end(),
			[&current](const std::shared_ptr<GenerationState>& state) {
				return state && SameBinding(state->Binding, current);
			});
		if (existing != m_States.end())
		{
			error = BlueprintEvidenceSourceError::None;
			return *existing;
		}
		if (m_States.size() >= kMaxRetainedGenerationStates)
		{
			error = BlueprintEvidenceSourceError::LimitExceeded;
			return nullptr;
		}

		BlueprintScriptArrayLayoutWitness layout;
		if (!BuildLayoutWitness(current, layout))
		{
			error = BlueprintEvidenceSourceError::InvalidEvidence;
			return nullptr;
		}
		auto state = std::make_shared<GenerationState>(
			m_Engine,
			m_GameThread,
			current,
			std::move(layout));
		if (!state->Evidence.IsConfigured() || !state->Source.IsConfigured())
		{
			error = BlueprintEvidenceSourceError::InvalidEvidence;
			return nullptr;
		}
		m_States.push_back(state);
		error = BlueprintEvidenceSourceError::None;
		return state;
	}
	catch (const std::bad_alloc&)
	{
		error = BlueprintEvidenceSourceError::AllocationFailed;
		return nullptr;
	}
	catch (...)
	{
		error = BlueprintEvidenceSourceError::InternalError;
		return nullptr;
	}
}

BlueprintBytecodeCaptureResult BlueprintBytecodeRuntimeSource::Capture(
	const BlueprintBytecodeCaptureRequest& request)
{
	auto lease = m_Barrier.Enter();
	if (!lease.OwnedWorkAllowed())
		return {.Error = BlueprintEvidenceSourceError::Stopped};
	BlueprintEvidenceSourceError error = BlueprintEvidenceSourceError::Unavailable;
	std::shared_ptr<GenerationState> state =
		AcquireGenerationState(request.Binding, error);
	if (!state)
		return {.Error = error};
	return state->Source.Capture(request);
}

bool BlueprintBytecodeRuntimeSource::StopAndDrain(
	const std::chrono::milliseconds timeout)
{
	if (timeout.count() < 0)
		return false;
	m_Barrier.BeginStopping();
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	if (!m_Barrier.WaitForDrain(timeout))
		return false;

	std::vector<std::shared_ptr<GenerationState>> states;
	try
	{
		std::lock_guard lock(m_Mutex);
		if (m_Drained)
			return true;
		states = m_States;
	}
	catch (...)
	{
		return false;
	}

	for (const std::shared_ptr<GenerationState>& state : states)
	{
		if (!state)
			continue;
		const auto now = std::chrono::steady_clock::now();
		const auto remaining = now < deadline
			? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
			: std::chrono::milliseconds(0);
		if (!state->Source.StopAndDrain(remaining))
			return false;
	}
	for (const std::shared_ptr<GenerationState>& state : states)
	{
		if (state)
			state->Evidence.Stop();
	}
	{
		std::lock_guard lock(m_Mutex);
		m_Drained = true;
	}
	return true;
}

} // namespace UExplorer::Runtime
