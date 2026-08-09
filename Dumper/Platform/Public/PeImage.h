#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace UExplorer::Platform
{

enum class PeImageError : std::uint8_t
{
	None,
	ModuleNotLoaded,
	InvalidBase,
	MemoryReadFailed,
	InvalidDosHeader,
	InvalidNtHeader,
	UnsupportedArchitecture,
	InvalidImageSize,
	InvalidSectionTable,
	InvalidSection,
	AllocationFailed
};

const char* ToString(PeImageError error) noexcept;

struct PeSectionView
{
	std::array<char, 9> Name{};
	std::uintptr_t HeaderAddress = 0;
	std::uintptr_t Address = 0;
	std::size_t Size = 0;
	std::uint32_t Characteristics = 0;

	std::string_view NameView() const noexcept;
	bool IsReadable() const noexcept;
	bool IsExecutable() const noexcept;
};

struct PeImageView
{
	std::uintptr_t Base = 0;
	std::size_t Size = 0;
	std::vector<PeSectionView> Sections;

	const PeSectionView* FindSection(std::string_view name) const noexcept;
};

struct PeImageResult
{
	PeImageError Error = PeImageError::None;
	std::uint32_t NativeError = 0;

	bool Ok() const noexcept { return Error == PeImageError::None; }
};

PeImageResult InspectPeImage(std::uintptr_t base, PeImageView& image) noexcept;
PeImageResult InspectLoadedPeImage(const wchar_t* moduleName, PeImageView& image) noexcept;

} // namespace UExplorer::Platform
