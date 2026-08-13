#pragma once

#include "PropertyCodec.h"
#include "TypeSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace UExplorer::Runtime
{

enum class ParamFrameError : std::uint8_t
{
	None,
	SizeInvalid,
	ParameterInvalid,
	ParameterOutOfBounds,
	InputEncodeFailed,
	AllocationFailed,
	DestructorSlotInvalid,
	DestructorSlotDuplicate
};

const char* ToString(ParamFrameError error) noexcept;

struct ParamFrameResult
{
	ParamFrameError Error = ParamFrameError::None;
	PropertyEncodeError EncodeError = PropertyEncodeError::None;
	std::string Message;

	bool Ok() const noexcept { return Error == ParamFrameError::None; }
};

// Owns the exact ProcessEvent parameter storage, including direct FString buffers,
// converted FText slots, and recursively descriptor-backed value structs.
class ParamFrame final
{
public:
	using Destructor = void (*)(void* value, const void* context) noexcept;

	static constexpr std::uint32_t kMaxSize = 16 * 1024 * 1024;

	static bool SupportsLifetime(PropertyKind kind) noexcept;
	static bool SupportsLifetime(const PropertyDescriptor& descriptor) noexcept;
	static ParamFrameResult Create(std::uint32_t size, ParamFrame& frame) noexcept;

	ParamFrame() = default;
	~ParamFrame() noexcept;
	ParamFrame(const ParamFrame&) = delete;
	ParamFrame& operator=(const ParamFrame&) = delete;
	ParamFrame(ParamFrame&& other) noexcept;
	ParamFrame& operator=(ParamFrame&& other) noexcept;

	ParamFrameResult SetInput(
		const ReflectedProperty& property,
		const PropertyInputValue& value,
		const PropertyCodec& codec) noexcept;
	ParamFrameResult CopyValue(
		const ReflectedProperty& property,
		std::span<const std::byte> value) noexcept;
	ParamFrameResult RegisterDestructor(
		const ReflectedProperty& property,
		Destructor destructor,
		const void* context = nullptr) noexcept;
	std::uintptr_t ValueAddress(const ReflectedProperty& property) const noexcept;
	void* Data() noexcept { return m_Bytes.empty() ? nullptr : m_Bytes.data(); }
	const void* Data() const noexcept { return m_Bytes.empty() ? nullptr : m_Bytes.data(); }
	std::size_t Size() const noexcept { return m_Bytes.size(); }

private:
	struct DestructorSlot
	{
		std::uint32_t Offset = 0;
		std::uint32_t Size = 0;
		Destructor Callback = nullptr;
		const void* Context = nullptr;
		bool Armed = false;
	};

	void Reset() noexcept;

	std::vector<std::byte> m_Bytes;
	std::vector<DestructorSlot> m_DestructorJournal;
	std::vector<std::unique_ptr<wchar_t[]>> m_StringBuffers;
};

} // namespace UExplorer::Runtime
