#include "HookParameterCapture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::uint32_t kPayloadMagic = 0x31504348U; // "HCP1" in little endian.
constexpr std::uint8_t kPayloadVersion = 1;
constexpr std::size_t kMaximumFieldTextBytes = 4096;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool IsBoundedText(const std::string_view value) noexcept
{
	return !value.empty() && value.size() <= kMaximumFieldTextBytes
		&& std::none_of(value.begin(), value.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

std::uint32_t ExactScalarSize(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Bool:
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

bool IsInputPhase(const ReflectedParameterDirection direction) noexcept
{
	return direction == ReflectedParameterDirection::Input
		|| direction == ReflectedParameterDirection::InOut;
}

bool IsOutputPhase(const ReflectedParameterDirection direction) noexcept
{
	return direction == ReflectedParameterDirection::Output
		|| direction == ReflectedParameterDirection::InOut
		|| direction == ReflectedParameterDirection::Return;
}

bool IsKnownDirection(const ReflectedParameterDirection direction) noexcept
{
	switch (direction)
	{
	case ReflectedParameterDirection::Input:
	case ReflectedParameterDirection::Output:
	case ReflectedParameterDirection::InOut:
	case ReflectedParameterDirection::Return:
		return true;
	}
	return false;
}

void AppendByte(std::uint64_t& hash, const std::uint8_t value) noexcept
{
	hash ^= value;
	hash *= kFnvPrime;
}

template<typename T>
void AppendUnsigned(std::uint64_t& hash, const T value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index)
		AppendByte(hash, static_cast<std::uint8_t>(value >> (index * 8U)));
}

void AppendText(std::uint64_t& hash, const std::string_view value) noexcept
{
	AppendUnsigned(hash, static_cast<std::uint64_t>(value.size()));
	for (const unsigned char character : value)
		AppendByte(hash, character);
}

void AppendField(std::uint64_t& hash, const HookParameterCaptureField& field) noexcept
{
	AppendText(hash, field.Name);
	AppendText(hash, field.TypeName);
	AppendByte(hash, static_cast<std::uint8_t>(field.Direction));
	AppendByte(hash, static_cast<std::uint8_t>(field.Kind));
	AppendUnsigned(hash, field.FrameOffset);
	AppendUnsigned(hash, field.Size);
	AppendUnsigned(hash, field.BoolByteOffset);
	AppendByte(hash, field.BoolMask);
}

std::uint64_t ComputePlanFingerprint(const HookParameterCapturePlan& plan) noexcept
{
	std::uint64_t hash = kFnvOffset;
	AppendText(hash, "UExplorer.HookParameterCapturePlan.v1");
	AppendUnsigned(hash, plan.FunctionSignatureFingerprint);
	AppendUnsigned(hash, plan.ParameterSize);
	AppendUnsigned(hash, static_cast<std::uint64_t>(plan.EnterFields.size()));
	for (const HookParameterCaptureField& field : plan.EnterFields)
		AppendField(hash, field);
	AppendUnsigned(hash, static_cast<std::uint64_t>(plan.ExitFields.size()));
	for (const HookParameterCaptureField& field : plan.ExitFields)
		AppendField(hash, field);
	return hash == 0 ? 1 : hash;
}

template<typename T>
void WriteLittleEndian(std::span<std::byte> output, const std::size_t offset, const T value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index)
	{
		output[offset + index] = static_cast<std::byte>(
			static_cast<std::uint8_t>(value >> (index * 8U)));
	}
}

template<typename T>
bool ReadLittleEndian(
	const std::span<const std::byte> input,
	const std::size_t offset,
	T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > input.size() || sizeof(T) > input.size() - offset)
		return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index)
	{
		value |= static_cast<T>(std::to_integer<std::uint8_t>(input[offset + index]))
			<< (index * 8U);
	}
	return true;
}

void WriteHeader(
	const HookParameterCapturePlan& plan,
	const HookParameterCapturePhase phase,
	const HookParameterPayloadStatus status,
	const std::uint8_t fieldCount,
	const std::span<std::byte> output) noexcept
{
	WriteLittleEndian(output, 0, kPayloadMagic);
	output[4] = static_cast<std::byte>(kPayloadVersion);
	output[5] = static_cast<std::byte>(phase);
	output[6] = static_cast<std::byte>(status);
	output[7] = static_cast<std::byte>(fieldCount);
	WriteLittleEndian(output, 8, plan.Fingerprint);
}

const std::vector<HookParameterCaptureField>& FieldsForPhase(
	const HookParameterCapturePlan& plan,
	const HookParameterCapturePhase phase) noexcept
{
	return phase == HookParameterCapturePhase::Enter
		? plan.EnterFields : plan.ExitFields;
}

std::size_t BytesForPhase(
	const HookParameterCapturePlan& plan,
	const HookParameterCapturePhase phase) noexcept
{
	return phase == HookParameterCapturePhase::Enter
		? plan.EnterPayloadBytes : plan.ExitPayloadBytes;
}

bool CopyField(
	const void* const frame,
	const HookParameterCaptureField& field,
	std::byte* const destination) noexcept
{
	if (!frame || !destination)
		return false;
	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(frame);
	if (base > (std::numeric_limits<std::uintptr_t>::max)() - field.FrameOffset)
		return false;
	const void* source = reinterpret_cast<const void*>(base + field.FrameOffset);
	__try
	{
		std::memcpy(destination, source, field.Size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

template<typename T>
T ReadNative(const std::span<const std::byte> value) noexcept
{
	T output{};
	std::memcpy(&output, value.data(), sizeof(T));
	return output;
}

template<typename T>
std::string IntegerText(const T value)
{
	std::array<char, 32> output{};
	const auto converted = std::to_chars(output.data(), output.data() + output.size(), value);
	return converted.ec == std::errc{}
		? std::string(output.data(), converted.ptr)
		: std::string{};
}

template<typename T>
std::string FloatingText(const T value)
{
	if (std::isnan(value))
		return "nan";
	if (std::isinf(value))
		return std::signbit(value) ? "-inf" : "inf";
	std::array<char, 64> output{};
	const auto converted = std::to_chars(
		output.data(),
		output.data() + output.size(),
		value,
		std::chars_format::general,
		(std::numeric_limits<T>::max_digits10));
	return converted.ec == std::errc{}
		? std::string(output.data(), converted.ptr)
		: std::string{};
}

std::string DecodeScalar(
	const HookParameterCaptureField& field,
	const std::span<const std::byte> value)
{
	const std::uint32_t exactSize = ExactScalarSize(field.Kind);
	if (exactSize == 0 || field.Size != exactSize || value.size() != field.Size)
		return {};
	if ((field.Kind == PropertyKind::Bool
			&& (field.BoolMask == 0 || field.BoolByteOffset >= field.Size))
		|| (field.Kind != PropertyKind::Bool
			&& (field.BoolByteOffset != 0 || field.BoolMask != 0)))
	{
		return {};
	}
	switch (field.Kind)
	{
	case PropertyKind::Bool:
		return (std::to_integer<std::uint8_t>(value[field.BoolByteOffset])
			& field.BoolMask) != 0 ? "true" : "false";
	case PropertyKind::Int8: return IntegerText(ReadNative<std::int8_t>(value));
	case PropertyKind::Int16: return IntegerText(ReadNative<std::int16_t>(value));
	case PropertyKind::Int32: return IntegerText(ReadNative<std::int32_t>(value));
	case PropertyKind::Int64: return IntegerText(ReadNative<std::int64_t>(value));
	case PropertyKind::UInt8: return IntegerText(ReadNative<std::uint8_t>(value));
	case PropertyKind::UInt16: return IntegerText(ReadNative<std::uint16_t>(value));
	case PropertyKind::UInt32: return IntegerText(ReadNative<std::uint32_t>(value));
	case PropertyKind::UInt64: return IntegerText(ReadNative<std::uint64_t>(value));
	case PropertyKind::Float: return FloatingText(ReadNative<float>(value));
	case PropertyKind::Double: return FloatingText(ReadNative<double>(value));
	default: return {};
	}
}

} // namespace

const char* ToString(const HookParameterCapturePhase phase) noexcept
{
	switch (phase)
	{
	case HookParameterCapturePhase::Enter: return "enter";
	case HookParameterCapturePhase::Exit: return "exit";
	}
	return "unknown";
}

const char* ToString(const HookParameterPlanError error) noexcept
{
	switch (error)
	{
	case HookParameterPlanError::None: return "NONE";
	case HookParameterPlanError::InvalidLimit: return "HOOK_PARAMETER_LIMIT_INVALID";
	case HookParameterPlanError::FunctionInvalid: return "HOOK_PARAMETER_FUNCTION_INVALID";
	case HookParameterPlanError::NoParameters: return "HOOK_PARAMETER_PLAN_EMPTY";
	case HookParameterPlanError::FieldLimitExceeded: return "HOOK_PARAMETER_FIELD_LIMIT_EXCEEDED";
	case HookParameterPlanError::PropertyUnavailable: return "HOOK_PARAMETER_PROPERTY_UNAVAILABLE";
	case HookParameterPlanError::UnsupportedKind: return "HOOK_PARAMETER_KIND_UNSUPPORTED";
	case HookParameterPlanError::DescriptorInvalid: return "HOOK_PARAMETER_DESCRIPTOR_INVALID";
	case HookParameterPlanError::RangeInvalid: return "HOOK_PARAMETER_RANGE_INVALID";
	case HookParameterPlanError::MetadataLimitExceeded:
		return "HOOK_PARAMETER_METADATA_LIMIT_EXCEEDED";
	case HookParameterPlanError::PayloadLimitExceeded: return "HOOK_PARAMETER_PAYLOAD_LIMIT_EXCEEDED";
	case HookParameterPlanError::AllocationFailed: return "HOOK_PARAMETER_ALLOCATION_FAILED";
	}
	return "HOOK_PARAMETER_PLAN_UNKNOWN_ERROR";
}

const char* ToString(const HookParameterPayloadStatus status) noexcept
{
	switch (status)
	{
	case HookParameterPayloadStatus::Ok: return "ok";
	case HookParameterPayloadStatus::FrameUnavailable: return "frame_unavailable";
	case HookParameterPayloadStatus::ReadFailed: return "read_failed";
	case HookParameterPayloadStatus::InvalidPlan: return "invalid_plan";
	case HookParameterPayloadStatus::OutputTooSmall: return "output_too_small";
	case HookParameterPayloadStatus::MalformedPayload: return "malformed_payload";
	case HookParameterPayloadStatus::PlanMismatch: return "plan_mismatch";
	}
	return "unknown";
}

HookParameterPlanResult BuildHookParameterCapturePlan(
	const ReflectedFunction& function,
	const std::size_t maximumPayloadBytes) noexcept
{
	try
	{
		if (maximumPayloadBytes < kHookParameterPayloadHeaderBytes
			|| maximumPayloadBytes > kHookParameterMaximumPayloadBytes)
		{
			return {.Error = HookParameterPlanError::InvalidLimit};
		}
		if (function.Handle.SignatureFingerprint == 0)
			return {.Error = HookParameterPlanError::FunctionInvalid};
		if (function.ParameterSize == 0 || function.Parameters.empty())
			return {.Error = HookParameterPlanError::NoParameters};
		if (function.Parameters.size() > kHookParameterMaximumFields)
			return {.Error = HookParameterPlanError::FieldLimitExceeded};

		auto plan = std::make_shared<HookParameterCapturePlan>();
		plan->FunctionSignatureFingerprint = function.Handle.SignatureFingerprint;
		plan->ParameterSize = function.ParameterSize;
		plan->MaximumPayloadBytes = maximumPayloadBytes;
		plan->EnterFields.reserve(function.Parameters.size());
		plan->ExitFields.reserve(function.Parameters.size());

		for (const ReflectedParameter& parameter : function.Parameters)
		{
			if (!IsKnownDirection(parameter.Direction))
				return {.Error = HookParameterPlanError::DescriptorInvalid};
			const ReflectedProperty& property = parameter.Property;
			if (property.State != ReflectedMemberState::Supported || !property.Descriptor)
			{
				return {
					.Error = HookParameterPlanError::PropertyUnavailable,
					.Detail = property.Name
				};
			}
			const std::uint32_t exactSize = ExactScalarSize(property.Kind);
			if (exactSize == 0)
			{
				return {
					.Error = HookParameterPlanError::UnsupportedKind,
					.Detail = property.Name
				};
			}
			const PropertyDescriptor& descriptor = *property.Descriptor;
			if (property.ArrayDim != 1
				|| property.Size != exactSize
				|| descriptor.Kind != property.Kind
				|| descriptor.Size != property.Size
				|| !IsBoundedText(property.Name)
				|| !IsBoundedText(property.TypeName))
			{
				return {
					.Error = HookParameterPlanError::DescriptorInvalid,
					.Detail = property.Name
				};
			}
			if (property.Offset > function.ParameterSize
				|| property.Size > function.ParameterSize - property.Offset)
			{
				return {
					.Error = HookParameterPlanError::RangeInvalid,
					.Detail = property.Name
				};
			}
			HookParameterCaptureField field{
				.Name = property.Name,
				.TypeName = property.TypeName,
				.Direction = parameter.Direction,
				.Kind = property.Kind,
				.FrameOffset = property.Offset,
				.Size = property.Size,
				.BoolByteOffset = descriptor.BoolByteOffset,
				.BoolMask = descriptor.BoolMask
			};
			if (property.Kind == PropertyKind::Bool)
			{
				if (field.BoolMask == 0 || field.BoolByteOffset >= field.Size)
				{
					return {
						.Error = HookParameterPlanError::DescriptorInvalid,
						.Detail = property.Name
					};
				}
			}
			else if (field.BoolByteOffset != 0 || field.BoolMask != 0)
			{
				return {
					.Error = HookParameterPlanError::DescriptorInvalid,
					.Detail = property.Name
				};
			}
			if (IsInputPhase(parameter.Direction))
				plan->EnterFields.push_back(field);
			if (IsOutputPhase(parameter.Direction))
				plan->ExitFields.push_back(std::move(field));
		}

		const auto payloadBytes = [](const auto& fields) noexcept {
			std::size_t total = kHookParameterPayloadHeaderBytes;
			for (const HookParameterCaptureField& field : fields)
			{
				if (field.Size > kHookParameterMaximumPayloadBytes - total)
					return kHookParameterMaximumPayloadBytes + 1;
				total += field.Size;
			}
			return total;
		};
		const auto metadataBytes = [](const auto& fields) noexcept {
			std::size_t total = 0;
			for (const HookParameterCaptureField& field : fields)
			{
				if (field.Name.size() > kHookParameterMaximumMetadataBytes - total)
					return kHookParameterMaximumMetadataBytes + 1;
				total += field.Name.size();
				if (field.TypeName.size() > kHookParameterMaximumMetadataBytes - total)
					return kHookParameterMaximumMetadataBytes + 1;
				total += field.TypeName.size();
			}
			return total;
		};
		if (metadataBytes(plan->EnterFields) > kHookParameterMaximumMetadataBytes
			|| metadataBytes(plan->ExitFields) > kHookParameterMaximumMetadataBytes)
		{
			return {.Error = HookParameterPlanError::MetadataLimitExceeded};
		}
		plan->EnterPayloadBytes = payloadBytes(plan->EnterFields);
		plan->ExitPayloadBytes = payloadBytes(plan->ExitFields);
		if (plan->EnterPayloadBytes > maximumPayloadBytes
			|| plan->ExitPayloadBytes > maximumPayloadBytes)
		{
			return {.Error = HookParameterPlanError::PayloadLimitExceeded};
		}
		plan->Fingerprint = ComputePlanFingerprint(*plan);
		return {.Plan = std::move(plan)};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = HookParameterPlanError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = HookParameterPlanError::DescriptorInvalid};
	}
}

HookParameterEncodeResult EncodeHookParameterPayload(
	const HookParameterCapturePlan& plan,
	const HookParameterCapturePhase phase,
	const void* const parameterFrame,
	const std::span<std::byte> output) noexcept
{
	if (plan.Fingerprint == 0 || plan.ParameterSize == 0
		|| plan.MaximumPayloadBytes < kHookParameterPayloadHeaderBytes
		|| plan.MaximumPayloadBytes > kHookParameterMaximumPayloadBytes
		|| (phase != HookParameterCapturePhase::Enter
			&& phase != HookParameterCapturePhase::Exit))
	{
		return {.Status = HookParameterPayloadStatus::InvalidPlan};
	}
	const auto& fields = FieldsForPhase(plan, phase);
	const std::size_t required = BytesForPhase(plan, phase);
	if (fields.size() > kHookParameterMaximumFields
		|| required < kHookParameterPayloadHeaderBytes
		|| required > plan.MaximumPayloadBytes)
	{
		return {.Status = HookParameterPayloadStatus::InvalidPlan};
	}
	if (output.size() < kHookParameterPayloadHeaderBytes)
		return {.Status = HookParameterPayloadStatus::OutputTooSmall};
	if (!parameterFrame)
	{
		WriteHeader(plan, phase, HookParameterPayloadStatus::FrameUnavailable, 0, output);
		return {
			.Status = HookParameterPayloadStatus::FrameUnavailable,
			.PayloadBytes = kHookParameterPayloadHeaderBytes
		};
	}
	if (output.size() < required)
	{
		WriteHeader(plan, phase, HookParameterPayloadStatus::OutputTooSmall, 0, output);
		return {
			.Status = HookParameterPayloadStatus::OutputTooSmall,
			.PayloadBytes = kHookParameterPayloadHeaderBytes
		};
	}

	std::size_t cursor = kHookParameterPayloadHeaderBytes;
	for (const HookParameterCaptureField& field : fields)
	{
		if (field.Size == 0 || field.FrameOffset > plan.ParameterSize
			|| field.Size > plan.ParameterSize - field.FrameOffset
			|| cursor > required
			|| field.Size > required - cursor
			|| !CopyField(parameterFrame, field, output.data() + cursor))
		{
			WriteHeader(plan, phase, HookParameterPayloadStatus::ReadFailed, 0, output);
			return {
				.Status = HookParameterPayloadStatus::ReadFailed,
				.PayloadBytes = kHookParameterPayloadHeaderBytes
			};
		}
		cursor += field.Size;
	}
	WriteHeader(
		plan,
		phase,
		HookParameterPayloadStatus::Ok,
		static_cast<std::uint8_t>(fields.size()),
		output);
	return {.Status = HookParameterPayloadStatus::Ok, .PayloadBytes = cursor};
}

HookParameterDecodeResult DecodeHookParameterPayload(
	const HookParameterCapturePlan& plan,
	const std::span<const std::byte> payload) noexcept
{
	HookParameterDecodeResult result;
	try
	{
		std::uint32_t magic = 0;
		std::uint64_t fingerprint = 0;
		if (payload.size() < kHookParameterPayloadHeaderBytes
			|| !ReadLittleEndian(payload, 0, magic)
			|| !ReadLittleEndian(payload, 8, fingerprint)
			|| magic != kPayloadMagic
			|| std::to_integer<std::uint8_t>(payload[4]) != kPayloadVersion)
		{
			return result;
		}
		result.PlanFingerprint = fingerprint;
		const auto phase = static_cast<HookParameterCapturePhase>(
			std::to_integer<std::uint8_t>(payload[5]));
		if (phase != HookParameterCapturePhase::Enter
			&& phase != HookParameterCapturePhase::Exit)
		{
			return result;
		}
		result.Phase = phase;
		if (fingerprint != plan.Fingerprint)
		{
			result.Status = HookParameterPayloadStatus::PlanMismatch;
			return result;
		}
		const auto encodedStatus = static_cast<HookParameterPayloadStatus>(
			std::to_integer<std::uint8_t>(payload[6]));
		if (encodedStatus != HookParameterPayloadStatus::Ok)
		{
			if ((encodedStatus == HookParameterPayloadStatus::FrameUnavailable
					|| encodedStatus == HookParameterPayloadStatus::ReadFailed
					|| encodedStatus == HookParameterPayloadStatus::OutputTooSmall)
				&& payload.size() == kHookParameterPayloadHeaderBytes
				&& std::to_integer<std::uint8_t>(payload[7]) == 0)
			{
				result.Status = encodedStatus;
			}
			return result;
		}

		const auto& fields = FieldsForPhase(plan, phase);
		if (std::to_integer<std::uint8_t>(payload[7]) != fields.size()
			|| payload.size() != BytesForPhase(plan, phase))
		{
			return result;
		}
		result.Values.reserve(fields.size());
		std::size_t cursor = kHookParameterPayloadHeaderBytes;
		for (std::size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex)
		{
			const HookParameterCaptureField& field = fields[fieldIndex];
			if (field.Size == 0 || cursor > payload.size()
				|| field.Size > payload.size() - cursor)
				return {};
			const std::span<const std::byte> value(payload.data() + cursor, field.Size);
			std::string canonical = DecodeScalar(field, value);
			if (canonical.empty())
				return {};
			result.Values.push_back({
				.FieldIndex = fieldIndex,
				.CanonicalValue = std::move(canonical)
			});
			cursor += field.Size;
		}
		result.Status = HookParameterPayloadStatus::Ok;
		return result;
	}
	catch (...)
	{
		return {};
	}
}

} // namespace UExplorer::Runtime
