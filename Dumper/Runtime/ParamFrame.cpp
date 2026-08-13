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
	case ParamFrameError::DestructorSlotInvalid: return "PARAM_FRAME_DESTRUCTOR_SLOT_INVALID";
	case ParamFrameError::DestructorSlotDuplicate: return "PARAM_FRAME_DESTRUCTOR_SLOT_DUPLICATE";
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
		|| IsDescriptorProvenEnum(descriptor)
		|| ClassifyCanonicalMathStruct(descriptor) != CanonicalMathStructKind::None;
}

ParamFrame::~ParamFrame() noexcept
{
	Reset();
}

ParamFrame::ParamFrame(ParamFrame&& other) noexcept
	: m_Bytes(std::move(other.m_Bytes)),
	  m_DestructorJournal(std::move(other.m_DestructorJournal))
{
	other.m_DestructorJournal.clear();
	other.m_Bytes.clear();
}

ParamFrame& ParamFrame::operator=(ParamFrame&& other) noexcept
{
	if (this == &other)
		return *this;
	Reset();
	m_Bytes = std::move(other.m_Bytes);
	m_DestructorJournal = std::move(other.m_DestructorJournal);
	other.m_DestructorJournal.clear();
	other.m_Bytes.clear();
	return *this;
}

void ParamFrame::Reset() noexcept
{
	for (auto slot = m_DestructorJournal.rbegin();
		slot != m_DestructorJournal.rend();
		++slot)
	{
		if (!slot->Armed || !slot->Callback)
			continue;
		slot->Armed = false;
		void* const value = m_Bytes.empty()
			? nullptr
			: m_Bytes.data() + slot->Offset;
		if (value)
			slot->Callback(value, slot->Context);
	}
	m_DestructorJournal.clear();
	m_Bytes.clear();
}

ParamFrameResult ParamFrame::Create(
	const std::uint32_t size,
	ParamFrame& frame) noexcept
{
	frame.Reset();
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

ParamFrameResult ParamFrame::RegisterDestructor(
	const ReflectedProperty& property,
	const Destructor destructor,
	const void* const context) noexcept
{
	if (!destructor
		|| !property.Descriptor
		|| property.State != ReflectedMemberState::Supported
		|| property.ArrayDim != 1
		|| !IsParameterRangeValid(property, m_Bytes.size()))
	{
		return Failure(
			ParamFrameError::DestructorSlotInvalid,
			"The owned parameter destructor slot is incomplete or outside the frame");
	}
	for (const DestructorSlot& existing : m_DestructorJournal)
	{
		const std::uint64_t existingEnd =
			static_cast<std::uint64_t>(existing.Offset) + existing.Size;
		const std::uint64_t requestedEnd =
			static_cast<std::uint64_t>(property.Offset) + property.Size;
		if (static_cast<std::uint64_t>(property.Offset) < existingEnd
			&& static_cast<std::uint64_t>(existing.Offset) < requestedEnd)
		{
			return Failure(
				ParamFrameError::DestructorSlotDuplicate,
				"The owned parameter destructor slot overlaps an already armed slot");
		}
	}
	try
	{
		m_DestructorJournal.push_back({
			.Offset = property.Offset,
			.Size = property.Size,
			.Callback = destructor,
			.Context = context,
			.Armed = true
		});
		return {};
	}
	catch (...)
	{
		return Failure(
			ParamFrameError::AllocationFailed,
			"The owned parameter destructor journal could not be extended");
	}
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
