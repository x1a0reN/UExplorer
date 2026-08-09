#pragma once

#include "EngineNameCodec.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace UExplorer::Runtime
{

class PropertyCodec;

struct ReflectionLayoutLimits final
{
	static constexpr std::size_t MaxSourceBytes = 1024;
	static constexpr std::size_t MaxWitnessIdBytes = 128;
	static constexpr std::size_t MaxWitnessNameBytes = 1024;
	static constexpr std::size_t MaxFields = 64;
	static constexpr std::size_t MaxWitnesses = 256;
	static constexpr std::int32_t MaxRecordSize = 4096;
};

enum class ReflectionPropertySystem : std::uint8_t
{
	Unavailable,
	UProperty,
	FProperty
};

const char* ToString(ReflectionPropertySystem system) noexcept;

enum class ReflectionRecordKind : std::uint8_t
{
	UStruct,
	UField,
	FField,
	FFieldClass,
	Property,
	BoolProperty,
	ByteProperty,
	ObjectProperty,
	StructProperty,
	ArrayProperty,
	MapProperty,
	SetProperty,
	EnumProperty
};

const char* ToString(ReflectionRecordKind kind) noexcept;

enum class ReflectionField : std::uint8_t
{
	StructSuper,
	StructChildren,
	StructChildProperties,
	StructPropertiesSize,
	StructMinAlignment,
	UFieldNext,
	FFieldClass,
	FFieldNext,
	FFieldName,
	FFieldClassCastFlags,
	PropertyArrayDim,
	PropertyElementSize,
	PropertyFlags,
	PropertyOffset,
	BoolFieldSize,
	BoolByteOffset,
	BoolByteMask,
	BoolFieldMask,
	BytePropertyEnum,
	ObjectPropertyClass,
	StructPropertyStruct,
	ArrayPropertyInner,
	MapPropertyKey,
	MapPropertyValue,
	SetPropertyElement,
	EnumPropertyUnderlying,
	EnumPropertyEnum
};

const char* ToString(ReflectionField field) noexcept;
ReflectionRecordKind RecordKindFor(ReflectionField field) noexcept;

enum class ReflectionFieldValueKind : std::uint8_t
{
	UInt8,
	Int32,
	UInt64,
	Pointer,
	FName
};

const char* ToString(ReflectionFieldValueKind kind) noexcept;
ReflectionFieldValueKind ValueKindFor(ReflectionField field) noexcept;

struct ReflectionFieldCandidate
{
	ReflectionField Field = ReflectionField::StructSuper;
	std::int32_t Offset = -1;
	std::int32_t ContainerSize = -1;
	std::string Source;
};

using ReflectionWitnessValue = std::variant<std::uint64_t, std::string>;

struct ReflectionFieldWitness
{
	std::string Id;
	ReflectionField Field = ReflectionField::StructSuper;
	std::uintptr_t BaseAddress = 0;
	ReflectionWitnessValue Expected;
};

struct ReflectionLayoutCandidate
{
	std::uint64_t ContextGeneration = 0;
	ReflectionPropertySystem PropertySystem = ReflectionPropertySystem::Unavailable;
	std::string Source;
	std::vector<ReflectionFieldCandidate> Fields;
	std::vector<ReflectionFieldWitness> Witnesses;
};

enum class ReflectionValidationError : std::uint8_t
{
	None,
	InvalidContextGeneration,
	PropertySystemUnavailable,
	SourceInvalid,
	FieldLimitExceeded,
	DuplicateField,
	UnexpectedField,
	MissingRequiredField,
	FieldLayoutInvalid,
	FieldOverlap,
	WitnessLimitExceeded,
	WitnessInvalid,
	WitnessMissing,
	WitnessMemoryUnavailable,
	WitnessValueMismatch,
	WitnessSemanticInvalid,
	NameCodecUnavailable,
	NameWitnessMismatch,
	AllocationFailed
};

const char* ToString(ReflectionValidationError error) noexcept;

struct ReflectionFieldReport
{
	ReflectionField Field = ReflectionField::StructSuper;
	ReflectionRecordKind RecordKind = ReflectionRecordKind::UStruct;
	ReflectionFieldValueKind ValueKind = ReflectionFieldValueKind::Pointer;
	std::int32_t Offset = -1;
	std::int32_t Width = -1;
	std::int32_t ContainerSize = -1;
	bool Validated = false;
	std::string Source;
	std::vector<std::string> Checks;
	std::vector<std::string> WitnessIds;
	std::string ReasonCode;
	std::string Reason;
};

struct ReflectionLayoutValidationResult;

class ReflectionLayout final
{
public:
	std::uint64_t ContextGeneration() const noexcept { return m_ContextGeneration; }
	std::uint64_t Fingerprint() const noexcept { return m_Fingerprint; }
	std::uint32_t ValidatedOnThreadId() const noexcept { return m_ValidatedOnThreadId; }
	ReflectionPropertySystem PropertySystem() const noexcept { return m_PropertySystem; }
	const std::string& Source() const noexcept { return m_Source; }
	const std::map<ReflectionField, ReflectionFieldReport>& Fields() const noexcept { return m_Fields; }
	const ReflectionFieldReport* Find(ReflectionField field) const noexcept;

private:
	friend struct ReflectionLayoutValidationResult;
	friend ReflectionLayoutValidationResult ValidateReflectionLayout(
		const ReflectionLayoutCandidate&,
		const EngineNameCodec&) noexcept;

	std::uint64_t m_ContextGeneration = 0;
	std::uint64_t m_Fingerprint = 0;
	std::uint32_t m_ValidatedOnThreadId = 0;
	ReflectionPropertySystem m_PropertySystem = ReflectionPropertySystem::Unavailable;
	std::string m_Source;
	std::map<ReflectionField, ReflectionFieldReport> m_Fields;
};

struct ReflectionLayoutValidationResult
{
	ReflectionValidationError Error = ReflectionValidationError::None;
	std::shared_ptr<const ReflectionLayout> Layout;
	std::vector<ReflectionFieldReport> FieldReports;
	std::string Reason;

	bool Ok() const noexcept
	{
		return Error == ReflectionValidationError::None && static_cast<bool>(Layout);
	}
};

ReflectionLayoutValidationResult ValidateReflectionLayout(
	const ReflectionLayoutCandidate& candidate,
	const EngineNameCodec& names) noexcept;

bool IsReflectionLayoutValid(
	const ReflectionLayout& layout,
	std::uint64_t expectedContextGeneration) noexcept;

struct ReflectionRuntimeSnapshot
{
	std::shared_ptr<const ReflectionLayout> Layout;
	std::shared_ptr<const PropertyCodec> Properties;

	bool IsLayoutConfigured(std::uint64_t expectedContextGeneration) const noexcept;
	bool IsPropertyCodecConfigured(std::uint64_t expectedContextGeneration) const noexcept;
	bool IsConfigured(std::uint64_t expectedContextGeneration) const noexcept;
};

} // namespace UExplorer::Runtime
