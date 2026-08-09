
#include "TmpUtils.h"
#include "PlatformWindows.h"
#include "Arch_x86.h"
#include "Platform/Public/BytePattern.h"
#include "Platform/Public/PeImage.h"
#include "Runtime/SafeMemory.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Private implementation to ensure that there is no accidental usage of platform-specific functions
namespace
{
	// Spills over into the rest of the file (ouside of this anonymous namespace). This may be ignored, as all other functions in this file are part of namepsace PlatformWindows;
	using namespace PlatformWindows;

	struct WindowsSectionInfo
	{
		const uintptr_t Imagebase = NULL;
		const IMAGE_SECTION_HEADER* SectionHeader = nullptr;

	public:
		inline bool IsValid() const
		{
			return Imagebase != NULL && SectionHeader != nullptr;
		}
	};
	static_assert(sizeof(WindowsSectionInfo) == sizeof(SectionInfo), "To allow interchangable clasting between SectionInfo types you must ensure that the size matches.");

	inline WindowsSectionInfo SectionInfoToWinSectionInfo(const SectionInfo& Info)
	{
		return std::bit_cast<WindowsSectionInfo>(Info);
	}
	inline SectionInfo WinSectionInfoToSectionInfo(const WindowsSectionInfo& Info)
	{
		return std::bit_cast<SectionInfo>(Info);
	}


	struct CLIENT_ID
	{
		HANDLE UniqueProcess;
		HANDLE UniqueThread;
	};

	struct TEB
	{
		NT_TIB NtTib;
		PVOID EnvironmentPointer;
		CLIENT_ID ClientId;
		PVOID ActiveRpcHandle;
		PVOID ThreadLocalStoragePointer;
		struct PEB* ProcessEnvironmentBlock;
	};

	struct PEB_LDR_DATA
	{
		ULONG Length;
		BOOLEAN Initialized;
		HANDLE SsHandle;
		LIST_ENTRY InLoadOrderModuleList;
		LIST_ENTRY InMemoryOrderModuleList;
		LIST_ENTRY InInitializationOrderModuleList;
		PVOID EntryInProgress;
		BOOLEAN ShutdownInProgress;
		HANDLE ShutdownThreadId;
	};

	struct PEB
	{
		BOOLEAN InheritedAddressSpace;
		BOOLEAN ReadImageFileExecOptions;
		BOOLEAN BeingDebugged;
		union
		{
			BOOLEAN BitField;
			struct
			{
				BOOLEAN ImageUsesLargePages : 1;
				BOOLEAN IsProtectedProcess : 1;
				BOOLEAN IsImageDynamicallyRelocated : 1;
				BOOLEAN SkipPatchingUser32Forwarders : 1;
				BOOLEAN IsPackagedProcess : 1;
				BOOLEAN IsAppContainer : 1;
				BOOLEAN IsProtectedProcessLight : 1;
				BOOLEAN SpareBits : 1;
			};
		};
		HANDLE Mutant;
		PVOID ImageBaseAddress;
		PEB_LDR_DATA* Ldr;
	};

	struct UNICODE_STRING
	{
		USHORT Length;
		USHORT MaximumLength;
		PWCH Buffer;
	};

	struct LDR_DATA_TABLE_ENTRY
	{
		LIST_ENTRY InLoadOrderLinks;
		LIST_ENTRY InMemoryOrderLinks;
		LIST_ENTRY InInitializationOrderLinks;
		PVOID DllBase;
		PVOID EntryPoint;
		ULONG SizeOfImage;
		UNICODE_STRING FullDllName;
		UNICODE_STRING BaseDllName;
	};
	static_assert(offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks) == 0x10);
	static_assert(offsetof(LDR_DATA_TABLE_ENTRY, DllBase) == 0x30);
	static_assert(offsetof(LDR_DATA_TABLE_ENTRY, SizeOfImage) == 0x40);

	inline _TEB* _NtCurrentTeb()
	{
		return reinterpret_cast<struct _TEB*>(__readgsqword(((LONG)__builtin_offsetof(NT_TIB, Self))));
	}

	inline PEB* GetPEB()
	{
		return reinterpret_cast<TEB*>(_NtCurrentTeb())->ProcessEnvironmentBlock;
	}

	bool TryAddAddress(
		const uintptr_t base,
		const std::size_t offset,
		uintptr_t& result) noexcept
	{
		if (base == 0 || offset > (std::numeric_limits<uintptr_t>::max)() - base)
			return false;
		result = base + offset;
		return true;
	}

	bool TryApplySignedDisplacement(
		const uintptr_t instructionEnd,
		const int32_t displacement,
		uintptr_t& target) noexcept
	{
		if (displacement >= 0)
			return TryAddAddress(instructionEnd, static_cast<std::size_t>(displacement), target);
		const std::uint64_t magnitude = static_cast<std::uint64_t>(
			-static_cast<std::int64_t>(displacement));
		if (magnitude > instructionEnd)
			return false;
		target = instructionEnd - static_cast<uintptr_t>(magnitude);
		return target != 0;
	}

	bool CompareMemoryValueWithSeh(
		bool(*comparison)(const void*, const void*),
		const void* expected,
		const void* candidate,
		bool& equal) noexcept
	{
#if defined(_MSC_VER)
		__try
		{
			equal = comparison(expected, candidate);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			equal = false;
			return false;
		}
#else
		equal = comparison(expected, candidate);
		return true;
#endif
	}

	template<typename Callback>
	bool VisitLoadedModules(Callback&& callback)
	{
		const PEB* Peb = GetPEB();
		if (!Peb)
			return false;
		PEB_LDR_DATA* Ldr = nullptr;
		if (!UExplorer::Runtime::ReadValue(
			reinterpret_cast<uintptr_t>(Peb) + offsetof(PEB, Ldr),
			Ldr).Ok()
			|| !Ldr)
		{
			return false;
		}

		const uintptr_t headAddress = reinterpret_cast<uintptr_t>(Ldr)
			+ offsetof(PEB_LDR_DATA, InMemoryOrderModuleList);
		LIST_ENTRY head{};
		if (!UExplorer::Runtime::ReadValue(headAddress, head).Ok())
			return false;
		if (!head.Flink || !head.Blink)
			return false;
		const LIST_ENTRY* current = head.Flink;
		uintptr_t previousLinkAddress = headAddress;
		for (std::size_t visited = 0;
			current && reinterpret_cast<uintptr_t>(current) != headAddress && visited < 4096;
			++visited)
		{
			const uintptr_t linkAddress = reinterpret_cast<uintptr_t>(current);
			if (linkAddress < offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks))
				return false;
			const uintptr_t entryAddress =
				linkAddress - offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
			LDR_DATA_TABLE_ENTRY entry{};
			if (!UExplorer::Runtime::ReadValue(entryAddress, entry).Ok())
				return false;
			if (reinterpret_cast<uintptr_t>(entry.InMemoryOrderLinks.Blink) != previousLinkAddress)
				return false;
			if (entry.DllBase && entry.SizeOfImage != 0)
			{
				uintptr_t moduleEnd = 0;
				if (!UExplorer::Runtime::CheckedAddressRange(
					reinterpret_cast<uintptr_t>(entry.DllBase),
					entry.SizeOfImage,
					moduleEnd))
				{
					return false;
				}
			}
			if (callback(entry))
				return true;
			previousLinkAddress = linkAddress;
			current = entry.InMemoryOrderLinks.Flink;
		}
		return false;
	}

	inline std::pair<uintptr_t, uintptr_t> GetImageBaseAndSize(const char* const ModuleName = Settings::General::DefaultModuleName)
	{
		const uintptr_t ImageBase = GetModuleBase(ModuleName);
		UExplorer::Platform::PeImageView image;
		if (!UExplorer::Platform::InspectPeImage(ImageBase, image).Ok())
			return {0, 0};
		return {image.Base, image.Size};
	}

	inline const IMAGE_SECTION_HEADER* IterateAllSectionObjects(const uintptr_t ImageBase, const std::function<bool(const IMAGE_SECTION_HEADER*)>& Callback)
	{
		if (ImageBase == 0 || !Callback)
			return nullptr;
		UExplorer::Platform::PeImageView image;
		if (!UExplorer::Platform::InspectPeImage(ImageBase, image).Ok())
			return nullptr;
		for (const UExplorer::Platform::PeSectionView& section : image.Sections)
		{
			if (!section.IsReadable() || section.HeaderAddress == 0)
				continue;
			const auto* header = reinterpret_cast<const IMAGE_SECTION_HEADER*>(section.HeaderAddress);
			if (Callback(header))
				return header;
		}
		return nullptr;
	}

	/* Returns the base address of the section and it's size */
	inline std::pair<uintptr_t, DWORD> GetSectionByName(uintptr_t ImageBase, const std::string& ReqestedSectionName)
	{
		UExplorer::Platform::PeImageView image;
		if (!UExplorer::Platform::InspectPeImage(ImageBase, image).Ok())
			return {NULL, 0};
		const UExplorer::Platform::PeSectionView* section = image.FindSection(ReqestedSectionName);
		if (section && section->Size <= (std::numeric_limits<DWORD>::max)())
			return {section->Address, static_cast<DWORD>(section->Size)};
		return { NULL, 0 };
	}

	inline int64_t GetAlignedSizeWithOffsetFromEnd(const uint32_t SizeToAlign, const uint32_t Alignment, const uint32_t OffsetFromEnd)
	{
		const uint32_t ValueToAlign = (SizeToAlign - (Alignment - 1) - OffsetFromEnd);

		// There was an underflow in the above subtraction
		if (ValueToAlign > SizeToAlign)
			return -1;

		return Align(ValueToAlign, Alignment);
	}

	bool TryImageAddress(
		const UExplorer::Platform::PeImageView& image,
		const std::uint32_t rva,
		const std::size_t size,
		uintptr_t& address) noexcept
	{
		if (image.Base == 0 || image.Size == 0 || rva >= image.Size
			|| size > image.Size - rva)
		{
			return false;
		}
		return TryAddAddress(image.Base, rva, address);
	}

	bool TryReadImageAsciiString(
		const UExplorer::Platform::PeImageView& image,
		const std::uint32_t rva,
		std::string& value)
	{
		value.clear();
		if (rva >= image.Size)
			return false;
		constexpr std::size_t kMaxImageStringBytes = 512;
		const std::size_t available = (std::min)(
			image.Size - rva,
			kMaxImageStringBytes);
		uintptr_t address = 0;
		if (!TryImageAddress(image, rva, 1, address))
			return false;
		value.reserve((std::min)(available, std::size_t{64}));
		for (std::size_t index = 0; index < available; ++index)
		{
			char current = '\0';
			if (!UExplorer::Runtime::ReadValue(address + index, current).Ok())
				return false;
			if (current == '\0')
				return true;
			value.push_back(current);
		}
		value.clear();
		return false;
	}

	inline const PIMAGE_THUNK_DATA GetImportAddress(
		const uintptr_t ModuleBase,
		const char* ModuleToImportFrom,
		const char* SearchFunctionName)
	{
		if (ModuleBase == 0 || !ModuleToImportFrom || !*ModuleToImportFrom
			|| !SearchFunctionName || !*SearchFunctionName)
		{
			return nullptr;
		}

		UExplorer::Platform::PeImageView image;
		if (!UExplorer::Platform::InspectPeImage(ModuleBase, image).Ok())
			return nullptr;
		IMAGE_DOS_HEADER dos{};
		if (!UExplorer::Runtime::ReadValue(ModuleBase, dos).Ok())
			return nullptr;
		uintptr_t ntAddress = 0;
		if (!TryAddAddress(ModuleBase, static_cast<std::size_t>(dos.e_lfanew), ntAddress))
			return nullptr;
		IMAGE_NT_HEADERS64 nt{};
		if (!UExplorer::Runtime::ReadValue(ntAddress, nt).Ok())
			return nullptr;
		const IMAGE_DATA_DIRECTORY directory =
			nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		uintptr_t tableAddress = 0;
		if (directory.VirtualAddress == 0
			|| directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)
			|| !TryImageAddress(image, directory.VirtualAddress, directory.Size, tableAddress))
		{
			return nullptr;
		}

		const std::size_t descriptorCount = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
		for (std::size_t descriptorIndex = 0; descriptorIndex < descriptorCount; ++descriptorIndex)
		{
			IMAGE_IMPORT_DESCRIPTOR descriptor{};
			if (!UExplorer::Runtime::ReadValue(
				tableAddress + descriptorIndex * sizeof(descriptor),
				descriptor).Ok())
			{
				return nullptr;
			}
			if (descriptor.Characteristics == 0 && descriptor.FirstThunk == 0)
				break;
			std::string importedModule;
			if (!TryReadImageAsciiString(image, descriptor.Name, importedModule)
				|| _stricmp(importedModule.c_str(), ModuleToImportFrom) != 0)
			{
				continue;
			}

			const std::uint32_t nameThunkRva = descriptor.OriginalFirstThunk != 0
				? descriptor.OriginalFirstThunk
				: descriptor.FirstThunk;
			if (nameThunkRva >= image.Size || descriptor.FirstThunk >= image.Size)
				return nullptr;
			const std::size_t nameThunkCount =
				(image.Size - nameThunkRva) / sizeof(IMAGE_THUNK_DATA64);
			const std::size_t functionThunkCount =
				(image.Size - descriptor.FirstThunk) / sizeof(IMAGE_THUNK_DATA64);
			const std::size_t thunkCount = (std::min)(nameThunkCount, functionThunkCount);
			for (std::size_t thunkIndex = 0; thunkIndex < thunkCount; ++thunkIndex)
			{
				uintptr_t nameThunkAddress = 0;
				uintptr_t functionThunkAddress = 0;
				if (!TryImageAddress(
					image,
					nameThunkRva + static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64)),
					sizeof(IMAGE_THUNK_DATA64),
					nameThunkAddress)
					|| !TryImageAddress(
						image,
						descriptor.FirstThunk + static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64)),
						sizeof(IMAGE_THUNK_DATA64),
						functionThunkAddress))
				{
					return nullptr;
				}
				IMAGE_THUNK_DATA64 nameThunk{};
				if (!UExplorer::Runtime::ReadValue(nameThunkAddress, nameThunk).Ok())
					return nullptr;
				if (nameThunk.u1.AddressOfData == 0)
					break;
				if (IMAGE_SNAP_BY_ORDINAL64(nameThunk.u1.Ordinal))
					continue;
				if (nameThunk.u1.AddressOfData > (std::numeric_limits<std::uint32_t>::max)() - sizeof(WORD))
					return nullptr;
				std::string importedFunction;
				if (!TryReadImageAsciiString(
					image,
					static_cast<std::uint32_t>(nameThunk.u1.AddressOfData) + sizeof(WORD),
					importedFunction))
				{
					return nullptr;
				}
				if (importedFunction == SearchFunctionName)
					return reinterpret_cast<PIMAGE_THUNK_DATA>(functionThunkAddress);
			}
		}
		return nullptr;
	}

	std::pair<uintptr_t, uint32_t> GetSearchStartAndRangeBasedOnOverrides(const uintptr_t ModuleBase, const IMAGE_SECTION_HEADER* SectionHeader, const uintptr_t StartAddress, int32_t Range)
	{
		if (ModuleBase == 0 || !SectionHeader
			|| SectionHeader->VirtualAddress == 0
			|| SectionHeader->Misc.VirtualSize == 0)
			return { NULL, 0x0 };

		uintptr_t SectionStartAddress = 0;
		uintptr_t SectionEndAddress = 0;
		if (!TryAddAddress(ModuleBase, SectionHeader->VirtualAddress, SectionStartAddress)
			|| !TryAddAddress(SectionStartAddress, SectionHeader->Misc.VirtualSize, SectionEndAddress))
		{
			return {NULL, 0};
		}
		uintptr_t SearchStartAddress = SectionStartAddress;

		// Check if this section contains the StartAddress
		if (StartAddress != 0 && StartAddress > SectionStartAddress)
		{
			// This section does not contain any address greater than StartAddress
			if (StartAddress >= SectionEndAddress)
				return { NULL, 0x0 };

			SearchStartAddress = StartAddress;
		}
		const std::size_t available = static_cast<std::size_t>(SectionEndAddress - SearchStartAddress);
		const std::size_t requested = Range > 0
			? (std::min)(available, static_cast<std::size_t>(Range))
			: available;
		if (requested == 0 || requested > (std::numeric_limits<uint32_t>::max)())
			return {NULL, 0};

		return { SearchStartAddress, static_cast<uint32_t>(requested) };
	}

	void* ResolvePatternAddress(
		const uintptr_t start,
		const std::size_t range,
		const std::size_t matchOffset,
		const std::size_t patternSize,
		const bool relative,
		const uint32_t relativeOffset) noexcept
	{
		uintptr_t matchAddress = 0;
		if (!TryAddAddress(start, matchOffset, matchAddress))
			return nullptr;
		if (!relative)
			return reinterpret_cast<void*>(matchAddress);

		const std::size_t displacementOffset = relativeOffset
			== (std::numeric_limits<uint32_t>::max)()
			? patternSize
			: static_cast<std::size_t>(relativeOffset);
		if (matchOffset > range
			|| displacementOffset > range - matchOffset
			|| sizeof(int32_t) > range - matchOffset - displacementOffset)
		{
			return nullptr;
		}
		uintptr_t displacementAddress = 0;
		uintptr_t instructionEnd = 0;
		if (!TryAddAddress(matchAddress, displacementOffset, displacementAddress)
			|| !TryAddAddress(displacementAddress, sizeof(int32_t), instructionEnd))
		{
			return nullptr;
		}
		int32_t displacement = 0;
		if (!UExplorer::Runtime::ReadValue(displacementAddress, displacement).Ok())
			return nullptr;
		uintptr_t target = 0;
		return TryApplySignedDisplacement(instructionEnd, displacement, target)
			? reinterpret_cast<void*>(target)
			: nullptr;
	}

	void* FindPatternInReadableRange(
		const std::span<const int> pattern,
		const uintptr_t start,
		const std::size_t range,
		const bool relative,
		const uint32_t relativeOffset,
		const std::size_t skipCount) noexcept
	{
		if (pattern.empty() || pattern.size() > range)
			return nullptr;
		uintptr_t ignoredEnd = 0;
		if (!UExplorer::Runtime::CheckedAddressRange(start, range, ignoredEnd))
			return nullptr;
		for (const int value : pattern)
		{
			if (value < -1 || value > 0xFF)
				return nullptr;
		}

		constexpr std::size_t kReadChunkSize = 64 * 1024;
		const std::size_t overlap = pattern.size() - 1;
		if (overlap > (std::numeric_limits<std::size_t>::max)() - kReadChunkSize)
			return nullptr;
		try
		{
			std::vector<std::byte> window((std::min)(range, kReadChunkSize + overlap));
			std::size_t remainingSkips = skipCount;
			for (std::size_t globalOffset = 0; globalOffset < range;)
			{
				const std::size_t remaining = range - globalOffset;
				const std::size_t primarySize = (std::min)(remaining, kReadChunkSize);
				const std::size_t readSize = (std::min)(
					remaining,
					primarySize + overlap);
				uintptr_t readAddress = 0;
				if (!TryAddAddress(start, globalOffset, readAddress)
					|| !UExplorer::Runtime::ReadMemory(
						readAddress,
						std::span<std::byte>(window.data(), readSize)).Ok())
				{
					return nullptr;
				}
				if (readSize < pattern.size())
					break;

				const std::span<const uint8_t> bytes(
					reinterpret_cast<const uint8_t*>(window.data()),
					readSize);
				const bool finalWindow = primarySize == remaining;
				const std::size_t candidateLimit = finalWindow
					? readSize - pattern.size() + 1
					: primarySize;
				std::size_t searchBase = 0;
				while (searchBase < candidateLimit)
				{
					const std::optional<std::size_t> found =
						UExplorer::Platform::FindBytePatternOffset(
							bytes.subspan(searchBase),
							pattern);
					if (!found)
						break;
					const std::size_t localOffset = searchBase + *found;
					if (localOffset >= candidateLimit)
						break;
					if (remainingSkips == 0)
					{
						return ResolvePatternAddress(
							start,
							range,
							globalOffset + localOffset,
							pattern.size(),
							relative,
							relativeOffset);
					}
					--remainingSkips;
					searchBase = localOffset + 1;
				}
				globalOffset += primarySize;
			}
		}
		catch (...)
		{
			return nullptr;
		}
		return nullptr;
	}

	template<typename CharType>
	bool TryMeasureStringWithSeh(
		const CharType* value,
		std::size_t& length) noexcept
	{
		if (!value)
			return false;
		constexpr std::size_t kMaxReferenceCharacters = 64 * 1024;
#if defined(_MSC_VER)
		__try
		{
#endif
			for (length = 0; length < kMaxReferenceCharacters; ++length)
			{
				if (value[length] == CharType{})
					return true;
			}
#if defined(_MSC_VER)
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			length = 0;
			return false;
		}
#endif
		length = 0;
		return false;
	}

	template<typename CharType>
	bool SafeStringEquals(
		const CharType* expected,
		const uintptr_t address,
		const std::size_t characterCount) noexcept
	{
		if (!expected || address == 0 || characterCount == 0
			|| characterCount > (std::numeric_limits<std::size_t>::max)() / sizeof(CharType))
		{
			return false;
		}
		try
		{
			std::vector<CharType> actual(characterCount);
			if (!UExplorer::Runtime::ReadMemory(
				address,
				std::as_writable_bytes(std::span<CharType>(actual))).Ok())
			{
				return false;
			}
			return std::equal(actual.begin(), actual.end(), expected);
		}
		catch (...)
		{
			return false;
		}
	}
}


void* WindowsPrivateImplHelper::FinAlignedValueInRangeImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, uintptr_t StartAddress, uint32_t Range)
{
	if (!ValuePtr || !ComparisonFunction || ValueTypeSize <= 0 || Alignment <= 0
		|| !UExplorer::Runtime::ValidateReadableMemory(StartAddress, Range).Ok())
	{
		return nullptr;
	}
	const auto SizeFromEnd = GetAlignedSizeWithOffsetFromEnd(Range, Alignment, ValueTypeSize);

	if (SizeFromEnd == -1)
		return nullptr;

	for (int64_t i = 0x0; i <= SizeFromEnd; i += Alignment)
	{
		void* TypedPtr = reinterpret_cast<void*>(StartAddress + i);
		bool equal = false;
		if (!CompareMemoryValueWithSeh(ComparisonFunction, ValuePtr, TypedPtr, equal))
			return nullptr;
		if (equal)
			return TypedPtr;
	}

	return nullptr;
}

void* WindowsPrivateImplHelper::FindAlignedValueInSectionImpl(const SectionInfo& Info, const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment)
{
	const WindowsSectionInfo WinSectionInfo = SectionInfoToWinSectionInfo(Info);
	if (!WinSectionInfo.IsValid())
		return nullptr;

	const uint32_t Range = WinSectionInfo.SectionHeader->Misc.VirtualSize;
	uintptr_t SectionBaseAddrss = 0;
	if (!TryAddAddress(
		WinSectionInfo.Imagebase,
		WinSectionInfo.SectionHeader->VirtualAddress,
		SectionBaseAddrss))
	{
		return nullptr;
	}

	return FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, SectionBaseAddrss, Range);
}

void* WindowsPrivateImplHelper::FindAlignedValueInAllSectionsImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, const uintptr_t StartAddress, int32_t Range, const char* const ModuleName)
{
	const auto ModuleBase = GetModuleBase(ModuleName);
	if (ModuleBase == 0 || !ValuePtr || !ComparisonFunction
		|| ValueTypeSize <= 0 || Alignment <= 0)
	{
		return nullptr;
	}

	void* Result = nullptr;
	auto FindStringInSection = [&Result, &Range, StartAddress, ModuleBase, ValuePtr, ComparisonFunction, ValueTypeSize, Alignment](const IMAGE_SECTION_HEADER* SectionHeader) -> bool
	{
		const auto [SearchStartAddress, SearchRange] = GetSearchStartAndRangeBasedOnOverrides(ModuleBase, SectionHeader, StartAddress, Range);
	
		if (SearchStartAddress == NULL || SearchRange == 0x0)
			return false;

		if (Range > 0x0)
			Range -= SearchRange;

		Result = FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, SearchStartAddress, SearchRange);

		return Result != nullptr;
	};

	IterateAllSectionObjects(ModuleBase, FindStringInSection);

	return Result;
}



uintptr_t PlatformWindows::GetModuleBase(const char* const ModuleName)
{
	const HMODULE module = ModuleName
		? GetModuleHandleA(ModuleName)
		: GetModuleHandleW(nullptr);
	return reinterpret_cast<uintptr_t>(module);
}

uintptr_t PlatformWindows::GetOffset(const uintptr_t Address, const char* const ModuleName)
{
	const uintptr_t moduleBase = GetModuleBase(ModuleName);
	if (Address == 0 || moduleBase == 0 || Address < moduleBase)
		return 0;
	return Address - moduleBase;
}

uintptr_t PlatformWindows::GetOffset(const void* Address, const char* const ModuleName)
{
	return GetOffset(reinterpret_cast<const uintptr_t>(Address), ModuleName);
}

std::pair<std::string, uintptr_t> PlatformWindows::GetModuleAndOffset(const void* Address)
{
	if (Address == nullptr)
		return { "", 0x0 };

	HMODULE ModuleHandle = nullptr;
	constexpr DWORD GetModuleHandleFlags = (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);

	if (!GetModuleHandleExA(GetModuleHandleFlags, static_cast<LPCSTR>(Address), &ModuleHandle) || ModuleHandle == nullptr)
		return { "", 0x0 };

	const uintptr_t ModuleBase = reinterpret_cast<uintptr_t>(ModuleHandle);
	const uintptr_t Offset = reinterpret_cast<uintptr_t>(Address) - ModuleBase;

	char ModulePath[MAX_PATH] = {};
	const DWORD ModulePathSize = GetModuleFileNameA(ModuleHandle, ModulePath, MAX_PATH);

	if (ModulePathSize == 0x0 || ModulePathSize >= MAX_PATH)
		return { "", Offset };

	const std::string FullModulePath(ModulePath, ModulePathSize);
	const size_t LastSlash = FullModulePath.find_last_of("\\/");
	const std::string ModuleName = (LastSlash == std::string::npos) ? FullModulePath : FullModulePath.substr(LastSlash + 1);

	return { ModuleName, Offset };
}

SectionInfo PlatformWindows::GetSectionInfo(const std::string& SectionName, const char* const ModuleName)
{
	const uintptr_t ModuleBase = GetModuleBase(ModuleName);
	WindowsSectionInfo WinSectionInfo = { .Imagebase = ModuleBase };
	UExplorer::Platform::PeImageView image;
	if (!SectionName.empty() && UExplorer::Platform::InspectPeImage(ModuleBase, image).Ok())
	{
		const UExplorer::Platform::PeSectionView* section = image.FindSection(SectionName);
		if (section)
		{
			WinSectionInfo.SectionHeader =
				reinterpret_cast<const IMAGE_SECTION_HEADER*>(section->HeaderAddress);
		}
	}
	
	return WinSectionInfoToSectionInfo(WinSectionInfo);
}

void* PlatformWindows::IterateSectionWithCallback(const SectionInfo& Info, const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd)
{
	const WindowsSectionInfo WinSectionInfo = SectionInfoToWinSectionInfo(Info);

	if (!WinSectionInfo.IsValid() || !Callback || Granularity == 0)
		return nullptr;

	uintptr_t SectionBaseAddrss = 0;
	if (!TryAddAddress(
		WinSectionInfo.Imagebase,
		WinSectionInfo.SectionHeader->VirtualAddress,
		SectionBaseAddrss))
	{
		return nullptr;
	}
	const int64_t SectionIterationSize = GetAlignedSizeWithOffsetFromEnd(
		WinSectionInfo.SectionHeader->Misc.VirtualSize,
		Granularity,
		OffsetFromEnd);

	if (SectionIterationSize == -1)
		return nullptr;
	if (!UExplorer::Runtime::ValidateReadableMemory(
		SectionBaseAddrss,
		static_cast<std::size_t>(SectionIterationSize) + OffsetFromEnd).Ok())
	{
		return nullptr;
	}

	for (uintptr_t CurrentAddress = SectionBaseAddrss;
		CurrentAddress - SectionBaseAddrss < static_cast<uintptr_t>(SectionIterationSize);
		CurrentAddress += Granularity)
	{
		if (Callback(reinterpret_cast<void*>(CurrentAddress)))
			return reinterpret_cast<void*>(CurrentAddress);
	}

	return nullptr;
}

void* PlatformWindows::IterateAllSectionsWithCallback(const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd, const char* const ModuleName)
{
	void* Result = nullptr;

	const uintptr_t ModuleBase = GetModuleBase(ModuleName);

	IterateAllSectionObjects(ModuleBase, [ModuleBase, &Result, &Callback, Granularity, OffsetFromEnd](const IMAGE_SECTION_HEADER* Section) -> bool
		{
			const WindowsSectionInfo WinSectionInfo = { ModuleBase, Section };

			if (void* Address = IterateSectionWithCallback(WinSectionInfoToSectionInfo(WinSectionInfo), Callback, Granularity, OffsetFromEnd))
			{
				Result = Address;
				return true;
			}

			return false;
		});

	return Result;
}


bool PlatformWindows::IsAddressInAnyModule(const uintptr_t Address)
{
	return IsAddressInAnyModule(reinterpret_cast<const void*>(Address));
}

bool PlatformWindows::IsAddressInAnyModule(const void* Address)
{
	if (!Address)
		return false;
	const uintptr_t target = reinterpret_cast<uintptr_t>(Address);
	bool found = false;
	VisitLoadedModules([target, &found](const LDR_DATA_TABLE_ENTRY& entry) {
		if (!entry.DllBase || entry.SizeOfImage == 0)
			return false;
		const uintptr_t moduleBegin = reinterpret_cast<uintptr_t>(entry.DllBase);
		uintptr_t moduleEnd = 0;
		if (!UExplorer::Runtime::CheckedAddressRange(
			moduleBegin,
			entry.SizeOfImage,
			moduleEnd))
		{
			return false;
		}
		found = target >= moduleBegin && target < moduleEnd;
		return found;
	});
	return found;
}

bool PlatformWindows::IsAddressInProcessRange(const uintptr_t Address)
{
	if (Address == 0)
		return false;

	if constexpr (!Is32Bit())
	{
		if (!Architecture_x86_64::IsValid64BitVirtualAddress(reinterpret_cast<const void*>(Address)))
			return false;
	}

	// Keep this as a pure "currently readable" probe.
	// Do not mix in module-range heuristics here.
	return !IsBadReadPtr(reinterpret_cast<const void*>(Address), 1);
}
bool PlatformWindows::IsAddressInProcessRange(const void* Address)
{
	return IsAddressInProcessRange(reinterpret_cast<uintptr_t>(Address));
}
bool PlatformWindows::IsBadReadPtr(const uintptr_t Address)
{
	return IsBadReadPtr(Address, sizeof(void*));
}
bool PlatformWindows::IsBadReadPtr(const void* Address)
{
	return IsBadReadPtr(Address, sizeof(void*));
}

bool PlatformWindows::IsBadReadPtr(const uintptr_t Address, const std::size_t Size)
{
	return IsBadReadPtr(reinterpret_cast<const void*>(Address), Size);
}

bool PlatformWindows::IsBadReadPtr(const void* Address, const std::size_t Size)
{
	const uintptr_t begin = reinterpret_cast<uintptr_t>(Address);
	uintptr_t endExclusive = 0;
	if (!Address || !UExplorer::Runtime::CheckedAddressRange(begin, Size, endExclusive)
		|| !Architecture_x86_64::IsValid64BitVirtualAddress(Address)
		|| !Architecture_x86_64::IsValid64BitVirtualAddress(
			reinterpret_cast<const void*>(endExclusive - 1)))
	{
		return true;
	}
	if (!UExplorer::Runtime::ValidateReadableMemory(begin, Size).Ok())
		return true;
	std::uint8_t probe = 0;
	if (!UExplorer::Runtime::ReadValue(begin, probe).Ok())
		return true;
	return !UExplorer::Runtime::ReadValue(endExclusive - 1, probe).Ok();
}

const void* PlatformWindows::GetAddressOfImportedFunction(const char* SearchModuleName, const char* ModuleToImportFrom, const char* SearchFunctionName)
{
	if (!ModuleToImportFrom || !*ModuleToImportFrom
		|| !SearchFunctionName || !*SearchFunctionName)
	{
		return nullptr;
	}
	const uintptr_t SearchModule = GetModuleBase(SearchModuleName);
	if (SearchModule == 0)
		return nullptr;
	return GetImportAddress(SearchModule, ModuleToImportFrom, SearchFunctionName);
}
const void* PlatformWindows::GetAddressOfImportedFunctionFromAnyModule(const char* ModuleToImportFrom, const char* SearchFunctionName)
{
	if (!ModuleToImportFrom || !*ModuleToImportFrom
		|| !SearchFunctionName || !*SearchFunctionName)
	{
		return nullptr;
	}
	const void* result = nullptr;
	VisitLoadedModules([&result, ModuleToImportFrom, SearchFunctionName](
		const LDR_DATA_TABLE_ENTRY& entry) {
		const PIMAGE_THUNK_DATA slot = GetImportAddress(
			reinterpret_cast<uintptr_t>(entry.DllBase),
			ModuleToImportFrom,
			SearchFunctionName);
		if (!slot)
			return false;
		IMAGE_THUNK_DATA thunk{};
		if (!UExplorer::Runtime::ReadValue(reinterpret_cast<uintptr_t>(slot), thunk).Ok()
			|| thunk.u1.Function == 0)
		{
			return false;
		}
		result = reinterpret_cast<const void*>(thunk.u1.Function);
		return true;
	});
	return result;
}

const void* PlatformWindows::GetAddressOfExportedFunction(const char* SearchModuleName, const char* SearchFunctionName)
{
	if (!SearchFunctionName || !*SearchFunctionName)
		return nullptr;
	const HMODULE module = reinterpret_cast<HMODULE>(GetModuleBase(SearchModuleName));
	if (!module)
		return nullptr;
	return reinterpret_cast<const void*>(GetProcAddress(module, SearchFunctionName));
}

template<bool bShouldResolve32BitJumps>
std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions(void** VTable, const std::function<bool(const uint8_t* Address, int32_t Index)>& CallBackForEachFunc, int32_t NumFunctions, int32_t OffsetFromStart)
{
	if (!VTable || !CallBackForEachFunc || NumFunctions <= 0
		|| OffsetFromStart < 0 || OffsetFromStart >= NumFunctions)
		return { nullptr, -1 };

	for (int32_t i = OffsetFromStart; i < NumFunctions; ++i)
	{
		void* currentFunction = nullptr;
		uintptr_t slotAddress = 0;
		if (!TryAddAddress(
			reinterpret_cast<uintptr_t>(VTable),
			static_cast<std::size_t>(i) * sizeof(void*),
			slotAddress))
		{
			break;
		}
		if (!UExplorer::Runtime::ReadValue(slotAddress, currentFunction).Ok())
			break;
		const uintptr_t currentAddress = reinterpret_cast<uintptr_t>(currentFunction);
		if (currentAddress == 0 || !IsAddressInProcessRange(currentAddress))
			break;

		uintptr_t resolvedAddress = currentAddress;
		if constexpr (bShouldResolve32BitJumps)
		{
			uint8_t opcode = 0;
			if (!UExplorer::Runtime::ReadValue(currentAddress, opcode).Ok())
				break;
			if (opcode == 0xE9)
			{
				int32_t displacement = 0;
				uintptr_t displacementAddress = 0;
				uintptr_t instructionEnd = 0;
				uintptr_t jumpTarget = 0;
				if (TryAddAddress(currentAddress, 1, displacementAddress)
					&& UExplorer::Runtime::ReadValue(displacementAddress, displacement).Ok()
					&& TryAddAddress(currentAddress, 5, instructionEnd)
					&& TryApplySignedDisplacement(instructionEnd, displacement, jumpTarget)
					&& IsAddressInProcessRange(jumpTarget))
				{
					resolvedAddress = jumpTarget;
				}
			}
		}

		const auto* resolved = reinterpret_cast<const uint8_t*>(resolvedAddress);
		if (CallBackForEachFunc(resolved, i))
			return { resolved, i };
	}

	return { nullptr, -1 };
}


void* PlatformWindows::FindPattern(const char* Signature, const uint32_t Offset, const bool bSearchAllSections, const uintptr_t StartAddress, const char* const ModuleName)
{
	const auto ModuleBase = GetModuleBase(ModuleName);
	if (ModuleBase == 0 || !Signature || !*Signature)
		return nullptr;

	void* Result = nullptr;
	auto FindPatternInRangeLambda = [&Result, ModuleBase, Signature, Offset, StartAddress](const IMAGE_SECTION_HEADER* SectionHeader) -> bool
	{
		const auto [SearchStartAddress, SearchRange] = GetSearchStartAndRangeBasedOnOverrides(ModuleBase, SectionHeader, StartAddress, 0x0);

		if (SearchStartAddress == NULL || SearchRange == 0x0)
			return false;

		Result = FindPatternInRange(Signature, reinterpret_cast<const uint8_t*>(SearchStartAddress), SearchRange, Offset != 0x0, Offset);

		return Result != nullptr;
	};

	if (bSearchAllSections)
	{
		IterateAllSectionObjects(ModuleBase, FindPatternInRangeLambda);
	}
	else
	{
		const WindowsSectionInfo WinSectionInfo = SectionInfoToWinSectionInfo(GetSectionInfo(".text", ModuleName));
		if (!WinSectionInfo.IsValid())
			return nullptr;

		const uint32_t Range = WinSectionInfo.SectionHeader->Misc.VirtualSize;
		uintptr_t SectionBaseAddrss = 0;
		if (!TryAddAddress(
			WinSectionInfo.Imagebase,
			WinSectionInfo.SectionHeader->VirtualAddress,
			SectionBaseAddrss))
		{
			return nullptr;
		}

		return FindPatternInRange(Signature, reinterpret_cast<const uint8_t*>(SectionBaseAddrss), Range, Offset != 0x0, Offset);
	}

	return Result;
}

void* PlatformWindows::FindPatternInRange(const char* Signature, const void* Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
{
	if (!Signature || !Start || Range == 0)
		return nullptr;
	std::vector<int> pattern;
	if (!UExplorer::Platform::TryParseBytePattern(Signature, pattern))
		return nullptr;
	return FindPatternInReadableRange(
		pattern,
		reinterpret_cast<uintptr_t>(Start),
		static_cast<std::size_t>(Range),
		bRelative,
		Offset,
		0);
}

void* PlatformWindows::FindPatternInRange(const char* Signature, const uintptr_t Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
{
	return FindPatternInRange(Signature, reinterpret_cast<void*>(Start), Range, bRelative, Offset);
}

void* PlatformWindows::FindPatternInRange(std::vector<int>&& Signature, const void* Start, const uintptr_t Range, const bool bRelative, uint32_t Offset, const uint32_t SkipCount)
{
	if (!Start || Range == 0)
		return nullptr;
	return FindPatternInReadableRange(
		Signature,
		reinterpret_cast<uintptr_t>(Start),
		static_cast<std::size_t>(Range),
		bRelative,
		Offset,
		SkipCount);
}

/* Slower than FindByString */
template<bool bCheckIfLeaIsStrPtr, typename CharType>
void* PlatformWindows::FindByStringInAllSections(const CharType* RefStr,const uintptr_t StartAddress, int32_t Range, const bool bSearchOnlyExecutableSections, const char* const ModuleName)
{
	static_assert(std::is_same_v<CharType, char> || std::is_same_v<CharType, wchar_t>, "FindByStringInAllSections only supports 'char' and 'wchar_t', but was called with other type.");
	const auto ModuleBase = GetModuleBase(ModuleName);
	if (!RefStr || ModuleBase == 0)
		return nullptr;

	void* Result = nullptr;
	auto FindStringInSection = [&Result, &Range, StartAddress, ModuleBase, bSearchOnlyExecutableSections, RefStr](const IMAGE_SECTION_HEADER* SectionHeader) -> bool
	{
		if (bSearchOnlyExecutableSections && !(SectionHeader->Characteristics & IMAGE_SCN_MEM_EXECUTE))
			return false;

		const auto [SearchStartAddress, SearchRange] = GetSearchStartAndRangeBasedOnOverrides(ModuleBase, SectionHeader, StartAddress, Range);

		constexpr uint32_t InstructionBytesLength = 0x7;
		if (SearchStartAddress == NULL || SearchRange <= InstructionBytesLength)
			return false;

		if (Range > 0x0)
			Range -= static_cast<int32_t>(SearchRange);
		if (SearchRange - InstructionBytesLength
			> static_cast<uint32_t>((std::numeric_limits<int32_t>::max)()))
		{
			return false;
		}

		Result = FindStringInRange<bCheckIfLeaIsStrPtr, CharType>(
			RefStr,
			SearchStartAddress,
			static_cast<int32_t>(SearchRange - InstructionBytesLength));

		return Result != nullptr;
	};

	IterateAllSectionObjects(ModuleBase, FindStringInSection);

	return Result;
}

template<bool bCheckIfLeaIsStrPtr, typename CharType>
inline void* PlatformWindows::FindStringInRange(const CharType* RefStr, const uintptr_t StartAddress, const int32_t Range)
{
	if (!RefStr || StartAddress == 0 || Range <= 0)
		return nullptr;
	// Ensure the null-terminator is also compared, else strings that are substrings of other strings might be falsely matched.
	std::size_t rawLength = 0;
	if (!TryMeasureStringWithSeh(RefStr, rawLength)
		|| rawLength == (std::numeric_limits<std::size_t>::max)())
		return nullptr;
	const std::size_t RefStrLen = rawLength + 1;
	constexpr std::size_t InstructionBytesLength = 0x7;
	const std::size_t searchBytes = static_cast<std::size_t>(Range) + InstructionBytesLength;
	std::vector<uint8_t> instructions;
	try
	{
		instructions.resize(searchBytes);
	}
	catch (...)
	{
		return nullptr;
	}
	if (!UExplorer::Runtime::ReadMemory(
		StartAddress,
		std::as_writable_bytes(std::span<uint8_t>(instructions))).Ok())
	{
		return nullptr;
	}

	for (int32 i = 0; i < Range; i++)
	{
		// opcode: lea
		if ((instructions[i] == uint8_t(0x4C) || instructions[i] == uint8_t(0x48))
			&& instructions[i + 1] == uint8_t(0x8D))
		{
			int32_t displacement = 0;
			std::memcpy(&displacement, instructions.data() + i + 3, sizeof(displacement));
			uintptr_t instructionAddress = 0;
			uintptr_t instructionEnd = 0;
			uintptr_t StrPtr = 0;
			if (!TryAddAddress(StartAddress, static_cast<std::size_t>(i), instructionAddress)
				|| !TryAddAddress(instructionAddress, InstructionBytesLength, instructionEnd)
				|| !TryApplySignedDisplacement(instructionEnd, displacement, StrPtr))
			{
				continue;
			}
			if (SafeStringEquals(RefStr, StrPtr, RefStrLen))
				return reinterpret_cast<void*>(instructionAddress);

			if constexpr (bCheckIfLeaIsStrPtr)
			{
				const CharType* indirectString = nullptr;
				if (!UExplorer::Runtime::ReadValue(StrPtr, indirectString).Ok()
					|| !indirectString)
				{
					continue;
				}
				if (SafeStringEquals(
					RefStr,
					reinterpret_cast<uintptr_t>(indirectString),
					RefStrLen))
				{
					return reinterpret_cast<void*>(instructionAddress);
				}
			}
		}
	}

	return nullptr;
}



/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
template void* PlatformWindows::FindByStringInAllSections<false, char>(const char*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<false, wchar_t>(const wchar_t*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<true, char>(const char*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<true, wchar_t>(const wchar_t*, const uintptr_t, int32_t, const bool, const char* const);

template std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions<true>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);
template std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions<false>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);

