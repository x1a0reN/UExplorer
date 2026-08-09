#include "EngineNameCodec.h"

#include "SafeMemory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaxNameUnits = 1024;
constexpr std::size_t kMaxRedirectDepth = 8;
constexpr std::uint32_t kMaxComparisonIndex = 0x3FFFFFFFu;
constexpr std::uint32_t kLegacyNamesPerChunk = 0x4000u;

EngineNameResult Failure(const EngineNameError error)
{
	return {.Error = error};
}

bool CheckedAdd(
	const std::uintptr_t base,
	const std::size_t offset,
	std::uintptr_t& result) noexcept
{
	if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	result = base + offset;
	return true;
}

bool CheckedMultiply(
	const std::size_t left,
	const std::size_t right,
	std::size_t& result) noexcept
{
	if (left != 0 && right > (std::numeric_limits<std::size_t>::max)() / left)
		return false;
	result = left * right;
	return true;
}

bool CheckedFieldAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	std::uintptr_t& result) noexcept
{
	return offset >= 0 && CheckedAdd(base, static_cast<std::size_t>(offset), result);
}

template<typename T>
bool ReadField(
	const std::uintptr_t base,
	const std::int32_t offset,
	T& value) noexcept
{
	std::uintptr_t address = 0;
	return CheckedFieldAddress(base, offset, address) && ReadValue(address, value).Ok();
}

bool FitsField(
	const std::int32_t structureSize,
	const std::int32_t offset,
	const std::size_t fieldSize) noexcept
{
	return structureSize > 0
		&& offset >= 0
		&& fieldSize <= static_cast<std::size_t>(structureSize)
		&& static_cast<std::size_t>(offset) <= static_cast<std::size_t>(structureSize) - fieldSize;
}

bool FitsCurrentPoolBlock(
	const std::uint32_t blockIndex,
	const std::int32_t currentBlock,
	const std::size_t inBlockOffset,
	const std::size_t size,
	const std::int32_t byteCursor) noexcept
{
	if (blockIndex != static_cast<std::uint32_t>(currentBlock))
		return true;
	return byteCursor >= 0
		&& inBlockOffset <= static_cast<std::size_t>(byteCursor)
		&& size <= static_cast<std::size_t>(byteCursor) - inBlockOffset;
}

EngineNameResult DecodeUtf8(const std::uintptr_t address, const std::size_t length)
{
	if (length == 0)
		return {.Value = {}};
	std::vector<char> bytes(length);
	if (!ReadMemory(address, std::as_writable_bytes(std::span<char>(bytes))).Ok())
		return Failure(EngineNameError::EntryUnavailable);
	if (std::find(bytes.begin(), bytes.end(), '\0') != bytes.end())
		return Failure(EngineNameError::HeaderInvalid);
	if (MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		bytes.data(),
		static_cast<int>(bytes.size()),
		nullptr,
		0) <= 0)
	{
		return Failure(EngineNameError::EncodingInvalid);
	}
	return {.Value = std::string(bytes.begin(), bytes.end())};
}

EngineNameResult DecodeUtf16(const std::uintptr_t address, const std::size_t length)
{
	if (length == 0)
		return {.Value = {}};
	std::vector<wchar_t> units(length);
	if (!ReadMemory(address, std::as_writable_bytes(std::span<wchar_t>(units))).Ok())
		return Failure(EngineNameError::EntryUnavailable);
	if (std::find(units.begin(), units.end(), L'\0') != units.end())
		return Failure(EngineNameError::HeaderInvalid);
	const int required = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		units.data(),
		static_cast<int>(units.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (required <= 0)
		return Failure(EngineNameError::EncodingInvalid);
	std::string output(static_cast<std::size_t>(required), '\0');
	if (WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		units.data(),
		static_cast<int>(units.size()),
		output.data(),
		required,
		nullptr,
		nullptr) != required)
	{
		return Failure(EngineNameError::EncodingInvalid);
	}
	return {.Value = std::move(output)};
}

EngineNameResult ReadNullTerminatedUtf8(const std::uintptr_t address)
{
	std::vector<char> bytes;
	bytes.reserve(kMaxNameUnits);
	for (std::size_t index = 0; index <= kMaxNameUnits; ++index)
	{
		std::uintptr_t unitAddress = 0;
		char value = '\0';
		if (!CheckedAdd(address, index, unitAddress) || !ReadValue(unitAddress, value).Ok())
			return Failure(EngineNameError::EntryUnavailable);
		if (value == '\0')
			return DecodeUtf8(address, bytes.size());
		if (index == kMaxNameUnits)
			return Failure(EngineNameError::NameTooLong);
		bytes.push_back(value);
	}
	return Failure(EngineNameError::NameTooLong);
}

EngineNameResult ReadNullTerminatedUtf16(const std::uintptr_t address)
{
	std::vector<wchar_t> units;
	units.reserve(kMaxNameUnits);
	for (std::size_t index = 0; index <= kMaxNameUnits; ++index)
	{
		std::size_t byteOffset = 0;
		std::uintptr_t unitAddress = 0;
		wchar_t value = L'\0';
		if (!CheckedMultiply(index, sizeof(wchar_t), byteOffset)
			|| !CheckedAdd(address, byteOffset, unitAddress)
			|| !ReadValue(unitAddress, value).Ok())
		{
			return Failure(EngineNameError::EntryUnavailable);
		}
		if (value == L'\0')
			return DecodeUtf16(address, units.size());
		if (index == kMaxNameUnits)
			return Failure(EngineNameError::NameTooLong);
		units.push_back(value);
	}
	return Failure(EngineNameError::NameTooLong);
}

void AppendNumber(std::string& value, const std::uint32_t number)
{
	if (number > 0)
		value += "_" + std::to_string(number - 1);
}

} // namespace

const char* ToString(const EngineNameError error) noexcept
{
	switch (error)
	{
	case EngineNameError::None: return "NONE";
	case EngineNameError::InvalidProfile: return "NAME_PROFILE_INVALID";
	case EngineNameError::InvalidAddress: return "FNAME_ADDRESS_INVALID";
	case EngineNameError::IndexOutOfRange: return "NAME_INDEX_OUT_OF_RANGE";
	case EngineNameError::LayoutInvalid: return "NAME_LAYOUT_INVALID";
	case EngineNameError::EntryUnavailable: return "NAME_ENTRY_UNAVAILABLE";
	case EngineNameError::HeaderInvalid: return "NAME_HEADER_INVALID";
	case EngineNameError::NameTooLong: return "NAME_TOO_LONG";
	case EngineNameError::EncodingInvalid: return "NAME_ENCODING_INVALID";
	case EngineNameError::RedirectCycle: return "NAME_REDIRECT_CYCLE";
	case EngineNameError::NumberInvalid: return "NAME_NUMBER_INVALID";
	}
	return "NAME_ERROR_UNKNOWN";
}

bool IsEngineNameProfileLayoutValid(const EngineNameProfile& profile) noexcept
{
	if (profile.StorageAddress == 0
		|| profile.FNameSize < static_cast<std::int32_t>(sizeof(std::uint32_t))
		|| profile.FNameSize > 64
		|| !FitsField(profile.FNameSize, profile.ComparisonIndexOffset, sizeof(std::uint32_t))
		|| (!profile.UsesOutlineNumber
			&& !FitsField(profile.FNameSize, profile.NumberOffset, sizeof(std::uint32_t))))
	{
		return false;
	}

	if (profile.Storage == EngineNameStorageKind::NamePool)
	{
		return profile.BlockOffsetBits > 0 && profile.BlockOffsetBits < 31
			&& profile.EntryStride > 0 && profile.EntryStride <= 16
			&& profile.ChunksStart >= 0 && profile.ChunksStart <= 0x10000
			&& (profile.ChunksStart % static_cast<std::int32_t>(alignof(std::uintptr_t))) == 0
			&& profile.MaxChunkIndexOffset >= 0 && profile.MaxChunkIndexOffset <= 0x10000
			&& profile.ByteCursorOffset >= 0 && profile.ByteCursorOffset <= 0x10000
			&& profile.EntryStringOffset > 0 && profile.EntryStringOffset <= 64
			&& profile.EntryHeaderOffset >= 0 && profile.EntryHeaderOffset <= 64
			&& profile.EntryLengthShift > 0 && profile.EntryLengthShift < 16;
	}
	if (profile.Storage == EngineNameStorageKind::ChunkedArray)
	{
		return profile.ChunksStart >= 0 && profile.ChunksStart <= 0x10000
			&& (profile.ChunksStart % static_cast<std::int32_t>(alignof(std::uintptr_t))) == 0
			&& profile.MaxChunkIndexOffset >= 0 && profile.MaxChunkIndexOffset <= 0x10000
			&& profile.NumElementsOffset >= 0 && profile.NumElementsOffset <= 0x10000
			&& profile.EntryStringOffset >= 0 && profile.EntryStringOffset <= 0x100
			&& profile.EntryIndexOffset >= 0 && profile.EntryIndexOffset <= 0x100;
	}
	return false;
}

EngineNameCodec::EngineNameCodec(EngineNameProfile profile)
	: m_Profile(std::move(profile)),
	  m_Configured(m_Profile.Validated && IsEngineNameProfileLayoutValid(m_Profile))
{
}

EngineNameResult EngineNameCodec::DecodeFName(const std::uintptr_t fnameAddress) const noexcept
{
	try
	{
		if (!m_Configured)
			return Failure(EngineNameError::InvalidProfile);
		std::uintptr_t endExclusive = 0;
		if (!CheckedAddressRange(fnameAddress, static_cast<std::size_t>(m_Profile.FNameSize), endExclusive))
			return Failure(EngineNameError::InvalidAddress);

		std::uint32_t comparisonIndex = 0;
		if (!ReadField(fnameAddress, m_Profile.ComparisonIndexOffset, comparisonIndex))
			return Failure(EngineNameError::EntryUnavailable);
		std::uint32_t number = 0;
		if (!m_Profile.UsesOutlineNumber
			&& !ReadField(fnameAddress, m_Profile.NumberOffset, number))
		{
			return Failure(EngineNameError::EntryUnavailable);
		}
		return Decode(comparisonIndex, number);
	}
	catch (...)
	{
		return Failure(EngineNameError::EntryUnavailable);
	}
}

EngineNameResult EngineNameCodec::Decode(
	const std::uint32_t comparisonIndex,
	const std::uint32_t number) const noexcept
{
	try
	{
		if (!m_Configured)
			return Failure(EngineNameError::InvalidProfile);
		if (comparisonIndex > kMaxComparisonIndex)
			return Failure(EngineNameError::IndexOutOfRange);
		if (m_Profile.UsesOutlineNumber && number != 0)
			return Failure(EngineNameError::NumberInvalid);

		EngineNameResult result = m_Profile.Storage == EngineNameStorageKind::NamePool
			? DecodeNamePool(comparisonIndex)
			: DecodeChunkedArray(comparisonIndex);
		if (result.Ok() && !m_Profile.UsesOutlineNumber)
			AppendNumber(result.Value, number);
		return result;
	}
	catch (...)
	{
		return Failure(EngineNameError::EntryUnavailable);
	}
}

EngineNameResult EngineNameCodec::DecodeNamePool(std::uint32_t comparisonIndex) const
{
	std::array<std::uint32_t, kMaxRedirectDepth> visited{};
	std::size_t visitedCount = 0;
	std::uint32_t outlineNumber = 0;
	bool hasOutlineNumber = false;

	for (std::size_t depth = 0; depth < kMaxRedirectDepth; ++depth)
	{
		for (std::size_t index = 0; index < visitedCount; ++index)
		{
			if (visited[index] == comparisonIndex)
				return Failure(EngineNameError::RedirectCycle);
		}
		visited[visitedCount++] = comparisonIndex;

		std::int32_t currentBlock = -1;
		std::int32_t byteCursor = -1;
		if (!ReadField(m_Profile.StorageAddress, m_Profile.MaxChunkIndexOffset, currentBlock)
			|| !ReadField(m_Profile.StorageAddress, m_Profile.ByteCursorOffset, byteCursor))
		{
			return Failure(EngineNameError::EntryUnavailable);
		}
		if (currentBlock < 0 || currentBlock > 0x10000 || byteCursor < 0)
			return Failure(EngineNameError::LayoutInvalid);

		const std::uint32_t blockIndex = comparisonIndex >> m_Profile.BlockOffsetBits;
		const std::uint32_t blockMask = (std::uint32_t{1} << m_Profile.BlockOffsetBits) - 1;
		if (blockIndex > static_cast<std::uint32_t>(currentBlock))
			return Failure(EngineNameError::IndexOutOfRange);

		std::size_t slotOffset = 0;
		std::size_t blockPointerOffset = 0;
		std::uintptr_t blockPointerAddress = 0;
		if (!CheckedMultiply(static_cast<std::size_t>(blockIndex), sizeof(std::uintptr_t), slotOffset)
			|| static_cast<std::size_t>(m_Profile.ChunksStart)
				> (std::numeric_limits<std::size_t>::max)() - slotOffset)
		{
			return Failure(EngineNameError::LayoutInvalid);
		}
		blockPointerOffset = static_cast<std::size_t>(m_Profile.ChunksStart) + slotOffset;
		if (!CheckedAdd(m_Profile.StorageAddress, blockPointerOffset, blockPointerAddress))
			return Failure(EngineNameError::LayoutInvalid);

		std::uintptr_t blockAddress = 0;
		if (!ReadValue(blockPointerAddress, blockAddress).Ok() || blockAddress == 0)
			return Failure(EngineNameError::EntryUnavailable);

		std::size_t inBlockOffset = 0;
		if (!CheckedMultiply(
			static_cast<std::size_t>(comparisonIndex & blockMask),
			static_cast<std::size_t>(m_Profile.EntryStride),
			inBlockOffset))
		{
			return Failure(EngineNameError::LayoutInvalid);
		}
		std::uintptr_t entryAddress = 0;
		if (!CheckedAdd(blockAddress, inBlockOffset, entryAddress))
			return Failure(EngineNameError::LayoutInvalid);

		std::uintptr_t headerAddress = 0;
		if (!CheckedFieldAddress(entryAddress, m_Profile.EntryHeaderOffset, headerAddress)
			|| !FitsCurrentPoolBlock(
				blockIndex,
				currentBlock,
				inBlockOffset + static_cast<std::size_t>(m_Profile.EntryHeaderOffset),
				sizeof(std::uint16_t),
				byteCursor))
		{
			return Failure(EngineNameError::IndexOutOfRange);
		}
		std::uint16_t header = 0;
		if (!ReadValue(headerAddress, header).Ok())
			return Failure(EngineNameError::EntryUnavailable);

		const std::size_t nameLength = header >> m_Profile.EntryLengthShift;
		if (nameLength > kMaxNameUnits)
			return Failure(EngineNameError::NameTooLong);
		if (nameLength == 0)
		{
			if (!m_Profile.UsesOutlineNumber)
				return Failure(EngineNameError::HeaderInvalid);
			const std::size_t redirectOffset = static_cast<std::size_t>(m_Profile.EntryStringOffset)
				+ (m_Profile.EntryStringOffset == 6 ? 2u : 0u);
			if (!FitsCurrentPoolBlock(
				blockIndex,
				currentBlock,
				inBlockOffset + redirectOffset,
				sizeof(std::int32_t) * 2,
				byteCursor))
			{
				return Failure(EngineNameError::IndexOutOfRange);
			}
			std::uintptr_t redirectAddress = 0;
			std::int32_t nextEntry = -1;
			std::int32_t nextNumber = -1;
			if (!CheckedAdd(entryAddress, redirectOffset, redirectAddress)
				|| !ReadValue(redirectAddress, nextEntry).Ok()
				|| !CheckedAdd(redirectAddress, sizeof(std::int32_t), redirectAddress)
				|| !ReadValue(redirectAddress, nextNumber).Ok())
			{
				return Failure(EngineNameError::EntryUnavailable);
			}
			if (nextEntry < 0 || static_cast<std::uint32_t>(nextEntry) > kMaxComparisonIndex
				|| nextNumber < 0 || (nextNumber > 0 && hasOutlineNumber))
			{
				return Failure(EngineNameError::HeaderInvalid);
			}
			comparisonIndex = static_cast<std::uint32_t>(nextEntry);
			if (nextNumber > 0)
			{
				outlineNumber = static_cast<std::uint32_t>(nextNumber);
				hasOutlineNumber = true;
			}
			continue;
		}

		const bool wide = (header & 1u) != 0;
		std::size_t byteLength = 0;
		if (!CheckedMultiply(nameLength, wide ? sizeof(wchar_t) : sizeof(char), byteLength))
			return Failure(EngineNameError::LayoutInvalid);
		if (!FitsCurrentPoolBlock(
			blockIndex,
			currentBlock,
			inBlockOffset + static_cast<std::size_t>(m_Profile.EntryStringOffset),
			byteLength,
			byteCursor))
		{
			return Failure(EngineNameError::IndexOutOfRange);
		}
		std::uintptr_t stringAddress = 0;
		if (!CheckedFieldAddress(entryAddress, m_Profile.EntryStringOffset, stringAddress))
			return Failure(EngineNameError::LayoutInvalid);
		EngineNameResult result = wide
			? DecodeUtf16(stringAddress, nameLength)
			: DecodeUtf8(stringAddress, nameLength);
		if (result.Ok() && hasOutlineNumber)
			AppendNumber(result.Value, outlineNumber);
		return result;
	}
	return Failure(EngineNameError::RedirectCycle);
}

EngineNameResult EngineNameCodec::DecodeChunkedArray(const std::uint32_t comparisonIndex) const
{
	std::int32_t numElements = -1;
	std::int32_t numChunks = -1;
	if (!ReadField(m_Profile.StorageAddress, m_Profile.NumElementsOffset, numElements)
		|| !ReadField(m_Profile.StorageAddress, m_Profile.MaxChunkIndexOffset, numChunks))
	{
		return Failure(EngineNameError::EntryUnavailable);
	}
	if (numElements < 0 || numChunks < 0 || numChunks > 0x10000)
		return Failure(EngineNameError::LayoutInvalid);
	if (comparisonIndex >= static_cast<std::uint32_t>(numElements))
		return Failure(EngineNameError::IndexOutOfRange);

	const std::uint32_t chunkIndex = comparisonIndex / kLegacyNamesPerChunk;
	const std::uint32_t inChunkIndex = comparisonIndex % kLegacyNamesPerChunk;
	if (chunkIndex >= static_cast<std::uint32_t>(numChunks))
		return Failure(EngineNameError::LayoutInvalid);

	std::size_t pointerOffset = 0;
	std::uintptr_t chunkPointerAddress = 0;
	if (!CheckedMultiply(static_cast<std::size_t>(chunkIndex), sizeof(std::uintptr_t), pointerOffset)
		|| static_cast<std::size_t>(m_Profile.ChunksStart)
			> (std::numeric_limits<std::size_t>::max)() - pointerOffset
		|| !CheckedAdd(
			m_Profile.StorageAddress,
			static_cast<std::size_t>(m_Profile.ChunksStart) + pointerOffset,
			chunkPointerAddress))
	{
		return Failure(EngineNameError::LayoutInvalid);
	}
	std::uintptr_t chunkAddress = 0;
	if (!ReadValue(chunkPointerAddress, chunkAddress).Ok() || chunkAddress == 0)
		return Failure(EngineNameError::EntryUnavailable);

	if (!CheckedMultiply(static_cast<std::size_t>(inChunkIndex), sizeof(std::uintptr_t), pointerOffset)
		|| !CheckedAdd(chunkAddress, pointerOffset, chunkPointerAddress))
	{
		return Failure(EngineNameError::LayoutInvalid);
	}
	std::uintptr_t entryAddress = 0;
	if (!ReadValue(chunkPointerAddress, entryAddress).Ok() || entryAddress == 0)
		return Failure(EngineNameError::EntryUnavailable);

	std::uint32_t encodedIndex = 0;
	if (!ReadField(entryAddress, m_Profile.EntryIndexOffset, encodedIndex))
		return Failure(EngineNameError::EntryUnavailable);
	if ((encodedIndex >> 1) != comparisonIndex)
		return Failure(EngineNameError::HeaderInvalid);
	std::uintptr_t stringAddress = 0;
	if (!CheckedFieldAddress(entryAddress, m_Profile.EntryStringOffset, stringAddress))
		return Failure(EngineNameError::LayoutInvalid);
	return (encodedIndex & 1u) != 0
		? ReadNullTerminatedUtf16(stringAddress)
		: ReadNullTerminatedUtf8(stringAddress);
}

} // namespace UExplorer::Runtime
