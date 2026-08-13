#include "SafeMemory.h"

#include <Windows.h>

#include "API/WinMemApi.h"

extern "C" __declspec(dllimport) SIZE_T __stdcall VirtualQuery(
	LPCVOID lpAddress,
	PMEMORY_BASIC_INFORMATION lpBuffer,
	SIZE_T dwLength);

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

struct RegionSegment
{
	std::uintptr_t Address = 0;
	std::size_t Size = 0;
	DWORD Protection = 0;
};

struct ChangedProtection
{
	RegionSegment Segment;
	DWORD OriginalProtection = 0;
	DWORD AppliedProtection = 0;
};

struct ProtectionRestoreResult
{
	bool Restored = true;
	bool RaceDetected = false;
	DWORD NativeError = 0;
};

MemoryResult Failure(const MemoryError error, const DWORD nativeError = 0) noexcept
{
	return {.Error = error, .NativeError = nativeError};
}

DWORD BaseProtection(const DWORD protection) noexcept
{
	return protection & 0xFFu;
}

bool IsReadableProtection(const DWORD protection) noexcept
{
	if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;
	switch (BaseProtection(protection))
	{
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsWritableProtection(const DWORD protection) noexcept
{
	if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;
	switch (BaseProtection(protection))
	{
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsExecutableProtection(const DWORD protection) noexcept
{
	switch (BaseProtection(protection))
	{
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

MemoryResult QuerySegments(
	const std::uintptr_t address,
	const std::size_t size,
	std::vector<RegionSegment>& segments)
{
	std::uintptr_t endExclusive = 0;
	if (!CheckedAddressRange(address, size, endExclusive))
		return Failure(MemoryError::InvalidRange);

	std::uintptr_t cursor = address;
	while (cursor < endExclusive)
	{
		MEMORY_BASIC_INFORMATION information{};
		if (VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information)) == 0)
			return Failure(MemoryError::QueryFailed, GetLastError());
		if (information.State != MEM_COMMIT)
			return Failure(MemoryError::RegionNotCommitted);
		if ((information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			return Failure(MemoryError::AccessDenied);

		const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
		if (information.RegionSize == 0
			|| regionBase > (std::numeric_limits<std::uintptr_t>::max)() - information.RegionSize)
		{
			return Failure(MemoryError::QueryFailed);
		}
		const std::uintptr_t regionEnd = regionBase + information.RegionSize;
		if (cursor < regionBase || cursor >= regionEnd)
			return Failure(MemoryError::QueryFailed);

		const std::uintptr_t segmentEnd = (std::min)(regionEnd, endExclusive);
		segments.push_back({
			.Address = cursor,
			.Size = static_cast<std::size_t>(segmentEnd - cursor),
			.Protection = information.Protect
		});
		cursor = segmentEnd;
	}
	return {.BytesProcessed = size};
}

bool TryQueryCurrentProtection(
	const RegionSegment& segment,
	DWORD& protection,
	DWORD& nativeError) noexcept
{
	MEMORY_BASIC_INFORMATION information{};
	if (VirtualQuery(
		reinterpret_cast<const void*>(segment.Address),
		&information,
		sizeof(information)) == 0)
	{
		nativeError = GetLastError();
		return false;
	}
	if (information.State != MEM_COMMIT || information.RegionSize == 0)
		return false;

	const std::uintptr_t regionBase =
		reinterpret_cast<std::uintptr_t>(information.BaseAddress);
	if (regionBase > (std::numeric_limits<std::uintptr_t>::max)()
		- information.RegionSize)
	{
		return false;
	}
	const std::uintptr_t regionEnd = regionBase + information.RegionSize;
	std::uintptr_t segmentEnd = 0;
	if (!CheckedAddressRange(segment.Address, segment.Size, segmentEnd)
		|| segment.Address < regionBase
		|| segmentEnd > regionEnd)
	{
		return false;
	}
	protection = information.Protect;
	return true;
}

ProtectionRestoreResult RestoreProtections(
	const std::vector<ChangedProtection>& changes) noexcept
{
	ProtectionRestoreResult result;
	for (auto it = changes.rbegin(); it != changes.rend(); ++it)
	{
		DWORD replacedProtection = 0;
		if (!VirtualProtect(
			reinterpret_cast<void*>(it->Segment.Address),
			it->Segment.Size,
			it->OriginalProtection,
			&replacedProtection))
		{
			result.Restored = false;
			if (result.NativeError == 0)
				result.NativeError = GetLastError();
			continue;
		}
		if (replacedProtection != it->AppliedProtection)
			result.RaceDetected = true;

		DWORD observedProtection = 0;
		DWORD queryError = 0;
		if (!TryQueryCurrentProtection(it->Segment, observedProtection, queryError))
		{
			result.Restored = false;
			if (result.NativeError == 0)
				result.NativeError = queryError;
		}
		else if (observedProtection != it->OriginalProtection)
		{
			result.RaceDetected = true;
		}
	}
	return result;
}

MemoryResult FailureAfterRestore(
	const MemoryError error,
	const DWORD nativeError,
	const std::vector<ChangedProtection>& changes) noexcept
{
	const ProtectionRestoreResult restore = RestoreProtections(changes);
	if (!restore.Restored)
		return Failure(MemoryError::ProtectionRestoreFailed, restore.NativeError);
	if (restore.RaceDetected)
		return Failure(MemoryError::ProtectionRaceDetected);
	return Failure(error, nativeError);
}

MemoryResult MakeWritable(
	const std::vector<RegionSegment>& segments,
	const MemoryWriteOptions options,
	std::vector<ChangedProtection>& changes)
{
	for (const RegionSegment& segment : segments)
	{
		DWORD currentProtection = 0;
		DWORD queryError = 0;
		if (!TryQueryCurrentProtection(segment, currentProtection, queryError))
		{
			return FailureAfterRestore(
				MemoryError::ProtectionRaceDetected,
				queryError,
				changes);
		}
		if (currentProtection != segment.Protection)
		{
			return FailureAfterRestore(
				MemoryError::ProtectionRaceDetected,
				0,
				changes);
		}
		if (IsExecutableProtection(currentProtection) && !options.AllowExecutableWrite)
		{
			return FailureAfterRestore(
				MemoryError::ExecutableWriteDenied,
				0,
				changes);
		}
		if (IsExecutableProtection(currentProtection) && !options.FlushInstructionCache)
		{
			return FailureAfterRestore(
				MemoryError::InstructionCacheFlushRequired,
				0,
				changes);
		}
		if (!options.AllowProtectionChange)
		{
			if (IsWritableProtection(currentProtection))
				continue;
			return Failure(MemoryError::AccessDenied);
		}

		const DWORD writableProtection = IsExecutableProtection(currentProtection)
			? PAGE_EXECUTE_READWRITE
			: PAGE_READWRITE;
		DWORD originalProtection = 0;
		if (!VirtualProtect(
			reinterpret_cast<void*>(segment.Address),
			segment.Size,
			writableProtection,
			&originalProtection))
		{
			const DWORD protectionError = GetLastError();
			return FailureAfterRestore(
				MemoryError::ProtectionChangeFailed,
				protectionError,
				changes);
		}
		changes.push_back({
			.Segment = segment,
			.OriginalProtection = originalProtection,
			.AppliedProtection = writableProtection
		});
		if (originalProtection != currentProtection)
		{
			return FailureAfterRestore(
				MemoryError::ProtectionRaceDetected,
				0,
				changes);
		}
		if (IsExecutableProtection(originalProtection) && !options.AllowExecutableWrite)
		{
			return FailureAfterRestore(
				MemoryError::ExecutableWriteDenied,
				0,
				changes);
		}
	}
	return {};
}

MemoryResult ValidateAppliedProtections(
	const std::vector<ChangedProtection>& changes,
	const MemoryWriteOptions options) noexcept
{
	for (const ChangedProtection& change : changes)
	{
		DWORD observedProtection = 0;
		DWORD nativeError = 0;
		if (!TryQueryCurrentProtection(
			change.Segment,
			observedProtection,
			nativeError))
		{
			return Failure(MemoryError::ProtectionRaceDetected, nativeError);
		}
		if (observedProtection != change.AppliedProtection)
			return Failure(MemoryError::ProtectionRaceDetected);
		if (!IsWritableProtection(observedProtection))
			return Failure(MemoryError::ProtectionRaceDetected);
		if (IsExecutableProtection(observedProtection) && !options.AllowExecutableWrite)
			return Failure(MemoryError::ExecutableWriteDenied);
	}
	return {};
}

bool CopyWithSeh(void* destination, const void* source, const std::size_t size, DWORD& exceptionCode) noexcept
{
#if defined(_MSC_VER)
	__try
	{
		std::memcpy(destination, source, size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		exceptionCode = GetExceptionCode();
		return false;
	}
#else
	std::memcpy(destination, source, size);
	return true;
#endif
}

bool CompareExchangePointerWithSeh(
	void* volatile* slot,
	void* expected,
	void* replacement,
	void*& actual,
	DWORD& exceptionCode) noexcept
{
#if defined(_MSC_VER)
	__try
	{
		actual = InterlockedCompareExchangePointer(slot, replacement, expected);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		exceptionCode = GetExceptionCode();
		return false;
	}
#else
	actual = InterlockedCompareExchangePointer(slot, replacement, expected);
	return true;
#endif
}

} // namespace

const char* ToString(const MemoryError error) noexcept
{
	switch (error)
	{
	case MemoryError::None: return "NONE";
	case MemoryError::InvalidRange: return "INVALID_RANGE";
	case MemoryError::QueryFailed: return "QUERY_FAILED";
	case MemoryError::RegionNotCommitted: return "REGION_NOT_COMMITTED";
	case MemoryError::AccessDenied: return "ACCESS_DENIED";
	case MemoryError::ProtectionChangeFailed: return "PROTECTION_CHANGE_FAILED";
	case MemoryError::ProtectionRaceDetected: return "PROTECTION_RACE_DETECTED";
	case MemoryError::AccessViolation: return "ACCESS_VIOLATION";
	case MemoryError::ProtectionRestoreFailed: return "PROTECTION_RESTORE_FAILED";
	case MemoryError::ExecutableWriteDenied: return "EXECUTABLE_WRITE_DENIED";
	case MemoryError::InstructionCacheFlushRequired: return "INSTRUCTION_CACHE_FLUSH_REQUIRED";
	case MemoryError::InstructionCacheFlushFailed: return "INSTRUCTION_CACHE_FLUSH_FAILED";
	case MemoryError::ValueMismatch: return "VALUE_MISMATCH";
	case MemoryError::AllocationFailed: return "MEMORY_ALLOCATION_FAILED";
	}
	return "UNKNOWN";
}

bool CheckedAddressRange(
	const std::uintptr_t address,
	const std::size_t size,
	std::uintptr_t& endExclusive) noexcept
{
	if (address == 0 || size == 0
		|| size > (std::numeric_limits<std::uintptr_t>::max)() - address)
	{
		return false;
	}
	endExclusive = address + size;
	return true;
}

MemoryResult ValidateReadableMemory(
	const std::uintptr_t address,
	const std::size_t size) noexcept
{
	try
	{
		std::vector<RegionSegment> segments;
		MemoryResult query = QuerySegments(address, size, segments);
		if (!query.Ok())
			return query;
		for (const RegionSegment& segment : segments)
		{
			if (!IsReadableProtection(segment.Protection))
				return Failure(MemoryError::AccessDenied);
		}
		return {.BytesProcessed = size};
	}
	catch (...)
	{
		return Failure(MemoryError::AllocationFailed);
	}
}

MemoryResult ReadMemory(const std::uintptr_t address, const std::span<std::byte> output) noexcept
{
	try
	{
		MemoryResult readable = ValidateReadableMemory(address, output.size());
		if (!readable.Ok())
			return readable;
		DWORD exceptionCode = 0;
		if (!CopyWithSeh(output.data(), reinterpret_cast<const void*>(address), output.size(), exceptionCode))
			return Failure(MemoryError::AccessViolation, exceptionCode);
		return {.BytesProcessed = output.size()};
	}
	catch (...)
	{
		return Failure(MemoryError::AllocationFailed);
	}
}

MemoryResult WriteMemory(
	const std::uintptr_t address,
	const std::span<const std::byte> input,
	const MemoryWriteOptions options) noexcept
{
	try
	{
		std::vector<RegionSegment> segments;
		MemoryResult query = QuerySegments(address, input.size(), segments);
		if (!query.Ok())
			return query;
		const bool touchesExecutableMemory = std::ranges::any_of(
			segments,
			[](const RegionSegment& segment) { return IsExecutableProtection(segment.Protection); });
		if (touchesExecutableMemory && !options.AllowExecutableWrite)
			return Failure(MemoryError::ExecutableWriteDenied);
		if (touchesExecutableMemory && !options.FlushInstructionCache)
			return Failure(MemoryError::InstructionCacheFlushRequired);

		std::vector<ChangedProtection> changes;
		changes.reserve(segments.size());
		MemoryResult writable = MakeWritable(segments, options, changes);
		if (!writable.Ok())
			return writable;
		MemoryResult boundary = ValidateAppliedProtections(changes, options);
		if (!boundary.Ok())
			return FailureAfterRestore(boundary.Error, boundary.NativeError, changes);

		MemoryResult result{.BytesProcessed = input.size()};
		DWORD exceptionCode = 0;
		if (!CopyWithSeh(reinterpret_cast<void*>(address), input.data(), input.size(), exceptionCode))
			result = Failure(MemoryError::AccessViolation, exceptionCode);

		boundary = ValidateAppliedProtections(changes, options);
		if (!boundary.Ok() && result.Ok())
			result = boundary;

		const ProtectionRestoreResult restore = RestoreProtections(changes);
		if (!restore.Restored)
			return Failure(MemoryError::ProtectionRestoreFailed, restore.NativeError);
		if (restore.RaceDetected)
			return Failure(MemoryError::ProtectionRaceDetected);
		if (!result.Ok())
			return result;
		if (options.FlushInstructionCache
			&& !FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), input.size()))
		{
			return Failure(MemoryError::InstructionCacheFlushFailed, GetLastError());
		}
		return result;
	}
	catch (...)
	{
		return Failure(MemoryError::AllocationFailed);
	}
}

MemoryResult CompareExchangePointer(
	void** slot,
	void* expected,
	void* replacement,
	void** observed) noexcept
{
	try
	{
		if (!slot || reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0)
			return Failure(MemoryError::InvalidRange);

		std::vector<RegionSegment> segments;
		MemoryResult query = QuerySegments(
			reinterpret_cast<std::uintptr_t>(slot), sizeof(void*), segments);
		if (!query.Ok())
			return query;
		if (std::ranges::any_of(
			segments,
			[](const RegionSegment& segment) { return IsExecutableProtection(segment.Protection); }))
		{
			return Failure(MemoryError::ExecutableWriteDenied);
		}

		std::vector<ChangedProtection> changes;
		changes.reserve(segments.size());
		const MemoryWriteOptions options{
			.AllowProtectionChange = true,
			.AllowExecutableWrite = false,
			.FlushInstructionCache = false
		};
		MemoryResult writable = MakeWritable(segments, options, changes);
		if (!writable.Ok())
			return writable;
		MemoryResult boundary = ValidateAppliedProtections(changes, options);
		if (!boundary.Ok())
			return FailureAfterRestore(boundary.Error, boundary.NativeError, changes);

		void* actual = nullptr;
		DWORD exceptionCode = 0;
		const bool exchanged = CompareExchangePointerWithSeh(
			reinterpret_cast<void* volatile*>(slot),
			expected,
			replacement,
			actual,
			exceptionCode);
		if (observed)
			*observed = actual;

		boundary = ValidateAppliedProtections(changes, options);
		const ProtectionRestoreResult restore = RestoreProtections(changes);
		if (!restore.Restored)
			return Failure(MemoryError::ProtectionRestoreFailed, restore.NativeError);
		if (restore.RaceDetected || !boundary.Ok())
			return Failure(MemoryError::ProtectionRaceDetected, boundary.NativeError);
		if (!exchanged)
			return Failure(MemoryError::AccessViolation, exceptionCode);
		if (actual != expected)
			return Failure(MemoryError::ValueMismatch);
		return {.BytesProcessed = sizeof(void*)};
	}
	catch (...)
	{
		return Failure(MemoryError::AllocationFailed);
	}
}

} // namespace UExplorer::Runtime
