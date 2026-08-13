#include "ParamFrame.h"

#include <Windows.h>

#include <cstring>
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
	case PropertyKind::String:
	case PropertyKind::Text:
	case PropertyKind::Object:
		return true;
	default:
		return false;
	}
}

bool ParamFrame::SupportsLifetime(const PropertyDescriptor& descriptor) noexcept
{
	const auto supports = [&](auto&& self,
		const PropertyDescriptor& candidate,
		const std::size_t depth) noexcept -> bool {
		if (depth > 32)
			return false;
		if (SupportsLifetime(candidate.Kind))
			return true;
		if (candidate.Kind == PropertyKind::Enum)
			return IsDescriptorProvenEnum(candidate);
		if (candidate.Kind != PropertyKind::Struct
			|| candidate.Size == 0
			|| candidate.Fields.empty())
		{
			return false;
		}
		for (const PropertyFieldDescriptor& field : candidate.Fields)
		{
			if (!field.Descriptor
				|| field.Offset > candidate.Size
				|| field.Descriptor->Size > candidate.Size - field.Offset
				|| !self(self, *field.Descriptor, depth + 1))
			{
				return false;
			}
		}
		return true;
	};
	return supports(supports, descriptor, 0);
}

ParamFrame::~ParamFrame() noexcept
{
	Reset();
}

ParamFrame::ParamFrame(ParamFrame&& other) noexcept
	: m_Bytes(std::move(other.m_Bytes)),
	  m_DestructorJournal(std::move(other.m_DestructorJournal)),
	  m_StringBuffers(std::move(other.m_StringBuffers))
{
	other.m_DestructorJournal.clear();
	other.m_StringBuffers.clear();
	other.m_Bytes.clear();
}

ParamFrame& ParamFrame::operator=(ParamFrame&& other) noexcept
{
	if (this == &other)
		return *this;
	Reset();
	m_Bytes = std::move(other.m_Bytes);
	m_DestructorJournal = std::move(other.m_DestructorJournal);
	m_StringBuffers = std::move(other.m_StringBuffers);
	other.m_DestructorJournal.clear();
	other.m_StringBuffers.clear();
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
	m_StringBuffers.clear();
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
	if (property.Kind == PropertyKind::String)
	{
		const auto* input = std::get_if<std::string>(&value);
		const DynamicArrayLayout& layout = codec.Profile().DynamicArray;
		if (!input
			|| !codec.Supports(PropertyKind::String)
			|| !layout.Validated
			|| layout.HeaderSize <= 0
			|| property.Size < static_cast<std::uint32_t>(layout.HeaderSize)
			|| layout.DataOffset < 0
			|| layout.NumOffset < 0
			|| layout.MaxOffset < 0
			|| static_cast<std::size_t>(layout.DataOffset) + sizeof(std::uintptr_t) > property.Size
			|| static_cast<std::size_t>(layout.NumOffset) + sizeof(std::int32_t) > property.Size
			|| static_cast<std::size_t>(layout.MaxOffset) + sizeof(std::int32_t) > property.Size
			|| input->size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		{
			return Failure(
				ParamFrameError::InputEncodeFailed,
				"The FString input or dynamic-array layout is invalid",
				PropertyEncodeError::ValueTypeMismatch);
		}
		const int required = input->empty()
			? 0
			: MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				input->data(),
				static_cast<int>(input->size()),
				nullptr,
				0);
		if (required < 0 || (!input->empty() && required == 0)
			|| required == (std::numeric_limits<std::int32_t>::max)())
		{
			return Failure(
				ParamFrameError::InputEncodeFailed,
				"The FString input is not valid UTF-8 or exceeds the UTF-16 limit",
				PropertyEncodeError::ValueOutOfRange);
		}
		try
		{
			const std::int32_t count = static_cast<std::int32_t>(required + 1);
			auto units = std::make_unique<wchar_t[]>(static_cast<std::size_t>(count));
			if (required > 0
				&& MultiByteToWideChar(
					CP_UTF8,
					MB_ERR_INVALID_CHARS,
					input->data(),
					static_cast<int>(input->size()),
					units.get(),
					required) != required)
			{
				return Failure(
					ParamFrameError::InputEncodeFailed,
					"The FString UTF-8 to UTF-16 conversion failed",
					PropertyEncodeError::ValueTypeMismatch);
			}
			units[static_cast<std::size_t>(required)] = L'\0';
			const std::uintptr_t data = reinterpret_cast<std::uintptr_t>(units.get());
			std::byte* const slot = m_Bytes.data() + property.Offset;
			std::memcpy(slot + layout.DataOffset, &data, sizeof(data));
			std::memcpy(slot + layout.NumOffset, &count, sizeof(count));
			std::memcpy(slot + layout.MaxOffset, &count, sizeof(count));
			m_StringBuffers.push_back(std::move(units));
			return {};
		}
		catch (...)
		{
			return Failure(
				ParamFrameError::AllocationFailed,
				"The FString input buffer could not be allocated");
		}
	}
	if (property.Kind == PropertyKind::Text)
	{
		return Failure(
			ParamFrameError::InputEncodeFailed,
			"FText input requires game-thread Conv_StringToText conversion",
			PropertyEncodeError::KindUnavailable);
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

ParamFrameResult ParamFrame::CopyValue(
	const ReflectedProperty& property,
	const std::span<const std::byte> value) noexcept
{
	if (!property.Descriptor
		|| property.State != ReflectedMemberState::Supported
		|| property.ArrayDim != 1
		|| !SupportsLifetime(*property.Descriptor))
	{
		return Failure(
			ParamFrameError::ParameterInvalid,
			"The reflected parameter cannot receive an owned value copy");
	}
	if (!IsParameterRangeValid(property, m_Bytes.size())
		|| value.size() != property.Size)
	{
		return Failure(
			ParamFrameError::ParameterOutOfBounds,
			"The copied parameter value does not match the exact frame slot");
	}
	std::memcpy(m_Bytes.data() + property.Offset, value.data(), value.size());
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
