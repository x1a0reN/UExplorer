#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace UExplorer::Runtime
{

enum class ObjectItemLayoutError : std::uint8_t
{
	None,
	UnsupportedPointerSize,
	UnsupportedObjectOffset,
	UnsupportedItemSize,
	InvalidObjectCount,
	InsufficientCoherentSamples,
	NoPositiveSerialWitness
};

const char* ToString(ObjectItemLayoutError error) noexcept;

struct ObjectItemLayoutSample
{
	std::int32_t SlotIndex = -1;
	std::int32_t InternalIndex = -1;
	std::uintptr_t ObjectAddress = 0;
	std::int32_t ClusterRootIndex = -1;
	std::int32_t SerialNumber = 0;
	bool Stable = false;
};

struct ObjectItemLayoutValidation
{
	ObjectItemLayoutError Error = ObjectItemLayoutError::None;
	std::uint32_t ItemSize = 0;
	std::uint32_t ObjectOffset = 0;
	std::uint32_t SerialOffset = 0;
	std::uint32_t CoherentSamples = 0;
	std::uint32_t PositiveSerialSamples = 0;
	std::string Profile;
	std::vector<std::string> Checks;

	bool Ok() const noexcept { return Error == ObjectItemLayoutError::None; }
};

// This accepts one explicit ABI profile only. It never searches arbitrary fields
// for a value that merely resembles a serial number.
ObjectItemLayoutValidation ValidateEpic64ObjectItemLayoutV1(
	std::uint32_t itemSize,
	std::uint32_t objectOffset,
	std::int32_t objectCount,
	std::span<const ObjectItemLayoutSample> samples);

} // namespace UExplorer::Runtime
