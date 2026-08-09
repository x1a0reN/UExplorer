
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <stdexcept>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Runtime/FUObjectItemLayout.h"
#include "Runtime/SafeMemory.h"
#include "Utils.h"

#include "Platform.h"


namespace fs = std::filesystem;

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,								// 0x00
		.MaxObjectsOffset = sizeof(void*),					// 0x08 (64bit) OR 0x04 (32bit)
		.NumObjectsOffset = sizeof(void*) + sizeof(int)		// 0x0C (64bit) OR 0x08 (32bit)
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
	FChunkedFixedUObjectArrayLayout // MindsEye
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x00, // first
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x10,
		.NumChunksOffset = 0x04,
	}
};

static int32 CheckedModuleOffset(const void* address, const char* fieldName)
{
	const uintptr_t offset = Platform::GetOffset(address);
	if (offset > static_cast<uintptr_t>((std::numeric_limits<int32>::max)()))
		throw std::runtime_error(std::string(fieldName) + " exceeds the signed 32-bit module-offset contract");
	return static_cast<int32>(offset);
}

bool IsAddressValidGObjects(const uintptr_t Address, const FFixedUObjectArrayLayout& Layout)
{
	/* It is assumed that the FUObjectItem layout is constant amongst all games using FFixedUObjectArray for ObjObjects. */
	struct FUObjectItem
	{
		void* Object;
		uint8_t Pad[sizeof(void*) * 2];
	};

	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxObjectsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumObjectsOffset);

	FUObjectItem* ObjectsButDecrypted = reinterpret_cast<FUObjectItem*>(ObjectArray::DecryptPtr(Objects));

	if (NumElements > MaxElements)
		return false;

	if (MaxElements > 0x400000)
		return false;

	if (NumElements < 0x1000)
		return false;

	if (Platform::IsBadReadPtr(ObjectsButDecrypted))
		return false;

	if (Platform::IsBadReadPtr(ObjectsButDecrypted[5].Object))
		return false;

	const uintptr_t FifthObject = reinterpret_cast<uintptr_t>(ObjectsButDecrypted[0x5].Object);
	const int32 IndexOfFithobject = *reinterpret_cast<int32_t*>(FifthObject + sizeof(void*) + sizeof(int32)); // FifthObject -> InternalIndex

	if (IndexOfFithobject != 0x5)
		return false;

	return true;
}

bool IsAddressValidGObjects(const uintptr_t Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxElementsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumElementsOffset);
	const int32 MaxChunks   = *reinterpret_cast<const int32*>(Address + Layout.MaxChunksOffset);
	const int32 NumChunks   = *reinterpret_cast<const int32*>(Address + Layout.NumChunksOffset);

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	if (NumChunks > 0x14 || NumChunks < 0x1)
		return false;

	if (MaxChunks > 0x5FF || MaxChunks < 0x6)
		return false;

	if (NumElements <= 0x800 || MaxElements <= 0x10000)
		return false;

	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;

	if ((MaxElements % 0x10) != 0)
		return false;

	const int32_t ElementsPerChunk = MaxElements / MaxChunks;

	if ((ElementsPerChunk % 0x10) != 0)
		return false;

	if (ElementsPerChunk < 0x8000 || ElementsPerChunk > 0x80000)
		return false;

	const bool bNumChunksFitsNumElements = ((NumElements / ElementsPerChunk) + 1) == NumChunks;

	if (!bNumChunksFitsNumElements)
		return false;

	const bool bMaxChunksFitsMaxElements = (MaxElements / ElementsPerChunk) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;

	if (!ObjectsPtrButDecrypted || Platform::IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;

	for (int i = 0; i < NumChunks; i++)
	{
		if (!ObjectsPtrButDecrypted[i] || Platform::IsBadReadPtr(ObjectsPtrButDecrypted[i]))
			return false;
	}

	return true;
}


void ObjectArray::InitializeFUObjectItem(uint8_t* FirstItemPtr)
{
	IdentityLayout = {};
	Off::InSDK::ObjArray::FUObjectItemSerialNumberOffset = -1;
	for (int i = 0x0; i < 0x20; i += 4)
	{
		if (!Platform::IsBadReadPtr(*reinterpret_cast<uint8_t**>(FirstItemPtr + i)))
		{
			FUObjectItemInitialOffset = i;
			break;
		}
	}

	for (int i = FUObjectItemInitialOffset + sizeof(void*); i <= 0x38; i += 4)
	{
		void* SecondObject = *reinterpret_cast<uint8**>(FirstItemPtr + i);
		void* ThirdObject  = *reinterpret_cast<uint8**>(FirstItemPtr + (i * 2) - FUObjectItemInitialOffset);

		if (!Platform::IsBadReadPtr(SecondObject) && !Platform::IsBadReadPtr(*reinterpret_cast<void**>(SecondObject)) &&
			!Platform::IsBadReadPtr(ThirdObject) && !Platform::IsBadReadPtr(*reinterpret_cast<void**>(ThirdObject)))
		{
			SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
			break;
		}
	}

	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;

	std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;
}

bool ObjectArray::TryResolveItemAddress(const int32 Index, uintptr_t& ItemAddress)
{
	ItemAddress = 0;
	if (!GObjects || Index < 0 || SizeOfFUObjectItem == 0)
		return false;

	const int32 objectsOffset = Off::FUObjectArray::GetObjectsOffset();
	const int32 countOffset = Off::FUObjectArray::GetNumElementsOffset();
	const int32 capacityOffset = Off::FUObjectArray::GetMaxElementsOffset();
	if (objectsOffset < 0 || countOffset < 0 || capacityOffset < 0)
		return false;

	int32 count = 0;
	int32 capacity = 0;
	if (!UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects) + static_cast<uintptr_t>(countOffset),
		count).Ok()
		|| !UExplorer::Runtime::ReadValue(
			reinterpret_cast<uintptr_t>(GObjects) + static_cast<uintptr_t>(capacityOffset),
			capacity).Ok()
		|| count <= 0
		|| capacity <= 0
		|| count > capacity
		|| Index >= count)
	{
		return false;
	}

	void* encodedObjects = nullptr;
	if (!UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects) + static_cast<uintptr_t>(objectsOffset),
		encodedObjects).Ok()
		|| !encodedObjects)
	{
		return false;
	}

	uint8_t* objects = nullptr;
#if defined(_MSC_VER)
	__try
	{
		objects = DecryptPtr(encodedObjects);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	objects = DecryptPtr(encodedObjects);
#endif
	if (!objects)
		return false;

	uintptr_t itemBase = reinterpret_cast<uintptr_t>(objects);
	uint32 inContainerIndex = static_cast<uint32>(Index);
	if (Off::FUObjectArray::bIsChunked)
	{
		if (NumElementsPerChunk == 0 || NumElementsPerChunk == (std::numeric_limits<uint32>::max)())
			return false;
		const uint32 chunkIndex = static_cast<uint32>(Index) / NumElementsPerChunk;
		inContainerIndex = static_cast<uint32>(Index) % NumElementsPerChunk;
		const int32 chunkCount = NumChunks();
		if (chunkCount <= 0 || chunkIndex >= static_cast<uint32>(chunkCount))
			return false;

		const uintptr_t tableAddress = reinterpret_cast<uintptr_t>(objects);
		if (chunkIndex > ((std::numeric_limits<uintptr_t>::max)() - tableAddress) / sizeof(void*))
			return false;
		void* chunk = nullptr;
		if (!UExplorer::Runtime::ReadValue(
			tableAddress + static_cast<uintptr_t>(chunkIndex) * sizeof(void*),
			chunk).Ok()
			|| !chunk)
		{
			return false;
		}
		itemBase = reinterpret_cast<uintptr_t>(chunk);
	}

	if (inContainerIndex > ((std::numeric_limits<uintptr_t>::max)() - itemBase) / SizeOfFUObjectItem)
		return false;
	const uintptr_t candidate = itemBase
		+ static_cast<uintptr_t>(inContainerIndex) * SizeOfFUObjectItem;
	uintptr_t endExclusive = 0;
	if (!UExplorer::Runtime::CheckedAddressRange(candidate, SizeOfFUObjectItem, endExclusive))
		return false;

	ItemAddress = candidate;
	return true;
}

bool ObjectArray::TryReadIdentityCandidate(
	const int32 Index,
	const int32 SerialOffset,
	FUObjectItemIdentity& Identity,
	int32* ClusterRootIndex)
{
	Identity = {};
	Identity.Index = -1;
	if (SerialOffset < 0
		|| FUObjectItemInitialOffset + sizeof(void*) + sizeof(int32) * 2 > SizeOfFUObjectItem
		|| static_cast<uint32>(SerialOffset) + sizeof(int32) > SizeOfFUObjectItem
		|| Off::UObject::Index <= 0)
	{
		return false;
	}

	uintptr_t itemAddress = 0;
	if (!TryResolveItemAddress(Index, itemAddress))
		return false;

	const uintptr_t objectField = itemAddress + FUObjectItemInitialOffset;
	const uintptr_t clusterField = objectField + sizeof(void*) + sizeof(int32);
	const uintptr_t serialField = itemAddress + static_cast<uint32>(SerialOffset);
	void* objectFirst = nullptr;
	void* objectSecond = nullptr;
	int32 serialFirst = 0;
	int32 serialSecond = 0;
	int32 clusterFirst = -1;
	int32 clusterSecond = -1;
	if (!UExplorer::Runtime::ReadValue(objectField, objectFirst).Ok()
		|| !UExplorer::Runtime::ReadValue(serialField, serialFirst).Ok()
		|| !UExplorer::Runtime::ReadValue(clusterField, clusterFirst).Ok()
		|| !UExplorer::Runtime::ReadValue(objectField, objectSecond).Ok()
		|| !UExplorer::Runtime::ReadValue(serialField, serialSecond).Ok()
		|| !UExplorer::Runtime::ReadValue(clusterField, clusterSecond).Ok()
		|| !objectFirst
		|| objectFirst != objectSecond
		|| serialFirst != serialSecond
		|| clusterFirst != clusterSecond)
	{
		return false;
	}

	int32 internalIndex = -1;
	const uintptr_t objectAddress = reinterpret_cast<uintptr_t>(objectFirst);
	const uintptr_t internalIndexOffset = static_cast<uintptr_t>(Off::UObject::Index);
	if (internalIndexOffset > (std::numeric_limits<uintptr_t>::max)() - objectAddress)
		return false;
	if (!UExplorer::Runtime::ReadValue(
		objectAddress + internalIndexOffset,
		internalIndex).Ok()
		|| internalIndex != Index)
	{
		return false;
	}

	void* objectFinal = nullptr;
	int32 serialFinal = 0;
	if (!UExplorer::Runtime::ReadValue(objectField, objectFinal).Ok()
		|| !UExplorer::Runtime::ReadValue(serialField, serialFinal).Ok()
		|| objectFinal != objectFirst
		|| serialFinal != serialFirst)
	{
		return false;
	}

	Identity = {
		.Index = Index,
		.SerialNumber = serialFirst,
		.ObjectAddress = objectAddress
	};
	if (ClusterRootIndex)
		*ClusterRootIndex = clusterFirst;
	return true;
}

bool ObjectArray::ValidateIdentityLayout()
{
	IdentityLayout = {
		.ItemSize = SizeOfFUObjectItem,
		.ObjectOffset = FUObjectItemInitialOffset,
		.ReasonCode = "FUOBJECTITEM_LAYOUT_NOT_VALIDATED"
	};
	Off::InSDK::ObjArray::FUObjectItemSerialNumberOffset = -1;

	const int32 objectCount = Num();
	constexpr int32 candidateSerialOffset = 0x10;
	const bool supportedProfileShape = FUObjectItemInitialOffset == 0
		&& (SizeOfFUObjectItem == 0x18 || SizeOfFUObjectItem == 0x20);
	const int32 attempts = supportedProfileShape ? (std::min)(objectCount, 4096) : 0;
	std::vector<UExplorer::Runtime::ObjectItemLayoutSample> samples;
	if (attempts > 0)
		samples.reserve(static_cast<size_t>(attempts));
	for (int32 attempt = 0; attempt < attempts; ++attempt)
	{
		const int32 index = static_cast<int32>(
			(static_cast<int64_t>(attempt) * objectCount) / attempts);
		FUObjectItemIdentity identity;
		int32 clusterRootIndex = -1;
		if (!TryReadIdentityCandidate(
			index,
			candidateSerialOffset,
			identity,
			&clusterRootIndex))
		{
			continue;
		}
		samples.push_back({
			.SlotIndex = index,
			.InternalIndex = identity.Index,
			.ObjectAddress = identity.ObjectAddress,
			.ClusterRootIndex = clusterRootIndex,
			.SerialNumber = identity.SerialNumber,
			.Stable = true
		});
	}

	const UExplorer::Runtime::ObjectItemLayoutValidation validation =
		UExplorer::Runtime::ValidateEpic64ObjectItemLayoutV1(
			SizeOfFUObjectItem,
			FUObjectItemInitialOffset,
			objectCount,
			samples);
	IdentityLayout = {
		.Validated = validation.Ok(),
		.ItemSize = validation.ItemSize,
		.ObjectOffset = validation.ObjectOffset,
		.SerialOffset = validation.Ok() ? static_cast<int32>(validation.SerialOffset) : -1,
		.CoherentSamples = validation.CoherentSamples,
		.PositiveSerialSamples = validation.PositiveSerialSamples,
		.Profile = validation.Profile,
		.ReasonCode = validation.Ok() ? std::string{} : UExplorer::Runtime::ToString(validation.Error),
		.Checks = validation.Checks
	};
	if (!validation.Ok())
	{
		std::cerr << "[ObjectArray] Object identity unavailable: "
			<< IdentityLayout.ReasonCode << " item_size=0x" << std::hex << SizeOfFUObjectItem
			<< " object_offset=0x" << FUObjectItemInitialOffset << std::dec
			<< " coherent_samples=" << validation.CoherentSamples
			<< " positive_serial_samples=" << validation.PositiveSerialSamples << std::endl;
		return false;
	}

	Off::InSDK::ObjArray::FUObjectItemSerialNumberOffset =
		static_cast<int32>(validation.SerialOffset);
	std::cerr << "[ObjectArray] Object identity layout validated: profile="
		<< validation.Profile << " serial_offset=0x" << std::hex << validation.SerialOffset
		<< std::dec << " coherent_samples=" << validation.CoherentSamples
		<< " positive_serial_samples=" << validation.PositiveSerialSamples << std::endl;
	return true;
}

const FUObjectItemIdentityLayout& ObjectArray::GetIdentityLayout()
{
	return IdentityLayout;
}

bool ObjectArray::TryReadIdentity(const int32 Index, FUObjectItemIdentity& Identity)
{
	if (!IdentityLayout.Validated || IdentityLayout.SerialOffset < 0)
		return false;
	return TryReadIdentityCandidate(Index, IdentityLayout.SerialOffset, Identity);
}

void ObjectArray::InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
}


/* We don't speak about this function... */
void ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
	if (!bScanAllMemory)
	{
		std::cerr << "\nDumper-7 by me, you & him\n\n\n";
		std::cerr << "Searching for GObjects...\n\n";
	}

	auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr_t Address)
	{
		for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
		{
			if (!IsAddressValidGObjects(Address, Layout))
				continue;

			if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
			{
				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::FixedLayout = Layout;
			}
			else
			{
				Off::FUObjectArray::bIsChunked = true;
				Off::FUObjectArray::ChunkedFixedLayout = Layout;
			}

			return true;
		}
		
		return false;
	};

	bool bIsGObjectsChunked = false;
	auto IsAddressValidGObjects = [MatchesAnyLayout, &bIsGObjectsChunked](const void* CurrentAddress) -> bool
	{
		//std::cerr << "checking addr: " << CurrentAddress << "\n";
		if (MatchesAnyLayout(FFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = false;
			return true;
		}
		else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = true;
			return true;
		}

		return false;
	};

	void* GObjectsAddress = nullptr;

	if (bScanAllMemory)
	{
		GObjectsAddress = Platform::IterateAllSectionsWithCallback(IsAddressValidGObjects, 0x4, 0x50, ModuleName);
	}
	else
	{
		GObjectsAddress = Platform::IterateSectionWithCallback(Platform::GetSectionInfo(".data"), IsAddressValidGObjects, 0x4, 0x50);
	}


	if (GObjectsAddress)
	{
		if (!bIsGObjectsChunked)
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			NumElementsPerChunk = (std::numeric_limits<uint32>::max)();

			Off::InSDK::ObjArray::GObjects = CheckedModuleOffset(GObjectsAddress, "GObjects");

			std::cerr << "Found FFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				uint8_t* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray));

				return *reinterpret_cast<void**>(ChunkPtr + FUObjectItemOffset + (Index * FUObjectItemSize));
			};

			uint8_t* FirstItem = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

			ObjectArray::InitializeFUObjectItem(FirstItem);
		}
		else
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			const int32 maxElements = Max();
			const int32 maxChunks = MaxChunks();
			if (maxElements <= 0 || maxChunks <= 0 || (maxElements % maxChunks) != 0)
				throw std::runtime_error("Chunked GObjects capacity is inconsistent");
			NumElementsPerChunk = static_cast<uint32>(maxElements / maxChunks);
			Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;

			SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
			FUObjectItemInitialOffset = 0x0;

			Off::InSDK::ObjArray::GObjects = CheckedModuleOffset(GObjectsAddress, "GObjects");

			std::cerr << "Found FChunkedFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				const int32 ChunkIndex = Index / PerChunk;
				const int32 InChunkIdx = Index % PerChunk;

				uint8_t* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray));

				uint8_t* Chunk = reinterpret_cast<uint8_t**>(ChunkPtr)[ChunkIndex];
				uint8_t* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize);

				return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
			};
			
			uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

			ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8_t**>(ChunksPtr));
		}

		return;
	}

	if (!bScanAllMemory)
	{
		ObjectArray::Init(true);
		return;
	}

	if (GObjects == nullptr)
	{
		std::cerr << "\nGObjects couldn't be found, please overwrite the offset in Generator.cpp.\n\n\n";
		Sleep(10000);
		exit(1);
	}
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	std::cerr << "GObjects: 0x" << (void*)GObjects << "\n" << std::endl;

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		uint8_t* ItemPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray))
			+ (Index * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	std::cerr << "Overwrote FFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(ChunksPtr);
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	if (ElementsPerChunk <= 0)
		throw std::invalid_argument("ElementsPerChunk must be positive");
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = static_cast<uint32>(ElementsPerChunk);
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		uint8_t* ChunkTable = DecryptPtr(*reinterpret_cast<uint8_t**>(ObjectsArray));
		uint8_t* Chunk = reinterpret_cast<uint8_t**>(ChunkTable)[ChunkIndex];
		uint8_t* ItemPtr = reinterpret_cast<uint8_t*>(Chunk) + (InChunkIdx * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8_t* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8_t**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	std::cerr << "Overwrote FChunkedFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8_t**>(ChunksPtr));
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}
	}

	DumpStream.close();
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				DumpStream << std::format("[{:08X}] {{{}}}     {} {}\n", Prop.GetOffset(), Prop.GetAddress(), Prop.GetPropClassName(), Prop.GetName());
			}
		}
	}

	DumpStream.close();
}


int32 ObjectArray::Num()
{
	if (!GObjects || Off::FUObjectArray::GetNumElementsOffset() < 0)
		return 0;
	int32 value = 0;
	return UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects)
			+ static_cast<uintptr_t>(Off::FUObjectArray::GetNumElementsOffset()),
		value).Ok() && value >= 0
		? value
		: 0;
}

int32 ObjectArray::Max()
{
	if (!GObjects || Off::FUObjectArray::GetMaxElementsOffset() < 0)
		return 0;
	int32 value = 0;
	return UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects)
			+ static_cast<uintptr_t>(Off::FUObjectArray::GetMaxElementsOffset()),
		value).Ok() && value >= 0
		? value
		: 0;
}

int32 ObjectArray::NumChunks()
{
	if (!GObjects || !Off::FUObjectArray::bIsChunked
		|| Off::FUObjectArray::GetNumChunksOffset() < 0)
	{
		return 0;
	}
	int32 value = 0;
	return UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects)
			+ static_cast<uintptr_t>(Off::FUObjectArray::GetNumChunksOffset()),
		value).Ok() && value >= 0
		? value
		: 0;
}

int32 ObjectArray::MaxChunks()
{
	if (!GObjects || !Off::FUObjectArray::bIsChunked
		|| Off::FUObjectArray::GetMaxChunksOffset() < 0)
	{
		return 0;
	}
	int32 value = 0;
	return UExplorer::Runtime::ReadValue(
		reinterpret_cast<uintptr_t>(GObjects)
			+ static_cast<uintptr_t>(Off::FUObjectArray::GetMaxChunksOffset()),
		value).Ok() && value >= 0
		? value
		: 0;
}

template<typename UEType>
static UEType ObjectArray::GetByIndex(int32 Index)
{
	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
static UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
}

UEObject ObjectArray::ObjectsIterator::operator*() const
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator==(const ObjectsIterator& Other) const
{
	return CurrentIndex == Other.CurrentIndex;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
	return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
	if (CurrenStructHasMoreMembers())
	{
		PropertyIndex++;

		return *this;
	}

	IterateToNextStructWithMembers();

	return *this;
}

UEProperty AllFieldIterator::operator*() const
{
	return Fields[PropertyIndex];
}


void AllFieldIterator::IterateToNextStruct()
{
	if (IsEndIterator())
		return;

	++CurrentObject;

	while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
		++CurrentObject;
}
void AllFieldIterator::IterateToNextStructWithMembers()
{
	// Loop, in case we meet a struct wihtout any properties
	while (!CurrenStructHasMoreMembers())
	{
		IterateToNextStruct();
		PropertyIndex = 0;

		if (IsEndIterator())
			return;

		Fields = GetCurrentStruct().GetProperties();
	}
}

/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
[[maybe_unused]] void TemplateTypeCreationForObjectArray(void)
{
	ObjectArray::FindObject<UEObject>("");
	ObjectArray::FindObject<UEField>("");
	ObjectArray::FindObject<UEEnum>("");
	ObjectArray::FindObject<UEStruct>("");
	ObjectArray::FindObject<UEClass>("");
	ObjectArray::FindObject<UEFunction>("");
	ObjectArray::FindObject<UEProperty>("");
	ObjectArray::FindObject<UEByteProperty>("");
	ObjectArray::FindObject<UEBoolProperty>("");
	ObjectArray::FindObject<UEObjectProperty>("");
	ObjectArray::FindObject<UEClassProperty>("");
	ObjectArray::FindObject<UEStructProperty>("");
	ObjectArray::FindObject<UEArrayProperty>("");
	ObjectArray::FindObject<UEMapProperty>("");
	ObjectArray::FindObject<UESetProperty>("");
	ObjectArray::FindObject<UEEnumProperty>("");

	ObjectArray::FindObjectFast<UEObject>("");
	ObjectArray::FindObjectFast<UEField>("");
	ObjectArray::FindObjectFast<UEEnum>("");
	ObjectArray::FindObjectFast<UEStruct>("");
	ObjectArray::FindObjectFast<UEClass>("");
	ObjectArray::FindObjectFast<UEFunction>("");
	ObjectArray::FindObjectFast<UEProperty>("");
	ObjectArray::FindObjectFast<UEByteProperty>("");
	ObjectArray::FindObjectFast<UEBoolProperty>("");
	ObjectArray::FindObjectFast<UEObjectProperty>("");
	ObjectArray::FindObjectFast<UEClassProperty>("");
	ObjectArray::FindObjectFast<UEStructProperty>("");
	ObjectArray::FindObjectFast<UEArrayProperty>("");
	ObjectArray::FindObjectFast<UEMapProperty>("");
	ObjectArray::FindObjectFast<UESetProperty>("");
	ObjectArray::FindObjectFast<UEEnumProperty>("");

	ObjectArray::FindObjectFastInOuter<UEObject>("", "");
	ObjectArray::FindObjectFastInOuter<UEField>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnum>("", "");
	ObjectArray::FindObjectFastInOuter<UEStruct>("", "");
	ObjectArray::FindObjectFastInOuter<UEClass>("", "");
	ObjectArray::FindObjectFastInOuter<UEFunction>("", "");
	ObjectArray::FindObjectFastInOuter<UEProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEByteProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEBoolProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEObjectProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEClassProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEStructProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEArrayProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEMapProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UESetProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnumProperty>("", "");

	ObjectArray::GetByIndex<UEObject>(-1);
	ObjectArray::GetByIndex<UEField>(-1);
	ObjectArray::GetByIndex<UEEnum>(-1);
	ObjectArray::GetByIndex<UEStruct>(-1);
	ObjectArray::GetByIndex<UEClass>(-1);
	ObjectArray::GetByIndex<UEFunction>(-1);
	ObjectArray::GetByIndex<UEProperty>(-1);
	ObjectArray::GetByIndex<UEByteProperty>(-1);
	ObjectArray::GetByIndex<UEBoolProperty>(-1);
	ObjectArray::GetByIndex<UEObjectProperty>(-1);
	ObjectArray::GetByIndex<UEClassProperty>(-1);
	ObjectArray::GetByIndex<UEStructProperty>(-1);
	ObjectArray::GetByIndex<UEArrayProperty>(-1);
	ObjectArray::GetByIndex<UEMapProperty>(-1);
	ObjectArray::GetByIndex<UESetProperty>(-1);
	ObjectArray::GetByIndex<UEEnumProperty>(-1);
}
