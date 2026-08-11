#include "ParamFrame.h"

#include <limits>
#include <new>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

ParamFrameResult Failure(
	const ParamFrameError error,
	std::string message,
	const PropertyEncodeError encodeError = PropertyEncodeError::None)
{
	return {
		.Error = error,
		.EncodeError = encodeError,
		.Message = std::move(message)
	};
}

bool IsParameterRangeValid(
	const ReflectedProperty& property,
	const std::size_t frameSize) noexcept
{
	return property.Descriptor
		&& property.Size != 0
		&& property.Descriptor->Size == property.Size
		&& property.Offset <= frameSize
		&& property.Size <= frameSize - property.Offset;
}

} // namespace

const char* ToString(const ParamFrameError error) noexcept
{
	switch (error)
	{
	case ParamFrameError::None: return "NONE";
	case ParamFrameError::SizeInvalid: return "PARAM_FRAME_SIZE_INVALID";
	case ParamFrameError::ParameterInvalid: return "PARAM_FRAME_PARAMETER_INVALID";
	case ParamFrameError::ParameterOutOfBounds: return "PARAM_FRAME_PARAMETER_OUT_OF_BOUNDS";
	case ParamFrameError::InputEncodeFailed: return "PARAM_FRAME_INPUT_ENCODE_FAILED";
	case ParamFrameError::AllocationFailed: return "PARAM_FRAME_ALLOCATION_FAILED";
	}
	return "PARAM_FRAME_UNKNOWN_ERROR";
}

bool ParamFrame::SupportsLifetime(const PropertyKind kind) noexcept
{
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
	case PropertyKind::Name:
	case PropertyKind::Object:
		return true;
	default:
		return false;
	}
}

bool ParamFrame::SupportsLifetime(const PropertyDescriptor& descriptor) noexcept
{
	return SupportsLifetime(descriptor.Kind)
		|| ClassifyCanonicalMathStruct(descriptor) != CanonicalMathStructKind::None;
}

ParamFrameResult ParamFrame::Create(
	const std::uint32_t size,
	ParamFrame& frame) noexcept
{
	frame.m_Bytes.clear();
	if (size > kMaxSize)
	{
		return Failure(
			ParamFrameError::SizeInvalid,
			"The reflected ProcessEvent parameter frame exceeds its hard limit");
	}
	try
	{
		frame.m_Bytes.resize(size, std::byte{0});
		return {};
	}
	catch (const std::bad_alloc&)
	{
		return Failure(
			ParamFrameError::AllocationFailed,
			"The owned ProcessEvent parameter frame could not be allocated");
	}
	catch (...)
	{
		return Failure(
			ParamFrameError::AllocationFailed,
			"The owned ProcessEvent parameter frame could not be initialized");
	}
}

ParamFrameResult ParamFrame::SetInput(
	const ReflectedProperty& property,
	const PropertyInputValue& value,
	const PropertyCodec& codec) noexcept
{
	if (!property.Descriptor
		|| property.State != ReflectedMemberState::Supported
		|| property.ArrayDim != 1
		|| !SupportsLifetime(*property.Descriptor))
	{
		return Failure(
			ParamFrameError::ParameterInvalid,
			"The reflected parameter is not a supported trivial frame member");
	}
	if (!IsParameterRangeValid(property, m_Bytes.size()))
	{
		return Failure(
			ParamFrameError::ParameterOutOfBounds,
			"The reflected parameter range is outside the owned frame");
	}
	const PropertyEncodeResult encoded = codec.EncodeOwned(
		std::span<std::byte>(m_Bytes).subspan(property.Offset, property.Size),
		*property.Descriptor,
		value);
	if (!encoded.Ok())
	{
		return Failure(
			ParamFrameError::InputEncodeFailed,
			encoded.Message,
			encoded.Error);
	}
	return {};
}

std::uintptr_t ParamFrame::ValueAddress(
	const ReflectedProperty& property) const noexcept
{
	if (!IsParameterRangeValid(property, m_Bytes.size()) || m_Bytes.empty())
		return 0;
	const auto base = reinterpret_cast<std::uintptr_t>(m_Bytes.data());
	if (property.Offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return 0;
	return base + property.Offset;
}

} // namespace UExplorer::Runtime
