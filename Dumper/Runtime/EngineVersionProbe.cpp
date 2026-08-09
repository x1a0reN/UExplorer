#include "EngineVersionProbe.h"

#include "SafeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::array<std::string_view, 4> kVersionTags{
	"++UE4+Release-",
	"++UE5+Release-",
	"UE4+Release-",
	"UE5+Release-"
};
constexpr std::size_t kMaxVersionBytes = 16;
constexpr std::size_t kReadChunkBytes = 64 * 1024;
constexpr std::size_t kWindowOverlapBytes = 64;

EngineVersionProbeResult Failure(
	const EngineVersionProbeError error,
	const Platform::PeImageError imageError = Platform::PeImageError::None,
	const MemoryError memoryFailure = MemoryError::None,
	const std::uint32_t nativeError = 0) noexcept
{
	return {
		.Error = error,
		.ImageError = imageError,
		.MemoryFailure = memoryFailure,
		.NativeError = nativeError
	};
}

bool IsValidVersion(const std::string_view version) noexcept
{
	if (version.empty() || version.front() == '.' || version.back() == '.')
		return false;
	std::size_t componentCount = 1;
	std::size_t componentLength = 0;
	for (const char value : version)
	{
		if (value == '.')
		{
			if (componentLength == 0 || componentLength > 3)
				return false;
			++componentCount;
			componentLength = 0;
			continue;
		}
		if (value < '0' || value > '9' || ++componentLength > 3)
			return false;
	}
	return componentLength != 0 && componentLength <= 3
		&& componentCount >= 2 && componentCount <= 4
		&& (version.front() == '4' || version.front() == '5');
}

} // namespace

const char* ToString(const EngineVersionProbeError error) noexcept
{
	switch (error)
	{
	case EngineVersionProbeError::None: return "NONE";
	case EngineVersionProbeError::ImageUnavailable: return "ENGINE_VERSION_IMAGE_UNAVAILABLE";
	case EngineVersionProbeError::NoReadableSections: return "ENGINE_VERSION_NO_READABLE_SECTIONS";
	case EngineVersionProbeError::MemoryReadFailed: return "ENGINE_VERSION_MEMORY_READ_FAILED";
	case EngineVersionProbeError::MarkerNotFound: return "ENGINE_VERSION_MARKER_NOT_FOUND";
	case EngineVersionProbeError::AllocationFailed: return "ENGINE_VERSION_ALLOCATION_FAILED";
	}
	return "ENGINE_VERSION_UNKNOWN_ERROR";
}

EngineVersionProbeResult ParseEngineVersionMarkers(
	const std::span<const std::byte> bytes) noexcept
{
	try
	{
		const std::string_view text(
			reinterpret_cast<const char*>(bytes.data()),
			bytes.size());
		for (const std::string_view tag : kVersionTags)
		{
			std::size_t searchOffset = 0;
			while (searchOffset < text.size())
			{
				const std::size_t found = text.find(tag, searchOffset);
				if (found == std::string_view::npos)
					break;
				const std::size_t versionStart = found + tag.size();
				std::size_t versionEnd = versionStart;
				while (versionEnd < text.size()
					&& versionEnd - versionStart < kMaxVersionBytes)
				{
					const char value = text[versionEnd];
					if ((value < '0' || value > '9') && value != '.')
						break;
					++versionEnd;
				}
				const std::string_view candidate = text.substr(
					versionStart,
					versionEnd - versionStart);
				if (IsValidVersion(candidate))
					return {.Version = std::string(candidate)};
				searchOffset = versionStart;
			}
		}
		return Failure(EngineVersionProbeError::MarkerNotFound);
	}
	catch (...)
	{
		return Failure(EngineVersionProbeError::AllocationFailed);
	}
}

EngineVersionProbeResult ProbeEngineVersion(
	const Platform::PeImageView& image) noexcept
{
	if (image.Base == 0 || image.Size == 0 || image.Sections.empty())
		return Failure(EngineVersionProbeError::ImageUnavailable);
	try
	{
		bool readableSectionObserved = false;
		bool readFailureObserved = false;
		MemoryError firstMemoryFailure = MemoryError::None;
		std::uint32_t firstNativeError = 0;
		std::vector<std::byte> window;
		window.resize(kReadChunkBytes + kWindowOverlapBytes);
		for (const Platform::PeSectionView& section : image.Sections)
		{
			if (!section.IsReadable() || section.Address == 0 || section.Size == 0)
				continue;
			std::uintptr_t imageEnd = 0;
			std::uintptr_t sectionEnd = 0;
			if (!CheckedAddressRange(image.Base, image.Size, imageEnd)
				|| !CheckedAddressRange(section.Address, section.Size, sectionEnd)
				|| section.Address < image.Base
				|| sectionEnd > imageEnd)
			{
				return Failure(
					EngineVersionProbeError::ImageUnavailable,
					Platform::PeImageError::InvalidSection);
			}
			readableSectionObserved = true;
			for (std::size_t offset = 0; offset < section.Size; offset += kReadChunkBytes)
			{
				const std::size_t readSize = (std::min)(
					section.Size - offset,
					kReadChunkBytes + kWindowOverlapBytes);
				if (offset > (std::numeric_limits<std::uintptr_t>::max)() - section.Address)
					return Failure(EngineVersionProbeError::ImageUnavailable);
				const MemoryResult read = ReadMemory(
					section.Address + offset,
					std::span<std::byte>(window.data(), readSize));
				if (!read.Ok())
				{
					readFailureObserved = true;
					if (firstMemoryFailure == MemoryError::None)
					{
						firstMemoryFailure = read.Error;
						firstNativeError = read.NativeError;
					}
					break;
				}
				EngineVersionProbeResult parsed = ParseEngineVersionMarkers(
					std::span<const std::byte>(window.data(), readSize));
				if (parsed.Ok())
					return parsed;
			}
		}
		if (!readableSectionObserved)
			return Failure(EngineVersionProbeError::NoReadableSections);
		return readFailureObserved
			? Failure(
				EngineVersionProbeError::MemoryReadFailed,
				Platform::PeImageError::None,
				firstMemoryFailure,
				firstNativeError)
			: Failure(EngineVersionProbeError::MarkerNotFound);
	}
	catch (...)
	{
		return Failure(EngineVersionProbeError::AllocationFailed);
	}
}

EngineVersionProbeResult ProbeLoadedEngineVersion() noexcept
{
	Platform::PeImageView image;
	const Platform::PeImageResult inspected = Platform::InspectLoadedPeImage(nullptr, image);
	if (!inspected.Ok())
	{
		return Failure(
			EngineVersionProbeError::ImageUnavailable,
			inspected.Error,
			MemoryError::None,
			inspected.NativeError);
	}
	return ProbeEngineVersion(image);
}

} // namespace UExplorer::Runtime
