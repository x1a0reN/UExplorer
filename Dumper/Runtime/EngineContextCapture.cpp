#include "EngineContextCapture.h"

#include "OffsetFinder/OffsetFinder.h"
#include "OffsetFinder/Offsets.h"
#include "Settings.h"
#include "Unreal/ObjectArray.h"

#include <Windows.h>

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
	builder.AddOffset(ModuleOffset("gworld", Off::InSDK::World::GWorld, false, "reference_scan"));
	builder.AddOffset(ModuleOffset("gengine", Off::InSDK::Engine::GEngine, false, "reference_scan"));

	builder.AddOffset(MemberOffset("uobject.flags", Off::UObject::Flags, true));
	builder.AddOffset(MemberOffset("uobject.index", Off::UObject::Index, true));
	builder.AddOffset(MemberOffset("uobject.class", Off::UObject::Class, true));
	builder.AddOffset(MemberOffset("uobject.name", Off::UObject::Name, true));
	builder.AddOffset(MemberOffset("uobject.outer", Off::UObject::Outer, true));
	builder.AddOffset(MemberOffset("ustruct.super_struct", Off::UStruct::SuperStruct, true));
	builder.AddOffset(MemberOffset("ustruct.children", Off::UStruct::Children, true));
	builder.AddOffset(MemberOffset("ustruct.child_properties", Off::UStruct::ChildProperties, Settings::Internal::bUseFProperty));
	builder.AddOffset(MemberOffset("ustruct.size", Off::UStruct::Size, true));
	builder.AddOffset(MemberOffset("uclass.cast_flags", Off::UClass::CastFlags, false));
	builder.AddOffset(MemberOffset("uclass.default_object", Off::UClass::ClassDefaultObject, true));
	builder.AddOffset(MemberOffset("ufunction.function_flags", Off::UFunction::FunctionFlags, true));
	builder.AddOffset(MemberOffset("ufunction.exec_function", Off::UFunction::ExecFunction, false));
	builder.AddOffset(MemberOffset("ufunction.script", Off::UFunction::Script, false, "scored_runtime_validation"));
	builder.AddOffset(MemberOffset("property.array_dim", Off::Property::ArrayDim, true));
	builder.AddOffset(MemberOffset("property.element_size", Off::Property::ElementSize, true));
	builder.AddOffset(MemberOffset("property.flags", Off::Property::PropertyFlags, true));
	builder.AddOffset(MemberOffset("property.offset_internal", Off::Property::Offset_Internal, true));
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
