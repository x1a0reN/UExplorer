#include "PropertyCodec.h"

#include "SafeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <type_traits>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::uint32_t kMaximumDescriptorFields = 1024;
constexpr std::uint32_t kMaximumEnumEntries = 65'536;
constexpr std::uint32_t kMaximumSparseSlots = 1'000'000;
constexpr std::size_t kMaximumSessionIdLength = 128;
constexpr std::size_t kMaximumDescriptorTextBytes = 4096;
constexpr std::size_t kMaximumErrorCodeBytes = 128;
constexpr std::int32_t kMaximumLayoutSize = 4096;

constexpr std::array<std::string_view, 3> kVectorFields{"X", "Y", "Z"};
constexpr std::array<std::string_view, 3> kRotatorFields{"Pitch", "Yaw", "Roll"};

bool HasNoNestedDescriptorMetadata(const PropertyDescriptor& descriptor) noexcept
{
	return !descriptor.Element
		&& !descriptor.Key
		&& !descriptor.Mapped
		&& descriptor.Fields.empty()
		&& descriptor.EnumEntries.empty()
		&& descriptor.BoolByteOffset == 0
		&& descriptor.BoolMask == 0
		&& descriptor.ElementStride == 0
		&& descriptor.ElementValueOffset == 0
		&& descriptor.MapKeyOffset == 0
		&& descriptor.MapValueOffset == 0;
}

CanonicalMathStructKind ClassifyCanonicalMathStructImpl(
	const PropertyDescriptor& descriptor) noexcept
{
	if (descriptor.Kind != PropertyKind::Struct
		|| descriptor.Fields.size() != 3
		|| descriptor.Element
		|| descriptor.Key
		|| descriptor.Mapped
		|| !descriptor.EnumEntries.empty()
		|| descriptor.BoolByteOffset != 0
		|| descriptor.BoolMask != 0
		|| descriptor.ElementStride != 0
		|| descriptor.ElementValueOffset != 0
		|| descriptor.MapKeyOffset != 0
		|| descriptor.MapValueOffset != 0)
	{
		return CanonicalMathStructKind::None;
	}

	CanonicalMathStructKind kind = CanonicalMathStructKind::None;
	const std::array<std::string_view, 3>* expectedFields = nullptr;
	if (descriptor.TypeName == "/Script/CoreUObject.Vector")
	{
		kind = CanonicalMathStructKind::Vector;
		expectedFields = &kVectorFields;
	}
	else if (descriptor.TypeName == "/Script/CoreUObject.Rotator")
	{
		kind = CanonicalMathStructKind::Rotator;
		expectedFields = &kRotatorFields;
	}
	else
	{
		return CanonicalMathStructKind::None;
	}

	PropertyKind componentKind = PropertyKind::Unknown;
	std::uint32_t componentSize = 0;
	for (std::size_t index = 0; index < descriptor.Fields.size(); ++index)
	{
		const PropertyFieldDescriptor& field = descriptor.Fields[index];
		if (field.Name != (*expectedFields)[index]
			|| !field.Descriptor
			|| (field.Descriptor->Kind != PropertyKind::Float
				&& field.Descriptor->Kind != PropertyKind::Double)
			|| field.Descriptor->TypeName.empty()
			|| !HasNoNestedDescriptorMetadata(*field.Descriptor))
		{
			return CanonicalMathStructKind::None;
		}
		const std::uint32_t exactSize = field.Descriptor->Kind == PropertyKind::Float
			? static_cast<std::uint32_t>(sizeof(float))
			: static_cast<std::uint32_t>(sizeof(double));
		if (index == 0)
		{
			componentKind = field.Descriptor->Kind;
			componentSize = exactSize;
		}
		if (field.Descriptor->Kind != componentKind
			|| field.Descriptor->Size != componentSize
			|| field.Offset != index * componentSize)
		{
			return CanonicalMathStructKind::None;
		}
	}
	return componentSize != 0 && descriptor.Size == descriptor.Fields.size() * componentSize
		? kind
		: CanonicalMathStructKind::None;
}

struct FieldRange
{
	std::int32_t Offset = -1;
	std::size_t Size = 0;
};

bool FitsField(
	const std::int32_t offset,
	const std::size_t size,
	const std::int32_t enclosingSize) noexcept
{
	return offset >= 0
		&& enclosingSize > 0
		&& static_cast<std::size_t>(offset) <= static_cast<std::size_t>(enclosingSize)
		&& size <= static_cast<std::size_t>(enclosingSize) - static_cast<std::size_t>(offset);
}

bool AreFieldsDisjoint(const std::span<const FieldRange> fields) noexcept
{
	for (std::size_t left = 0; left < fields.size(); ++left)
	{
		if (fields[left].Offset < 0 || fields[left].Size == 0)
			return false;
		const std::uint64_t leftBegin = static_cast<std::uint64_t>(fields[left].Offset);
		const std::uint64_t leftEnd = leftBegin + fields[left].Size;
		for (std::size_t right = left + 1; right < fields.size(); ++right)
		{
			if (fields[right].Offset < 0 || fields[right].Size == 0)
				return false;
			const std::uint64_t rightBegin = static_cast<std::uint64_t>(fields[right].Offset);
			const std::uint64_t rightEnd = rightBegin + fields[right].Size;
			if (leftBegin < rightEnd && rightBegin < leftEnd)
				return false;
		}
	}
	return true;
}

bool IsDynamicArrayLayoutValid(const DynamicArrayLayout& layout) noexcept
{
	const std::array fields{
		FieldRange{layout.DataOffset, sizeof(std::uintptr_t)},
		FieldRange{layout.NumOffset, sizeof(std::int32_t)},
		FieldRange{layout.MaxOffset, sizeof(std::int32_t)}
	};
	return layout.Validated
		&& layout.HeaderSize > 0
		&& layout.HeaderSize <= kMaximumLayoutSize
		&& AreFieldsDisjoint(fields)
		&& FitsField(layout.DataOffset, sizeof(std::uintptr_t), layout.HeaderSize)
		&& FitsField(layout.NumOffset, sizeof(std::int32_t), layout.HeaderSize)
		&& FitsField(layout.MaxOffset, sizeof(std::int32_t), layout.HeaderSize);
}

bool IsDynamicArrayLayoutAbsent(const DynamicArrayLayout& layout) noexcept
{
	return !layout.Validated
		&& layout.DataOffset == -1
		&& layout.NumOffset == -1
		&& layout.MaxOffset == -1
		&& layout.HeaderSize == -1;
}

bool IsTextLayoutValid(const TextLayout& layout) noexcept
{
	return layout.Validated
		&& layout.MinimumValueSize > 0
		&& layout.MinimumValueSize <= kMaximumLayoutSize
		&& FitsField(layout.DataPointerOffset, sizeof(std::uintptr_t), layout.MinimumValueSize)
		&& layout.StringOffsetInData >= 0
		&& layout.StringOffsetInData <= 0x10000;
}

bool IsTextLayoutAbsent(const TextLayout& layout) noexcept
{
	return !layout.Validated
		&& layout.DataPointerOffset == -1
		&& layout.StringOffsetInData == -1
		&& layout.MinimumValueSize == -1;
}

bool IsWeakObjectLayoutValid(const WeakObjectLayout& layout) noexcept
{
	const std::array fields{
		FieldRange{layout.IndexOffset, sizeof(std::int32_t)},
		FieldRange{layout.SerialOffset, sizeof(std::int32_t)}
	};
	return layout.Validated
		&& layout.ValueSize > 0
		&& layout.ValueSize <= kMaximumLayoutSize
		&& AreFieldsDisjoint(fields)
		&& FitsField(layout.IndexOffset, sizeof(std::int32_t), layout.ValueSize)
		&& FitsField(layout.SerialOffset, sizeof(std::int32_t), layout.ValueSize);
}

bool IsWeakObjectLayoutAbsent(const WeakObjectLayout& layout) noexcept
{
	return !layout.Validated
		&& layout.IndexOffset == -1
		&& layout.SerialOffset == -1
		&& layout.ValueSize == -1;
}

bool IsSoftObjectLayoutValid(
	const SoftObjectLayout& layout,
	const EngineNameProfile& names) noexcept
{
	if (!layout.Validated
		|| layout.AssetPathNameCount == 0
		|| layout.AssetPathNameCount > layout.AssetPathNameOffsets.size()
		|| names.FNameSize <= 0
		|| layout.MinimumValueSize <= 0
		|| layout.MinimumValueSize > kMaximumLayoutSize)
	{
		return false;
	}
	for (std::uint8_t index = 0; index < layout.AssetPathNameCount; ++index)
	{
		if (!FitsField(
			layout.AssetPathNameOffsets[index],
			static_cast<std::size_t>(names.FNameSize),
			layout.MinimumValueSize))
		{
			return false;
		}
	}
	if (layout.AssetPathNameCount == 2)
	{
		const std::array fields{
			FieldRange{layout.AssetPathNameOffsets[0], static_cast<std::size_t>(names.FNameSize)},
			FieldRange{layout.AssetPathNameOffsets[1], static_cast<std::size_t>(names.FNameSize)}
		};
		if (!AreFieldsDisjoint(fields))
			return false;
	}
	return layout.SubPathStringOffset == -1
		|| (layout.SubPathStringOffset >= 0
			&& layout.SubPathStringOffset < layout.MinimumValueSize);
}

bool IsSoftObjectLayoutAbsent(const SoftObjectLayout& layout) noexcept
{
	return !layout.Validated
		&& layout.AssetPathNameOffsets == std::array<std::int32_t, 2>{-1, -1}
		&& layout.AssetPathNameCount == 0
		&& layout.SubPathStringOffset == -1
		&& layout.MinimumValueSize == -1;
}

bool IsSoftObjectSubPathValid(
	const SoftObjectLayout& layout,
	const DynamicArrayLayout& dynamicArray,
	const EngineNameProfile& names) noexcept
{
	if (layout.SubPathStringOffset == -1)
		return true;
	if (!FitsField(
		layout.SubPathStringOffset,
		static_cast<std::size_t>(dynamicArray.HeaderSize),
		layout.MinimumValueSize))
	{
		return false;
	}
	for (std::uint8_t index = 0; index < layout.AssetPathNameCount; ++index)
	{
		const std::array fields{
			FieldRange{
				layout.AssetPathNameOffsets[index],
				static_cast<std::size_t>(names.FNameSize)},
			FieldRange{
				layout.SubPathStringOffset,
				static_cast<std::size_t>(dynamicArray.HeaderSize)}
		};
		if (!AreFieldsDisjoint(fields))
			return false;
	}
	return true;
}

bool IsSparseContainerLayoutValid(const SparseContainerLayout& layout) noexcept
{
	const std::array fields{
		FieldRange{layout.ElementsDataOffset, sizeof(std::uintptr_t)},
		FieldRange{layout.ElementsNumOffset, sizeof(std::int32_t)},
		FieldRange{layout.ElementsMaxOffset, sizeof(std::int32_t)},
		FieldRange{
			layout.AllocationInlineDataOffset,
			static_cast<std::size_t>(layout.InlineBitWordCount) * sizeof(std::uint32_t)},
		FieldRange{layout.AllocationSecondaryDataOffset, sizeof(std::uintptr_t)},
		FieldRange{layout.AllocationNumBitsOffset, sizeof(std::int32_t)},
		FieldRange{layout.AllocationMaxBitsOffset, sizeof(std::int32_t)}
	};
	return layout.Validated
		&& layout.HeaderSize > 0
		&& layout.HeaderSize <= kMaximumLayoutSize
		&& layout.InlineBitWordCount > 0
		&& layout.InlineBitWordCount <= 64
		&& AreFieldsDisjoint(fields)
		&& FitsField(layout.ElementsDataOffset, sizeof(std::uintptr_t), layout.HeaderSize)
		&& FitsField(layout.ElementsNumOffset, sizeof(std::int32_t), layout.HeaderSize)
		&& FitsField(layout.ElementsMaxOffset, sizeof(std::int32_t), layout.HeaderSize)
		&& FitsField(
			layout.AllocationInlineDataOffset,
			static_cast<std::size_t>(layout.InlineBitWordCount) * sizeof(std::uint32_t),
			layout.HeaderSize)
		&& FitsField(layout.AllocationSecondaryDataOffset, sizeof(std::uintptr_t), layout.HeaderSize)
		&& FitsField(layout.AllocationNumBitsOffset, sizeof(std::int32_t), layout.HeaderSize)
		&& FitsField(layout.AllocationMaxBitsOffset, sizeof(std::int32_t), layout.HeaderSize);
}

bool IsSparseContainerLayoutAbsent(const SparseContainerLayout& layout) noexcept
{
	return !layout.Validated
		&& layout.ElementsDataOffset == -1
		&& layout.ElementsNumOffset == -1
		&& layout.ElementsMaxOffset == -1
		&& layout.AllocationInlineDataOffset == -1
		&& layout.AllocationSecondaryDataOffset == -1
		&& layout.AllocationNumBitsOffset == -1
		&& layout.AllocationMaxBitsOffset == -1
		&& layout.HeaderSize == -1
		&& layout.InlineBitWordCount == 0;
}

PropertyValue MakeValue(
	const PropertyDescriptor& descriptor,
	const PropertyValueState state)
{
	return {
		.State = state,
		.Kind = descriptor.Kind,
		.TypeName = descriptor.TypeName
	};
}

PropertyValue MakeFailure(
	const PropertyDescriptor& descriptor,
	const PropertyValueState state,
	std::string code,
	std::string message)
{
	PropertyValue value = MakeValue(descriptor, state);
	value.ErrorCode = std::move(code);
	value.ErrorMessage = std::move(message);
	return value;
}

PropertyValue MakeMemoryFailure(
	const PropertyDescriptor& descriptor,
	const MemoryResult& memory)
{
	return MakeFailure(
		descriptor,
		PropertyValueState::Error,
		"PROPERTY_MEMORY_READ_FAILED",
		std::string(ToString(memory.Error)) + " (native="
			+ std::to_string(memory.NativeError) + ")");
}

bool TryAddAddress(
	const std::uintptr_t base,
	const std::uint64_t offset,
	const std::size_t size,
	std::uintptr_t& result) noexcept
{
	result = 0;
	if (base == 0
		|| offset > static_cast<std::uint64_t>((std::numeric_limits<std::uintptr_t>::max)())
		|| static_cast<std::uintptr_t>(offset)
			> (std::numeric_limits<std::uintptr_t>::max)() - base)
	{
		return false;
	}
	result = base + static_cast<std::uintptr_t>(offset);
	std::uintptr_t ignored = 0;
	return CheckedAddressRange(result, size, ignored);
}

bool TryMultiply(
	const std::size_t left,
	const std::size_t right,
	std::size_t& result) noexcept
{
	result = 0;
	if (left != 0 && right > (std::numeric_limits<std::size_t>::max)() / left)
		return false;
	result = left * right;
	return true;
}

template<typename T>
MemoryResult ReadAt(
	const std::uintptr_t base,
	const std::int32_t offset,
	T& output) noexcept
{
	std::uintptr_t address = 0;
	if (offset < 0 || !TryAddAddress(base, static_cast<std::uint64_t>(offset), sizeof(T), address))
		return {.Error = MemoryError::InvalidRange};
	return ReadValue(address, output);
}

template<typename T>
MemoryResult ReadStableValue(const std::uintptr_t address, T& output) noexcept
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
	output = first;
	return {.BytesProcessed = sizeof(T)};
}

struct DynamicHeader
{
	std::uintptr_t Data = 0;
	std::int32_t Num = 0;
	std::int32_t Max = 0;

	bool operator==(const DynamicHeader&) const = default;
};

MemoryResult ReadDynamicHeader(
	const std::uintptr_t address,
	const DynamicArrayLayout& layout,
	DynamicHeader& header) noexcept
{
	MemoryResult memory = ReadAt(address, layout.DataOffset, header.Data);
	if (!memory.Ok())
		return memory;
	memory = ReadAt(address, layout.NumOffset, header.Num);
	if (!memory.Ok())
		return memory;
	return ReadAt(address, layout.MaxOffset, header.Max);
}

bool IsDynamicHeaderValid(const DynamicHeader& header) noexcept
{
	return header.Num >= 0
		&& header.Max >= header.Num
		&& ((header.Num == 0) || header.Data != 0);
}

bool TryUtf16ToUtf8(
	const std::span<const wchar_t> input,
	std::string& output) noexcept
{
	output.clear();
	if (input.empty())
		return true;
	if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		return false;
	const int required = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		input.data(),
		static_cast<int>(input.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (required <= 0)
		return false;
	try
	{
		output.resize(static_cast<std::size_t>(required));
	}
	catch (...)
	{
		return false;
	}
	return WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		input.data(),
		static_cast<int>(input.size()),
		output.data(),
		required,
		nullptr,
		nullptr) == required;
}

PropertyValueState MergeState(
	const PropertyValueState current,
	const PropertyValueState child) noexcept
{
	const auto rank = [](const PropertyValueState state) {
		switch (state)
		{
		case PropertyValueState::Error: return 4;
		case PropertyValueState::Unavailable: return 3;
		case PropertyValueState::Unsupported: return 2;
		case PropertyValueState::Ok:
		case PropertyValueState::Empty: return 1;
		}
		return 4;
	};
	return rank(child) > rank(current) ? child : current;
}

bool IsIntegerKind(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8:
	case PropertyKind::Int16:
	case PropertyKind::Int32:
	case PropertyKind::Int64:
	case PropertyKind::UInt8:
	case PropertyKind::UInt16:
	case PropertyKind::UInt32:
	case PropertyKind::UInt64:
		return true;
	default:
		return false;
	}
}

std::uint32_t ExactIntegerSize(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8:
	case PropertyKind::UInt8: return 1;
	case PropertyKind::Int16:
	case PropertyKind::UInt16: return 2;
	case PropertyKind::Int32:
	case PropertyKind::UInt32: return 4;
	case PropertyKind::Int64:
	case PropertyKind::UInt64: return 8;
	default: return 0;
	}
}

bool IsSignedIntegerKind(const PropertyKind kind) noexcept
{
	return kind == PropertyKind::Int8
		|| kind == PropertyKind::Int16
		|| kind == PropertyKind::Int32
		|| kind == PropertyKind::Int64;
}

bool IsEnumRawValueInRange(
	const PropertyKind kind,
	const std::uint64_t rawValue) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8:
		return rawValue <= static_cast<std::uint64_t>((std::numeric_limits<std::int8_t>::max)())
			|| rawValue >= static_cast<std::uint64_t>((std::numeric_limits<std::int8_t>::min)());
	case PropertyKind::Int16:
		return rawValue <= static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::max)())
			|| rawValue >= static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::min)());
	case PropertyKind::Int32:
		return rawValue <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
			|| rawValue >= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::min)());
	case PropertyKind::Int64: return true;
	case PropertyKind::UInt8:
		return rawValue <= (std::numeric_limits<std::uint8_t>::max)();
	case PropertyKind::UInt16:
		return rawValue <= (std::numeric_limits<std::uint16_t>::max)();
	case PropertyKind::UInt32:
		return rawValue <= (std::numeric_limits<std::uint32_t>::max)();
	case PropertyKind::UInt64: return true;
	default: return false;
	}
}

bool IsDescriptorProvenEnumImpl(const PropertyDescriptor& descriptor)
{
	if (descriptor.Kind != PropertyKind::Enum
		|| descriptor.TypeName.empty()
		|| descriptor.TypeName.size() > kMaximumDescriptorTextBytes
		|| !descriptor.Element
		|| !IsIntegerKind(descriptor.Element->Kind)
		|| descriptor.Size == 0
		|| descriptor.Element->Size != descriptor.Size
		|| ExactIntegerSize(descriptor.Element->Kind) != descriptor.Size
		|| !HasNoNestedDescriptorMetadata(*descriptor.Element)
		|| descriptor.Key
		|| descriptor.Mapped
		|| !descriptor.Fields.empty()
		|| descriptor.EnumEntries.empty()
		|| descriptor.EnumEntries.size() > kMaximumEnumEntries
		|| descriptor.BoolByteOffset != 0
		|| descriptor.BoolMask != 0
		|| descriptor.ElementStride != 0
		|| descriptor.ElementValueOffset != 0
		|| descriptor.MapKeyOffset != 0
		|| descriptor.MapValueOffset != 0)
	{
		return false;
	}
	std::set<std::string_view> names;
	std::set<std::uint64_t> values;
	for (const PropertyEnumEntry& entry : descriptor.EnumEntries)
	{
		if (entry.Name.empty()
			|| entry.Name.size() > kMaximumDescriptorTextBytes
			|| !names.emplace(entry.Name).second
			|| !values.emplace(entry.RawValue).second
			|| !IsEnumRawValueInRange(descriptor.Element->Kind, entry.RawValue))
		{
			return false;
		}
	}
	return true;
}

bool IsRecursiveKind(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Enum:
	case PropertyKind::Struct:
	case PropertyKind::Array:
	case PropertyKind::Map:
	case PropertyKind::Set:
		return true;
	default:
		return false;
	}
}

bool IsStableObjectHandle(const ObjectHandle& handle) noexcept
{
	return !handle.SessionId.empty()
		&& handle.SessionId.size() <= kMaximumSessionIdLength
		&& handle.ContextGeneration != 0
		&& handle.Index >= 0
		&& handle.SerialNumber > 0
		&& handle.Address != 0
		&& handle.ClassFingerprint != 0;
}

std::uint64_t ScalarRawValue(const PropertyScalar& scalar) noexcept
{
	if (const auto* signedValue = std::get_if<std::int64_t>(&scalar))
		return static_cast<std::uint64_t>(*signedValue);
	if (const auto* unsignedValue = std::get_if<std::uint64_t>(&scalar))
		return *unsignedValue;
	return 0;
}

class DecodeSession final
{
public:
	DecodeSession(
		const EngineNameCodec& names,
		const PropertyCodecProfile& profile,
		PropertyDecodeOptions options)
		: m_Names(names), m_Profile(profile), m_Options(std::move(options))
	{
	}

	PropertyValue Decode(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		const std::uint32_t depth = 0,
		const std::uintptr_t identityAddress = 0)
	{
		if (descriptor.TypeName.empty()
			|| descriptor.TypeName.size() > kMaximumDescriptorTextBytes)
		{
			return {
				.State = PropertyValueState::Error,
				.Kind = descriptor.Kind,
				.ErrorCode = "PROPERTY_DESCRIPTOR_INVALID",
				.ErrorMessage = "The property type name is empty or exceeds its byte limit"
			};
		}
		if (address == 0)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_ADDRESS_INVALID", "The property address is null");
		if (depth > m_Options.Limits.MaxDepth)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DEPTH_LIMIT_EXCEEDED", "The property recursion depth exceeded its budget");
		if (m_Nodes >= m_Options.Limits.MaxTotalNodes)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_NODE_BUDGET_EXCEEDED", "The property node budget was exhausted");
		++m_Nodes;
		const bool recursive = IsRecursiveKind(descriptor.Kind);
		const std::uintptr_t recursionAddress = identityAddress == 0
			? address
			: identityAddress;
		const auto recursionKey = std::pair{recursionAddress, &descriptor};
		if (recursive
			&& std::ranges::find(m_RecursionStack, recursionKey) != m_RecursionStack.end())
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_RECURSION_CYCLE", "The property descriptor/address pair is recursive");
		}
		if (recursive)
			m_RecursionStack.push_back(recursionKey);
		struct StackPop final
		{
			std::vector<std::pair<std::uintptr_t, const PropertyDescriptor*>>& Stack;
			bool Active = false;
			~StackPop()
			{
				if (Active)
					Stack.pop_back();
			}
		};
		[[maybe_unused]] StackPop pop{m_RecursionStack, recursive};

		switch (descriptor.Kind)
		{
		case PropertyKind::Bool: return DecodeBool(address, descriptor);
		case PropertyKind::Int8: return DecodeNumber<std::int8_t>(address, descriptor);
		case PropertyKind::Int16: return DecodeNumber<std::int16_t>(address, descriptor);
		case PropertyKind::Int32: return DecodeNumber<std::int32_t>(address, descriptor);
		case PropertyKind::Int64: return DecodeNumber<std::int64_t>(address, descriptor);
		case PropertyKind::UInt8: return DecodeNumber<std::uint8_t>(address, descriptor);
		case PropertyKind::UInt16: return DecodeNumber<std::uint16_t>(address, descriptor);
		case PropertyKind::UInt32: return DecodeNumber<std::uint32_t>(address, descriptor);
		case PropertyKind::UInt64: return DecodeNumber<std::uint64_t>(address, descriptor);
		case PropertyKind::Float: return DecodeFloating<float>(address, descriptor);
		case PropertyKind::Double: return DecodeFloating<double>(address, descriptor);
		case PropertyKind::Name: return DecodeName(address, descriptor);
		case PropertyKind::String: return DecodeString(address, descriptor);
		case PropertyKind::Text: return DecodeText(address, descriptor);
		case PropertyKind::Object: return DecodeObject(address, descriptor);
		case PropertyKind::WeakObject: return DecodeWeakObject(address, descriptor);
		case PropertyKind::SoftObject: return DecodeSoftObject(address, descriptor);
		case PropertyKind::Enum: return DecodeEnum(address, descriptor, depth);
		case PropertyKind::Struct:
			return DecodeStruct(address, descriptor, depth, recursionAddress);
		case PropertyKind::Array: return DecodeArray(address, descriptor, depth);
		case PropertyKind::Map: return DecodeSparse(address, descriptor, depth, true);
		case PropertyKind::Set: return DecodeSparse(address, descriptor, depth, false);
		case PropertyKind::Delegate:
			return MakeFailure(descriptor, PropertyValueState::Unsupported,
				"PROPERTY_DELEGATE_UNSUPPORTED", "Delegate value decoding is not implemented");
		case PropertyKind::Unknown:
			return MakeFailure(descriptor, PropertyValueState::Unsupported,
				"PROPERTY_KIND_UNSUPPORTED", "The reflected property kind is unsupported");
		}
		return MakeFailure(descriptor, PropertyValueState::Unsupported,
			"PROPERTY_KIND_UNSUPPORTED", "The reflected property kind is unsupported");
	}

private:
	bool CanEmitNode() const noexcept
	{
		return m_Nodes < m_Options.Limits.MaxTotalNodes;
	}

	bool TryConsumeSyntheticNode() noexcept
	{
		if (!CanEmitNode())
			return false;
		++m_Nodes;
		return true;
	}

	bool IsReferenceResolverConfigured() const noexcept
	{
		if (!m_Options.ReferenceResolver)
			return false;
		const std::string_view sessionId = m_Options.ReferenceResolver->SessionId();
		return !sessionId.empty()
			&& sessionId.size() <= kMaximumSessionIdLength
			&& m_Options.ReferenceResolver->ContextGeneration() != 0;
	}

	static void MarkNodeBudgetExhausted(PropertyValue& value)
	{
		value.State = PropertyValueState::Error;
		value.Truncated = true;
		if (value.ErrorCode.empty())
		{
			value.ErrorCode = "PROPERTY_NODE_BUDGET_EXCEEDED";
			value.ErrorMessage = "The property node budget was exhausted";
		}
	}

	PropertyValue DecodeBool(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (descriptor.Size == 0
			|| descriptor.BoolMask == 0
			|| descriptor.BoolByteOffset >= descriptor.Size)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The bool descriptor is out of bounds");
		}
		std::uintptr_t byteAddress = 0;
		if (!TryAddAddress(address, descriptor.BoolByteOffset, sizeof(std::uint8_t), byteAddress))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_ADDRESS_OVERFLOW", "The bool field address overflowed");
		std::uint8_t byte = 0;
		const MemoryResult memory = ReadStableValue(byteAddress, byte);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.Scalar = (byte & descriptor.BoolMask) != 0;
		return value;
	}

	template<typename T>
	PropertyValue DecodeNumber(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (descriptor.Size != sizeof(T))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The numeric descriptor size is not exact");
		T number{};
		const MemoryResult memory = ReadStableValue(address, number);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		if constexpr (std::is_signed_v<T>)
			value.Scalar = static_cast<std::int64_t>(number);
		else
			value.Scalar = static_cast<std::uint64_t>(number);
		return value;
	}

	template<typename T>
	PropertyValue DecodeFloating(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (descriptor.Size != sizeof(T))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The floating-point descriptor size is not exact");
		T number{};
		const MemoryResult memory = ReadStableValue(address, number);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (!std::isfinite(number))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_FLOAT_NOT_FINITE", "The floating-point value is not finite");
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.Scalar = static_cast<double>(number);
		return value;
	}

	PropertyValue DecodeName(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (!m_Names.IsConfigured())
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_NAME_CODEC_UNAVAILABLE", "The immutable name codec is unavailable");
		if (descriptor.Size != static_cast<std::uint32_t>(m_Names.Profile().FNameSize))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The FName descriptor size does not match the immutable profile");
		std::vector<std::byte> first(descriptor.Size);
		MemoryResult memory = ReadMemory(address, first);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		const EngineNameResult decoded = m_Names.DecodeFName(address);
		if (!decoded.Ok())
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_NAME_DECODE_FAILED", ToString(decoded.Error));
		std::vector<std::byte> second(descriptor.Size);
		memory = ReadMemory(address, second);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (first != second)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The FName fields changed while they were decoded");
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.Scalar = decoded.Value;
		return value;
	}

	PropertyValue DecodeString(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (!m_Profile.Validated || !IsDynamicArrayLayoutValid(m_Profile.DynamicArray))
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_STRING_LAYOUT_UNAVAILABLE", "No validated FString layout is configured");
		if (descriptor.Size < static_cast<std::uint32_t>(m_Profile.DynamicArray.HeaderSize))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The FString descriptor is smaller than its header");

		DynamicHeader first;
		MemoryResult memory = ReadDynamicHeader(address, m_Profile.DynamicArray, first);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (!IsDynamicHeaderValid(first))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_HEADER_INVALID", "The FString Num/Max/Data header is invalid");
		if (first.Num == 0)
		{
			DynamicHeader second;
			memory = ReadDynamicHeader(address, m_Profile.DynamicArray, second);
			if (!memory.Ok())
				return MakeMemoryFailure(descriptor, memory);
			if (first != second)
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_VALUE_CHANGED_DURING_READ", "The empty FString header changed while it was decoded");
			return MakeValue(descriptor, PropertyValueState::Empty);
		}
		if (first.Num > static_cast<std::int32_t>(m_Options.Limits.MaxStringCodeUnits) + 1)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_STRING_LIMIT_EXCEEDED", "The FString exceeds its code-unit budget");

		std::size_t byteCount = 0;
		if (!TryMultiply(static_cast<std::size_t>(first.Num), sizeof(wchar_t), byteCount)
			|| byteCount > m_Options.Limits.MaxReadableContainerBytes)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_SIZE_OVERFLOW", "The FString byte range is invalid");
		}
		std::vector<wchar_t> units(static_cast<std::size_t>(first.Num));
		memory = ReadMemory(first.Data, std::as_writable_bytes(std::span(units)));
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (units.back() != L'\0')
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_STRING_NOT_TERMINATED", "The FString does not end with a null code unit");

		DynamicHeader second;
		memory = ReadDynamicHeader(address, m_Profile.DynamicArray, second);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (first != second)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The FString header changed while it was copied");
		std::vector<wchar_t> finalUnits(units.size());
		memory = ReadMemory(first.Data, std::as_writable_bytes(std::span(finalUnits)));
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		DynamicHeader finalHeader;
		memory = ReadDynamicHeader(address, m_Profile.DynamicArray, finalHeader);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (first != finalHeader || units != finalUnits)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The FString header/content changed while it was copied");

		std::string utf8;
		if (!TryUtf16ToUtf8(
			std::span<const wchar_t>(units.data(), units.size() - 1),
			utf8))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_STRING_ENCODING_INVALID", "The FString is not valid UTF-16");
		}
		PropertyValue value = MakeValue(
			descriptor,
			utf8.empty() ? PropertyValueState::Empty : PropertyValueState::Ok);
		value.Scalar = std::move(utf8);
		return value;
	}

	PropertyValue DecodeText(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (!m_Profile.Validated
			|| !IsTextLayoutValid(m_Profile.Text)
			|| !IsDynamicArrayLayoutValid(m_Profile.DynamicArray))
		{
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_TEXT_LAYOUT_UNAVAILABLE", "No validated FText/FString layout is configured");
		}
		if (descriptor.Size < static_cast<std::uint32_t>(m_Profile.Text.MinimumValueSize))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The FText descriptor is smaller than its profile");
		std::uintptr_t data = 0;
		std::uintptr_t dataAddress = 0;
		if (!TryAddAddress(
			address,
			static_cast<std::uint64_t>(m_Profile.Text.DataPointerOffset),
			sizeof(std::uintptr_t),
			dataAddress))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_ADDRESS_OVERFLOW", "The FText data-pointer address overflowed");
		}
		MemoryResult memory = ReadStableValue(dataAddress, data);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (data == 0)
			return MakeValue(descriptor, PropertyValueState::Empty);
		std::uintptr_t stringAddress = 0;
		if (!TryAddAddress(
			data,
			static_cast<std::uint64_t>(m_Profile.Text.StringOffsetInData),
			static_cast<std::size_t>(m_Profile.DynamicArray.HeaderSize),
			stringAddress))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_ADDRESS_OVERFLOW", "The FText string address overflowed");
		}
		PropertyDescriptor stringDescriptor{
			.Kind = PropertyKind::String,
			.TypeName = "FString",
			.Size = static_cast<std::uint32_t>(m_Profile.DynamicArray.HeaderSize)
		};
		PropertyValue value = DecodeString(stringAddress, stringDescriptor);
		value.Kind = descriptor.Kind;
		value.TypeName = descriptor.TypeName;
		std::uintptr_t finalData = 0;
		memory = ReadStableValue(dataAddress, finalData);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (finalData != data)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The FText data pointer changed while it was copied");
		return value;
	}

	PropertyValue DecodeObject(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (descriptor.Size != sizeof(std::uintptr_t))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The UObject reference size is not exact");
		std::uintptr_t objectAddress = 0;
		const MemoryResult memory = ReadStableValue(address, objectAddress);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (objectAddress == 0)
			return MakeValue(descriptor, PropertyValueState::Empty);
		if (!IsReferenceResolverConfigured())
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_REFERENCE_RESOLVER_UNAVAILABLE", "No stable session-bound object-handle resolver is configured");
		PropertyReferenceResult resolved = m_Options.ReferenceResolver->ResolveAddress(objectAddress);
		std::uintptr_t finalAddress = 0;
		const MemoryResult finalMemory = ReadStableValue(address, finalAddress);
		if (!finalMemory.Ok())
			return MakeMemoryFailure(descriptor, finalMemory);
		if (finalAddress != objectAddress)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The object reference changed while it was resolved");
		return FromReferenceResult(
			descriptor,
			std::move(resolved),
			objectAddress,
			-1,
			0);
	}

	PropertyValue DecodeWeakObject(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (!m_Profile.Validated || !IsWeakObjectLayoutValid(m_Profile.WeakObject))
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_WEAK_LAYOUT_UNAVAILABLE", "No validated weak-object layout is configured");
		if (descriptor.Size < static_cast<std::uint32_t>(m_Profile.WeakObject.ValueSize))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The weak-object descriptor is smaller than its profile");
		std::int32_t index = -1;
		std::int32_t serial = 0;
		MemoryResult memory = ReadAt(address, m_Profile.WeakObject.IndexOffset, index);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, m_Profile.WeakObject.SerialOffset, serial);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		std::int32_t finalIndex = -1;
		std::int32_t finalSerial = 0;
		memory = ReadAt(address, m_Profile.WeakObject.IndexOffset, finalIndex);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, m_Profile.WeakObject.SerialOffset, finalSerial);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (index != finalIndex || serial != finalSerial)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The weak-object identity changed while it was copied");
		if (index < 0 && serial == 0)
			return MakeValue(descriptor, PropertyValueState::Empty);
		if (index < 0 || serial <= 0)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_WEAK_IDENTITY_INVALID", "The weak-object index/serial pair is invalid");
		if (!IsReferenceResolverConfigured())
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_REFERENCE_RESOLVER_UNAVAILABLE", "No stable session-bound object-handle resolver is configured");
		PropertyReferenceResult resolved = m_Options.ReferenceResolver->ResolveWeak(index, serial);
		std::int32_t resolvedIndex = -1;
		std::int32_t resolvedSerial = 0;
		memory = ReadAt(address, m_Profile.WeakObject.IndexOffset, resolvedIndex);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, m_Profile.WeakObject.SerialOffset, resolvedSerial);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		std::int32_t confirmedIndex = -1;
		std::int32_t confirmedSerial = 0;
		memory = ReadAt(address, m_Profile.WeakObject.IndexOffset, confirmedIndex);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, m_Profile.WeakObject.SerialOffset, confirmedSerial);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (resolvedIndex != index
			|| resolvedSerial != serial
			|| confirmedIndex != index
			|| confirmedSerial != serial)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The weak-object identity changed while it was resolved");
		return FromReferenceResult(
			descriptor,
			std::move(resolved),
			0,
			index,
			serial);
	}

	PropertyValue DecodeSoftObject(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor)
	{
		if (!m_Profile.Validated
			|| !IsSoftObjectLayoutValid(m_Profile.SoftObject, m_Names.Profile())
			|| !IsDynamicArrayLayoutValid(m_Profile.DynamicArray)
			|| (m_Profile.SoftObject.SubPathStringOffset >= 0
				&& !FitsField(
					m_Profile.SoftObject.SubPathStringOffset,
					static_cast<std::size_t>(m_Profile.DynamicArray.HeaderSize),
					m_Profile.SoftObject.MinimumValueSize)))
		{
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_SOFT_LAYOUT_UNAVAILABLE", "No validated soft-object layout is configured");
		}
		if (descriptor.Size < static_cast<std::uint32_t>(m_Profile.SoftObject.MinimumValueSize))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The soft-object descriptor is smaller than its profile");

		std::string path;
		bool anyName = false;
		std::array<std::uintptr_t, 2> nameAddresses{};
		std::array<std::vector<std::byte>, 2> nameWitnesses;
		for (std::uint8_t index = 0; index < m_Profile.SoftObject.AssetPathNameCount; ++index)
		{
			std::uintptr_t nameAddress = 0;
			if (!TryAddAddress(
				address,
				static_cast<std::uint64_t>(m_Profile.SoftObject.AssetPathNameOffsets[index]),
				static_cast<std::size_t>(m_Names.Profile().FNameSize),
				nameAddress))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_ADDRESS_OVERFLOW", "The soft-object FName address overflowed");
			}
			nameAddresses[index] = nameAddress;
			nameWitnesses[index].resize(static_cast<std::size_t>(m_Names.Profile().FNameSize));
			MemoryResult memory = ReadMemory(nameAddress, nameWitnesses[index]);
			if (!memory.Ok())
				return MakeMemoryFailure(descriptor, memory);
			PropertyDescriptor nameDescriptor{
				.Kind = PropertyKind::Name,
				.TypeName = "FName",
				.Size = static_cast<std::uint32_t>(m_Names.Profile().FNameSize)
			};
			PropertyValue name = DecodeName(nameAddress, nameDescriptor);
			if (!name.Ok())
			{
				name.Kind = descriptor.Kind;
				name.TypeName = descriptor.TypeName;
				return name;
			}
			const auto* decoded = std::get_if<std::string>(&name.Scalar);
			if (!decoded)
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_NAME_RESULT_INVALID", "The soft-object FName did not produce text");
			if (*decoded != "None")
			{
				if (anyName)
					path.push_back('.');
				path += *decoded;
				anyName = true;
			}
		}
		if (m_Profile.SoftObject.SubPathStringOffset >= 0)
		{
			std::uintptr_t subPathAddress = 0;
			if (!TryAddAddress(
				address,
				static_cast<std::uint64_t>(m_Profile.SoftObject.SubPathStringOffset),
				static_cast<std::size_t>(m_Profile.DynamicArray.HeaderSize),
				subPathAddress))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_ADDRESS_OVERFLOW", "The soft-object subpath address overflowed");
			}
			PropertyDescriptor subPathDescriptor{
				.Kind = PropertyKind::String,
				.TypeName = "FString",
				.Size = static_cast<std::uint32_t>(m_Profile.DynamicArray.HeaderSize)
			};
			PropertyValue subPath = DecodeString(subPathAddress, subPathDescriptor);
			if (!subPath.Ok())
			{
				subPath.Kind = descriptor.Kind;
				subPath.TypeName = descriptor.TypeName;
				return subPath;
			}
			if (const auto* text = std::get_if<std::string>(&subPath.Scalar); text && !text->empty())
			{
				if (!path.empty())
					path.push_back(':');
				path += *text;
			}
		}
		for (std::uint8_t index = 0; index < m_Profile.SoftObject.AssetPathNameCount; ++index)
		{
			std::vector<std::byte> finalWitness(nameWitnesses[index].size());
			const MemoryResult memory = ReadMemory(nameAddresses[index], finalWitness);
			if (!memory.Ok())
				return MakeMemoryFailure(descriptor, memory);
			if (finalWitness != nameWitnesses[index])
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_VALUE_CHANGED_DURING_READ", "The soft-object asset path changed while it was decoded");
		}

		PropertyValue value = MakeValue(
			descriptor,
			path.empty() ? PropertyValueState::Empty : PropertyValueState::Ok);
		value.Scalar = std::move(path);
		return value;
	}

	PropertyValue DecodeEnum(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		const std::uint32_t depth)
	{
		if (!descriptor.Element
			|| !IsIntegerKind(descriptor.Element->Kind)
			|| descriptor.Element->Size != descriptor.Size
			|| descriptor.EnumEntries.size() > kMaximumEnumEntries)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The enum underlying descriptor is invalid");
		}
		PropertyValue underlying = Decode(address, *descriptor.Element, depth + 1);
		if (!underlying.Ok())
		{
			underlying.Kind = descriptor.Kind;
			underlying.TypeName = descriptor.TypeName;
			return underlying;
		}
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.Scalar = underlying.Scalar;
		const std::uint64_t rawValue = ScalarRawValue(underlying.Scalar);
		const auto entry = std::ranges::find_if(
			descriptor.EnumEntries,
			[rawValue](const PropertyEnumEntry& candidate) {
				return candidate.RawValue == rawValue;
			});
		if (entry != descriptor.EnumEntries.end())
		{
			if (entry->Name.empty() || entry->Name.size() > kMaximumDescriptorTextBytes)
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_DESCRIPTOR_INVALID", "The enum value name is empty or exceeds its byte limit");
			value.DisplayName = entry->Name;
		}
		return value;
	}

	PropertyValue DecodeStruct(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		const std::uint32_t depth,
		const std::uintptr_t identityAddress)
	{
		if (descriptor.Size == 0 || descriptor.Fields.size() > kMaximumDescriptorFields)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The struct descriptor is empty or exceeds its field limit");
		std::size_t snapshotBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(descriptor.Size), 2, snapshotBytes)
			|| snapshotBytes > m_Options.Limits.MaxReadableContainerBytes
				- (std::min)(m_StructSnapshotBytes,
					m_Options.Limits.MaxReadableContainerBytes))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_STRUCT_LIMIT_EXCEEDED", "Stable struct snapshots exceed the aggregate read budget");
		}
		m_StructSnapshotBytes += snapshotBytes;
		struct SnapshotBudgetGuard final
		{
			std::size_t& Used;
			std::size_t Reserved;
			~SnapshotBudgetGuard() { Used -= Reserved; }
		};
		[[maybe_unused]] SnapshotBudgetGuard snapshotBudget{
			m_StructSnapshotBytes,
			snapshotBytes
		};
		std::vector<std::byte> witness(descriptor.Size);
		std::vector<std::byte> stable(descriptor.Size);
		MemoryResult memory = ReadMemory(address, witness);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		memory = ReadMemory(address, stable);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (witness != stable)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The struct bytes changed before they could be decoded");
		const std::uintptr_t stableAddress =
			reinterpret_cast<std::uintptr_t>(stable.data());
		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.TotalCount = static_cast<std::uint32_t>(descriptor.Fields.size());
		std::set<std::string> names;
		for (const PropertyFieldDescriptor& field : descriptor.Fields)
		{
			if (field.Name.empty()
				|| field.Name.size() > kMaximumDescriptorTextBytes
				|| !field.Descriptor
				|| field.Descriptor->Size == 0
				|| !names.emplace(field.Name).second
				|| field.Offset > descriptor.Size
				|| field.Descriptor->Size > descriptor.Size - field.Offset)
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_DESCRIPTOR_INVALID", "A struct field descriptor is invalid or out of bounds");
			}
			if (!CanEmitNode())
			{
				MarkNodeBudgetExhausted(value);
				break;
			}
			std::uintptr_t fieldAddress = 0;
			std::uintptr_t fieldIdentityAddress = 0;
			if (!TryAddAddress(stableAddress, field.Offset, field.Descriptor->Size, fieldAddress)
				|| !TryAddAddress(
					identityAddress,
					field.Offset,
					field.Descriptor->Size,
					fieldIdentityAddress))
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_ADDRESS_OVERFLOW", "A struct field address overflowed");
			PropertyValue child = Decode(
				fieldAddress,
				*field.Descriptor,
				depth + 1,
				fieldIdentityAddress);
			child.Label = field.Name;
			value.State = MergeState(value.State, child.State);
			if (!child.ErrorCode.empty() && value.ErrorCode.empty())
			{
				value.ErrorCode = child.ErrorCode;
				value.ErrorMessage = child.ErrorMessage;
			}
			value.Children.push_back(std::move(child));
			if (value.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED")
			{
				value.Truncated = true;
				break;
			}
		}
		memory = ReadMemory(address, witness);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (witness != stable)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The struct bytes changed while they were decoded");
		return value;
	}

	PropertyValue DecodeArray(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		const std::uint32_t depth)
	{
		if (!m_Profile.Validated || !IsDynamicArrayLayoutValid(m_Profile.DynamicArray))
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_ARRAY_LAYOUT_UNAVAILABLE", "No validated dynamic-array layout is configured");
		if (!descriptor.Element
			|| descriptor.ElementStride == 0
			|| descriptor.Element->Size == 0
			|| descriptor.Element->Size > descriptor.ElementStride
			|| descriptor.Size < static_cast<std::uint32_t>(m_Profile.DynamicArray.HeaderSize))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The array descriptor is invalid");
		}

		DynamicHeader first;
		MemoryResult memory = ReadDynamicHeader(address, m_Profile.DynamicArray, first);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (!IsDynamicHeaderValid(first))
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_HEADER_INVALID", "The array Num/Max/Data header is invalid");
		if (first.Num == 0)
		{
			DynamicHeader second;
			memory = ReadDynamicHeader(address, m_Profile.DynamicArray, second);
			if (!memory.Ok())
				return MakeMemoryFailure(descriptor, memory);
			if (first != second)
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_VALUE_CHANGED_DURING_READ", "The empty array header changed while it was decoded");
			return MakeValue(descriptor, PropertyValueState::Empty);
		}

		std::size_t byteCount = 0;
		if (!TryMultiply(
			static_cast<std::size_t>(first.Num),
			descriptor.ElementStride,
			byteCount)
			|| byteCount > m_Options.Limits.MaxReadableContainerBytes)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_SIZE_OVERFLOW", "The array element range exceeds its byte budget");
		}
		memory = ValidateReadableMemory(first.Data, byteCount);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);

		PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
		value.TotalCount = static_cast<std::uint32_t>(first.Num);
		const std::uint32_t previewCount = (std::min)(
			static_cast<std::uint32_t>(first.Num),
			m_Options.Limits.MaxContainerElements);
		value.Truncated = previewCount < static_cast<std::uint32_t>(first.Num);
		value.Children.reserve(previewCount);
		for (std::uint32_t index = 0; index < previewCount; ++index)
		{
			if (!CanEmitNode())
			{
				MarkNodeBudgetExhausted(value);
				break;
			}
			std::uintptr_t elementAddress = 0;
			if (!TryAddAddress(
				first.Data,
				static_cast<std::uint64_t>(index) * descriptor.ElementStride,
				descriptor.Element->Size,
				elementAddress))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_ADDRESS_OVERFLOW", "An array element address overflowed");
			}
			PropertyValue child = Decode(elementAddress, *descriptor.Element, depth + 1);
			child.Label = std::to_string(index);
			value.State = MergeState(value.State, child.State);
			if (!child.ErrorCode.empty() && value.ErrorCode.empty())
			{
				value.ErrorCode = child.ErrorCode;
				value.ErrorMessage = child.ErrorMessage;
			}
			value.Children.push_back(std::move(child));
			if (value.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED")
			{
				value.Truncated = true;
				break;
			}
		}

		DynamicHeader second;
		memory = ReadDynamicHeader(address, m_Profile.DynamicArray, second);
		if (!memory.Ok())
			return MakeMemoryFailure(descriptor, memory);
		if (first != second)
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The array header changed while it was decoded");
		return value;
	}

	PropertyValue DecodeSparse(
		const std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		const std::uint32_t depth,
		const bool isMap)
	{
		const SparseContainerLayout& layout = m_Profile.SparseContainer;
		if (!m_Profile.Validated || !IsSparseContainerLayoutValid(layout))
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_SPARSE_LAYOUT_UNAVAILABLE", "No validated sparse-container layout is configured");
		if (descriptor.Size < static_cast<std::uint32_t>(layout.HeaderSize)
			|| descriptor.ElementStride == 0
			|| (isMap && (!descriptor.Key || !descriptor.Mapped))
			|| (!isMap && !descriptor.Element))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "The sparse-container descriptor is invalid");
		}
		if (isMap
			&& (descriptor.Key->Size == 0
				|| descriptor.Mapped->Size == 0
				|| descriptor.MapKeyOffset > descriptor.ElementStride
				|| descriptor.Key->Size > descriptor.ElementStride - descriptor.MapKeyOffset
				|| descriptor.MapValueOffset > descriptor.ElementStride
				|| descriptor.Mapped->Size > descriptor.ElementStride - descriptor.MapValueOffset
				|| (static_cast<std::uint64_t>(descriptor.MapKeyOffset)
					< static_cast<std::uint64_t>(descriptor.MapValueOffset) + descriptor.Mapped->Size
					&& static_cast<std::uint64_t>(descriptor.MapValueOffset)
					< static_cast<std::uint64_t>(descriptor.MapKeyOffset) + descriptor.Key->Size)))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "A map key/value descriptor is out of bounds");
		}
		if (!isMap
			&& (descriptor.Element->Size == 0
				|| descriptor.ElementValueOffset > descriptor.ElementStride
				|| descriptor.Element->Size > descriptor.ElementStride - descriptor.ElementValueOffset))
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DESCRIPTOR_INVALID", "A set element descriptor is out of bounds");
		}

		DynamicHeader first;
		MemoryResult memory = ReadAt(address, layout.ElementsDataOffset, first.Data);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.ElementsNumOffset, first.Num);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.ElementsMaxOffset, first.Max);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		std::int32_t numBits = 0;
		std::int32_t maxBits = 0;
		std::uintptr_t secondaryBits = 0;
		memory = ReadAt(address, layout.AllocationNumBitsOffset, numBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.AllocationMaxBitsOffset, maxBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.AllocationSecondaryDataOffset, secondaryBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		if (!IsDynamicHeaderValid(first)
			|| first.Num > static_cast<std::int32_t>(kMaximumSparseSlots)
			|| numBits != first.Num
			|| maxBits < numBits)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_HEADER_INVALID", "The sparse-array data/bit header is inconsistent");
		}
		if (first.Num == 0)
		{
			DynamicHeader finalHeader;
			memory = ReadAt(address, layout.ElementsDataOffset, finalHeader.Data);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			memory = ReadAt(address, layout.ElementsNumOffset, finalHeader.Num);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			memory = ReadAt(address, layout.ElementsMaxOffset, finalHeader.Max);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			std::int32_t finalNumBits = 0;
			std::int32_t finalMaxBits = 0;
			std::uintptr_t finalSecondaryBits = 0;
			memory = ReadAt(address, layout.AllocationNumBitsOffset, finalNumBits);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			memory = ReadAt(address, layout.AllocationMaxBitsOffset, finalMaxBits);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			memory = ReadAt(address, layout.AllocationSecondaryDataOffset, finalSecondaryBits);
			if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
			if (finalHeader != first
				|| finalNumBits != numBits
				|| finalMaxBits != maxBits
				|| finalSecondaryBits != secondaryBits)
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_VALUE_CHANGED_DURING_READ", "The empty sparse-container header changed while it was decoded");
			}
			return MakeValue(descriptor, PropertyValueState::Empty);
		}

		std::size_t elementBytes = 0;
		if (!TryMultiply(
			static_cast<std::size_t>(first.Num),
			descriptor.ElementStride,
			elementBytes)
			|| elementBytes > m_Options.Limits.MaxReadableContainerBytes)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_CONTAINER_SIZE_OVERFLOW", "The sparse element range exceeds its byte budget");
		}
		memory = ValidateReadableMemory(first.Data, elementBytes);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);

		const std::size_t wordCount =
			(static_cast<std::size_t>(numBits) + 31u) / 32u;
		const std::size_t bitBytes = wordCount * sizeof(std::uint32_t);
		std::uintptr_t bitAddress = secondaryBits;
		if (bitAddress == 0)
		{
			if (wordCount > layout.InlineBitWordCount
				|| !TryAddAddress(
					address,
					static_cast<std::uint64_t>(layout.AllocationInlineDataOffset),
					bitBytes,
					bitAddress))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_CONTAINER_HEADER_INVALID", "The sparse allocation bitset has no valid storage");
			}
		}
		std::vector<std::uint32_t> words(wordCount);
		memory = ReadMemory(bitAddress, std::as_writable_bytes(std::span(words)));
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);

		std::uint32_t activeCount = 0;
		for (std::int32_t slot = 0; slot < first.Num; ++slot)
		{
			if ((words[static_cast<std::size_t>(slot) / 32u]
				& (std::uint32_t{1} << (static_cast<std::uint32_t>(slot) & 31u))) != 0)
			{
				++activeCount;
			}
		}

		PropertyValue value = MakeValue(
			descriptor,
			activeCount == 0 ? PropertyValueState::Empty : PropertyValueState::Ok);
		value.TotalCount = activeCount;
		value.Truncated = activeCount > m_Options.Limits.MaxContainerElements;
		value.Children.reserve((std::min)(activeCount, m_Options.Limits.MaxContainerElements));
		std::uint32_t emitted = 0;
		for (std::int32_t slot = 0;
			slot < first.Num && emitted < m_Options.Limits.MaxContainerElements;
			++slot)
		{
			if ((words[static_cast<std::size_t>(slot) / 32u]
				& (std::uint32_t{1} << (static_cast<std::uint32_t>(slot) & 31u))) == 0)
			{
				continue;
			}
			if (!TryConsumeSyntheticNode())
			{
				MarkNodeBudgetExhausted(value);
				break;
			}
			std::uintptr_t slotAddress = 0;
			if (!TryAddAddress(
				first.Data,
				static_cast<std::uint64_t>(slot) * descriptor.ElementStride,
				descriptor.ElementStride,
				slotAddress))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_ADDRESS_OVERFLOW", "A sparse element address overflowed");
			}

			PropertyValue item = MakeValue(descriptor, PropertyValueState::Ok);
			item.Label = std::to_string(slot);
			bool budgetExhausted = false;
			if (isMap)
			{
				std::uintptr_t keyAddress = 0;
				std::uintptr_t mappedAddress = 0;
				if (!TryAddAddress(slotAddress, descriptor.MapKeyOffset, descriptor.Key->Size, keyAddress)
					|| !TryAddAddress(slotAddress, descriptor.MapValueOffset, descriptor.Mapped->Size, mappedAddress))
				{
					return MakeFailure(descriptor, PropertyValueState::Error,
						"PROPERTY_ADDRESS_OVERFLOW", "A map key/value address overflowed");
				}
				if (!CanEmitNode())
				{
					MarkNodeBudgetExhausted(item);
					budgetExhausted = true;
				}
				else
				{
					PropertyValue key = Decode(keyAddress, *descriptor.Key, depth + 1);
					key.Label = "key";
					item.State = MergeState(item.State, key.State);
					if (!key.ErrorCode.empty())
					{
						item.ErrorCode = key.ErrorCode;
						item.ErrorMessage = key.ErrorMessage;
					}
					budgetExhausted = key.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED";
					item.Children.push_back(std::move(key));
				}
				if (!budgetExhausted && !CanEmitNode())
				{
					MarkNodeBudgetExhausted(item);
					budgetExhausted = true;
				}
				if (!budgetExhausted)
				{
					PropertyValue mapped = Decode(mappedAddress, *descriptor.Mapped, depth + 1);
					mapped.Label = "value";
					item.State = MergeState(item.State, mapped.State);
					if (item.ErrorCode.empty() && !mapped.ErrorCode.empty())
					{
						item.ErrorCode = mapped.ErrorCode;
						item.ErrorMessage = mapped.ErrorMessage;
					}
					budgetExhausted = mapped.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED";
					item.Children.push_back(std::move(mapped));
				}
			}
			else
			{
				std::uintptr_t elementAddress = 0;
				if (!TryAddAddress(
					slotAddress,
					descriptor.ElementValueOffset,
					descriptor.Element->Size,
					elementAddress))
				{
					return MakeFailure(descriptor, PropertyValueState::Error,
						"PROPERTY_ADDRESS_OVERFLOW", "A set element address overflowed");
				}
				if (!CanEmitNode())
				{
					MarkNodeBudgetExhausted(item);
					budgetExhausted = true;
				}
				else
				{
					PropertyValue element = Decode(elementAddress, *descriptor.Element, depth + 1);
					element.Label = "value";
					item.State = MergeState(item.State, element.State);
					item.ErrorCode = element.ErrorCode;
					item.ErrorMessage = element.ErrorMessage;
					budgetExhausted = element.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED";
					item.Children.push_back(std::move(element));
				}
			}
			value.State = MergeState(value.State, item.State);
			if (!item.ErrorCode.empty() && value.ErrorCode.empty())
			{
				value.ErrorCode = item.ErrorCode;
				value.ErrorMessage = item.ErrorMessage;
			}
			value.Children.push_back(std::move(item));
			++emitted;
			if (budgetExhausted)
			{
				value.Truncated = true;
				break;
			}
		}

		DynamicHeader finalHeader;
		memory = ReadAt(address, layout.ElementsDataOffset, finalHeader.Data);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.ElementsNumOffset, finalHeader.Num);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.ElementsMaxOffset, finalHeader.Max);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		std::int32_t finalNumBits = 0;
		std::int32_t finalMaxBits = 0;
		std::uintptr_t finalSecondaryBits = 0;
		memory = ReadAt(address, layout.AllocationNumBitsOffset, finalNumBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.AllocationMaxBitsOffset, finalMaxBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		memory = ReadAt(address, layout.AllocationSecondaryDataOffset, finalSecondaryBits);
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		if (finalHeader != first
			|| finalNumBits != numBits
			|| finalMaxBits != maxBits
			|| finalSecondaryBits != secondaryBits)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The sparse-container header changed while it was decoded");
		}
		std::vector<std::uint32_t> finalWords(wordCount);
		memory = ReadMemory(bitAddress, std::as_writable_bytes(std::span(finalWords)));
		if (!memory.Ok()) return MakeMemoryFailure(descriptor, memory);
		if (finalWords != words)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_VALUE_CHANGED_DURING_READ", "The sparse-container allocation bitset changed while it was decoded");
		}
		return value;
	}

	PropertyValue FromReferenceResult(
		const PropertyDescriptor& descriptor,
		PropertyReferenceResult result,
		const std::uintptr_t expectedAddress,
		const std::int32_t expectedIndex,
		const std::int32_t expectedSerial)
	{
		if (result.State == PropertyValueState::Ok)
		{
			if (!IsStableObjectHandle(result.Handle)
				|| !IsReferenceResolverConfigured()
				|| result.Handle.SessionId != m_Options.ReferenceResolver->SessionId()
				|| result.Handle.ContextGeneration != m_Options.ReferenceResolver->ContextGeneration()
				|| (expectedAddress != 0 && result.Handle.Address != expectedAddress)
				|| (expectedIndex >= 0 && result.Handle.Index != expectedIndex)
				|| (expectedSerial > 0 && result.Handle.SerialNumber != expectedSerial))
			{
				return MakeFailure(descriptor, PropertyValueState::Error,
					"PROPERTY_REFERENCE_RESULT_INVALID", "The reference resolver returned a mismatched or incomplete stable handle");
			}
			PropertyValue value = MakeValue(descriptor, PropertyValueState::Ok);
			value.Scalar = PropertyObjectReference{.Handle = std::move(result.Handle)};
			return value;
		}
		if (result.State != PropertyValueState::Unavailable
			&& result.State != PropertyValueState::Error)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_REFERENCE_RESULT_INVALID", "The reference resolver returned an invalid state");
		}
		if (result.ErrorCode.size() > kMaximumErrorCodeBytes
			|| result.ErrorMessage.size() > kMaximumDescriptorTextBytes)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_REFERENCE_RESULT_INVALID", "The reference resolver error metadata exceeds its byte limit");
		}
		return MakeFailure(
			descriptor,
			result.State,
			result.ErrorCode.empty() ? "PROPERTY_REFERENCE_RESOLUTION_FAILED" : std::move(result.ErrorCode),
			result.ErrorMessage.empty() ? "The object reference could not be resolved" : std::move(result.ErrorMessage));
	}

	const EngineNameCodec& m_Names;
	const PropertyCodecProfile& m_Profile;
	PropertyDecodeOptions m_Options;
	std::uint32_t m_Nodes = 0;
	std::size_t m_StructSnapshotBytes = 0;
	std::vector<std::pair<std::uintptr_t, const PropertyDescriptor*>> m_RecursionStack;
};

} // namespace

CanonicalMathStructKind ClassifyCanonicalMathStruct(
	const PropertyDescriptor& descriptor) noexcept
{
	return ClassifyCanonicalMathStructImpl(descriptor);
}

bool IsDescriptorProvenEnum(const PropertyDescriptor& descriptor) noexcept
{
	try
	{
		return IsDescriptorProvenEnumImpl(descriptor);
	}
	catch (...)
	{
		return false;
	}
}

const char* ToString(const PropertyValueState state) noexcept
{
	switch (state)
	{
	case PropertyValueState::Ok: return "ok";
	case PropertyValueState::Empty: return "empty";
	case PropertyValueState::Unsupported: return "unsupported";
	case PropertyValueState::Unavailable: return "unavailable";
	case PropertyValueState::Error: return "error";
	}
	return "error";
}

const char* ToString(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Unknown: return "unknown";
	case PropertyKind::Bool: return "bool";
	case PropertyKind::Int8: return "int8";
	case PropertyKind::Int16: return "int16";
	case PropertyKind::Int32: return "int32";
	case PropertyKind::Int64: return "int64";
	case PropertyKind::UInt8: return "uint8";
	case PropertyKind::UInt16: return "uint16";
	case PropertyKind::UInt32: return "uint32";
	case PropertyKind::UInt64: return "uint64";
	case PropertyKind::Float: return "float";
	case PropertyKind::Double: return "double";
	case PropertyKind::Name: return "name";
	case PropertyKind::String: return "string";
	case PropertyKind::Text: return "text";
	case PropertyKind::Object: return "object";
	case PropertyKind::WeakObject: return "weak_object";
	case PropertyKind::SoftObject: return "soft_object";
	case PropertyKind::Enum: return "enum";
	case PropertyKind::Struct: return "struct";
	case PropertyKind::Array: return "array";
	case PropertyKind::Map: return "map";
	case PropertyKind::Set: return "set";
	case PropertyKind::Delegate: return "delegate";
	}
	return "unknown";
}

const char* ToString(const PropertyEncodeError error) noexcept
{
	switch (error)
	{
	case PropertyEncodeError::None: return "NONE";
	case PropertyEncodeError::CodecNotConfigured: return "PROPERTY_CODEC_NOT_CONFIGURED";
	case PropertyEncodeError::KindUnavailable: return "PROPERTY_INPUT_KIND_UNAVAILABLE";
	case PropertyEncodeError::DescriptorInvalid: return "PROPERTY_DESCRIPTOR_INVALID";
	case PropertyEncodeError::DestinationTooSmall: return "PROPERTY_INPUT_DESTINATION_TOO_SMALL";
	case PropertyEncodeError::ValueTypeMismatch: return "PROPERTY_INPUT_TYPE_MISMATCH";
	case PropertyEncodeError::ValueOutOfRange: return "PROPERTY_INPUT_OUT_OF_RANGE";
	}
	return "PROPERTY_INPUT_UNKNOWN_ERROR";
}

bool IsPropertyCodecProfileValid(
	const PropertyCodecProfile& profile,
	const EngineNameProfile& nameProfile) noexcept
{
	return profile.Validated
		&& profile.ReflectionLayoutFingerprint != 0
		&& !profile.Source.empty()
		&& profile.Source.size() <= 1024
		&& nameProfile.Validated
		&& IsEngineNameProfileLayoutValid(nameProfile)
		&& (IsDynamicArrayLayoutAbsent(profile.DynamicArray)
			|| IsDynamicArrayLayoutValid(profile.DynamicArray))
		&& (IsTextLayoutAbsent(profile.Text)
			|| IsTextLayoutValid(profile.Text))
		&& (IsWeakObjectLayoutAbsent(profile.WeakObject)
			|| IsWeakObjectLayoutValid(profile.WeakObject))
		&& (IsSoftObjectLayoutAbsent(profile.SoftObject)
			|| (IsSoftObjectLayoutValid(profile.SoftObject, nameProfile)
				&& (profile.SoftObject.SubPathStringOffset == -1
					|| (IsDynamicArrayLayoutValid(profile.DynamicArray)
						&& IsSoftObjectSubPathValid(
							profile.SoftObject,
							profile.DynamicArray,
							nameProfile)))))
		&& (IsSparseContainerLayoutAbsent(profile.SparseContainer)
			|| IsSparseContainerLayoutValid(profile.SparseContainer));
}

PropertyCodec::PropertyCodec(
	const EngineNameCodec& names,
	PropertyCodecProfile profile)
	: m_Names(names),
	  m_Profile(std::move(profile)),
	  m_Configured(names.IsConfigured()
		&& IsPropertyCodecProfileValid(m_Profile, names.Profile()))
{
}

bool PropertyCodec::Supports(const PropertyKind kind) const noexcept
{
	if (!m_Configured)
		return false;
	switch (kind)
	{
	case PropertyKind::Unknown:
	case PropertyKind::Delegate:
		return false;
	case PropertyKind::String:
	case PropertyKind::Array:
		return IsDynamicArrayLayoutValid(m_Profile.DynamicArray);
	case PropertyKind::Text:
		return IsTextLayoutValid(m_Profile.Text)
			&& IsDynamicArrayLayoutValid(m_Profile.DynamicArray);
	case PropertyKind::WeakObject:
		return IsWeakObjectLayoutValid(m_Profile.WeakObject);
	case PropertyKind::SoftObject:
		return IsSoftObjectLayoutValid(m_Profile.SoftObject, m_Names.Profile())
			&& (m_Profile.SoftObject.SubPathStringOffset == -1
				|| (IsDynamicArrayLayoutValid(m_Profile.DynamicArray)
					&& IsSoftObjectSubPathValid(
						m_Profile.SoftObject,
						m_Profile.DynamicArray,
						m_Names.Profile())));
	case PropertyKind::Map:
	case PropertyKind::Set:
		return IsSparseContainerLayoutValid(m_Profile.SparseContainer);
	case PropertyKind::Bool:
	case PropertyKind::Int8:
	case PropertyKind::Int16:
	case PropertyKind::Int32:
	case PropertyKind::Int64:
	case PropertyKind::UInt8:
	case PropertyKind::UInt16:
	case PropertyKind::UInt32:
	case PropertyKind::UInt64:
	case PropertyKind::Float:
	case PropertyKind::Double:
	case PropertyKind::Name:
	case PropertyKind::Object:
	case PropertyKind::Enum:
	case PropertyKind::Struct:
		return true;
	}
	return false;
}

bool PropertyCodec::SupportsInput(const PropertyKind kind) const noexcept
{
	if (!m_Configured)
		return false;
	switch (kind)
	{
	case PropertyKind::Bool:
	case PropertyKind::Int8:
	case PropertyKind::Int16:
	case PropertyKind::Int32:
	case PropertyKind::Int64:
	case PropertyKind::UInt8:
	case PropertyKind::UInt16:
	case PropertyKind::UInt32:
	case PropertyKind::UInt64:
	case PropertyKind::Float:
	case PropertyKind::Double:
	case PropertyKind::Object:
		return true;
	default:
		return false;
	}
}

bool PropertyCodec::SupportsInput(const PropertyDescriptor& descriptor) const noexcept
{
	return m_Configured
		&& ((descriptor.Kind == PropertyKind::Enum
			&& IsDescriptorProvenEnum(descriptor))
		|| (descriptor.Kind != PropertyKind::Enum && SupportsInput(descriptor.Kind))
		|| ClassifyCanonicalMathStruct(descriptor) != CanonicalMathStructKind::None);
}

PropertyEncodeResult PropertyCodec::EncodeOwned(
	const std::span<std::byte> destination,
	const PropertyDescriptor& descriptor,
	const PropertyInputValue& value) const noexcept
{
	const auto failure = [](const PropertyEncodeError error, std::string message) {
		return PropertyEncodeResult{.Error = error, .Message = std::move(message)};
	};
	try
	{
		if (!m_Configured)
		{
			return failure(
				PropertyEncodeError::CodecNotConfigured,
				"No immutable property codec profile has passed validation");
		}
		if (descriptor.Kind == PropertyKind::Enum
			&& !IsDescriptorProvenEnum(descriptor))
		{
			return failure(
				PropertyEncodeError::DescriptorInvalid,
				"The enum descriptor does not prove an exact table and width-correct integer backing property");
		}
		if (!SupportsInput(descriptor))
		{
			return failure(
				PropertyEncodeError::KindUnavailable,
				"The property kind is not available for owned parameter input");
		}
		if (descriptor.TypeName.empty() || descriptor.Size == 0)
		{
			return failure(
				PropertyEncodeError::DescriptorInvalid,
				"The input descriptor is incomplete");
		}
		if (destination.size() < descriptor.Size)
		{
			return failure(
				PropertyEncodeError::DestinationTooSmall,
				"The owned parameter slot is smaller than its descriptor");
		}

		const auto copyValue = [&destination]<typename T>(const T encoded) {
			std::memcpy(destination.data(), &encoded, sizeof(encoded));
		};
		const auto encodeSigned = [&]<typename T>() -> PropertyEncodeResult {
			if (descriptor.Size != sizeof(T))
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The signed descriptor width does not match its reflected kind");
			}
			const auto* input = std::get_if<std::int64_t>(&value);
			if (!input)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The input must be a signed integer");
			}
			if (*input < static_cast<std::int64_t>((std::numeric_limits<T>::min)())
				|| *input > static_cast<std::int64_t>((std::numeric_limits<T>::max)()))
			{
				return failure(
					PropertyEncodeError::ValueOutOfRange,
					"The signed input is outside the reflected width");
			}
			copyValue(static_cast<T>(*input));
			return {};
		};
		const auto encodeUnsigned = [&]<typename T>() -> PropertyEncodeResult {
			if (descriptor.Size != sizeof(T))
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The unsigned descriptor width does not match its reflected kind");
			}
			const auto* input = std::get_if<std::uint64_t>(&value);
			if (!input)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The input must be an unsigned integer");
			}
			if (*input > static_cast<std::uint64_t>((std::numeric_limits<T>::max)()))
			{
				return failure(
					PropertyEncodeError::ValueOutOfRange,
					"The unsigned input is outside the reflected width");
			}
			copyValue(static_cast<T>(*input));
			return {};
		};

		switch (descriptor.Kind)
		{
		case PropertyKind::Bool:
		{
			const auto* input = std::get_if<bool>(&value);
			if (!input)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The input must be boolean");
			}
			if (descriptor.BoolMask == 0 || descriptor.BoolByteOffset >= descriptor.Size)
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The bool mask or byte offset is invalid");
			}
			auto& target = destination[descriptor.BoolByteOffset];
			const std::byte mask{descriptor.BoolMask};
			target = *input ? target | mask : target & ~mask;
			return {};
		}
		case PropertyKind::Int8: return encodeSigned.template operator()<std::int8_t>();
		case PropertyKind::Int16: return encodeSigned.template operator()<std::int16_t>();
		case PropertyKind::Int32: return encodeSigned.template operator()<std::int32_t>();
		case PropertyKind::Int64: return encodeSigned.template operator()<std::int64_t>();
		case PropertyKind::UInt8: return encodeUnsigned.template operator()<std::uint8_t>();
		case PropertyKind::UInt16: return encodeUnsigned.template operator()<std::uint16_t>();
		case PropertyKind::UInt32: return encodeUnsigned.template operator()<std::uint32_t>();
		case PropertyKind::UInt64: return encodeUnsigned.template operator()<std::uint64_t>();
		case PropertyKind::Float:
		{
			if (descriptor.Size != sizeof(float))
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The float descriptor width does not match its reflected kind");
			}
			const auto* input = std::get_if<double>(&value);
			if (!input)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The input must be a floating-point value");
			}
			const float encoded = static_cast<float>(*input);
			if (!std::isfinite(*input) || !std::isfinite(encoded))
			{
				return failure(
					PropertyEncodeError::ValueOutOfRange,
					"The floating-point input is non-finite or outside float range");
			}
			copyValue(encoded);
			return {};
		}
		case PropertyKind::Double:
		{
			if (descriptor.Size != sizeof(double))
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The double descriptor width does not match its reflected kind");
			}
			const auto* input = std::get_if<double>(&value);
			if (!input)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The input must be a floating-point value");
			}
			if (!std::isfinite(*input))
			{
				return failure(
					PropertyEncodeError::ValueOutOfRange,
					"The floating-point input must be finite");
			}
			copyValue(*input);
			return {};
		}
		case PropertyKind::Object:
		{
			if (descriptor.Size != sizeof(std::uintptr_t))
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The object descriptor width does not match the target pointer width");
			}
			std::uintptr_t encoded = 0;
			if (!std::holds_alternative<std::monostate>(value))
			{
				const auto* input = std::get_if<PropertyObjectReference>(&value);
				if (!input || !IsStableObjectHandle(input->Handle))
				{
					return failure(
						PropertyEncodeError::ValueTypeMismatch,
						"The object input must be null or a complete stable handle");
				}
				encoded = input->Handle.Address;
			}
			copyValue(encoded);
			return {};
		}
		case PropertyKind::Enum:
		{
			const auto* input = std::get_if<PropertyEnumInput>(&value);
			if (!input || input->TypeName != descriptor.TypeName)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The enum input type must match the exact reflected enum type");
			}
			std::uint64_t rawValue = 0;
			if (const auto* name = std::get_if<std::string>(&input->Selection))
			{
				const auto entry = std::ranges::find_if(
					descriptor.EnumEntries,
					[&name](const PropertyEnumEntry& candidate) {
						return candidate.Name == *name;
					});
				if (entry == descriptor.EnumEntries.end())
				{
					return failure(
						PropertyEncodeError::ValueOutOfRange,
						"The enum name is absent from the exact reflected enum table");
				}
				rawValue = entry->RawValue;
			}
			else if (const auto* signedValue = std::get_if<std::int64_t>(&input->Selection))
			{
				if (!IsSignedIntegerKind(descriptor.Element->Kind))
				{
					return failure(
						PropertyEncodeError::ValueTypeMismatch,
						"The enum raw value must match its unsigned reflected backing kind");
				}
				rawValue = static_cast<std::uint64_t>(*signedValue);
				if (!IsEnumRawValueInRange(descriptor.Element->Kind, rawValue))
				{
					return failure(
						PropertyEncodeError::ValueOutOfRange,
						"The enum raw value is outside its reflected signed width");
				}
			}
			else if (const auto* unsignedValue = std::get_if<std::uint64_t>(&input->Selection))
			{
				if (IsSignedIntegerKind(descriptor.Element->Kind))
				{
					return failure(
						PropertyEncodeError::ValueTypeMismatch,
						"The enum raw value must match its signed reflected backing kind");
				}
				rawValue = *unsignedValue;
				if (!IsEnumRawValueInRange(descriptor.Element->Kind, rawValue))
				{
					return failure(
						PropertyEncodeError::ValueOutOfRange,
						"The enum raw value is outside its reflected unsigned width");
				}
			}
			else
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The enum input must select an exact reflected name or raw value");
			}
			if (std::ranges::none_of(
				descriptor.EnumEntries,
				[rawValue](const PropertyEnumEntry& candidate) {
					return candidate.RawValue == rawValue;
				}))
			{
				return failure(
					PropertyEncodeError::ValueOutOfRange,
					"The enum raw value is absent from the exact reflected enum table");
			}
			const PropertyInputValue underlying = IsSignedIntegerKind(descriptor.Element->Kind)
				? PropertyInputValue{std::bit_cast<std::int64_t>(rawValue)}
				: PropertyInputValue{rawValue};
			return EncodeOwned(destination, *descriptor.Element, underlying);
		}
		case PropertyKind::Struct:
		{
			if (ClassifyCanonicalMathStruct(descriptor) == CanonicalMathStructKind::None)
			{
				return failure(
					PropertyEncodeError::DescriptorInvalid,
					"The struct descriptor is not an exact canonical FVector or FRotator");
			}
			const auto* input = std::get_if<PropertyMathStructInput>(&value);
			if (!input || input->TypeName != descriptor.TypeName)
			{
				return failure(
					PropertyEncodeError::ValueTypeMismatch,
					"The struct input type must match the exact reflected canonical type");
			}
			std::array<std::byte, 3 * sizeof(double)> encoded{};
			for (std::size_t index = 0; index < descriptor.Fields.size(); ++index)
			{
				const PropertyFieldDescriptor& field = descriptor.Fields[index];
				const PropertyEncodeResult component = EncodeOwned(
					std::span<std::byte>(encoded).subspan(
						field.Offset,
						field.Descriptor->Size),
					*field.Descriptor,
					PropertyInputValue{input->Components[index]});
				if (!component.Ok())
					return component;
			}
			std::memcpy(destination.data(), encoded.data(), descriptor.Size);
			return {};
		}
		default:
			return failure(
				PropertyEncodeError::KindUnavailable,
				"The property kind is not available for owned parameter input");
		}
	}
	catch (const std::bad_alloc&)
	{
		return failure(
			PropertyEncodeError::ValueTypeMismatch,
			"The property input error could not allocate diagnostic storage");
	}
	catch (...)
	{
		return failure(
			PropertyEncodeError::ValueTypeMismatch,
			"The property input could not be encoded");
	}
}

PropertyValue PropertyCodec::Decode(
	const std::uintptr_t address,
	const PropertyDescriptor& descriptor,
	PropertyDecodeOptions options) const noexcept
{
	try
	{
		if (!m_Configured)
		{
			return MakeFailure(descriptor, PropertyValueState::Unavailable,
				"PROPERTY_CODEC_NOT_CONFIGURED", "No immutable property codec profile has passed validation");
		}
		if (options.Limits.MaxDepth == 0
			|| options.Limits.MaxDepth > 64
			|| options.Limits.MaxContainerElements == 0
			|| options.Limits.MaxContainerElements > 4096
			|| options.Limits.MaxTotalNodes == 0
			|| options.Limits.MaxTotalNodes > 65536
			|| options.Limits.MaxStringCodeUnits == 0
			|| options.Limits.MaxStringCodeUnits > 1'048'576
			|| options.Limits.MaxReadableContainerBytes == 0
			|| options.Limits.MaxReadableContainerBytes > 256 * 1024 * 1024)
		{
			return MakeFailure(descriptor, PropertyValueState::Error,
				"PROPERTY_DECODE_LIMITS_INVALID", "The property decode limits are outside the supported range");
		}
		DecodeSession session(m_Names, m_Profile, std::move(options));
		return session.Decode(address, descriptor);
	}
	catch (const std::bad_alloc&)
	{
		return MakeFailure(descriptor, PropertyValueState::Error,
			"PROPERTY_ALLOCATION_FAILED", "The property decoder could not allocate bounded output storage");
	}
	catch (...)
	{
		return MakeFailure(descriptor, PropertyValueState::Error,
			"PROPERTY_DECODE_EXCEPTION", "The property decoder raised an unexpected exception");
	}
}

} // namespace UExplorer::Runtime
