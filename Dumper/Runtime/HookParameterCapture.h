#pragma once

#include "TypeSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace UExplorer::Runtime
{

inline constexpr std::size_t kHookParameterPayloadHeaderBytes = 16;
inline constexpr std::size_t kHookParameterMaximumFields = 64;
inline constexpr std::size_t kHookParameterMaximumPayloadBytes = 512;
inline constexpr std::size_t kHookParameterMaximumMetadataBytes = 8 * 1024;

enum class HookParameterCapturePhase : std::uint8_t
{
	Enter = 1,
	Exit = 2
};

const char* ToString(HookParameterCapturePhase phase) noexcept;

enum class HookParameterPlanError : std::uint8_t
{
	None,
	InvalidLimit,
	FunctionInvalid,
	NoParameters,
	FieldLimitExceeded,
	PropertyUnavailable,
	UnsupportedKind,
	DescriptorInvalid,
	RangeInvalid,
	MetadataLimitExceeded,
	PayloadLimitExceeded,
	AllocationFailed
};

const char* ToString(HookParameterPlanError error) noexcept;

struct HookParameterCaptureField
{
	std::string Name;
	std::string TypeName;
	ReflectedParameterDirection Direction = ReflectedParameterDirection::Input;
	PropertyKind Kind = PropertyKind::Unknown;
	std::uint32_t FrameOffset = 0;
	std::uint32_t Size = 0;
	std::uint32_t BoolByteOffset = 0;
	std::uint8_t BoolMask = 0;
};

struct HookParameterCapturePlan
{
	std::uint64_t Fingerprint = 0;
	std::uint64_t FunctionSignatureFingerprint = 0;
	std::uint32_t ParameterSize = 0;
	std::size_t MaximumPayloadBytes = 0;
	std::size_t EnterPayloadBytes = 0;
	std::size_t ExitPayloadBytes = 0;
	std::vector<HookParameterCaptureField> EnterFields;
	std::vector<HookParameterCaptureField> ExitFields;
};

struct HookParameterPlanResult
{
	HookParameterPlanError Error = HookParameterPlanError::None;
	std::shared_ptr<const HookParameterCapturePlan> Plan;
	std::string Detail;

	bool Ok() const noexcept
	{
		return Error == HookParameterPlanError::None && static_cast<bool>(Plan);
	}
};

enum class HookParameterPayloadStatus : std::uint8_t
{
	Ok = 0,
	FrameUnavailable = 1,
	ReadFailed = 2,
	InvalidPlan = 3,
	OutputTooSmall = 4,
	MalformedPayload = 5,
	PlanMismatch = 6
};

const char* ToString(HookParameterPayloadStatus status) noexcept;

struct HookParameterEncodeResult
{
	HookParameterPayloadStatus Status = HookParameterPayloadStatus::InvalidPlan;
	std::size_t PayloadBytes = 0;

	bool Encoded() const noexcept { return PayloadBytes != 0; }
};

struct HookCapturedParameter
{
	std::size_t FieldIndex = 0;
	std::string CanonicalValue;
};

struct HookParameterDecodeResult
{
	HookParameterPayloadStatus Status = HookParameterPayloadStatus::MalformedPayload;
	HookParameterCapturePhase Phase = HookParameterCapturePhase::Enter;
	std::uint64_t PlanFingerprint = 0;
	std::vector<HookCapturedParameter> Values;

	bool Ok() const noexcept { return Status == HookParameterPayloadStatus::Ok; }
};

HookParameterPlanResult BuildHookParameterCapturePlan(
	const ReflectedFunction& function,
	std::size_t maximumPayloadBytes) noexcept;

// The callback-side encoder performs no allocation, locking, decoding, or live
// UObject traversal. It copies only fields admitted by the immutable plan.
HookParameterEncodeResult EncodeHookParameterPayload(
	const HookParameterCapturePlan& plan,
	HookParameterCapturePhase phase,
	const void* parameterFrame,
	std::span<std::byte> output) noexcept;

// Decoding is worker-only and operates exclusively on the copied payload.
HookParameterDecodeResult DecodeHookParameterPayload(
	const HookParameterCapturePlan& plan,
	std::span<const std::byte> payload) noexcept;

} // namespace UExplorer::Runtime
