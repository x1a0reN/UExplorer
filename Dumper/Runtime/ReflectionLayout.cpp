#include "ReflectionLayout.h"

#include "PropertyCodec.h"
#include "SafeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <span>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaximumSourceBytes = ReflectionLayoutLimits::MaxSourceBytes;
constexpr std::size_t kMaximumWitnessIdBytes = ReflectionLayoutLimits::MaxWitnessIdBytes;
constexpr std::size_t kMaximumWitnessNameBytes = ReflectionLayoutLimits::MaxWitnessNameBytes;
constexpr std::size_t kMaximumFields = ReflectionLayoutLimits::MaxFields;
constexpr std::size_t kMaximumWitnesses = ReflectionLayoutLimits::MaxWitnesses;
constexpr std::int32_t kMaximumRecordSize = ReflectionLayoutLimits::MaxRecordSize;

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
	switch (system)
	{
	case ReflectionPropertySystem::FProperty: return kFPropertyFields;
	case ReflectionPropertySystem::UProperty: return kUPropertyFields;
	case ReflectionPropertySystem::Unavailable: return {};
	}
	return {};
}

std::int32_t FieldWidth(
	const ReflectionField field,
	const EngineNameProfile& names) noexcept
{
	switch (ValueKindFor(field))
	{
	case ReflectionFieldValueKind::UInt8: return 1;
	case ReflectionFieldValueKind::Int32: return 4;
	case ReflectionFieldValueKind::UInt64: return 8;
	case ReflectionFieldValueKind::Pointer: return static_cast<std::int32_t>(sizeof(std::uintptr_t));
	case ReflectionFieldValueKind::FName: return names.FNameSize;
	}
	return -1;
}

bool IsDerivedPropertyRecord(const ReflectionRecordKind kind) noexcept
{
	switch (kind)
	{
	case ReflectionRecordKind::BoolProperty:
	case ReflectionRecordKind::ByteProperty:
	case ReflectionRecordKind::ObjectProperty:
	case ReflectionRecordKind::StructProperty:
	case ReflectionRecordKind::ArrayProperty:
	case ReflectionRecordKind::MapProperty:
	case ReflectionRecordKind::SetProperty:
	case ReflectionRecordKind::EnumProperty:
		return true;
	default:
		return false;
	}
}

bool ShareRecordStorage(
	const ReflectionPropertySystem system,
	const ReflectionRecordKind left,
	const ReflectionRecordKind right) noexcept
{
	if (left == right)
		return true;
	if ((left == ReflectionRecordKind::Property && IsDerivedPropertyRecord(right))
		|| (right == ReflectionRecordKind::Property && IsDerivedPropertyRecord(left)))
	{
		return true;
	}
	if (system == ReflectionPropertySystem::FProperty)
	{
		return (left == ReflectionRecordKind::FField
				&& (right == ReflectionRecordKind::Property || IsDerivedPropertyRecord(right)))
			|| (right == ReflectionRecordKind::FField
				&& (left == ReflectionRecordKind::Property || IsDerivedPropertyRecord(left)));
	}
	if (system == ReflectionPropertySystem::UProperty)
	{
		return (left == ReflectionRecordKind::UField
				&& (right == ReflectionRecordKind::Property || IsDerivedPropertyRecord(right)))
			|| (right == ReflectionRecordKind::UField
				&& (left == ReflectionRecordKind::Property || IsDerivedPropertyRecord(left)));
	}
	return false;
}

bool RangesOverlap(
	const std::int32_t leftOffset,
	const std::int32_t leftWidth,
	const std::int32_t rightOffset,
	const std::int32_t rightWidth) noexcept
{
	const std::int64_t leftEnd = static_cast<std::int64_t>(leftOffset) + leftWidth;
	const std::int64_t rightEnd = static_cast<std::int64_t>(rightOffset) + rightWidth;
	return leftOffset < rightEnd && rightOffset < leftEnd;
}

bool TryAddAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	const std::size_t width,
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
	std::uintptr_t end = 0;
	return CheckedAddressRange(address, width, end);
}

template<typename T>
MemoryResult ReadStable(const std::uintptr_t address, T& value) noexcept
{
	T first{};
	T second{};
	MemoryResult memory = ReadValue(address, first);
	if (!memory.Ok())
		return memory;
	memory = ReadValue(address, second);
	if (!memory.Ok())
		return memory;
	if (std::memcmp(&first, &second, sizeof(T)) != 0)
		return {.Error = MemoryError::ValueMismatch};
	value = first;
	return {.BytesProcessed = sizeof(T)};
}

bool IsPointerField(const ReflectionField field) noexcept
{
	return ValueKindFor(field) == ReflectionFieldValueKind::Pointer;
}

bool IsWitnessValueSemantic(
	const ReflectionField field,
	const std::uint64_t value) noexcept
{
	switch (field)
	{
	case ReflectionField::PropertyArrayDim:
		return value > 0 && value <= 1024;
	case ReflectionField::PropertyElementSize:
		return value > 0 && value <= 16 * 1024 * 1024;
	case ReflectionField::PropertyOffset:
		return value <= 1024ull * 1024ull * 1024ull;
	case ReflectionField::StructPropertiesSize:
		return value <= 1024ull * 1024ull * 1024ull;
	case ReflectionField::StructMinAlignment:
		return value > 0 && value <= 4096 && (value & (value - 1)) == 0;
	case ReflectionField::FFieldClassCastFlags:
		return value != 0;
	case ReflectionField::BoolFieldSize:
	case ReflectionField::BoolByteMask:
	case ReflectionField::BoolFieldMask:
		return value > 0 && value <= 0xFF;
	case ReflectionField::BoolByteOffset:
		return value <= 0xFF;
	default:
		return true;
	}
}

std::uint64_t ReadUnsigned(
	const std::uintptr_t address,
	const ReflectionFieldValueKind kind,
	MemoryResult& memory) noexcept
{
	memory = {};
	switch (kind)
	{
	case ReflectionFieldValueKind::UInt8:
	{
		std::uint8_t value = 0;
		memory = ReadStable(address, value);
		return value;
	}
	case ReflectionFieldValueKind::Int32:
	{
		std::int32_t value = 0;
		memory = ReadStable(address, value);
		return value < 0 ? (std::numeric_limits<std::uint64_t>::max)() : static_cast<std::uint64_t>(value);
	}
	case ReflectionFieldValueKind::UInt64:
	{
		std::uint64_t value = 0;
		memory = ReadStable(address, value);
		return value;
	}
	case ReflectionFieldValueKind::Pointer:
	{
		std::uintptr_t value = 0;
		memory = ReadStable(address, value);
		return static_cast<std::uint64_t>(value);
	}
	case ReflectionFieldValueKind::FName:
		memory = {.Error = MemoryError::InvalidRange};
		return 0;
	}
	memory = {.Error = MemoryError::InvalidRange};
	return 0;
}

void HashBytes(std::uint64_t& hash, const void* data, const std::size_t size) noexcept
{
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	for (std::size_t index = 0; index < size; ++index)
	{
		hash ^= bytes[index];
		hash *= 1099511628211ull;
	}
}

template<typename T>
void HashValue(std::uint64_t& hash, const T& value) noexcept
{
	HashBytes(hash, &value, sizeof(value));
}

std::uint64_t LayoutFingerprint(
	const std::uint64_t contextGeneration,
	const ReflectionPropertySystem system,
	const std::map<ReflectionField, ReflectionFieldReport>& fields) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	HashValue(hash, contextGeneration);
	HashValue(hash, system);
	for (const auto& [field, report] : fields)
	{
		HashValue(hash, field);
		HashValue(hash, report.Offset);
		HashValue(hash, report.Width);
		HashValue(hash, report.ContainerSize);
	}
	return hash == 0 ? 1 : hash;
}

ReflectionLayoutValidationResult Failure(
	const ReflectionValidationError error,
	std::string reason,
	const std::map<ReflectionField, ReflectionFieldReport>& reports)
{
	ReflectionLayoutValidationResult result{
		.Error = error,
		.Reason = std::move(reason)
	};
	result.FieldReports.reserve(reports.size());
	for (const auto& [field, report] : reports)
	{
		(void)field;
		result.FieldReports.push_back(report);
	}
	return result;
}

} // namespace

const char* ToString(const ReflectionPropertySystem system) noexcept
{
	switch (system)
	{
	case ReflectionPropertySystem::Unavailable: return "unavailable";
	case ReflectionPropertySystem::UProperty: return "uproperty";
	case ReflectionPropertySystem::FProperty: return "fproperty";
	}
	return "unavailable";
}

const char* ToString(const ReflectionRecordKind kind) noexcept
{
	switch (kind)
	{
	case ReflectionRecordKind::UStruct: return "ustruct";
	case ReflectionRecordKind::UField: return "ufield";
	case ReflectionRecordKind::FField: return "ffield";
	case ReflectionRecordKind::FFieldClass: return "ffield_class";
	case ReflectionRecordKind::Property: return "property";
	case ReflectionRecordKind::BoolProperty: return "bool_property";
	case ReflectionRecordKind::ByteProperty: return "byte_property";
	case ReflectionRecordKind::ObjectProperty: return "object_property";
	case ReflectionRecordKind::StructProperty: return "struct_property";
	case ReflectionRecordKind::ArrayProperty: return "array_property";
	case ReflectionRecordKind::MapProperty: return "map_property";
	case ReflectionRecordKind::SetProperty: return "set_property";
	case ReflectionRecordKind::EnumProperty: return "enum_property";
	}
	return "property";
}

const char* ToString(const ReflectionField field) noexcept
{
	switch (field)
	{
	case ReflectionField::StructSuper: return "ustruct.super";
	case ReflectionField::StructChildren: return "ustruct.children";
	case ReflectionField::StructChildProperties: return "ustruct.child_properties";
	case ReflectionField::StructPropertiesSize: return "ustruct.properties_size";
	case ReflectionField::StructMinAlignment: return "ustruct.min_alignment";
	case ReflectionField::UFieldNext: return "ufield.next";
	case ReflectionField::FFieldClass: return "ffield.class";
	case ReflectionField::FFieldNext: return "ffield.next";
	case ReflectionField::FFieldName: return "ffield.name";
	case ReflectionField::FFieldClassCastFlags: return "ffield_class.cast_flags";
	case ReflectionField::PropertyArrayDim: return "property.array_dim";
	case ReflectionField::PropertyElementSize: return "property.element_size";
	case ReflectionField::PropertyFlags: return "property.flags";
	case ReflectionField::PropertyOffset: return "property.offset";
	case ReflectionField::BoolFieldSize: return "bool_property.field_size";
	case ReflectionField::BoolByteOffset: return "bool_property.byte_offset";
	case ReflectionField::BoolByteMask: return "bool_property.byte_mask";
	case ReflectionField::BoolFieldMask: return "bool_property.field_mask";
	case ReflectionField::BytePropertyEnum: return "byte_property.enum";
	case ReflectionField::ObjectPropertyClass: return "object_property.class";
	case ReflectionField::StructPropertyStruct: return "struct_property.struct";
	case ReflectionField::ArrayPropertyInner: return "array_property.inner";
	case ReflectionField::MapPropertyKey: return "map_property.key";
	case ReflectionField::MapPropertyValue: return "map_property.value";
	case ReflectionField::SetPropertyElement: return "set_property.element";
	case ReflectionField::EnumPropertyUnderlying: return "enum_property.underlying";
	case ReflectionField::EnumPropertyEnum: return "enum_property.enum";
	}
	return "unknown";
}

ReflectionRecordKind RecordKindFor(const ReflectionField field) noexcept
{
	switch (field)
	{
	case ReflectionField::StructSuper:
	case ReflectionField::StructChildren:
	case ReflectionField::StructChildProperties:
	case ReflectionField::StructPropertiesSize:
	case ReflectionField::StructMinAlignment: return ReflectionRecordKind::UStruct;
	case ReflectionField::UFieldNext: return ReflectionRecordKind::UField;
	case ReflectionField::FFieldClass:
	case ReflectionField::FFieldNext:
	case ReflectionField::FFieldName: return ReflectionRecordKind::FField;
	case ReflectionField::FFieldClassCastFlags: return ReflectionRecordKind::FFieldClass;
	case ReflectionField::PropertyArrayDim:
	case ReflectionField::PropertyElementSize:
	case ReflectionField::PropertyFlags:
	case ReflectionField::PropertyOffset: return ReflectionRecordKind::Property;
	case ReflectionField::BoolFieldSize:
	case ReflectionField::BoolByteOffset:
	case ReflectionField::BoolByteMask:
	case ReflectionField::BoolFieldMask: return ReflectionRecordKind::BoolProperty;
	case ReflectionField::BytePropertyEnum: return ReflectionRecordKind::ByteProperty;
	case ReflectionField::ObjectPropertyClass: return ReflectionRecordKind::ObjectProperty;
	case ReflectionField::StructPropertyStruct: return ReflectionRecordKind::StructProperty;
	case ReflectionField::ArrayPropertyInner: return ReflectionRecordKind::ArrayProperty;
	case ReflectionField::MapPropertyKey:
	case ReflectionField::MapPropertyValue: return ReflectionRecordKind::MapProperty;
	case ReflectionField::SetPropertyElement: return ReflectionRecordKind::SetProperty;
	case ReflectionField::EnumPropertyUnderlying:
	case ReflectionField::EnumPropertyEnum: return ReflectionRecordKind::EnumProperty;
	}
	return ReflectionRecordKind::Property;
}

const char* ToString(const ReflectionFieldValueKind kind) noexcept
{
	switch (kind)
	{
	case ReflectionFieldValueKind::UInt8: return "uint8";
	case ReflectionFieldValueKind::Int32: return "int32";
	case ReflectionFieldValueKind::UInt64: return "uint64";
	case ReflectionFieldValueKind::Pointer: return "pointer";
	case ReflectionFieldValueKind::FName: return "fname";
	}
	return "unknown";
}

ReflectionFieldValueKind ValueKindFor(const ReflectionField field) noexcept
{
	switch (field)
	{
	case ReflectionField::BoolFieldSize:
	case ReflectionField::BoolByteOffset:
	case ReflectionField::BoolByteMask:
	case ReflectionField::BoolFieldMask: return ReflectionFieldValueKind::UInt8;
	case ReflectionField::StructPropertiesSize:
	case ReflectionField::StructMinAlignment:
	case ReflectionField::PropertyArrayDim:
	case ReflectionField::PropertyElementSize:
	case ReflectionField::PropertyOffset: return ReflectionFieldValueKind::Int32;
	case ReflectionField::FFieldClassCastFlags:
	case ReflectionField::PropertyFlags: return ReflectionFieldValueKind::UInt64;
	case ReflectionField::FFieldName: return ReflectionFieldValueKind::FName;
	default: return ReflectionFieldValueKind::Pointer;
	}
}

const char* ToString(const ReflectionValidationError error) noexcept
{
	switch (error)
	{
	case ReflectionValidationError::None: return "NONE";
	case ReflectionValidationError::InvalidContextGeneration: return "REFLECTION_CONTEXT_GENERATION_INVALID";
	case ReflectionValidationError::PropertySystemUnavailable: return "REFLECTION_PROPERTY_SYSTEM_UNAVAILABLE";
	case ReflectionValidationError::SourceInvalid: return "REFLECTION_SOURCE_INVALID";
	case ReflectionValidationError::FieldLimitExceeded: return "REFLECTION_FIELD_LIMIT_EXCEEDED";
	case ReflectionValidationError::DuplicateField: return "REFLECTION_FIELD_DUPLICATE";
	case ReflectionValidationError::UnexpectedField: return "REFLECTION_FIELD_UNEXPECTED";
	case ReflectionValidationError::MissingRequiredField: return "REFLECTION_FIELD_MISSING";
	case ReflectionValidationError::FieldLayoutInvalid: return "REFLECTION_FIELD_LAYOUT_INVALID";
	case ReflectionValidationError::FieldOverlap: return "REFLECTION_FIELD_OVERLAP";
	case ReflectionValidationError::WitnessLimitExceeded: return "REFLECTION_WITNESS_LIMIT_EXCEEDED";
	case ReflectionValidationError::WitnessInvalid: return "REFLECTION_WITNESS_INVALID";
	case ReflectionValidationError::WitnessMissing: return "REFLECTION_WITNESS_MISSING";
	case ReflectionValidationError::WitnessMemoryUnavailable: return "REFLECTION_WITNESS_MEMORY_UNAVAILABLE";
	case ReflectionValidationError::WitnessValueMismatch: return "REFLECTION_WITNESS_VALUE_MISMATCH";
	case ReflectionValidationError::WitnessSemanticInvalid: return "REFLECTION_WITNESS_SEMANTIC_INVALID";
	case ReflectionValidationError::NameCodecUnavailable: return "REFLECTION_NAME_CODEC_UNAVAILABLE";
	case ReflectionValidationError::NameWitnessMismatch: return "REFLECTION_NAME_WITNESS_MISMATCH";
	case ReflectionValidationError::AllocationFailed: return "REFLECTION_ALLOCATION_FAILED";
	}
	return "REFLECTION_VALIDATION_UNKNOWN";
}

const ReflectionFieldReport* ReflectionLayout::Find(const ReflectionField field) const noexcept
{
	const auto it = m_Fields.find(field);
	return it == m_Fields.end() ? nullptr : &it->second;
}

ReflectionLayoutValidationResult ValidateReflectionLayout(
	const ReflectionLayoutCandidate& candidate,
	const EngineNameCodec& names) noexcept
{
	try
	{
		std::map<ReflectionField, ReflectionFieldReport> reports;
		if (candidate.ContextGeneration == 0)
			return Failure(ReflectionValidationError::InvalidContextGeneration,
				"Reflection context generation must be non-zero", reports);
		if (candidate.PropertySystem == ReflectionPropertySystem::Unavailable)
			return Failure(ReflectionValidationError::PropertySystemUnavailable,
				"The reflection property system must be explicit", reports);
		if (candidate.Source.empty() || candidate.Source.size() > kMaximumSourceBytes)
			return Failure(ReflectionValidationError::SourceInvalid,
				"Reflection source is empty or exceeds its byte limit", reports);
		const std::span<const ReflectionField> required = RequiredFields(candidate.PropertySystem);
		if (candidate.Fields.size() > kMaximumFields)
		{
			return Failure(ReflectionValidationError::FieldLimitExceeded,
				"The candidate field set exceeds the bounded limit", reports);
		}
		if (candidate.PropertySystem == ReflectionPropertySystem::FProperty
			&& !names.IsConfigured())
		{
			return Failure(ReflectionValidationError::NameCodecUnavailable,
				"FProperty reflection requires the immutable FName codec", reports);
		}

		for (const ReflectionFieldCandidate& field : candidate.Fields)
		{
			if (std::ranges::find(required, field.Field) == required.end())
				return Failure(ReflectionValidationError::UnexpectedField,
					"The candidate contains a field outside its property-system contract", reports);
			const std::int32_t width = FieldWidth(field.Field, names.Profile());
			ReflectionFieldReport report{
				.Field = field.Field,
				.RecordKind = RecordKindFor(field.Field),
				.ValueKind = ValueKindFor(field.Field),
				.Offset = field.Offset,
				.Width = width,
				.ContainerSize = field.ContainerSize,
				.Source = field.Source,
				.Checks = {
					"field_present_once",
					"field_width_exact",
					"field_within_bounded_record",
					"field_range_disjoint",
					"stable_semantic_witness"
				}
			};
			if (reports.contains(field.Field))
				return Failure(ReflectionValidationError::DuplicateField,
					"A reflection field appears more than once", reports);
			if (width <= 0
				|| field.Offset < 0
				|| field.ContainerSize <= 0
				|| field.ContainerSize > kMaximumRecordSize
				|| field.Offset > field.ContainerSize
				|| width > field.ContainerSize - field.Offset
				|| field.Source.empty()
				|| field.Source.size() > kMaximumSourceBytes)
			{
				report.ReasonCode = ToString(ReflectionValidationError::FieldLayoutInvalid);
				report.Reason = "Field offset/width/source is outside the structural contract";
				reports.emplace(field.Field, std::move(report));
				return Failure(ReflectionValidationError::FieldLayoutInvalid,
					"A reflection field failed structural validation", reports);
			}
			reports.emplace(field.Field, std::move(report));
		}
		for (const ReflectionField field : required)
		{
			if (!reports.contains(field))
				return Failure(ReflectionValidationError::MissingRequiredField,
					std::string("Missing required reflection field: ") + ToString(field), reports);
		}
		for (auto left = reports.begin(); left != reports.end(); ++left)
		{
			for (auto right = std::next(left); right != reports.end(); ++right)
			{
				if (left->second.RecordKind == right->second.RecordKind
					&& left->second.ContainerSize != right->second.ContainerSize)
				{
					left->second.ReasonCode = ToString(ReflectionValidationError::FieldLayoutInvalid);
					left->second.Reason = std::string("Container size differs from ") + ToString(right->first);
					return Failure(ReflectionValidationError::FieldLayoutInvalid,
						"Fields in one reflection record disagree on its container size", reports);
				}
				if (ShareRecordStorage(
						candidate.PropertySystem,
						left->second.RecordKind,
						right->second.RecordKind)
					&& RangesOverlap(
						left->second.Offset,
						left->second.Width,
						right->second.Offset,
						right->second.Width))
				{
					left->second.ReasonCode = ToString(ReflectionValidationError::FieldOverlap);
					left->second.Reason = std::string("Overlaps ") + ToString(right->first);
					return Failure(ReflectionValidationError::FieldOverlap,
						"Reflection fields overlap within one record layout", reports);
				}
			}
		}

		if (candidate.Witnesses.size() > kMaximumWitnesses)
			return Failure(ReflectionValidationError::WitnessLimitExceeded,
				"Reflection witnesses exceed the bounded limit", reports);
		std::set<std::string> witnessIds;
		for (const ReflectionFieldWitness& witness : candidate.Witnesses)
		{
			auto report = reports.find(witness.Field);
			if (report == reports.end())
			{
				return Failure(ReflectionValidationError::WitnessInvalid,
					"A reflection witness references a field outside the validated layout", reports);
			}
			const auto failWitness = [&](
				const ReflectionValidationError error,
				const char* reason) {
				report->second.ReasonCode = ToString(error);
				report->second.Reason = reason;
				return Failure(error, reason, reports);
			};
			if (witness.Id.empty()
				|| witness.Id.size() > kMaximumWitnessIdBytes
				|| witness.BaseAddress == 0
				|| !witnessIds.emplace(witness.Id).second)
			{
				return failWitness(ReflectionValidationError::WitnessInvalid,
					"A reflection witness has an invalid id or base address");
			}
			std::uintptr_t fieldAddress = 0;
			if (!TryAddAddress(
				witness.BaseAddress,
				report->second.Offset,
				static_cast<std::size_t>(report->second.Width),
				fieldAddress))
			{
				return failWitness(ReflectionValidationError::WitnessInvalid,
					"A reflection witness field address overflowed");
			}

			if (report->second.ValueKind == ReflectionFieldValueKind::FName)
			{
				const auto* expected = std::get_if<std::string>(&witness.Expected);
				if (!expected || expected->empty() || expected->size() > kMaximumWitnessNameBytes)
					return failWitness(ReflectionValidationError::WitnessInvalid,
						"An FName witness has no bounded expected name");
				std::vector<std::byte> first(static_cast<std::size_t>(report->second.Width));
				MemoryResult memory = ReadMemory(fieldAddress, first);
				if (!memory.Ok())
					return failWitness(ReflectionValidationError::WitnessMemoryUnavailable,
						"An FName witness could not be read");
				const EngineNameResult decoded = names.DecodeFName(fieldAddress);
				if (!decoded.Ok())
					return failWitness(ReflectionValidationError::NameWitnessMismatch,
						"An FName witness could not be decoded");
				std::vector<std::byte> second(first.size());
				memory = ReadMemory(fieldAddress, second);
				if (!memory.Ok())
					return failWitness(ReflectionValidationError::WitnessMemoryUnavailable,
						"An FName witness could not be re-read");
				if (first != second || decoded.Value != *expected)
					return failWitness(ReflectionValidationError::NameWitnessMismatch,
						"An FName witness changed or decoded to a different semantic name");
			}
			else
			{
				const auto* expected = std::get_if<std::uint64_t>(&witness.Expected);
				if (!expected)
					return failWitness(ReflectionValidationError::WitnessInvalid,
						"A scalar reflection witness has no raw expected value");
				MemoryResult memory;
				const std::uint64_t observed = ReadUnsigned(
					fieldAddress,
					report->second.ValueKind,
					memory);
				if (!memory.Ok())
					return failWitness(ReflectionValidationError::WitnessMemoryUnavailable,
						"A scalar reflection witness could not be read coherently");
				if (observed != *expected)
					return failWitness(ReflectionValidationError::WitnessValueMismatch,
						"A scalar reflection witness did not match its semantic expectation");
				if (!IsWitnessValueSemantic(witness.Field, observed))
					return failWitness(ReflectionValidationError::WitnessSemanticInvalid,
						"A scalar reflection witness matched bytes but failed semantic bounds");
				if (IsPointerField(witness.Field))
				{
					if (observed == 0
						|| observed > (std::numeric_limits<std::uintptr_t>::max)()
						|| !ValidateReadableMemory(
							static_cast<std::uintptr_t>(observed),
							1).Ok())
					{
						return failWitness(ReflectionValidationError::WitnessSemanticInvalid,
							"A pointer witness does not reference readable memory");
					}
				}
			}
			report->second.WitnessIds.push_back(witness.Id);
		}
		for (auto& [field, report] : reports)
		{
			(void)field;
			if (report.WitnessIds.empty())
			{
				report.ReasonCode = ToString(ReflectionValidationError::WitnessMissing);
				report.Reason = "No semantic witness validated this field";
				return Failure(ReflectionValidationError::WitnessMissing,
					"Every required reflection field needs a semantic witness", reports);
			}
			report.Validated = true;
		}

		auto layout = std::shared_ptr<ReflectionLayout>(new ReflectionLayout());
		layout->m_ContextGeneration = candidate.ContextGeneration;
		layout->m_PropertySystem = candidate.PropertySystem;
		layout->m_ValidatedOnThreadId = GetCurrentThreadId();
		layout->m_Source = candidate.Source;
		layout->m_Fields = reports;
		layout->m_Fingerprint = LayoutFingerprint(
			layout->m_ContextGeneration,
			layout->m_PropertySystem,
			layout->m_Fields);
		ReflectionLayoutValidationResult result{
			.Layout = std::shared_ptr<const ReflectionLayout>(std::move(layout))
		};
		result.FieldReports.reserve(reports.size());
		for (const auto& [field, report] : reports)
		{
			(void)field;
			result.FieldReports.push_back(report);
		}
		return result;
	}
	catch (const std::bad_alloc&)
	{
		return {
			.Error = ReflectionValidationError::AllocationFailed,
			.Reason = "Reflection validation could not allocate bounded evidence storage"
		};
	}
	catch (...)
	{
		return {
			.Error = ReflectionValidationError::WitnessMemoryUnavailable,
			.Reason = "Reflection validation raised an unexpected exception"
		};
	}
}

bool IsReflectionLayoutValid(
	const ReflectionLayout& layout,
	const std::uint64_t expectedContextGeneration) noexcept
{
	if (expectedContextGeneration == 0
		|| layout.ContextGeneration() != expectedContextGeneration
		|| layout.Fingerprint() == 0
		|| layout.ValidatedOnThreadId() == 0
		|| layout.PropertySystem() == ReflectionPropertySystem::Unavailable
		|| layout.Source().empty()
		|| layout.Source().size() > kMaximumSourceBytes)
	{
		return false;
	}
	const std::span<const ReflectionField> required = RequiredFields(layout.PropertySystem());
	if (layout.Fields().size() != required.size())
		return false;
	for (const ReflectionField field : required)
	{
		const ReflectionFieldReport* report = layout.Find(field);
		if (!report || !report->Validated || report->WitnessIds.empty())
			return false;
	}
	return layout.Fingerprint() == LayoutFingerprint(
		layout.ContextGeneration(),
		layout.PropertySystem(),
		layout.Fields());
}

bool ReflectionRuntimeSnapshot::IsLayoutConfigured(
	const std::uint64_t expectedContextGeneration) const noexcept
{
	return Layout
		&& IsReflectionLayoutValid(*Layout, expectedContextGeneration);
}

bool ReflectionRuntimeSnapshot::IsPropertyCodecConfigured(
	const std::uint64_t expectedContextGeneration) const noexcept
{
	return IsLayoutConfigured(expectedContextGeneration)
		&& Properties
		&& Properties->IsConfigured()
		&& Properties->Profile().ReflectionLayoutFingerprint == Layout->Fingerprint();
}

bool ReflectionRuntimeSnapshot::IsConfigured(
	const std::uint64_t expectedContextGeneration) const noexcept
{
	return IsPropertyCodecConfigured(expectedContextGeneration);
}

} // namespace UExplorer::Runtime
