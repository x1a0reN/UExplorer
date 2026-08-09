#pragma once

#include "Platform/Public/PeImage.h"
#include "SafeMemory.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace UExplorer::Runtime
{

enum class EngineVersionProbeError : std::uint8_t
{
	None,
	ImageUnavailable,
	NoReadableSections,
	MemoryReadFailed,
	MarkerNotFound,
	AllocationFailed
};

const char* ToString(EngineVersionProbeError error) noexcept;

struct EngineVersionProbeResult
{
	EngineVersionProbeError Error = EngineVersionProbeError::None;
	Platform::PeImageError ImageError = Platform::PeImageError::None;
	MemoryError MemoryFailure = MemoryError::None;
	std::uint32_t NativeError = 0;
	std::string Version;

	bool Ok() const noexcept { return Error == EngineVersionProbeError::None; }
};

EngineVersionProbeResult ParseEngineVersionMarkers(
	std::span<const std::byte> bytes) noexcept;
EngineVersionProbeResult ProbeEngineVersion(
	const Platform::PeImageView& image) noexcept;
EngineVersionProbeResult ProbeLoadedEngineVersion() noexcept;

} // namespace UExplorer::Runtime
