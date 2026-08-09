#pragma once

#include "EngineNameCodec.h"
#include "ObjectHandle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace UExplorer::Runtime
{

enum class PropertyValueState : std::uint8_t
{
	Ok,
	Empty,
	Unsupported,
	Unavailable,
	Error
};

const char* ToString(PropertyValueState state) noexcept;

enum class PropertyKind : std::uint8_t
{
	Unknown,
	Bool,
	Int8,
	Int16,
	Int32,
	Int64,
	UInt8,
	UInt16,
	UInt32,
	UInt64,
	Float,
	Double,
	Name,
	String,
	Text,
	Object,
	WeakObject,
	SoftObject,
	Enum,
	Struct,
	Array,
	Map,
	Set,
	Delegate
};

const char* ToString(PropertyKind kind) noexcept;

struct PropertyObjectReference
{
	ObjectHandle Handle;
};

using PropertyScalar = std::variant<
	std::monostate,
	bool,
	std::int64_t,
	std::uint64_t,
	double,
	std::string,
	PropertyObjectReference>;

struct PropertyValue
{
	PropertyValueState State = PropertyValueState::Error;
	PropertyKind Kind = PropertyKind::Unknown;
	std::string Label;
	std::string TypeName;
	PropertyScalar Scalar;
	std::string DisplayName;
	std::vector<PropertyValue> Children;
	std::uint32_t TotalCount = 0;
	bool Truncated = false;
	std::string ErrorCode;
	std::string ErrorMessage;

	bool Ok() const noexcept
	{
		return State == PropertyValueState::Ok || State == PropertyValueState::Empty;
	}
};

struct DynamicArrayLayout
{
	bool Validated = false;
	std::int32_t DataOffset = -1;
	std::int32_t NumOffset = -1;
	std::int32_t MaxOffset = -1;
	std::int32_t HeaderSize = -1;
};

struct TextLayout
{
	bool Validated = false;
	std::int32_t DataPointerOffset = -1;
	std::int32_t StringOffsetInData = -1;
	std::int32_t MinimumValueSize = -1;
};

struct WeakObjectLayout
{
	bool Validated = false;
	std::int32_t IndexOffset = -1;
	std::int32_t SerialOffset = -1;
	std::int32_t ValueSize = -1;
};

struct SoftObjectLayout
{
	bool Validated = false;
	std::array<std::int32_t, 2> AssetPathNameOffsets{-1, -1};
	std::uint8_t AssetPathNameCount = 0;
	std::int32_t SubPathStringOffset = -1;
	std::int32_t MinimumValueSize = -1;
};

struct SparseContainerLayout
{
	bool Validated = false;
	std::int32_t ElementsDataOffset = -1;
	std::int32_t ElementsNumOffset = -1;
	std::int32_t ElementsMaxOffset = -1;
	std::int32_t AllocationInlineDataOffset = -1;
	std::int32_t AllocationSecondaryDataOffset = -1;
	std::int32_t AllocationNumBitsOffset = -1;
	std::int32_t AllocationMaxBitsOffset = -1;
	std::int32_t HeaderSize = -1;
	std::uint8_t InlineBitWordCount = 0;
};

struct PropertyCodecProfile
{
	bool Validated = false;
	std::string Source;
	std::string ReasonCode;
	std::string Reason;
	DynamicArrayLayout DynamicArray;
	TextLayout Text;
	WeakObjectLayout WeakObject;
	SoftObjectLayout SoftObject;
	SparseContainerLayout SparseContainer;
};

bool IsPropertyCodecProfileValid(
	const PropertyCodecProfile& profile,
	const EngineNameProfile& nameProfile) noexcept;

struct PropertyDescriptor;

struct PropertyFieldDescriptor
{
	std::string Name;
	std::uint32_t Offset = 0;
	std::shared_ptr<const PropertyDescriptor> Descriptor;
};

struct PropertyEnumEntry
{
	std::uint64_t RawValue = 0;
	std::string Name;
};

struct PropertyDescriptor
{
	PropertyKind Kind = PropertyKind::Unknown;
	std::string TypeName;
	std::uint32_t Size = 0;
	std::uint32_t BoolByteOffset = 0;
	std::uint8_t BoolMask = 0;
	std::uint32_t ElementStride = 0;
	std::uint32_t ElementValueOffset = 0;
	std::uint32_t MapKeyOffset = 0;
	std::uint32_t MapValueOffset = 0;
	std::shared_ptr<const PropertyDescriptor> Element;
	std::shared_ptr<const PropertyDescriptor> Key;
	std::shared_ptr<const PropertyDescriptor> Mapped;
	std::vector<PropertyFieldDescriptor> Fields;
	std::vector<PropertyEnumEntry> EnumEntries;
};

struct PropertyReferenceResult
{
	PropertyValueState State = PropertyValueState::Error;
	ObjectHandle Handle;
	std::string ErrorCode;
	std::string ErrorMessage;

	bool Ok() const noexcept { return State == PropertyValueState::Ok; }
};

class IPropertyReferenceResolver
{
public:
	virtual ~IPropertyReferenceResolver() = default;
	// Resolver authority is immutable for the lifetime of one Decode call.
	virtual std::string_view SessionId() const noexcept = 0;
	virtual std::uint64_t ContextGeneration() const noexcept = 0;
	virtual PropertyReferenceResult ResolveAddress(std::uintptr_t address) = 0;
	virtual PropertyReferenceResult ResolveWeak(
		std::int32_t index,
		std::int32_t serialNumber) = 0;
};

struct PropertyDecodeLimits
{
	std::uint32_t MaxDepth = 8;
	std::uint32_t MaxContainerElements = 256;
	std::uint32_t MaxTotalNodes = 2048;
	std::uint32_t MaxStringCodeUnits = 65536;
	std::size_t MaxReadableContainerBytes = 64 * 1024 * 1024;
};

struct PropertyDecodeOptions
{
	PropertyDecodeLimits Limits;
	IPropertyReferenceResolver* ReferenceResolver = nullptr;
};

// Decodes only from immutable descriptors/profiles through SafeMemory. It does
// not consult mutable Off/Settings globals or legacy UE wrapper objects.
class PropertyCodec final
{
public:
	PropertyCodec(
		const EngineNameCodec& names,
		PropertyCodecProfile profile = {});

	bool IsConfigured() const noexcept { return m_Configured; }
	const PropertyCodecProfile& Profile() const noexcept { return m_Profile; }
	PropertyValue Decode(
		std::uintptr_t address,
		const PropertyDescriptor& descriptor,
		PropertyDecodeOptions options = {}) const noexcept;

private:
	EngineNameCodec m_Names;
	PropertyCodecProfile m_Profile;
	bool m_Configured = false;
};

} // namespace UExplorer::Runtime
