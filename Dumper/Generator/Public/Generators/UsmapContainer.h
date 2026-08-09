#pragma once

#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>

namespace UExplorer::Usmap
{

inline constexpr std::uint16_t kMagic = 0x30C4;
inline constexpr std::uint8_t kExplicitEnumValuesVersion = 4;
inline constexpr std::uint8_t kCompressionNone = 0;
inline constexpr std::size_t kContainerHeaderSize = 16;

inline void WriteU32LittleEndian(std::uint8_t* destination, std::uint32_t value)
{
	destination[0] = static_cast<std::uint8_t>(value & 0xFFu);
	destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
	destination[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
	destination[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

inline void WriteUncompressed(std::ostream& output, std::span<const std::uint8_t> payload)
{
	if (payload.size() > (std::numeric_limits<std::uint32_t>::max)())
		throw std::length_error("USMAP payload exceeds uint32 size limit");

	const auto payloadSize = static_cast<std::uint32_t>(payload.size());
	std::uint8_t header[kContainerHeaderSize]{};
	header[0] = static_cast<std::uint8_t>(kMagic & 0xFFu);
	header[1] = static_cast<std::uint8_t>((kMagic >> 8) & 0xFFu);
	header[2] = kExplicitEnumValuesVersion;
	WriteU32LittleEndian(header + 3, 0); // No package versioning fields follow.
	header[7] = kCompressionNone;
	WriteU32LittleEndian(header + 8, payloadSize);
	WriteU32LittleEndian(header + 12, payloadSize);

	output.write(reinterpret_cast<const char*>(header), sizeof(header));
	if (!payload.empty())
		output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
	if (!output)
		throw std::runtime_error("Failed to write USMAP container");
}

} // namespace UExplorer::Usmap
