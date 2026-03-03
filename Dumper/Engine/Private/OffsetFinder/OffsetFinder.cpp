#include <algorithm>
#include <climits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "OffsetFinder/OffsetFinder.h"
#include "Unreal/ObjectArray.h"

#include "Platform.h"
#include "Settings.h"

namespace
{
	// Keep this trivial: SEH helper must avoid C++ objects with destructors.
	static bool TryReadU8Safe(uintptr_t address, uint8& outValue)
	{
		if (address == 0)
			return false;

		__try
		{
			outValue = *reinterpret_cast<const uint8*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	struct ScriptOffsetDiagnostics
	{
		int32 SelectedOffset = OffsetNotFound;
		int32 SelectedScore = INT32_MIN;
		int32 ScoreGapTop2 = 0;
		int32 BpEndHits = 0;
		int32 WeightedBpEndHits = 0;
		int32 GenericScriptHits = 0;

		int32 VerifyProbed = 0;
		int32 VerifyHeaderValid = 0;
		int32 VerifyEndHits = 0;
		int32 VerifyFirstOpcodeValid = 0;
		int32 VerifySizeSane = 0;
		int32 VerifyEndRate = 0;
		int32 VerifyOpcodeRate = 0;

		std::string Confidence = "unknown"; // unknown/high/medium/low
		std::string AnomalyTags = "";
		bool FromCache = false;
		uint64 CacheKey = 0;
	};

	static std::mutex sScriptOffsetDiagnosticsMutex;
	static ScriptOffsetDiagnostics sScriptOffsetDiagnostics;

	static void SetScriptOffsetDiagnostics(const ScriptOffsetDiagnostics& in)
	{
		std::lock_guard<std::mutex> lock(sScriptOffsetDiagnosticsMutex);
		sScriptOffsetDiagnostics = in;
	}

	static ScriptOffsetDiagnostics GetScriptOffsetDiagnosticsSnapshot()
	{
		std::lock_guard<std::mutex> lock(sScriptOffsetDiagnosticsMutex);
		return sScriptOffsetDiagnostics;
	}
}

extern "C" const char* UExplorer_GetScriptOffsetConfidence()
{
	thread_local std::string value;
	value = GetScriptOffsetDiagnosticsSnapshot().Confidence;
	return value.c_str();
}

extern "C" const char* UExplorer_GetScriptOffsetAnomalyTags()
{
	thread_local std::string value;
	value = GetScriptOffsetDiagnosticsSnapshot().AnomalyTags;
	return value.c_str();
}

extern "C" int32 UExplorer_GetScriptOffsetSelectedOffset()
{
	return GetScriptOffsetDiagnosticsSnapshot().SelectedOffset;
}

extern "C" int32 UExplorer_GetScriptOffsetSelectedScore()
{
	return GetScriptOffsetDiagnosticsSnapshot().SelectedScore;
}

extern "C" int32 UExplorer_GetScriptOffsetScoreGapTop2()
{
	return GetScriptOffsetDiagnosticsSnapshot().ScoreGapTop2;
}

extern "C" int32 UExplorer_GetScriptOffsetBpEndHits()
{
	return GetScriptOffsetDiagnosticsSnapshot().BpEndHits;
}

extern "C" int32 UExplorer_GetScriptOffsetWeightedBpEndHits()
{
	return GetScriptOffsetDiagnosticsSnapshot().WeightedBpEndHits;
}

extern "C" int32 UExplorer_GetScriptOffsetGenericScriptHits()
{
	return GetScriptOffsetDiagnosticsSnapshot().GenericScriptHits;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyProbed()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyProbed;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyHeaderValid()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyHeaderValid;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyEndHits()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyEndHits;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyFirstOpcodeValid()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyFirstOpcodeValid;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifySizeSane()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifySizeSane;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyEndRate()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyEndRate;
}

extern "C" int32 UExplorer_GetScriptOffsetVerifyOpcodeRate()
{
	return GetScriptOffsetDiagnosticsSnapshot().VerifyOpcodeRate;
}

/* UObject */
int32_t OffsetFinder::FindUObjectFlagsOffset()
{
	constexpr auto EnumFlagValueToSearch = 0x43;

	/* We're looking for a commonly occuring flag and this number basically defines the minimum number that counts ad "commonly occuring". */
	constexpr auto MinNumFlagValuesRequiredAtOffset = 0xA0;

	for (int i = 0; i < 0x20; i++)
	{
		int Offset = 0x0;
		while (Offset != OffsetNotFound)
		{
			// Look for 0x43 in this object, as it is a really common value for UObject::Flags
			Offset = FindOffset(std::vector{ std::pair{ ObjectArray::GetByIndex(i).GetAddress(), EnumFlagValueToSearch } }, Offset, 0x40);

			if (Offset == OffsetNotFound)
				break; // Early exit

			/* We're looking for a common flag. To check if the flag  is common we're checking the first 0x100 objects to see how often the flag occures at this offset. */
			int32 NumObjectsWithFlagAtOffset = 0x0;

			int Counter = 0;
			for (UEObject Obj : ObjectArray())
			{
				// Only check the (possible) flags of the first 0x100 objects
				if (Counter++ == 0x100)
					break;

				const int32 TypedValueAtOffset = *reinterpret_cast<int32*>(reinterpret_cast<uintptr_t>(Obj.GetAddress()) + Offset);

				if (TypedValueAtOffset == EnumFlagValueToSearch)
					NumObjectsWithFlagAtOffset++;
			}

			if (NumObjectsWithFlagAtOffset > MinNumFlagValuesRequiredAtOffset)
				return Offset;
		}
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindUObjectIndexOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.emplace_back(ObjectArray::GetByIndex(0x055).GetAddress(), 0x055);
	Infos.emplace_back(ObjectArray::GetByIndex(0x123).GetAddress(), 0x123);

	return FindOffset<4>(Infos, sizeof(void*)); // Skip VTable
}

int32_t OffsetFinder::FindUObjectClassOffset()
{
	/* Checks for a pointer that points to itself in the end. The UObject::Class pointer of "Class CoreUObject.Class" will point to "Class CoreUObject.Class". */
	auto IsValidCyclicUClassPtrOffset = [](const uint8_t* ObjA, const uint8_t* ObjB, int32_t ClassPtrOffset)
	{
		/* Will be advanced before they are used. */
		const uint8_t* NextClassA = ObjA;
		const uint8_t* NextClassB = ObjB;

		for (int MaxLoopCount = 0; MaxLoopCount < 0x10; MaxLoopCount++)
		{
			const uint8_t* CurrentClassA = NextClassA;
			const uint8_t* CurrentClassB = NextClassB;

			NextClassA = *reinterpret_cast<const uint8_t* const*>(NextClassA + ClassPtrOffset);
			NextClassB = *reinterpret_cast<const uint8_t* const*>(NextClassB + ClassPtrOffset);

			/* If this was UObject::Class it would never be invalid. The pointer would simply point to itself.*/
			if (!NextClassA || !NextClassB || Platform::IsBadReadPtr(NextClassA) || Platform::IsBadReadPtr(NextClassB))
				return false;

			if (CurrentClassA == NextClassA && CurrentClassB == NextClassB)
				return true;
		}

		return false;
	};

	const uint8_t* const ObjA = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x055).GetAddress());
	const uint8_t* const ObjB = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x123).GetAddress());

	int32_t Offset = 0;
	while (Offset != OffsetNotFound)
	{
		Offset = GetValidPointerOffset<true>(ObjA, ObjB, Offset + sizeof(void*), 0x50);

		if (IsValidCyclicUClassPtrOffset(ObjA, ObjB, Offset))
			return Offset;
	}

	return OffsetNotFound;
}

/*
* IsPotentialValidOffset: A function to filter offsets that can not possibly be valid for UObject::Name or FField::Name.
*						  Example for UObject::Name: it can 100% not be at the same offset as UObject::Class
* 
* DataGatherer: A function to gather values at the offsets not filterd by 'IsPotentialValidOffset'. Data is later used to filter more offsets, until hopefully only one is left.
*/
template<typename IteratorType>
int32_t FindNameOffsetForSomeClass(std::function<bool(int32_t Value)> IsPotentialValidOffset, IteratorType DataSetStartIterator, IteratorType DataSetEndIterator)
{
	/*
	* Requirements:
	*	- CmpIdx > 0x10 && CmpIdx < 0xF0000000
	*	- AverageValue >= 0x100 && AverageValue <= 0xFF00000;
	*	- Offset != { OtherOffsets }
	*/

	/* A struct describing the value */
	struct ValueInfo
	{
		int32 Offset;					   // Offset from the UObject start to this value
		int32 NumNamesWithLowCmpIdx = 0x0; // The number of names where the comparison index is in the range [0, 16]. Usually this should be far less than 0x20 names.
		uint64 TotalValue = 0x0;		   // The total value of the int32 data at this offset over all objects in GObjects
		bool bIsValidCmpIdxRange = true;   // Whether this value could be a valid FName::ComparisonIndex
	};


	std::vector<ValueInfo> PossibleOffsets;

	constexpr auto MaxAllowedComparisonIndexValue = 0x4000000; // Somewhat arbitrary limit. Make sure this isn't too low for games on FNamePool with lots of names and 0x14 block-size bits

	constexpr auto MaxAllowedAverageComparisonIndexValue = MaxAllowedComparisonIndexValue / 2; // Also somewhat arbitrary limit, but the average value shouldn't be as high as the max allowed one
	constexpr auto MinAllowedAverageComparisonIndexValue = 0x280; // If the average name is below 0x100 it is either the smallest UE application ever, or not the right offset

	constexpr auto LowComparisonIndexUpperCap = 0x10; // The upper limit of what is considered a "low" comparison index
	constexpr auto MaxAllowedNamesWithLowCmpIdx = 0x40;


	for (int i = sizeof(void*); i <= 0x40; i += 0x4)
	{
		if (!IsPotentialValidOffset(i))
			continue;

		PossibleOffsets.push_back(ValueInfo{ i });
	}

	auto GetDataAtOffsetAsInt = [](const void* Ptr, int32 Offset) -> uint32 { return *reinterpret_cast<const uint32*>(reinterpret_cast<const uintptr_t>(Ptr) + Offset); };

	int NumObjectsConsidered = 0;

	for (; DataSetStartIterator != DataSetEndIterator; ++DataSetStartIterator)
	{
		constexpr auto X86SmallPageSize = 0x1000;
		constexpr auto MaxAccessedSizeInUObject = 0x44;

		const void* CurrentObjectOrField = (*DataSetStartIterator).GetAddress();

		/*
		* Purpose: Make sure all offsets in the UObject::Name finder can be accessed
		* Reasoning: Objects are allocated in Blocks, these allocations are page-aligned in both size and base. If an object + MaxAccessedSizeInUObject goes past the page-bounds
		*            it might also go past the extends of an allocation. There's no reliable way of getting the size of UObject without knowing it's offsets first.
		*/
		const bool bIsGoingPastPageBounds = (reinterpret_cast<const uintptr_t>(CurrentObjectOrField) & (X86SmallPageSize - 1)) > (X86SmallPageSize - MaxAccessedSizeInUObject);
		if (bIsGoingPastPageBounds)
			continue;

		NumObjectsConsidered++;

		for (ValueInfo& Info : PossibleOffsets)
		{
			const uint32 ValueAtOffset = GetDataAtOffsetAsInt(CurrentObjectOrField, Info.Offset);

			Info.TotalValue += ValueAtOffset;
			Info.bIsValidCmpIdxRange = Info.bIsValidCmpIdxRange && ValueAtOffset < MaxAllowedComparisonIndexValue;
			Info.NumNamesWithLowCmpIdx += (ValueAtOffset <= LowComparisonIndexUpperCap);
		}
	}

	int32 FirstValidOffset = -1;
	for (const ValueInfo& Info : PossibleOffsets)
	{
		const auto AverageValue = (Info.TotalValue / NumObjectsConsidered);

		if (Info.bIsValidCmpIdxRange && Info.NumNamesWithLowCmpIdx <= MaxAllowedNamesWithLowCmpIdx
			&& AverageValue >= MinAllowedAverageComparisonIndexValue && AverageValue <= MaxAllowedAverageComparisonIndexValue)
		{
			if (FirstValidOffset == -1)
			{
				FirstValidOffset = Info.Offset;
				continue;
			}

			/* This shouldn't be the case, so log it as an info but continue, as the first offset is still likely the right one. */
			std::cerr << std::format("Dumper-7: Another [UObject/FField]::Name offset (0x{:04X}) is also considered valid.\n", Info.Offset);
		}
	}

	return FirstValidOffset;
}

int32_t OffsetFinder::FindUObjectNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
	{
		// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
		return Offset != Off::UObject::Class && Offset != (Off::UObject::Class + (sizeof(void*) / 2))
			&& Offset != Off::UObject::Outer && Offset != (Off::UObject::Outer + (sizeof(void*) / 2))
			&& Offset != Off::UObject::Flags
			&& Offset != Off::UObject::Index
			&& Offset != Off::UObject::Vft && Offset != (Off::UObject::Vft + (sizeof(void*) / 2));
	};

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, ObjectArray().begin(), ObjectArray().end());
}

int32_t OffsetFinder::FindUObjectOuterOffset()
{
	int32_t LowestFoundOffset = 0xFFFF;

	// loop a few times in case we accidentally choose a UPackage (which doesn't have an Outer) to find Outer
	for (int i = 0; i < 0x10; i++)
	{
		int32_t Offset = 0;

		const void* ObjA = ObjectArray::GetByIndex(rand() % 0x400).GetAddress();
		const void* ObjB = ObjectArray::GetByIndex(rand() % 0x400).GetAddress();

		while (Offset != OffsetNotFound)
		{
			Offset = GetValidPointerOffset(ObjA, ObjB, Offset + sizeof(void*), 0x50);

			// Make sure we didn't re-find the Class offset or Index (if the Index filed is a valid pionter for some ungodly reason). 
			if (Offset != Off::UObject::Class && Offset != Off::UObject::Index)
				break;
		}

		if (Offset != OffsetNotFound && Offset < LowestFoundOffset)
			LowestFoundOffset = Offset;
	}

	return LowestFoundOffset == 0xFFFF ? OffsetNotFound : LowestFoundOffset;
}

void OffsetFinder::FixupHardcodedOffsets()
{
	if (Settings::Internal::bUseCasePreservingName)
	{
		Off::FField::Flags += 0x8;

		Off::FFieldClass::Id += 0x08;
		Off::FFieldClass::CastFlags += 0x08;
		Off::FFieldClass::ClassFlags += 0x08;
		Off::FFieldClass::SuperClass += 0x08;
	}

	if (Settings::Internal::bUseFProperty)
	{
		/*
		* On versions below 5.1.1: class FFieldVariant { void*, bool } -> extends to { void*, bool, uint8[0x7] }
		* ON versions since 5.1.1: class FFieldVariant { void* }
		*
		* Check:
		* if FFieldVariant contains a bool, the memory at the bools offset will not be a valid pointer
		* if FFieldVariant doesn't contain a bool, the memory at the bools offset will be the next member of FField, the Next ptr [valid]
		*/

		const int32 OffsetToCheck = Off::FField::Owner + 0x8;
		void* PossibleNextPtrOrBool0 = *(void**)((uint8*)ObjectArray::FindClassFast("Actor").GetChildProperties().GetAddress() + OffsetToCheck);
		void* PossibleNextPtrOrBool1 = *(void**)((uint8*)ObjectArray::FindClassFast("ActorComponent").GetChildProperties().GetAddress() + OffsetToCheck);
		void* PossibleNextPtrOrBool2 = *(void**)((uint8*)ObjectArray::FindClassFast("Pawn").GetChildProperties().GetAddress() + OffsetToCheck);

		auto IsValidPtr = [](void* a) -> bool
		{
			return !Platform::IsBadReadPtr(a) && (uintptr_t(a) & 0x1) == 0; // realistically, there wont be any pointers to unaligned memory
		};

		if (IsValidPtr(PossibleNextPtrOrBool0) && IsValidPtr(PossibleNextPtrOrBool1) && IsValidPtr(PossibleNextPtrOrBool2))
		{
			std::cerr << "Applaying fix to hardcoded offsets \n" << std::endl;

			Settings::Internal::bUseMaskForFieldOwner = true;

			Off::FField::Next -= 0x08;
			Off::FField::Name -= 0x08;
			Off::FField::Flags -= 0x08;
		}
	}
}

void OffsetFinder::InitFNameSettings()
{
	UEObject FirstObject = ObjectArray::GetByIndex(0);

	const uint8* NameAddress = static_cast<const uint8*>(FirstObject.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = *reinterpret_cast<const int32*>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = *reinterpret_cast<const int32*>(NameAddress + 0x4);

	/* Some games move 'Name' before 'Class'. Just substract the offset of 'Name' with the offset of the member that follows right after it, to get an estimate of sizeof(FName). */
	const int32 FNameSize = !Settings::Internal::bIsObjectNameBeforeClass ? (Off::UObject::Outer - Off::UObject::Name) : (Off::UObject::Class - Off::UObject::Name);

	Off::FName::CompIdx = 0x0;
	Off::FName::Number = 0x4; // defaults for check

	 // FNames for which FName::Number == [1...4]
	auto GetNumNamesWithNumberOneToFour = []() -> int32
	{
		int32 NamesWithNumberOneToFour = 0x0;

		for (UEObject Obj : ObjectArray())
		{
			const uint32 Number = Obj.GetFName().GetNumber();

			if (Number > 0x0 && Number < 0x5)
				NamesWithNumberOneToFour++;
		}

		return NamesWithNumberOneToFour;
	};

	/*
	* Games without FNAME_OUTLINE_NUMBER have a min. percentage of 6% of all object-names for which FName::Number is in a [1...4] range
	* On games with FNAME_OUTLINE_NUMBER the (random) integer after FName::ComparisonIndex is in the range from [1...4] about 2% (or less) of times.
	*
	* The minimum percentage of names is set to 3% to give both normal names, as well as outline-numer names a buffer-zone.
	*
	* This doesn't work on some very small UE template games, which is why PostInitFNameSettings() was added to fix the incorrect behavior of this function
	*/
	constexpr float MinPercentage = 0.03f;

	/* Minimum required ammount of names for which FName::Number is in a [1...4] range */
	const int32 FNameNumberThreashold = (ObjectArray::Num() * MinPercentage);

	Off::FName::CompIdx = 0x0;

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize == 0x10) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (GetNumNamesWithNumberOneToFour() < FNameNumberThreashold) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Off::FName::Number = 0x4;

		Off::InSDK::Name::FNameSize = 0x8;
	}
}

void OffsetFinder::PostInitFNameSettings()
{
	const UEClass PlayerStart = ObjectArray::FindClassFast("PlayerStart");

	const int32 FNameSize = PlayerStart.FindMember("PlayerStartTag").GetSize();

	/* Nothing to do for us, everything is fine! */
	if (Off::InSDK::Name::FNameSize == FNameSize)
		return;

	/* We've used the wrong FNameSize to determine the offset of FField::Flags. Substract the old, wrong, size and add the new one.*/
	Off::FField::Flags = (Off::FField::Flags - Off::InSDK::Name::FNameSize) + FNameSize;

	const uint8* NameAddress = static_cast<const uint8*>(PlayerStart.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = *reinterpret_cast<const int32*>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = *reinterpret_cast<const int32*>(NameAddress + 0x4);

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize > 0x8) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (FNameSize == 0x4) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = 0x4;
		Off::InSDK::Name::FNameSize = 0x8;
	}
}

/* UField */
int32_t OffsetFinder::FindUFieldNextOffset()
{
	const void* KismetSystemLibraryChild = ObjectArray::FindObjectFast<UEStruct>("KismetSystemLibrary").GetChild().GetAddress();
	const void* KismetStringLibraryChild = ObjectArray::FindObjectFast<UEStruct>("KismetStringLibrary").GetChild().GetAddress();

#undef max
	const auto HighestUObjectOffset = std::max({ Off::UObject::Index, Off::UObject::Name, Off::UObject::Flags, Off::UObject::Outer, Off::UObject::Class });
#define max(a,b)            (((a) > (b)) ? (a) : (b))

	return GetValidPointerOffset(KismetSystemLibraryChild, KismetStringLibraryChild, Align(HighestUObjectOffset + 0x4, static_cast<int>(sizeof(void*))), 0x60);
}

/* FField */
int32_t OffsetFinder::FindFFieldNextOffset()
{
	const void* GuidChildren = ObjectArray::FindStructFast("Guid").GetChildProperties().GetAddress();
	const void* VectorChildren = ObjectArray::FindStructFast("Vector").GetChildProperties().GetAddress();

	return GetValidPointerOffset(GuidChildren, VectorChildren, Off::FField::Owner + 0x8, 0x48);
}

int32_t OffsetFinder::FindFFieldNameOffset()
{
	UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	UEFField VectorChild = ObjectArray::FindStructFast("Vector").GetChildProperties();

	std::string GuidChildName = GuidChild.GetName();
	std::string VectorChildName = VectorChild.GetName();

	if ((GuidChildName == "A" || GuidChildName == "D") && (VectorChildName == "X" || VectorChildName == "Z"))
		return Off::FField::Name;

	for (Off::FField::Name = Off::FField::Owner; Off::FField::Name < 0x40; Off::FField::Name += 4)
	{
		GuidChildName = GuidChild.GetName();
		VectorChildName = VectorChild.GetName();

		if ((GuidChildName == "A" || GuidChildName == "D") && (VectorChildName == "X" || VectorChildName == "Z"))
			return Off::FField::Name;
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::NewFindFFieldNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
	{
		// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
		return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
			&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
			&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2));
	};

	AllFieldIterator TmpIt;

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, TmpIt.begin(), TmpIt.end());
}

int32_t OffsetFinder::FindFFieldEditorOnlyMetaDataOffset()
{
	const UEFField GuidChild1 = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField GuidChild2 = GuidChild1.GetNext();

	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
		{
			// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
			return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
				&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
				&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2))
				&& Offset != Off::FField::Name && Offset != (Off::FField::Name + Off::InSDK::Name::FNameSize);
		};

	int32 StartingOffset = 0x8;

	// Only pay attention to the 0x8 aligned size-options of FName, since the pair in the TMap is 0x8 aligned because of FString
	struct alignas(0x4) Name08Byte { uint8 Pad[0x08]; };
	struct alignas(0x4) Name16Byte { uint8 Pad[0x10]; };

	static auto AreValidMetadataMaps = []<typename NameType>(const TMap<NameType, FString>* MetadataMap1, const TMap<NameType, FString>* MetadataMap2)
	{
		if (!MetadataMap1->IsValid() || !MetadataMap2->IsValid())
			return false;

		const FString& Value1 = MetadataMap1->operator[](0).Value();
		const FString& Value2 = MetadataMap2->operator[](0).Value();

		return Value1.IsValid() && Value2.IsValid();
	};

	while (true)
	{
		if (!IsPotentiallyValidOffset(StartingOffset))
		{
			StartingOffset += sizeof(void*);
			continue;
		}

		const int32 Offset = GetValidPointerOffset<false>(GuidChild1.GetAddress(), GuidChild2.GetAddress(), StartingOffset, 0x40);
		StartingOffset = Offset + sizeof(void*);

		if (Offset == OffsetNotFound)
			break;

		if (!IsPotentiallyValidOffset(Offset))
			continue;

		const TMap<Name08Byte, FString>* PossibleMetaDataPtr1 = *reinterpret_cast<TMap<Name08Byte, FString>**>(reinterpret_cast<uintptr_t>(GuidChild1.GetAddress()) + Offset);
		const TMap<Name08Byte, FString>* PossibleMetaDataPtr2 = *reinterpret_cast<TMap<Name08Byte, FString>**>(reinterpret_cast<uintptr_t>(GuidChild2.GetAddress()) + Offset);

		if (!PossibleMetaDataPtr1 || !PossibleMetaDataPtr2 || Platform::IsBadReadPtr(PossibleMetaDataPtr1) || Platform::IsBadReadPtr(PossibleMetaDataPtr2))
			continue;

		if (!PossibleMetaDataPtr1->IsValid() || !PossibleMetaDataPtr2->IsValid())
			continue;

		if (PossibleMetaDataPtr1->Num() <= 0 || PossibleMetaDataPtr2->Num() <= 0)
			continue;

		if (PossibleMetaDataPtr1->Num() >= 0x10 || PossibleMetaDataPtr2->Num() >= 0x10)
			continue;

		auto GetDataPtrOfArrayInMap = [](const auto& Map) -> const void*
		{
			// TMap data is stored at offset 0x0, this is a hacky way to get the TArray::Data member of the map
			return *reinterpret_cast<const void* const*>(&Map);
		};

		if (Platform::IsBadReadPtr(GetDataPtrOfArrayInMap(PossibleMetaDataPtr1)) || Platform::IsBadReadPtr(GetDataPtrOfArrayInMap(PossibleMetaDataPtr2)))
			continue;

		if (Off::InSDK::Name::FNameSize <= 0x8)
		{
			if (AreValidMetadataMaps(PossibleMetaDataPtr1, PossibleMetaDataPtr2))
				return Offset;
		}
		else
		{
			if (AreValidMetadataMaps(reinterpret_cast<const TMap<Name16Byte, FString>*>(PossibleMetaDataPtr1), reinterpret_cast<const TMap<Name16Byte, FString>*>(PossibleMetaDataPtr1)))
				return Offset;
		}
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindFFieldClassOffset()
{
	const UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField VectorChild = ObjectArray::FindStructFast("Vector").GetChildProperties();

	return GetValidPointerOffset<false>(GuidChild.GetAddress(), VectorChild.GetAddress(), 0x8, 0x30, true);
}

// This function assumes that the EnumObj passed in is valid and that the values of the enum are starting at 0
void InializeUEnumSettings(const void* EnumObj, const uint32_t UEnumNumValuesOffset)
{
	constexpr uintptr_t UE5EnumDynamicAllocationTag = 0x1;

	{
		// On UE5.6+ there are two arrays, one for just the FName*/UTF8Char* and one for just int64* values. Check if the array before NumValues contains just Values or TPair<Name, Value>.
		const uintptr_t PossibleValueArrayTaggedPtr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(EnumObj) + UEnumNumValuesOffset - sizeof(void*));
		const int64* PossibleValueArrayPtr = reinterpret_cast<const int64*>(PossibleValueArrayTaggedPtr & ~UE5EnumDynamicAllocationTag);

		if (!Platform::IsBadReadPtr(PossibleValueArrayPtr) && !Platform::IsBadReadPtr(PossibleValueArrayPtr + 1) && !Platform::IsBadReadPtr(PossibleValueArrayPtr + 2))
		{
			if (PossibleValueArrayPtr[0] == 0 && PossibleValueArrayPtr[1] == 1 && PossibleValueArrayPtr[2] == 2)
			{
				Settings::Internal::bIsNewUE5EnumNamesContainer = true;
				return;
			}
		}
	}

	using ValueType = std::conditional_t<sizeof(void*) == 0x8, int64, int32>;
	struct Name08Byte { uint8 Pad[0x08]; };
	struct Name16Byte { uint8 Pad[0x10]; };

	const uint8* ArrayAddress = static_cast<const uint8*>(EnumObj) + UEnumNumValuesOffset - 0x8;

	auto InitEnumSettings = []<typename NameType>(const TArray<TPair<NameType, ValueType>>&ArrayOfNameValuePairs)
	{
		if (ArrayOfNameValuePairs[1].Second == 1)
			return;

		if constexpr (Settings::EngineCore::bCheckEnumNamesInUEnum)
		{
			if (static_cast<uint8_t>(ArrayOfNameValuePairs[1].Second) == 1 && static_cast<uint8_t>(ArrayOfNameValuePairs[2].Second) == 2)
			{

				Settings::Internal::bIsSmallEnumValue = true;
				return;
			}
		}

		Settings::Internal::bIsEnumNameOnly = true;
	};


	if (Settings::Internal::bUseCasePreservingName)
	{
		InitEnumSettings(*reinterpret_cast<const TArray<TPair<Name16Byte, ValueType>>*>(ArrayAddress));
	}
	else
	{
		InitEnumSettings(*reinterpret_cast<const TArray<TPair<Name08Byte, ValueType>>*>(ArrayAddress));
	}
}

/* FFieldClass */
int32_t OffsetFinder::FindFieldClassCastFlagsOffset()
{
	std::vector<std::pair<void*, EClassCastFlags>> Infos;

	const UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField ColourChild = ObjectArray::FindStructFast("Color").GetChildProperties();

	Infos.push_back({ GuidChild.GetClass().GetAddress(),   EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::IntProperty  });
	Infos.push_back({ ColourChild.GetClass().GetAddress(), EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::ByteProperty });

	const int32_t Offset = FindOffset(Infos, sizeof(void*), 0x30);

	return Offset != OffsetNotFound ? Offset : 0x10;
}

/* UEnum */
int32_t OffsetFinder::FindEnumNamesOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("ENetRole", EClassCastFlags::Enum).GetAddress(), 0x5 });
	Infos.push_back({ ObjectArray::FindObjectFast("ETraceTypeQuery", EClassCastFlags::Enum).GetAddress(), 0x22 });

	int UEnumNumValuesOffset = FindOffset(Infos);

	if (UEnumNumValuesOffset == OffsetNotFound)
	{
		Infos[0] = { ObjectArray::FindObjectFast("EAlphaBlendOption", EClassCastFlags::Enum).GetAddress(), 0x10 };
		Infos[1] = { ObjectArray::FindObjectFast("EUpdateRateShiftBucket", EClassCastFlags::Enum).GetAddress(), 0x8 };

		UEnumNumValuesOffset = FindOffset(Infos);
	}

	InializeUEnumSettings(Infos[0].first, UEnumNumValuesOffset);

	return UEnumNumValuesOffset - sizeof(void*);
}

/* UStruct */
int32_t OffsetFinder::FindSuperOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Struct").GetAddress(), ObjectArray::FindObjectFast("Field").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), ObjectArray::FindObjectFast("Struct").GetAddress() });

	// Thanks to the ue4 dev who decided UStruct should be spelled Ustruct
	if (Infos[0].first == nullptr)
		Infos[0].first = Infos[1].second = ObjectArray::FindObjectFast("struct").GetAddress();

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	if (ObjectArray::FindObject("ObjectProperty Engine.Controller.TransformComponent", EClassCastFlags::ObjectProperty))
	{
		Infos.push_back({ ObjectArray::FindObjectFast("Vector").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector4").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector4").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector2D").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector2D").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), ObjectArray::FindObjectFastInOuter("A","Guid").GetAddress() });

		return FindOffset(Infos, 0x14);
	}

	Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), ObjectArray::FindObjectFastInOuter("WasInputKeyJustReleased", "PlayerController").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Controller").GetAddress(), ObjectArray::FindObjectFastInOuter("UnPossess", "Controller").GetAddress() });

	Settings::Internal::bUseFProperty = true;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildPropertiesOffset()
{
	const void* ObjA = ObjectArray::FindStructFast("Color").GetAddress();
	const void* ObjB = ObjectArray::FindStructFast("Guid").GetAddress();

	return GetValidPointerOffset(ObjA, ObjB, Off::UStruct::Children + 0x08, 0x80);
}

int32_t OffsetFinder::FindStructSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Color").GetAddress(), 0x04 });
	Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), 0x10 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindMinAlignmentOffset()
{
	std::vector<std::pair<void*, int16_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Transform").GetAddress(), 0x10 });

	if constexpr (Platform::Is32Bit())
	{
		Infos.push_back({ ObjectArray::FindObjectFast("InterpCurveLinearColor").GetAddress(), 0x04 });
	}
	else
	{
		Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), 0x8 });
	}

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindStructBaseChainOffset()
{
	// UStruct inherits from FStructBaseChain, so the members of base chain should come right after UField

	UEStruct Struct = ObjectArray::FindStructFast("Struct");
	if (!Struct)
		Struct = ObjectArray::FindStructFast("struct");

	const int32 UStructStart = Struct.GetSuper().GetStructSize();
	const int32 UStructEnd = UStructStart + Struct.GetStructSize();

	// If the members of UStruct come right after UField, FStructBaseChain either doesn't exist or is empty
	if (UStructStart == Off::UStruct::ChildProperties || UStructStart == Off::UStruct::Children)
		return OffsetNotFound;

	auto CountSuperClasses = [](const UEStruct InStruct) -> int32
	{
		int32 Count = 0;

		UEStruct CurrentSuper = InStruct.GetSuper();
		while (CurrentSuper)
		{
			Count++;
			CurrentSuper = CurrentSuper.GetSuper();
		}

		return Count;
	};

	/* Pair<UStruct, NumSuperClasses> */
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct APlayerController = ObjectArray::FindClassFast("PlayerController");
	UEStruct AActor = ObjectArray::FindClassFast("Actor");

	Infos.push_back({ Struct.GetAddress(),              CountSuperClasses(Struct)            });
	Infos.push_back({ APlayerController.GetAddress(),   CountSuperClasses(APlayerController) });
	Infos.push_back({ AActor.GetAddress(),              CountSuperClasses(AActor)            });

	// FStructBaseChain::NumStructBasesInChainMinusOne is at offset 0x8, after a pointer
	return FindOffset(Infos, UStructStart, UStructEnd) - sizeof(void*);
}

/* UFunction */
int32_t OffsetFinder::FindFunctionFlagsOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Final | EFunctionFlags::Native | EFunctionFlags::Public | EFunctionFlags::BlueprintCallable | EFunctionFlags::BlueprintPure | EFunctionFlags::Const });
	Infos.push_back({ ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });
	Infos.push_back({ ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (Infos[2].first == nullptr)
		Infos[2].first = ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress();

	const int32 Ret = FindOffset(Infos);

	if (Ret != OffsetNotFound)
		return Ret;

	for (auto& [_, Flags] : Infos)
		Flags |= EFunctionFlags::RequiredAPI;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindFunctionNativeFuncOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	uintptr_t WasInputKeyJustPressed = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress());
	uintptr_t ToggleSpeaking = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress());
	uintptr_t SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress());

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (SwitchLevel_Or_FOV == NULL)
		SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress());

	for (int i = 0x30; i < 0x140; i += sizeof(void*))
	{
		if (Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(WasInputKeyJustPressed + i)) &&
			Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(ToggleSpeaking + i)) && Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(SwitchLevel_Or_FOV + i)))
			return i;
	}

	return 0x0;
}

int32_t OffsetFinder::FindFunctionScriptOffset()
{
	constexpr uint8 ScriptEndToken = 0x53; // EExprToken::EX_EndOfScript

	auto BuildRuntimeCacheKey = []() -> uint64
	{
		uint64 key = 1469598103934665603ull; // FNV1a 64 offset basis
		auto HashBytes = [&key](const uint8* ptr, size_t size)
		{
			for (size_t i = 0; i < size; i++)
			{
				key ^= ptr[i];
				key *= 1099511628211ull;
			}
		};

		auto HashU64 = [&HashBytes](uint64 value)
		{
			HashBytes(reinterpret_cast<const uint8*>(&value), sizeof(value));
		};

		HMODULE exeModule = GetModuleHandleW(nullptr);
		if (!exeModule)
			return 0;

		const uint8* base = reinterpret_cast<const uint8*>(exeModule);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
			return 0;

		HashU64(reinterpret_cast<uint64>(exeModule));
		HashU64(static_cast<uint64>(nt->FileHeader.TimeDateStamp));
		HashU64(static_cast<uint64>(nt->OptionalHeader.CheckSum));
		HashU64(static_cast<uint64>(nt->OptionalHeader.SizeOfImage));
		HashU64(static_cast<uint64>(nt->OptionalHeader.SizeOfCode));
		HashBytes(reinterpret_cast<const uint8*>(Settings::Generator::GameVersion.data()), Settings::Generator::GameVersion.size());

		return key;
	};

	static std::unordered_map<uint64, int32> sRuntimeCache;
	static std::unordered_map<uint64, ScriptOffsetDiagnostics> sRuntimeDiagCache;
	const uint64 runtimeCacheKey = BuildRuntimeCacheKey();
	if (runtimeCacheKey != 0)
	{
		auto it = sRuntimeCache.find(runtimeCacheKey);
		if (it != sRuntimeCache.end())
		{
			std::cerr << "[UExplorer] Script offset cache hit: 0x" << std::hex << it->second << std::dec << std::endl;
			ScriptOffsetDiagnostics diag;
			auto diagIt = sRuntimeDiagCache.find(runtimeCacheKey);
			if (diagIt != sRuntimeDiagCache.end())
			{
				diag = diagIt->second;
			}
			else
			{
				diag.SelectedOffset = it->second;
				diag.Confidence = "medium";
				diag.AnomalyTags = "cache_hit_without_diag";
			}
			diag.FromCache = true;
			diag.CacheKey = runtimeCacheKey;
			SetScriptOffsetDiagnostics(diag);
			return it->second;
		}
	}

	auto AddUniqueAddress = [](std::vector<uintptr_t>& list, const UEObject& obj)
	{
		if (!obj)
			return;

		const uintptr_t addr = reinterpret_cast<uintptr_t>(obj.GetAddress());
		if (addr == 0)
			return;

		if (std::find(list.begin(), list.end(), addr) == list.end())
			list.push_back(addr);
	};

	struct BlueprintSample
	{
		uintptr_t Address = 0;
		int32 Weight = 1;
		uint8 Tier = 0xFF; // lower tier means higher confidence
	};

	std::vector<BlueprintSample> BlueprintSamples;
	std::vector<uintptr_t> VerificationPool;
	std::vector<uintptr_t> NativeFuncs;
	std::vector<uintptr_t> GeneralFuncs;

	auto AddOrUpdateBlueprintSample = [&BlueprintSamples](uintptr_t address, int32 weight, uint8 tier)
	{
		if (address == 0)
			return;

		for (BlueprintSample& sample : BlueprintSamples)
		{
			if (sample.Address != address)
				continue;

			if (weight > sample.Weight)
				sample.Weight = weight;
			if (tier < sample.Tier)
				sample.Tier = tier;
			return;
		}

		BlueprintSamples.push_back(BlueprintSample{ address, weight, tier });
	};

	auto ClassifyBlueprintSample = [](UEFunction func, const std::string& name, int32& outWeight, uint8& outTier) -> bool
	{
		if (func.HasFlags(EFunctionFlags::Native))
			return false;

		if (name.rfind("ExecuteUbergraph_", 0) == 0)
		{
			outWeight = 6;
			outTier = 0;
			return true;
		}

		if (name.rfind("BndEvt__", 0) == 0)
		{
			outWeight = 5;
			outTier = 1;
			return true;
		}

		if (func.HasFlags(EFunctionFlags::UbergraphFunction))
		{
			outWeight = 5;
			outTier = 1;
			return true;
		}

		if (func.HasFlags(EFunctionFlags::BlueprintEvent))
		{
			outWeight = 3;
			outTier = 2;
			return true;
		}

		if (name.rfind("Receive", 0) == 0)
		{
			outWeight = 2;
			outTier = 3;
			return true;
		}

		return false;
	};

	for (const char* name : { "ReceiveBeginPlay", "ReceiveTick", "ReceiveEndPlay", "ReceiveAnyDamage" })
	{
		UEObject obj = ObjectArray::FindObjectFast(name, EClassCastFlags::Function);
		UEFunction func = obj.Cast<UEFunction>();
		if (func)
		{
			int32 weight = 0;
			uint8 tier = 0xFF;
			if (ClassifyBlueprintSample(func, func.GetName(), weight, tier))
			{
				AddOrUpdateBlueprintSample(reinterpret_cast<uintptr_t>(obj.GetAddress()), weight, tier);
			}
		}
	}

	for (const char* name : { "WasInputKeyJustPressed", "ToggleSpeaking", "SwitchLevel", "FOV", "BeginPlay", "Tick" })
		AddUniqueAddress(NativeFuncs, ObjectArray::FindObjectFast(name, EClassCastFlags::Function));

	int32 highConfidenceCount = 0;

	for (UEObject obj : ObjectArray())
	{
		if (!obj || !obj.IsA(EClassCastFlags::Function))
			continue;

		AddUniqueAddress(GeneralFuncs, obj);

		UEFunction func = obj.Cast<UEFunction>();
		if (!func)
			continue;

		const bool isNative = func.HasFlags(EFunctionFlags::Native);
		if (!isNative)
		{
			const std::string funcName = func.GetName();
			int32 weight = 0;
			uint8 tier = 0xFF;
			if (ClassifyBlueprintSample(func, funcName, weight, tier))
			{
				AddOrUpdateBlueprintSample(reinterpret_cast<uintptr_t>(obj.GetAddress()), weight, tier);
				if (tier <= 1)
					highConfidenceCount++;
			}
		}
		else if (NativeFuncs.size() < 24)
		{
			AddUniqueAddress(NativeFuncs, obj);
		}

		if (highConfidenceCount >= 48 && NativeFuncs.size() >= 24 && GeneralFuncs.size() >= 320 && BlueprintSamples.size() >= 80)
			break;
	}

	if (!BlueprintSamples.empty())
	{
		std::sort(BlueprintSamples.begin(), BlueprintSamples.end(), [](const BlueprintSample& a, const BlueprintSample& b)
		{
			if (a.Tier != b.Tier) return a.Tier < b.Tier;
			if (a.Weight != b.Weight) return a.Weight > b.Weight;
			return a.Address < b.Address;
		});

		const size_t maxScoringSamples = 56;
		const size_t maxVerificationSamples = 192;
		VerificationPool.clear();
		VerificationPool.reserve((std::min)(maxVerificationSamples, BlueprintSamples.size()));
		for (size_t i = 0; i < BlueprintSamples.size() && i < maxVerificationSamples; i++)
		{
			VerificationPool.push_back(BlueprintSamples[i].Address);
		}

		if (BlueprintSamples.size() > maxScoringSamples)
			BlueprintSamples.resize(maxScoringSamples);
	}

	if (GeneralFuncs.empty())
	{
		std::cerr << "Dumper-7 WARNING: Could not gather function samples for Script offset discovery." << std::endl;
		ScriptOffsetDiagnostics diag;
		diag.SelectedOffset = OffsetNotFound;
		diag.Confidence = "low";
		diag.AnomalyTags = "no_function_samples";
		diag.CacheKey = runtimeCacheKey;
		SetScriptOffsetDiagnostics(diag);
		return OffsetNotFound;
	}

	if (BlueprintSamples.empty())
	{
		std::cerr << "Dumper-7 WARNING: Could not gather Blueprint functions; using generic fallback scan." << std::endl;
	}

	if (VerificationPool.empty())
	{
		for (const BlueprintSample& sample : BlueprintSamples)
		{
			if (sample.Address != 0)
				VerificationPool.push_back(sample.Address);
		}
	}

	auto TryReadArrayHeader = [](const uintptr_t funcAddr, const int32 offset, uintptr_t& outData, int32& outNum, int32& outMax) -> bool
	{
		if (funcAddr == 0 || offset <= 0)
			return false;

		const uintptr_t base = funcAddr + static_cast<uintptr_t>(offset);
		const uintptr_t numPtr = base + sizeof(void*);
		const uintptr_t maxPtr = numPtr + sizeof(int32);

		if (Platform::IsBadReadPtr(reinterpret_cast<void*>(base))
			|| Platform::IsBadReadPtr(reinterpret_cast<void*>(numPtr))
			|| Platform::IsBadReadPtr(reinterpret_cast<void*>(maxPtr)))
		{
			return false;
		}

		const uintptr_t data = *reinterpret_cast<const uintptr_t*>(base);
		const int32 num = *reinterpret_cast<const int32*>(numPtr);
		const int32 max = *reinterpret_cast<const int32*>(maxPtr);

		if (data == 0 || num <= 0 || num > 0x200000 || max < num || max > 0x400000)
			return false;

		if (Platform::IsBadReadPtr(reinterpret_cast<void*>(data)))
			return false;

		outData = data;
		outNum = num;
		outMax = max;
		return true;
	};

	auto TryReadScriptArray = [ScriptEndToken, &TryReadArrayHeader](const uintptr_t funcAddr, const int32 offset, uintptr_t& outData, int32& outNum, int32& outMax, uint8& outFirst) -> bool
	{
		if (!TryReadArrayHeader(funcAddr, offset, outData, outNum, outMax))
			return false;

		const uintptr_t tail = outData + static_cast<uintptr_t>(outNum - 1);
		if (tail < outData)
			return false;

		uint8 first = 0;
		uint8 last = 0;
		if (!TryReadU8Safe(outData, first) || !TryReadU8Safe(tail, last))
			return false;

		if (last != ScriptEndToken)
			return false;

		outFirst = first;
		return true;
	};

	auto TryReadNumOnly = [](const uintptr_t funcAddr, const int32 offset, int32& outNum) -> bool
	{
		if (funcAddr == 0 || offset <= 0)
			return false;

		const uintptr_t numPtr = funcAddr + static_cast<uintptr_t>(offset) + sizeof(void*);
		if (Platform::IsBadReadPtr(reinterpret_cast<void*>(numPtr)))
			return false;

		outNum = *reinterpret_cast<const int32*>(numPtr);
		return true;
	};

	const int32 StartOffset = 0x30;
	int32 EndOffset = Off::UFunction::ExecFunction + 0x40;
	if (EndOffset < 0x180) EndOffset = 0x180;
	if (EndOffset > 0x280) EndOffset = 0x280;

	struct CandidateScore
	{
		int32 Offset = OffsetNotFound;
		int32 Score = INT32_MIN;

		int32 BpTotal = 0;
		int32 BpWeightedTotal = 0;
		int32 BpHeaderValid = 0;
		int32 BpHeaderInvalid = 0;
		int32 BpEndTokenHits = 0;
		int32 BpWeightedEndTokenHits = 0;
		int32 BpTailMismatch = 0;
		int32 BpFirstOpcodeInRange = 0;
		int32 BpSmallScript = 0;
		int32 BpStableReread = 0;

		int32 NativeReadable = 0;
		int32 NativeZeroNum = 0;
		int32 NativeNonZeroNum = 0;
		int32 NativeLooksLikeScript = 0;

		int32 GenericProbed = 0;
		int32 GenericScriptHits = 0;
		int32 GenericZeroHits = 0;
		int32 GenericNoisyHits = 0;

		std::string Reason;

		bool HasBpEvidence() const
		{
			return BpEndTokenHits >= 4 || BpWeightedEndTokenHits >= 16;
		}
	};

	auto AddReason = [](std::string& reason, int32 delta, const std::string& why, int32 count)
	{
		if (!reason.empty())
			reason += "; ";

		reason += std::format("{}{}({}x {})",
			(delta >= 0 ? "+" : ""),
			delta,
			count,
			why);
	};

	std::cerr << "[UExplorer] Script offset discovery: BP samples=" << BlueprintSamples.size()
		<< " (high-confidence-collected=" << highConfidenceCount << ")"
		<< " Native samples=" << NativeFuncs.size() << std::endl;
	std::cerr << "[UExplorer] Script offset scan range: 0x" << std::hex << StartOffset
		<< " - 0x" << EndOffset << std::dec << std::endl;

	int32 bestOffset = OffsetNotFound;
	int32 bestScore = INT32_MIN;
	int32 bestBpEndTokenHits = 0;
	int32 bestBpWeightedEndTokenHits = 0;
	int32 bestGenericScriptHits = 0;
	bool bestHasBpEvidence = false;
	std::vector<CandidateScore> candidates;
	candidates.reserve((EndOffset - StartOffset) / static_cast<int32>(sizeof(void*)));

	for (int32 offset = StartOffset; offset < EndOffset; offset += sizeof(void*))
	{
		CandidateScore c;
		c.Offset = offset;
		c.Score = 0;
		c.BpTotal = static_cast<int32>(BlueprintSamples.size());

		for (const BlueprintSample& sample : BlueprintSamples)
		{
			const uintptr_t bpAddr = sample.Address;
			const int32 sampleWeight = sample.Weight;
			c.BpWeightedTotal += sampleWeight;

			uintptr_t data = 0;
			int32 num = 0;
			int32 max = 0;
			if (!TryReadArrayHeader(bpAddr, offset, data, num, max))
			{
				c.BpHeaderInvalid++;
				continue;
			}

			c.BpHeaderValid++;

			const uintptr_t tail = data + static_cast<uintptr_t>(num - 1);
			if (tail < data)
			{
				c.BpTailMismatch++;
				continue;
			}

			uint8 first = 0;
			uint8 last = 0;
			if (!TryReadU8Safe(data, first) || !TryReadU8Safe(tail, last))
			{
				c.BpTailMismatch++;
				continue;
			}

			if (last == ScriptEndToken)
			{
				c.BpEndTokenHits++;
				c.BpWeightedEndTokenHits += sampleWeight;
			}
			else
				c.BpTailMismatch++;

			if (first <= 0x6D)
				c.BpFirstOpcodeInRange++;

			if (num <= 0x8000)
				c.BpSmallScript++;

			uintptr_t data2 = 0;
			int32 num2 = 0;
			int32 max2 = 0;
			if (TryReadArrayHeader(bpAddr, offset, data2, num2, max2))
			{
				const uintptr_t tail2 = data2 + static_cast<uintptr_t>(num2 - 1);
				if (tail2 >= data2)
				{
					uint8 first2 = 0;
					uint8 last2 = 0;
					if (TryReadU8Safe(data2, first2) && TryReadU8Safe(tail2, last2)
						&& data2 == data && num2 == num && max2 == max
						&& first2 == first && last2 == last)
					{
						c.BpStableReread++;
					}
				}
			}
		}

		{
			const int32 deltaEnd = c.BpWeightedEndTokenHits * 6;
			c.Score += deltaEnd;
			AddReason(c.Reason, deltaEnd, "BP weighted end-token hits", c.BpWeightedEndTokenHits);

			const int32 deltaFirst = c.BpFirstOpcodeInRange * 3;
			c.Score += deltaFirst;
			AddReason(c.Reason, deltaFirst, "BP first-opcode in range", c.BpFirstOpcodeInRange);

			const int32 deltaSmall = c.BpSmallScript * 2;
			c.Score += deltaSmall;
			AddReason(c.Reason, deltaSmall, "BP script-size sanity", c.BpSmallScript);

			const int32 deltaStable = c.BpStableReread * 3;
			c.Score += deltaStable;
			AddReason(c.Reason, deltaStable, "BP stable re-read", c.BpStableReread);

			const int32 deltaTailMismatch = -(c.BpTailMismatch * 14);
			c.Score += deltaTailMismatch;
			AddReason(c.Reason, deltaTailMismatch, "BP tail mismatch", c.BpTailMismatch);

			const int32 deltaHeaderInvalid = -(c.BpHeaderInvalid * 6);
			c.Score += deltaHeaderInvalid;
			AddReason(c.Reason, deltaHeaderInvalid, "BP invalid array header", c.BpHeaderInvalid);

			if (!BlueprintSamples.empty() && c.BpEndTokenHits == 0)
			{
				c.Score -= 500;
				AddReason(c.Reason, -500, "no Blueprint script endings (hard confidence gate)", 1);
			}

			if (!BlueprintSamples.empty() && c.BpHeaderValid == 0)
			{
				c.Score -= 180;
				AddReason(c.Reason, -180, "no readable Blueprint headers", 1);
			}

			const int32 bpCheckedSafe = std::max<int32>(1, c.BpTotal);
			const int32 bpValidRate = (c.BpHeaderValid * 100) / bpCheckedSafe;
			if (bpValidRate < 25)
			{
				c.Score -= 80;
				AddReason(c.Reason, -80, "Blueprint header valid-rate too low%", bpValidRate);
			}
		}

		{
			const size_t totalFuncs = GeneralFuncs.size();
			const size_t targetProbes = 1024;
			const size_t stride = std::max<size_t>(1, totalFuncs / targetProbes);

			for (size_t idx = 0; idx < totalFuncs; idx += stride)
			{
				const uintptr_t funcAddr = GeneralFuncs[idx];
				int32 numOnly = 0;
				if (!TryReadNumOnly(funcAddr, offset, numOnly))
					continue;

				c.GenericProbed++;
				if (numOnly == 0)
				{
					c.GenericZeroHits++;
					continue;
				}

				uintptr_t data = 0;
				int32 num = 0;
				int32 max = 0;
				if (!TryReadArrayHeader(funcAddr, offset, data, num, max))
				{
					c.GenericNoisyHits++;
					continue;
				}

				const uintptr_t tail = data + static_cast<uintptr_t>(num - 1);
				uint8 last = 0;
				if (tail < data || !TryReadU8Safe(tail, last))
				{
					c.GenericNoisyHits++;
					continue;
				}

				if (last == ScriptEndToken)
					c.GenericScriptHits++;
				else
					c.GenericNoisyHits++;
			}
		}

		{
			const int32 genericProbeSafe = std::max<int32>(1, c.GenericProbed);
			const int32 genericZeroPct = (c.GenericZeroHits * 100) / genericProbeSafe;
			const int32 genericNoisyPct = (c.GenericNoisyHits * 100) / genericProbeSafe;
			const int32 genericScriptPct = (c.GenericScriptHits * 100) / genericProbeSafe;

			const int32 deltaGenericScript = (std::min)(160, c.GenericScriptHits * 6 + genericScriptPct * 2);
			c.Score += deltaGenericScript;
			AddReason(c.Reason, deltaGenericScript, "generic script-likeness", c.GenericScriptHits);

			const int32 deltaGenericZero = (std::min)(10, genericZeroPct / 10);
			c.Score += deltaGenericZero;
			AddReason(c.Reason, deltaGenericZero, "generic native-zero ratio%", genericZeroPct);

			const int32 deltaGenericNoisy = -(std::min)(80, genericNoisyPct);
			c.Score += deltaGenericNoisy;
			AddReason(c.Reason, deltaGenericNoisy, "generic noisy ratio%", genericNoisyPct);

			if (c.GenericScriptHits == 0)
			{
				c.Score -= 20;
				AddReason(c.Reason, -20, "no generic script hits", 1);
			}
		}

		for (const uintptr_t nativeAddr : NativeFuncs)
		{
			int32 nativeNum = 0;
			if (!TryReadNumOnly(nativeAddr, offset, nativeNum))
				continue;

			c.NativeReadable++;
			if (nativeNum == 0)
			{
				c.NativeZeroNum++;
			}
			else if (nativeNum > 0 && nativeNum < 0x200000)
			{
				c.NativeNonZeroNum++;

				uintptr_t data = 0;
				int32 num = 0;
				int32 max = 0;
				if (TryReadArrayHeader(nativeAddr, offset, data, num, max))
				{
					const uintptr_t tail = data + static_cast<uintptr_t>(num - 1);
					uint8 last = 0;
					if (tail >= data && TryReadU8Safe(tail, last) && last == ScriptEndToken)
						c.NativeLooksLikeScript++;
				}
			}
		}

		{
			const int32 deltaNativeZero = c.NativeZeroNum * 3;
			c.Score += deltaNativeZero;
			AddReason(c.Reason, deltaNativeZero, "native Num==0", c.NativeZeroNum);

			const int32 deltaNativeNonZero = -(c.NativeNonZeroNum * 5);
			c.Score += deltaNativeNonZero;
			AddReason(c.Reason, deltaNativeNonZero, "native Num unexpectedly non-zero", c.NativeNonZeroNum);

			const int32 deltaNativeLooksScript = -(c.NativeLooksLikeScript * 10);
			c.Score += deltaNativeLooksScript;
			AddReason(c.Reason, deltaNativeLooksScript, "native functions looked like script arrays", c.NativeLooksLikeScript);

			if (c.NativeReadable > 0 && c.NativeZeroNum == 0)
			{
				c.Score -= 35;
				AddReason(c.Reason, -35, "none of readable native samples had Num==0", 1);
			}
		}

		std::cerr << "[UExplorer]   candidate 0x" << std::hex << offset << std::dec
			<< " score=" << c.Score
			<< " | BP(end=" << c.BpEndTokenHits
			<< ", wEnd=" << c.BpWeightedEndTokenHits
			<< ", valid=" << c.BpHeaderValid
			<< ", invalid=" << c.BpHeaderInvalid
			<< ", tailMismatch=" << c.BpTailMismatch
			<< ", stable=" << c.BpStableReread
			<< ") Native(zero=" << c.NativeZeroNum << "/" << c.NativeReadable
			<< ", nonZero=" << c.NativeNonZeroNum
			<< ", scriptLike=" << c.NativeLooksLikeScript
			<< ") Generic(script=" << c.GenericScriptHits
			<< ", zero=" << c.GenericZeroHits
			<< ", noisy=" << c.GenericNoisyHits
			<< ", probed=" << c.GenericProbed
			<< ") | reason: " << c.Reason << std::endl;

		const bool cHasBpEvidence = c.HasBpEvidence();
		const bool better = (cHasBpEvidence && !bestHasBpEvidence)
			|| (cHasBpEvidence == bestHasBpEvidence && c.Score > bestScore)
			|| (cHasBpEvidence == bestHasBpEvidence && c.Score == bestScore && c.BpWeightedEndTokenHits > bestBpWeightedEndTokenHits)
			|| (cHasBpEvidence == bestHasBpEvidence && c.Score == bestScore && c.BpWeightedEndTokenHits == bestBpWeightedEndTokenHits && c.BpEndTokenHits > bestBpEndTokenHits)
			|| (cHasBpEvidence == bestHasBpEvidence && c.Score == bestScore && c.BpWeightedEndTokenHits == bestBpWeightedEndTokenHits && c.BpEndTokenHits == bestBpEndTokenHits && c.GenericScriptHits > bestGenericScriptHits)
			|| (cHasBpEvidence == bestHasBpEvidence && c.Score == bestScore && c.BpWeightedEndTokenHits == bestBpWeightedEndTokenHits && c.BpEndTokenHits == bestBpEndTokenHits && c.GenericScriptHits == bestGenericScriptHits
				&& (bestOffset == OffsetNotFound || offset < bestOffset));

		if (better)
		{
			bestScore = c.Score;
			bestOffset = offset;
			bestBpEndTokenHits = c.BpEndTokenHits;
			bestBpWeightedEndTokenHits = c.BpWeightedEndTokenHits;
			bestGenericScriptHits = c.GenericScriptHits;
			bestHasBpEvidence = cHasBpEvidence;
		}

		candidates.emplace_back(std::move(c));
	}

	if (bestOffset != OffsetNotFound)
	{
		std::vector<CandidateScore> sorted = candidates;
		std::sort(sorted.begin(), sorted.end(), [](const CandidateScore& a, const CandidateScore& b)
		{
			if (a.Score != b.Score) return a.Score > b.Score;
			if (a.BpEndTokenHits != b.BpEndTokenHits) return a.BpEndTokenHits > b.BpEndTokenHits;
			if (a.GenericScriptHits != b.GenericScriptHits) return a.GenericScriptHits > b.GenericScriptHits;
			return a.Offset < b.Offset;
		});

		std::cerr << "[UExplorer] Script offset ranking (top 8):" << std::endl;
		for (size_t i = 0; i < std::min<size_t>(8, sorted.size()); i++)
		{
			const CandidateScore& c = sorted[i];
			std::cerr << "  #" << (i + 1)
				<< " off=0x" << std::hex << c.Offset << std::dec
				<< " score=" << c.Score
				<< " bpEnd=" << c.BpEndTokenHits
				<< " wBpEnd=" << c.BpWeightedEndTokenHits
				<< " nativeZero=" << c.NativeZeroNum << "/" << c.NativeReadable
				<< " genericScript=" << c.GenericScriptHits
				<< " reason={" << c.Reason << "}" << std::endl;
		}

		struct VerificationResult
		{
			int32 Probed = 0;
			int32 HeaderValid = 0;
			int32 HeaderInvalid = 0;
			int32 EndHits = 0;
			int32 FirstOpcodeValid = 0;
			int32 SizeSane = 0;
		};

		auto RunVerification = [&](int32 offsetToVerify, size_t maxSamples) -> VerificationResult
		{
			VerificationResult vr;
			if (offsetToVerify == OffsetNotFound)
				return vr;

			const size_t limit = (std::min)(maxSamples, VerificationPool.size());
			for (size_t i = 0; i < limit; i++)
			{
				const uintptr_t addr = VerificationPool[i];
				uintptr_t data = 0;
				int32 num = 0;
				int32 max = 0;

				vr.Probed++;
				if (!TryReadArrayHeader(addr, offsetToVerify, data, num, max))
				{
					vr.HeaderInvalid++;
					continue;
				}

				vr.HeaderValid++;

				const uintptr_t tail = data + static_cast<uintptr_t>(num - 1);
				if (tail < data)
					continue;

				uint8 first = 0;
				uint8 last = 0;
				if (!TryReadU8Safe(data, first) || !TryReadU8Safe(tail, last))
					continue;

				if (last == ScriptEndToken)
				{
					vr.EndHits++;
					if (first <= 0x6D) vr.FirstOpcodeValid++;
					if (num <= 0x8000) vr.SizeSane++;
				}
			}

			return vr;
		};

		int32 selectedOffset = sorted.front().Offset;
		const int32 scoreGapTop2 = sorted.size() >= 2 ? (sorted[0].Score - sorted[1].Score) : INT32_MAX;

		VerificationResult bestVerification = RunVerification(sorted[0].Offset, 120);
		VerificationResult secondVerification{};
		bool bHasSecondVerification = false;

		if (sorted.size() >= 2 && (scoreGapTop2 < 180 || bestVerification.EndHits < 8))
		{
			secondVerification = RunVerification(sorted[1].Offset, 120);
			bHasSecondVerification = true;

			const bool bSecondClearlyBetter = (secondVerification.EndHits >= (bestVerification.EndHits + 4))
				&& (secondVerification.HeaderValid >= (bestVerification.HeaderValid - 2));

			if (bSecondClearlyBetter)
			{
				selectedOffset = sorted[1].Offset;
				bestVerification = secondVerification;
			}
		}

		std::cerr << "[UExplorer] Script offset verify: selected=0x" << std::hex << selectedOffset << std::dec
			<< " probed=" << bestVerification.Probed
			<< " headerValid=" << bestVerification.HeaderValid
			<< " endHits=" << bestVerification.EndHits
			<< " firstOpcodeValid=" << bestVerification.FirstOpcodeValid
			<< " sizeSane=" << bestVerification.SizeSane
			<< " scoreGapTop2=" << scoreGapTop2
			<< std::endl;

		if (bHasSecondVerification)
		{
			std::cerr << "[UExplorer] Script offset verify(second): off=0x" << std::hex << sorted[1].Offset << std::dec
				<< " probed=" << secondVerification.Probed
				<< " headerValid=" << secondVerification.HeaderValid
				<< " endHits=" << secondVerification.EndHits
				<< " firstOpcodeValid=" << secondVerification.FirstOpcodeValid
				<< " sizeSane=" << secondVerification.SizeSane
				<< std::endl;
		}

		const int32 verifySafe = std::max<int32>(1, bestVerification.HeaderValid);
		const int32 verifyEndRate = (bestVerification.EndHits * 100) / verifySafe;
		const int32 verifyOpcodeRate = (bestVerification.FirstOpcodeValid * 100) / std::max<int32>(1, bestVerification.EndHits);

		std::string confidence = "low";
		if (verifyEndRate >= 55 && scoreGapTop2 >= 180)
			confidence = "high";
		else if (verifyEndRate >= 20)
			confidence = "medium";

		std::vector<std::string> anomalyTags;
		if (!sorted.front().HasBpEvidence()) anomalyTags.emplace_back("no_bp_end_evidence");
		if (scoreGapTop2 < 120) anomalyTags.emplace_back("narrow_margin");
		if (bestVerification.HeaderValid == 0) anomalyTags.emplace_back("header_read_fail");
		if (verifyEndRate < 10) anomalyTags.emplace_back("possible_custom_layout_or_encrypted_script");
		if (confidence == "low") anomalyTags.emplace_back("low_confidence");

		std::cerr << "[UExplorer] Script offset confidence: " << confidence
			<< " verifyEndRate=" << verifyEndRate << "%"
			<< " verifyOpcodeRate=" << verifyOpcodeRate << "%"
			<< std::endl;

		if (!anomalyTags.empty())
		{
			std::cerr << "[UExplorer] Script offset anomaly tags: ";
			for (size_t i = 0; i < anomalyTags.size(); i++)
			{
				if (i > 0) std::cerr << ',';
				std::cerr << anomalyTags[i];
			}
			std::cerr << std::endl;
		}

		const CandidateScore* selectedCandidate = nullptr;
		for (const CandidateScore& c : sorted)
		{
			if (c.Offset == selectedOffset)
			{
				selectedCandidate = &c;
				break;
			}
		}

		if (!selectedCandidate)
			selectedCandidate = &sorted.front();

		std::cerr << "[UExplorer] Script offset selected: 0x" << std::hex << selectedOffset
			<< std::dec << " score=" << selectedCandidate->Score
			<< " bpEndHits=" << selectedCandidate->BpEndTokenHits
			<< " weightedBpEndHits=" << selectedCandidate->BpWeightedEndTokenHits
			<< " genericScriptHits=" << selectedCandidate->GenericScriptHits << std::endl;

		std::string anomalyCsv;
		for (size_t i = 0; i < anomalyTags.size(); i++)
		{
			if (i > 0) anomalyCsv += ",";
			anomalyCsv += anomalyTags[i];
		}

		ScriptOffsetDiagnostics diag;
		diag.SelectedOffset = selectedOffset;
		diag.SelectedScore = selectedCandidate->Score;
		diag.ScoreGapTop2 = scoreGapTop2;
		diag.BpEndHits = selectedCandidate->BpEndTokenHits;
		diag.WeightedBpEndHits = selectedCandidate->BpWeightedEndTokenHits;
		diag.GenericScriptHits = selectedCandidate->GenericScriptHits;
		diag.VerifyProbed = bestVerification.Probed;
		diag.VerifyHeaderValid = bestVerification.HeaderValid;
		diag.VerifyEndHits = bestVerification.EndHits;
		diag.VerifyFirstOpcodeValid = bestVerification.FirstOpcodeValid;
		diag.VerifySizeSane = bestVerification.SizeSane;
		diag.VerifyEndRate = verifyEndRate;
		diag.VerifyOpcodeRate = verifyOpcodeRate;
		diag.Confidence = confidence;
		diag.AnomalyTags = anomalyCsv;
		diag.CacheKey = runtimeCacheKey;
		diag.FromCache = false;
		SetScriptOffsetDiagnostics(diag);

		if (runtimeCacheKey != 0 && confidence != "low")
		{
			sRuntimeCache[runtimeCacheKey] = selectedOffset;
			sRuntimeDiagCache[runtimeCacheKey] = diag;
		}

		return selectedOffset;
	}

	std::cerr << "Dumper-7 WARNING: Could not find UFunction::Script offset." << std::endl;
	ScriptOffsetDiagnostics diag;
	diag.SelectedOffset = OffsetNotFound;
	diag.Confidence = "low";
	diag.AnomalyTags = "no_candidate_found";
	diag.CacheKey = runtimeCacheKey;
	SetScriptOffsetDiagnostics(diag);
	return OffsetNotFound;
}

/* UClass */
int32_t OffsetFinder::FindCastFlagsOffset()
{
	std::vector<std::pair<void*, EClassCastFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Actor").GetAddress(), EClassCastFlags::Actor });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), EClassCastFlags::Field | EClassCastFlags::Struct | EClassCastFlags::Class });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindDefaultObjectOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindClassFast("Object").GetAddress(), ObjectArray::FindObjectFast("Default__Object").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("Field").GetAddress(), ObjectArray::FindObjectFast("Default__Field").GetAddress() });

	return FindOffset(Infos, 0x28, 0x200);
}

int32_t OffsetFinder::FindImplementedInterfacesOffset()
{
	UEClass Interface_AssetUserDataClass = ObjectArray::FindClassFast("Interface_AssetUserData");

	const uint8_t* ActorComponentClassPtr = reinterpret_cast<const uint8_t*>(ObjectArray::FindClassFast("ActorComponent").GetAddress());

	for (int i = Off::UClass::ClassDefaultObject; i <= (0x350 - 0x10); i += sizeof(void*))
	{
		const auto& ActorArray = *reinterpret_cast<const TArray<FImplementedInterface>*>(ActorComponentClassPtr + i);

		if (ActorArray.IsValid() && !Platform::IsBadReadPtr(ActorArray.GetDataPtr()))
		{
			if (ActorArray[0].InterfaceClass == Interface_AssetUserDataClass)
				return i;
		}
	}

	return OffsetNotFound;
}

/* Property */
int32_t OffsetFinder::FindElementSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x04 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindArrayDimOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x01 });

	const int32_t MinOffset = Off::Property::ElementSize - 0x10;
	const int32_t MaxOffset = Off::Property::ElementSize + 0x10;

	return FindOffset(Infos, MinOffset, MaxOffset);
}

int32_t OffsetFinder::FindPropertyFlagsOffset()
{
	std::vector<std::pair<void*, EPropertyFlags>> Infos;


	UEStruct Guid = ObjectArray::FindStructFast("Guid");
	UEStruct Color = ObjectArray::FindStructFast("Color");

	constexpr EPropertyFlags GuidMemberFlags = EPropertyFlags::Edit | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;
	constexpr EPropertyFlags ColorMemberFlags = EPropertyFlags::Edit | EPropertyFlags::BlueprintVisible | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;

	Infos.push_back({ Guid.FindMember("A").GetAddress(), GuidMemberFlags });
	Infos.push_back({ Color.FindMember("R").GetAddress(), ColorMemberFlags });

	if (Infos[1].first == nullptr) [[unlikely]]
		Infos[1].first = Color.FindMember("r").GetAddress();

	int FlagsOffset = FindOffset(Infos);

	// Same flags without AccessSpecifier
	if (FlagsOffset == OffsetNotFound)
	{
		Infos[0].second |= EPropertyFlags::NativeAccessSpecifierPublic;
		Infos[1].second |= EPropertyFlags::NativeAccessSpecifierPublic;

		FlagsOffset = FindOffset(Infos);
	}

	return FlagsOffset;
}

int32_t OffsetFinder::FindOffsetInternalOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	const UEStruct Color = ObjectArray::FindStructFast("Color");
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Color.FindMember("B").GetAddress(), 0x00 });
	Infos.push_back({ Color.FindMember("G").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x08 });

	// Thanks to the ue5 dev who decided FColor::R should be spelled FColor::r
	if (Infos[2].first == nullptr) [[unlikely]]
		Infos[2].first = Color.FindMember("r").GetAddress();

	return FindOffset(Infos);
}

/* BoolProperty */
int32_t OffsetFinder::FindBoolPropertyBaseOffset()
{
	std::vector<std::pair<void*, uint8_t>> Infos;

	UEClass Engine = ObjectArray::FindClassFast("Engine");
	Infos.push_back({ Engine.FindMember("bIsOverridingSelectedColor").GetAddress(), 0xFF });
	Infos.push_back({ Engine.FindMember("bEnableOnScreenDebugMessagesDisplay").GetAddress(), 0b00000010 });
	Infos.push_back({ ObjectArray::FindClassFast("PlayerController").FindMember("bAutoManageActiveCameraTarget").GetAddress(), 0xFF });

	return (FindOffset<1>(Infos, Off::Property::Offset_Internal) - 0x3);
}

/* ObjectPrperty */
int32_t OffsetFinder::FindObjectPropertyClassOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	const UEClass Controller = ObjectArray::FindClassFast("Controller");
	Infos.push_back({ Controller.FindMember("PlayerState").GetAddress(), ObjectArray::FindClassFast("PlayerState").GetAddress() });
	Infos.push_back({ Controller.FindMember("Pawn").GetAddress(), ObjectArray::FindClassFast("Pawn").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("World").FindMember("PersistentLevel").GetAddress(), ObjectArray::FindClassFast("Level").GetAddress() });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* EnumProperty */
int32_t OffsetFinder::FindEnumPropertyBaseOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* ComponentCreationMethod = ObjectArray::FindObjectFast("EComponentCreationMethod", EClassCastFlags::Enum).GetAddress();
	const void* AutoPossessAI = ObjectArray::FindObjectFast("EAutoPossessAI", EClassCastFlags::Enum).GetAddress();

	if (!ComponentCreationMethod || !AutoPossessAI)
		return OffsetNotFound;

	void* CreationMethodMember = ObjectArray::FindClassFast("ActorComponent").FindMember("CreationMethod", EClassCastFlags::EnumProperty).GetAddress();
	void* AutoPossessAIMember = ObjectArray::FindClassFast("Pawn").FindMember("AutoPossessAI", EClassCastFlags::EnumProperty).GetAddress();

	// UE4.15 and below don't have EnumProperty
	if (!CreationMethodMember || !AutoPossessAIMember)
		return OffsetNotFound;

	Infos.push_back({ CreationMethodMember, ComponentCreationMethod });
	Infos.push_back({ AutoPossessAIMember , AutoPossessAI });

	// EnumProperty::Enum is the 2nd member after 'NumericProperty UnderlayingType'
	return FindOffset(Infos, Off::Property::Offset_Internal) - sizeof(void*);
}

/* ByteProperty */
int32_t OffsetFinder::FindBytePropertyEnumOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* CollisionResponseEnum = ObjectArray::FindObjectFast("ECollisionResponse", EClassCastFlags::Enum).GetAddress();

	const UEStruct CollisionResponseContainer = ObjectArray::FindStructFast("CollisionResponseContainer");

	if (!CollisionResponseEnum || !CollisionResponseContainer)
		return OffsetNotFound;

	const void* GameTraceChannel1 = CollisionResponseContainer.FindMember("GameTraceChannel1", EClassCastFlags::ByteProperty).GetAddress();
	const void* GameTraceChannel2 = CollisionResponseContainer.FindMember("GameTraceChannel2", EClassCastFlags::ByteProperty).GetAddress();

	if (!GameTraceChannel1 || !GameTraceChannel2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(GameTraceChannel1), CollisionResponseEnum });
	Infos.push_back({ const_cast<void*>(GameTraceChannel2), CollisionResponseEnum });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* StructProperty */
int32_t OffsetFinder::FindStructPropertyStructOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* VectorClass = ObjectArray::FindStructFast("Vector").GetAddress();

	if (VectorClass == nullptr)
		VectorClass = ObjectArray::FindClassFast("vector").GetAddress();

	const UEStruct TwoVectorsStruct = ObjectArray::FindStructFast("TwoVectors");

	if (!VectorClass || !TwoVectorsStruct)
		return OffsetNotFound;

	const void* v1 = TwoVectorsStruct.FindMember("v1", EClassCastFlags::StructProperty).GetAddress();
	const void* v2 = TwoVectorsStruct.FindMember("v2", EClassCastFlags::StructProperty).GetAddress();

	if (!v1 || !v2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(v1), VectorClass });
	Infos.push_back({ const_cast<void*>(v2), VectorClass });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* DelegateProperty */
int32_t OffsetFinder::FindDelegatePropertySignatureFunctionOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* DelegateSignature = ObjectArray::FindObjectFast("TimerDynamicDelegate__DelegateSignature", EClassCastFlags::Function).GetAddress();

	const UEStruct TwoVectorsStruct = ObjectArray::FindStructFast("TwoVectors");

	if (!DelegateSignature || !TwoVectorsStruct)
		return OffsetNotFound;

	const void* Delegate1 = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerElapsedTimeDelegate", EClassCastFlags::Function).FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress();
	const void* Delegate2 = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerRemainingTimeDelegate", EClassCastFlags::Function).FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress();

	if (!Delegate1 || !Delegate2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(Delegate1), DelegateSignature });
	Infos.push_back({ const_cast<void*>(Delegate2), DelegateSignature });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* ArrayProperty */
int32_t OffsetFinder::FindInnerTypeOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const UEProperty Property = ObjectArray::FindClassFast("GameViewportClient").FindMember("DebugProperties", EClassCastFlags::ArrayProperty))
	{
		void* AddressToCheck = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8*>(Property.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}

/* SetProperty */
int32_t OffsetFinder::FindSetPropertyBaseOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const auto Object = ObjectArray::FindStructFast("LevelCollection").FindMember("Levels", EClassCastFlags::SetProperty))
	{
		const void* AddressToCheck = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8*>(Object.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}


/* MapProperty */
int32_t OffsetFinder::FindMapPropertyBaseOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const auto Object = ObjectArray::FindClassFast("UserDefinedEnum").FindMember("DisplayNameMap", EClassCastFlags::MapProperty))
	{
		const void* AddressToCheck = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8*>(Object.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}

/* InSDK -> ULevel */
int32_t OffsetFinder::FindLevelActorsOffset()
{
	UEObject Level = nullptr;
	uintptr_t Lvl = 0x0;

	for (auto Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(EClassCastFlags::Level))
			continue;

		Level = Obj;
		Lvl = reinterpret_cast<uintptr_t>(Obj.GetAddress());
		break;
	}

	if (Lvl == 0x0)
		return OffsetNotFound;

	/*
	class ULevel : public UObject
	{
		FURL URL;
		TArray<AActor*> Actors;
		TArray<AActor*> GCActors;
	};

	SearchStart = sizeof(UObject) + sizeof(FURL)
	SearchEnd = offsetof(ULevel, OwningWorld)
	*/
	UEClass UObjectClass = ObjectArray::FindClassFast("Object");
	if (!UObjectClass)
		UObjectClass = ObjectArray::FindClassFast("object");

	const UEStruct FURLStruct = ObjectArray::FindObjectFast<UEStruct>("URL", EClassCastFlags::Struct);

	const UEProperty Level_OwningWorldProperty = Level.GetClass().FindMember("OwningWorld");

	if (!UObjectClass || !FURLStruct || !Level_OwningWorldProperty)
		return OffsetNotFound;

	const int32 SearchStart = UObjectClass.GetStructSize() + FURLStruct.GetStructSize();
	const int32 SearchEnd = Level_OwningWorldProperty.GetOffset();

	for (int i = SearchStart; i <= (SearchEnd - 0x10); i += sizeof(void*))
	{
		const TArray<void*>& ActorArray = *reinterpret_cast<TArray<void*>*>(Lvl + i);

		if (ActorArray.IsValid() && !Platform::IsBadReadPtr(ActorArray.GetDataPtr()))
		{
			return i;
		}
	}

	return OffsetNotFound;
}


/* InSDK -> UDataTable */
int32_t OffsetFinder::FindDatatableRowMapOffset()
{
	const UEClass DataTable = ObjectArray::FindClassFast("DataTable");

	constexpr int32 UObjectOuterSize = sizeof(void*);
	constexpr int32 RowStructSize = sizeof(void*);

	if (!DataTable)
	{
		std::cerr << "\nDumper-7: [DataTable] Couldn't find \"DataTable\" class, assuming default layout.\n" << std::endl;
		return (Off::UObject::Outer + UObjectOuterSize + RowStructSize);
	}

	UEProperty RowStructProp = DataTable.FindMember("RowStruct", EClassCastFlags::ObjectProperty);

	if (!RowStructProp)
	{
		std::cerr << "\nDumper-7: [DataTable] Couldn't find \"RowStruct\" property, assuming default layout.\n" << std::endl;
		return (Off::UObject::Outer + UObjectOuterSize + RowStructSize);
	}

	return RowStructProp.GetOffset() + RowStructProp.GetSize();
}

