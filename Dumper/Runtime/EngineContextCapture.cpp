#include "EngineContextCapture.h"

#include "EngineNameCodec.h"
#include "OffsetFinder/OffsetDiscovery.h"
#include "OffsetFinder/OffsetFinder.h"
#include "OffsetFinder/Offsets.h"
#include "Settings.h"
#include "Unreal/NameArray.h"
#include "Unreal/ObjectArray.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

OffsetReport MakeOffsetReport(
	std::string name,
	const std::int64_t value,
	const bool required,
	const bool discovered,
	const bool validated,
	std::string source,
	std::vector<std::string> checks)
{
	OffsetReport report{
		.Name = std::move(name),
		.Value = value,
		.Required = required,
		.State = validated
			? ValidationState::Validated
			: (discovered ? ValidationState::Discovered : ValidationState::Missing),
		.Source = std::move(source),
		.Checks = std::move(checks)
	};
	report.Confidence = validated ? "medium" : (discovered ? "low" : "none");
	if (!validated)
	{
		report.ReasonCode = discovered ? "OFFSET_VALIDATION_FAILED" : "OFFSET_NOT_FOUND";
		report.Reason = discovered
			? "The discovered value did not pass its validation rules"
			: "No offset was discovered";
	}
	return report;
}

OffsetReport MemberOffset(
	std::string name,
	const std::int32_t value,
	const bool required,
	std::string source = "runtime_discovery")
{
	const bool discovered = value != OffsetFinder::OffsetNotFound;
	const bool validated = discovered && value > 0 && value <= 0x10000;
	return MakeOffsetReport(
		std::move(name), value, required, discovered, validated, std::move(source),
		{"positive_member_offset", "member_offset_in_range"});
}

OffsetReport FunctionScriptOffset()
{
	const OffsetFinder::FunctionScriptOffsetDiagnostics diagnostics =
		OffsetFinder::GetFunctionScriptOffsetDiagnostics();
	const std::int32_t value = Off::UFunction::Script;
	const bool discovered = value != OffsetFinder::OffsetNotFound
		&& value > 0
		&& diagnostics.SelectedOffset == value;
	const bool validated = discovered
		&& value <= 0x10000
		&& diagnostics.Confidence == "high"
		&& diagnostics.AnomalyTags.empty()
		&& diagnostics.SelectedScore > 0
		&& diagnostics.ScoreGapTop2 >= 180
		&& diagnostics.BpEndHits >= 4
		&& diagnostics.WeightedBpEndHits >= 16
		&& diagnostics.VerifyProbed >= 4
		&& diagnostics.VerifyHeaderValid >= 4
		&& diagnostics.VerifyEndHits >= 4
		&& diagnostics.VerifyFirstOpcodeValid >= 4
		&& diagnostics.VerifySizeSane >= 4
		&& diagnostics.VerifyEndRate >= 55
		&& diagnostics.VerifyOpcodeRate >= 80;
	OffsetReport report = MakeOffsetReport(
		"ufunction.script",
		value,
		false,
		discovered,
		validated,
		"scored_runtime_blueprint_script_validation_v2",
		{
			"selected_offset_matches_runtime_discovery",
			"unique_candidate_score_gap_at_least_180",
			"blueprint_end_token_witnesses_at_least_4",
			"stable_tarray_headers_and_sane_sizes",
			"verification_end_rate_at_least_55_percent",
			"verification_opcode_rate_at_least_80_percent",
			"no_layout_anomaly_tags"
		});
	report.Confidence = diagnostics.Confidence;
	if (diagnostics.SelectedOffset > 0)
		report.Candidates.push_back(diagnostics.SelectedOffset);
	if (!validated)
	{
		report.ReasonCode = discovered
			? "UFUNCTION_SCRIPT_WITNESS_INSUFFICIENT"
			: "UFUNCTION_SCRIPT_OFFSET_NOT_FOUND";
		report.Reason = discovered
			? "The selected UFunction::Script candidate did not satisfy the high-confidence multi-function witness gate"
			: "No UFunction::Script candidate matched the recorded runtime discovery";
	}
	return report;
}

OffsetReport ModuleOffset(
	std::string name,
	const std::int32_t value,
	const bool required,
	std::string source = "runtime_discovery")
{
	const bool discovered = value > 0;
	return MakeOffsetReport(
		std::move(name), value, required, discovered, discovered, std::move(source),
		{"positive_module_relative_offset"});
}

OffsetReport VTableIndex(
	std::string name,
	const std::int32_t value,
	const bool required,
	std::string source)
{
	const bool discovered = value >= 0;
	const bool validated = discovered && value > 0 && value <= 512;
	return MakeOffsetReport(
		std::move(name), value, required, discovered, validated, std::move(source),
		{"positive_vtable_index", "vtable_index_at_most_512"});
}

OffsetReport GlobalPointerOffset(
	std::string name,
	const std::int32_t value,
	const bool required,
	const OffsetFinder::GlobalPointerDiscoveryReport& discovery)
{
	const bool discovered = !discovery.Candidates.empty();
	const bool validated = discovery.Ok()
		&& value == discovery.SelectedOffset;
	OffsetReport report = MakeOffsetReport(
		std::move(name),
		value,
		required,
		discovered,
		validated,
		discovery.Source,
		discovery.Checks);
	for (const OffsetFinder::GlobalPointerCandidateEvidence& candidate : discovery.Candidates)
	{
		if (candidate.ModuleOffset < 0
			|| std::ranges::find(report.Candidates, candidate.ModuleOffset)
				!= report.Candidates.end())
		{
			continue;
		}
		report.Candidates.push_back(candidate.ModuleOffset);
	}
	report.Confidence = discovery.Confidence;
	if (!validated)
	{
		report.ReasonCode = OffsetFinder::ToString(discovery.Error);
		report.Reason = discovery.Error == OffsetFinder::GlobalPointerDiscoveryError::AmbiguousCandidates
			? "More than one stable, typed module-data cross-reference remained"
			: "No unique stable, typed module-data cross-reference passed validation";
	}
	return report;
}

EngineNameProfile CaptureNameProfile()
{
	FNameStorageLayout layout;
	const bool layoutCaptured = NameArray::TryCaptureRuntimeLayout(layout);
	EngineNameProfile profile{
		.Storage = layout.Address == 0
			? EngineNameStorageKind::Unavailable
			: (layout.UsesNamePool
				? EngineNameStorageKind::NamePool
				: EngineNameStorageKind::ChunkedArray),
		.StorageAddress = layout.Address,
		.FNameSize = Off::InSDK::Name::FNameSize,
		.ComparisonIndexOffset = Off::FName::CompIdx,
		.NumberOffset = Off::FName::Number,
		.BlockOffsetBits = layout.BlockOffsetBits,
		.EntryStride = layout.EntryStride,
		.ChunksStart = layout.ChunksStart,
		.MaxChunkIndexOffset = layout.MaxChunkIndexOffset,
		.NumElementsOffset = layout.NumElementsOffset,
		.ByteCursorOffset = layout.ByteCursorOffset,
		.EntryStringOffset = layout.EntryStringOffset,
		.EntryHeaderOffset = layout.EntryHeaderOffset,
		.EntryIndexOffset = layout.EntryIndexOffset,
		.EntryLengthShift = layout.EntryLengthShift,
		.UsesOutlineNumber = layout.UsesOutlineNumber,
		.Source = "runtime_name_storage_layout",
		.Checks = {
			"storage_address_nonzero",
			"fname_fields_within_size",
			"storage_offsets_bounded",
			"entry_encoding_layout_bounded",
			"name_index_zero_decodes_none"
		}
	};
	const bool structurallyValid = layoutCaptured && IsEngineNameProfileLayoutValid(profile);
	profile.Validated = structurallyValid;
	EngineNameResult witness;
	if (structurallyValid)
		witness = EngineNameCodec(profile).Decode(0);
	profile.Validated = structurallyValid && witness.Ok() && witness.Value == "None";
	if (!profile.Validated)
	{
		if (!layoutCaptured)
		{
			profile.ReasonCode = "NAME_STORAGE_LAYOUT_NOT_CAPTURED";
			profile.Reason = "The initialized name storage did not expose a complete immutable layout";
		}
		else if (!structurallyValid)
		{
			profile.ReasonCode = "NAME_STORAGE_LAYOUT_INVALID";
			profile.Reason = "The captured name storage layout did not pass structural validation";
		}
		else
		{
			profile.ReasonCode = "NAME_STORAGE_SEMANTIC_VALIDATION_FAILED";
			profile.Reason = "Name index zero did not decode exactly to None through checked memory";
		}
	}
	return profile;
}

} // namespace

std::shared_ptr<const EngineContext> CaptureEngineContext(const std::uint64_t generation)
{
	const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
	const auto objectArrayAddress = reinterpret_cast<std::uintptr_t>(ObjectArray::DEBUGGetGObjects());
	const std::int32_t objectCount = ObjectArray::Num();
	const std::int32_t objectCapacity = ObjectArray::Max();
	const bool objectIdentityLayoutValidated = ObjectArray::ValidateIdentityLayout();
	const FUObjectItemIdentityLayout& objectIdentityLayout = ObjectArray::GetIdentityLayout();

	EngineContextBuilder builder(generation);
	builder.SetIdentity(
		moduleBase,
		objectArrayAddress,
		GetCurrentProcessId(),
		objectCount,
		Settings::Generator::GameName,
		Settings::Generator::GameVersion);
	builder.SetProfile({
		.UsesFProperty = Settings::Internal::bUseFProperty,
		.UsesNamePool = Settings::Internal::bUseNamePool,
		.UsesLargeWorldCoordinates = Settings::Internal::bUseLargeWorldCoordinates,
		.UsesCasePreservingName = Settings::Internal::bUseCasePreservingName,
		.EnumNameOnly = Settings::Internal::bIsEnumNameOnly,
		.SmallEnumValue = Settings::Internal::bIsSmallEnumValue
	});
	builder.SetNameProfile(CaptureNameProfile());

	const bool objectArrayValidated = Off::InSDK::ObjArray::GObjects > 0
		&& objectArrayAddress != 0
		&& objectCount >= 0
		&& objectCapacity >= objectCount;
	builder.AddOffset(MakeOffsetReport(
		"gobjects",
		Off::InSDK::ObjArray::GObjects,
		true,
		Off::InSDK::ObjArray::GObjects > 0,
		objectArrayValidated,
		"object_array_scan",
		{"positive_module_relative_offset", "object_array_address_readable", "count_within_capacity"}));
	OffsetReport serialOffsetReport{
		.Name = "fuobjectitem.serial_number",
		.Value = objectIdentityLayout.SerialOffset,
		.Required = false,
		.State = objectIdentityLayoutValidated
			? ValidationState::Validated
			: ValidationState::Missing,
		.Source = objectIdentityLayout.Profile.empty()
			? "unsupported_fuobjectitem_profile"
			: objectIdentityLayout.Profile,
		.Checks = objectIdentityLayout.Checks
	};
	if (!objectIdentityLayoutValidated)
	{
		serialOffsetReport.ReasonCode = objectIdentityLayout.ReasonCode.empty()
			? "FUOBJECTITEM_LAYOUT_NOT_VALIDATED"
			: objectIdentityLayout.ReasonCode;
		serialOffsetReport.Reason =
			"No supported FUObjectItem profile produced a stable positive serial witness";
	}
	builder.AddOffset(std::move(serialOffsetReport));
	const std::int32_t fNameSize = Off::InSDK::Name::FNameSize;
	builder.AddOffset(MakeOffsetReport(
		"fname.size",
		fNameSize,
		false,
		fNameSize > 0,
		fNameSize >= static_cast<std::int32_t>(sizeof(std::uint32_t)) && fNameSize <= 64,
		"runtime_name_layout",
		{"at_least_comparison_index_size", "fname_size_at_most_64"}));
	const auto nameFieldFits = [fNameSize](const std::int32_t offset) {
		return fNameSize >= static_cast<std::int32_t>(sizeof(std::uint32_t))
			&& offset >= 0
			&& offset <= fNameSize - static_cast<std::int32_t>(sizeof(std::uint32_t));
	};
	builder.AddOffset(MakeOffsetReport(
		"fname.comparison_index",
		Off::FName::CompIdx,
		false,
		Off::FName::CompIdx >= 0,
		nameFieldFits(Off::FName::CompIdx),
		"runtime_name_layout",
		{"non_negative_field_offset", "field_within_fname"}));
	builder.AddOffset(MakeOffsetReport(
		"fname.number",
		Off::FName::Number,
		false,
		Off::FName::Number >= 0,
		nameFieldFits(Off::FName::Number),
		"runtime_name_layout",
		{"non_negative_field_offset", "field_within_fname"}));
	builder.AddOffset(ModuleOffset("gnames", Off::InSDK::NameArray::GNames, false, "name_array_scan"));
	builder.AddOffset(GlobalPointerOffset(
		"gworld",
		Off::InSDK::World::GWorld,
		false,
		Off::InSDK::World::GetDiscoveryReport()));
	builder.AddOffset(GlobalPointerOffset(
		"gengine",
		Off::InSDK::Engine::GEngine,
		false,
		Off::InSDK::Engine::GetDiscoveryReport()));

	builder.AddOffset(MemberOffset("uobject.flags", Off::UObject::Flags, true));
	builder.AddOffset(MemberOffset("uobject.index", Off::UObject::Index, true));
	builder.AddOffset(MemberOffset("uobject.class", Off::UObject::Class, true));
	builder.AddOffset(MemberOffset("uobject.name", Off::UObject::Name, true));
	builder.AddOffset(MemberOffset("uobject.outer", Off::UObject::Outer, true));
	builder.AddOffset(MemberOffset("ustruct.super_struct", Off::UStruct::SuperStruct, false));
	builder.AddOffset(MemberOffset("ustruct.children", Off::UStruct::Children, false));
	builder.AddOffset(MemberOffset("ustruct.child_properties", Off::UStruct::ChildProperties, false));
	builder.AddOffset(MemberOffset("ustruct.size", Off::UStruct::Size, false));
	builder.AddOffset(MemberOffset("uclass.cast_flags", Off::UClass::CastFlags, true));
	builder.AddOffset(MemberOffset("uclass.default_object", Off::UClass::ClassDefaultObject, true));
	builder.AddOffset(MemberOffset("ufunction.function_flags", Off::UFunction::FunctionFlags, true));
	builder.AddOffset(MemberOffset("ufunction.exec_function", Off::UFunction::ExecFunction, false));
	builder.AddOffset(FunctionScriptOffset());
	builder.AddOffset(MemberOffset("property.array_dim", Off::Property::ArrayDim, false));
	builder.AddOffset(MemberOffset("property.element_size", Off::Property::ElementSize, false));
	builder.AddOffset(MemberOffset("property.flags", Off::Property::PropertyFlags, false));
	builder.AddOffset(MemberOffset("property.offset_internal", Off::Property::Offset_Internal, false));
	builder.AddOffset(MemberOffset("ulevel.actors", Off::InSDK::ULevel::Actors, false));

	builder.AddOffset(VTableIndex(
		"process_event.index",
		Off::InSDK::ProcessEvent::PEIndex,
		true,
		"vtable_signature_scan"));
	builder.AddOffset(ModuleOffset(
		"process_event.offset",
		Off::InSDK::ProcessEvent::PEOffset,
		true,
		"vtable_signature_scan"));
	builder.AddOffset(VTableIndex(
		"post_render.gvc_index",
		Off::InSDK::PostRender::GVCPostRenderIndex,
		false,
		Settings::PostRender::GVCPostRenderIndex >= 0 ? "validated_ini_override" : "vtable_callgraph_scan"));
	builder.AddOffset(VTableIndex(
		"post_render.hud_index",
		Off::InSDK::PostRender::HUDPostRenderIndex,
		false,
		Settings::PostRender::HUDPostRenderIndex >= 0 ? "validated_ini_override" : "vtable_signature_scan"));

	return builder.Build();
}

} // namespace UExplorer::Runtime
