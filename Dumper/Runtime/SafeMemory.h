#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace UExplorer::Runtime
{

enum class MemoryError : std::uint8_t
{
	None,
	InvalidRange,
	QueryFailed,
	RegionNotCommitted,
	AccessDenied,
	ProtectionChangeFailed,
	AccessViolation,
	ProtectionRestoreFailed,
	ExecutableWriteDenied,
	InstructionCacheFlushRequired,
	InstructionCacheFlushFailed,
	ValueMismatch,
	AllocationFailed
};

const char* ToString(MemoryError error) noexcept;

struct MemoryResult
{
	MemoryError Error = MemoryError::None;
	std::uint32_t NativeError = 0;
	std::size_t BytesProcessed = 0;

	bool Ok() const noexcept { return Error == MemoryError::None; }
};

struct MemoryWriteOptions
{
	bool AllowProtectionChange = true;
	bool AllowExecutableWrite = false;
	bool FlushInstructionCache = false;
};

bool CheckedAddressRange(
	std::uintptr_t address,
	std::size_t size,
	std::uintptr_t& endExclusive) noexcept;

MemoryResult ValidateReadableMemory(std::uintptr_t address, std::size_t size) noexcept;
MemoryResult ReadMemory(std::uintptr_t address, std::span<std::byte> output) noexcept;
MemoryResult WriteMemory(
	std::uintptr_t address,
	std::span<const std::byte> input,
	MemoryWriteOptions options = {}) noexcept;

MemoryResult CompareExchangePointer(
	void** slot,
	void* expected,
	void* replacement,
	void** observed = nullptr) noexcept;

template<typename T>
MemoryResult ReadValue(const std::uintptr_t address, T& output) noexcept
{
	static_assert(std::is_trivially_copyable_v<T>);
	return ReadMemory(address, std::as_writable_bytes(std::span<T>(&output, 1)));
}

template<typename T>
MemoryResult WriteValue(
	const std::uintptr_t address,
	const T& value,
	const MemoryWriteOptions options = {}) noexcept
{
	static_assert(std::is_trivially_copyable_v<T>);
	return WriteMemory(address, std::as_bytes(std::span<const T>(&value, 1)), options);
}

} // namespace UExplorer::Runtime
