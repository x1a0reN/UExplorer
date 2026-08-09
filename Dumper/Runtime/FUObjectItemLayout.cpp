#include "FUObjectItemLayout.h"

namespace UExplorer::Runtime
{
namespace
{

constexpr std::uint32_t kEpic64ObjectOffset = 0;
constexpr std::uint32_t kEpic64SerialOffset = 0x10;
constexpr std::uint32_t kEpic64ItemSize = 0x18;
constexpr std::uint32_t kEpic64PaddedItemSize = 0x20;
constexpr std::uint32_t kRequiredCoherentSamples = 16;

ObjectItemLayoutValidation Failure(
	const ObjectItemLayoutError error,
	const std::uint32_t itemSize,
	const std::uint32_t objectOffset)
{
	return {
		.Error = error,
		.ItemSize = itemSize,
		.ObjectOffset = objectOffset,
		.SerialOffset = kEpic64SerialOffset,
		.Profile = "epic_fuobjectitem_64_v1"
	};
}

} // namespace

const char* ToString(const ObjectItemLayoutError error) noexcept
{
	switch (error)
	{
	case ObjectItemLayoutError::None: return "NONE";
	case ObjectItemLayoutError::UnsupportedPointerSize: return "FUOBJECTITEM_POINTER_SIZE_UNSUPPORTED";
	case ObjectItemLayoutError::UnsupportedObjectOffset: return "FUOBJECTITEM_OBJECT_OFFSET_UNSUPPORTED";
	case ObjectItemLayoutError::UnsupportedItemSize: return "FUOBJECTITEM_SIZE_UNSUPPORTED";
	case ObjectItemLayoutError::InvalidObjectCount: return "FUOBJECTITEM_OBJECT_COUNT_INVALID";
	case ObjectItemLayoutError::InsufficientCoherentSamples: return "FUOBJECTITEM_COHERENT_SAMPLES_INSUFFICIENT";
	case ObjectItemLayoutError::NoPositiveSerialWitness: return "FUOBJECTITEM_POSITIVE_SERIAL_NOT_OBSERVED";
	}
	return "FUOBJECTITEM_LAYOUT_UNKNOWN_ERROR";
}

ObjectItemLayoutValidation ValidateEpic64ObjectItemLayoutV1(
	const std::uint32_t itemSize,
	const std::uint32_t objectOffset,
	const std::int32_t objectCount,
	const std::span<const ObjectItemLayoutSample> samples)
{
	if (sizeof(void*) != 8)
		return Failure(ObjectItemLayoutError::UnsupportedPointerSize, itemSize, objectOffset);
	if (objectOffset != kEpic64ObjectOffset)
		return Failure(ObjectItemLayoutError::UnsupportedObjectOffset, itemSize, objectOffset);
	if (itemSize != kEpic64ItemSize && itemSize != kEpic64PaddedItemSize)
		return Failure(ObjectItemLayoutError::UnsupportedItemSize, itemSize, objectOffset);
	if (objectCount <= 0)
		return Failure(ObjectItemLayoutError::InvalidObjectCount, itemSize, objectOffset);

	std::uint32_t coherent = 0;
	std::uint32_t positiveSerials = 0;
	for (const ObjectItemLayoutSample& sample : samples)
	{
		if (!sample.Stable
			|| sample.ObjectAddress == 0
			|| sample.SlotIndex < 0
			|| sample.SlotIndex >= objectCount
			|| sample.InternalIndex != sample.SlotIndex
			|| sample.SerialNumber < 0
			|| sample.ClusterRootIndex < -1
			|| sample.ClusterRootIndex >= objectCount)
		{
			continue;
		}

		++coherent;
		if (sample.SerialNumber > 0)
			++positiveSerials;
	}

	if (coherent < kRequiredCoherentSamples)
	{
		ObjectItemLayoutValidation result = Failure(
			ObjectItemLayoutError::InsufficientCoherentSamples,
			itemSize,
			objectOffset);
		result.CoherentSamples = coherent;
		result.PositiveSerialSamples = positiveSerials;
		return result;
	}
	if (positiveSerials == 0)
	{
		ObjectItemLayoutValidation result = Failure(
			ObjectItemLayoutError::NoPositiveSerialWitness,
			itemSize,
			objectOffset);
		result.CoherentSamples = coherent;
		return result;
	}

	return {
		.ItemSize = itemSize,
		.ObjectOffset = objectOffset,
		.SerialOffset = kEpic64SerialOffset,
		.CoherentSamples = coherent,
		.PositiveSerialSamples = positiveSerials,
		.Profile = "epic_fuobjectitem_64_v1",
		.Checks = {
			"x64_pointer_size",
			"exact_epic_object_offset",
			"supported_epic_item_size",
			"stable_double_read",
			"slot_matches_uobject_internal_index",
			"cluster_root_in_range",
			"non_negative_serials",
			"positive_serial_witness"
		}
	};
}

} // namespace UExplorer::Runtime
