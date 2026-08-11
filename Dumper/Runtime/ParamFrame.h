#pragma once

#include "PropertyCodec.h"
#include "TypeSnapshot.h"

#include <cstddef>
#include <cstdint>
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
	AllocationFailed
};

const char* ToString(ParamFrameError error) noexcept;

struct ParamFrameResult
{
	ParamFrameError Error = ParamFrameError::None;
	PropertyEncodeError EncodeError = PropertyEncodeError::None;
	std::string Message;

	bool Ok() const noexcept { return Error == ParamFrameError::None; }
};

// Owns the exact ProcessEvent parameter storage. It admits scalar/object slots
// plus descriptor-proven canonical FVector/FRotator; other UE lifetimes remain closed.
class ParamFrame final
{
public:
	static constexpr std::uint32_t kMaxSize = 16 * 1024 * 1024;

	static bool SupportsLifetime(PropertyKind kind) noexcept;
	static bool SupportsLifetime(const PropertyDescriptor& descriptor) noexcept;
	static ParamFrameResult Create(std::uint32_t size, ParamFrame& frame) noexcept;

	ParamFrame() = default;
	ParamFrame(const ParamFrame&) = delete;
	ParamFrame& operator=(const ParamFrame&) = delete;
	ParamFrame(ParamFrame&&) noexcept = default;
	ParamFrame& operator=(ParamFrame&&) noexcept = default;

	ParamFrameResult SetInput(
		const ReflectedProperty& property,
		const PropertyInputValue& value,
		const PropertyCodec& codec) noexcept;
	std::uintptr_t ValueAddress(const ReflectedProperty& property) const noexcept;
	void* Data() noexcept { return m_Bytes.empty() ? nullptr : m_Bytes.data(); }
	const void* Data() const noexcept { return m_Bytes.empty() ? nullptr : m_Bytes.data(); }
	std::size_t Size() const noexcept { return m_Bytes.size(); }

private:
	std::vector<std::byte> m_Bytes;
};

} // namespace UExplorer::Runtime
