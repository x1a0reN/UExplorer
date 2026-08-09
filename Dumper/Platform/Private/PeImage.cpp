#include "Platform/Public/PeImage.h"

#include "Runtime/SafeMemory.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace UExplorer::Platform
{
namespace
{

constexpr std::uint16_t kMaxSectionCount = 96;
constexpr std::size_t kMaxImageSize = 8ULL * 1024ULL * 1024ULL * 1024ULL;

PeImageResult Failure(const PeImageError error, const DWORD nativeError = 0) noexcept
{
	return {.Error = error, .NativeError = nativeError};
}

bool TryAdd(
	const std::uintptr_t base,
	const std::size_t offset,
	std::uintptr_t& result) noexcept
{
	if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	result = base + offset;
	return true;
}

template<typename T>
bool TryRead(const std::uintptr_t address, T& value) noexcept
{
	return Runtime::ReadValue(address, value).Ok();
}

} // namespace

const char* ToString(const PeImageError error) noexcept
{
	switch (error)
	{
	case PeImageError::None: return "NONE";
	case PeImageError::ModuleNotLoaded: return "MODULE_NOT_LOADED";
	case PeImageError::InvalidBase: return "PE_INVALID_BASE";
	case PeImageError::MemoryReadFailed: return "PE_MEMORY_READ_FAILED";
	case PeImageError::InvalidDosHeader: return "PE_INVALID_DOS_HEADER";
	case PeImageError::InvalidNtHeader: return "PE_INVALID_NT_HEADER";
	case PeImageError::UnsupportedArchitecture: return "PE_UNSUPPORTED_ARCHITECTURE";
	case PeImageError::InvalidImageSize: return "PE_INVALID_IMAGE_SIZE";
	case PeImageError::InvalidSectionTable: return "PE_INVALID_SECTION_TABLE";
	case PeImageError::InvalidSection: return "PE_INVALID_SECTION";
	case PeImageError::AllocationFailed: return "PE_ALLOCATION_FAILED";
	}
	return "PE_UNKNOWN_ERROR";
}

std::string_view PeSectionView::NameView() const noexcept
{
	std::size_t length = 0;
	while (length < Name.size() && Name[length] != '\0')
		++length;
	return {Name.data(), length};
}

bool PeSectionView::IsReadable() const noexcept
{
	return (Characteristics & IMAGE_SCN_MEM_READ) != 0;
}

bool PeSectionView::IsWritable() const noexcept
{
	return (Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
}

bool PeSectionView::IsExecutable() const noexcept
{
	return (Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
}

const PeSectionView* PeImageView::FindSection(const std::string_view name) const noexcept
{
	const auto found = std::ranges::find_if(Sections, [name](const PeSectionView& section) {
		return section.NameView() == name;
	});
	return found == Sections.end() ? nullptr : &*found;
}

PeImageResult InspectPeImage(const std::uintptr_t base, PeImageView& image) noexcept
{
	image = {};
	if (base == 0)
		return Failure(PeImageError::InvalidBase);

	try
	{
		IMAGE_DOS_HEADER dos{};
		if (!TryRead(base, dos))
			return Failure(PeImageError::MemoryReadFailed);
		if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
			return Failure(PeImageError::InvalidDosHeader);

		std::uintptr_t ntAddress = 0;
		if (!TryAdd(base, static_cast<std::size_t>(dos.e_lfanew), ntAddress))
			return Failure(PeImageError::InvalidNtHeader);
		DWORD signature = 0;
		IMAGE_FILE_HEADER fileHeader{};
		if (!TryRead(ntAddress, signature)
			|| !TryRead(ntAddress + sizeof(signature), fileHeader))
		{
			return Failure(PeImageError::MemoryReadFailed);
		}
		if (signature != IMAGE_NT_SIGNATURE)
			return Failure(PeImageError::InvalidNtHeader);
		if (fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
			|| fileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
		{
			return Failure(PeImageError::UnsupportedArchitecture);
		}

		std::uintptr_t optionalAddress = 0;
		if (!TryAdd(ntAddress, sizeof(signature) + sizeof(fileHeader), optionalAddress))
			return Failure(PeImageError::InvalidNtHeader);
		IMAGE_OPTIONAL_HEADER64 optionalHeader{};
		if (!TryRead(optionalAddress, optionalHeader))
			return Failure(PeImageError::MemoryReadFailed);
		if (optionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
			return Failure(PeImageError::UnsupportedArchitecture);
		if (optionalHeader.SizeOfImage == 0
			|| optionalHeader.SizeOfImage > kMaxImageSize
			|| optionalHeader.SizeOfHeaders == 0
			|| optionalHeader.SizeOfHeaders > optionalHeader.SizeOfImage)
		{
			return Failure(PeImageError::InvalidImageSize);
		}
		std::uintptr_t imageEnd = 0;
		if (!TryAdd(base, optionalHeader.SizeOfImage, imageEnd) || imageEnd <= base)
			return Failure(PeImageError::InvalidImageSize);
		if (fileHeader.NumberOfSections == 0 || fileHeader.NumberOfSections > kMaxSectionCount)
			return Failure(PeImageError::InvalidSectionTable);

		std::uintptr_t sectionTableAddress = 0;
		if (!TryAdd(optionalAddress, fileHeader.SizeOfOptionalHeader, sectionTableAddress)
			|| sectionTableAddress < base)
		{
			return Failure(PeImageError::InvalidSectionTable);
		}
		const std::size_t tableOffset = static_cast<std::size_t>(sectionTableAddress - base);
		const std::size_t tableSize =
			static_cast<std::size_t>(fileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
		if (tableOffset > optionalHeader.SizeOfHeaders
			|| tableSize > optionalHeader.SizeOfHeaders - tableOffset)
		{
			return Failure(PeImageError::InvalidSectionTable);
		}

		PeImageView inspected;
		inspected.Base = base;
		inspected.Size = optionalHeader.SizeOfImage;
		inspected.Sections.reserve(fileHeader.NumberOfSections);
		for (std::uint16_t index = 0; index < fileHeader.NumberOfSections; ++index)
		{
			std::uintptr_t headerAddress = 0;
			if (!TryAdd(
				sectionTableAddress,
				static_cast<std::size_t>(index) * sizeof(IMAGE_SECTION_HEADER),
				headerAddress))
			{
				return Failure(PeImageError::InvalidSectionTable);
			}
			IMAGE_SECTION_HEADER section{};
			if (!TryRead(headerAddress, section))
				return Failure(PeImageError::MemoryReadFailed);
			if (section.VirtualAddress >= inspected.Size)
				return Failure(PeImageError::InvalidSection);
			const std::size_t sectionSize = section.Misc.VirtualSize;
			if (sectionSize > inspected.Size - section.VirtualAddress)
				return Failure(PeImageError::InvalidSection);

			PeSectionView view;
			std::memcpy(view.Name.data(), section.Name, sizeof(section.Name));
			view.HeaderAddress = headerAddress;
			if (!TryAdd(base, section.VirtualAddress, view.Address)
				|| view.Address >= imageEnd)
			{
				return Failure(PeImageError::InvalidSection);
			}
			view.Size = sectionSize;
			view.Characteristics = section.Characteristics;
			inspected.Sections.push_back(view);
		}
		image = std::move(inspected);
		return {};
	}
	catch (...)
	{
		image = {};
		return Failure(PeImageError::AllocationFailed);
	}
}

PeImageResult InspectLoadedPeImage(const wchar_t* moduleName, PeImageView& image) noexcept
{
	image = {};
	const HMODULE module = GetModuleHandleW(moduleName);
	if (!module)
		return Failure(PeImageError::ModuleNotLoaded, GetLastError());
	return InspectPeImage(reinterpret_cast<std::uintptr_t>(module), image);
}

} // namespace UExplorer::Platform
