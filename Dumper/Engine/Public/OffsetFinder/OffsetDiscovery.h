#pragma once

#include "Platform/Public/PeImage.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace OffsetFinder
{

enum class GlobalPointerDiscoveryError : std::uint8_t
{
	None,
	InvalidImage,
	ExpectedTypeUnavailable,
	CandidateLimitExceeded,
	NoCandidates,
	NoValidatedCandidate,
	AmbiguousCandidates,
	AllocationFailed
};

const char* ToString(GlobalPointerDiscoveryError error) noexcept;

struct GlobalPointerCandidateObservation
{
	std::uintptr_t SlotAddress = 0;
	std::uintptr_t ExpectedTarget = 0;
	std::uintptr_t FirstValue = 0;
	std::uintptr_t SecondValue = 0;
	bool ExpectedTargetFromObjectArray = false;
	bool ExpectedTargetTypeValidated = false;
	bool FirstReadSucceeded = false;
	bool SecondReadSucceeded = false;
};

struct GlobalPointerCandidateEvidence
{
	std::uintptr_t SlotAddress = 0;
	std::int64_t ModuleOffset = -1;
	std::uintptr_t ExpectedTarget = 0;
	std::uintptr_t FirstValue = 0;
	std::uintptr_t SecondValue = 0;
	bool AddressInImage = false;
	bool PointerAligned = false;
	bool WritableDataSection = false;
	bool ExpectedTargetFromObjectArray = false;
	bool ExpectedTargetTypeValidated = false;
	bool ReadableTwice = false;
	bool Stable = false;
	bool MatchesExpectedTarget = false;
	bool Accepted = false;
	std::string RejectionCode;
};

struct GlobalPointerDiscoveryReport
{
	GlobalPointerDiscoveryError Error = GlobalPointerDiscoveryError::NoCandidates;
	std::uintptr_t SelectedAddress = 0;
	std::int32_t SelectedOffset = -1;
	std::string Source = "module_data_cross_reference";
	std::string Confidence = "none";
	std::vector<std::string> Checks{
		"candidate_slot_in_main_image",
		"candidate_slot_pointer_aligned",
		"candidate_slot_in_writable_non_executable_section",
		"expected_target_from_object_array",
		"expected_target_type_validated",
		"two_checked_reads",
		"stable_target_value",
		"exact_expected_object_cross_reference",
		"exactly_one_unique_candidate"
	};
	std::vector<GlobalPointerCandidateEvidence> Candidates;

	bool Ok() const noexcept
	{
		return Error == GlobalPointerDiscoveryError::None
			&& SelectedAddress != 0
			&& SelectedOffset > 0;
	}
};

GlobalPointerDiscoveryReport ResolveGlobalPointerCandidates(
	const UExplorer::Platform::PeImageView& image,
	std::span<const GlobalPointerCandidateObservation> observations);

GlobalPointerDiscoveryReport MakeGlobalPointerDiscoveryFailure(
	GlobalPointerDiscoveryError error);

} // namespace OffsetFinder
