#include "OffsetFinder/OffsetDiscovery.h"

#include <algorithm>
#include <limits>

namespace OffsetFinder
{
namespace
{

constexpr std::size_t kMaximumCandidateObservations = 4096;

bool ContainsPointerSlot(
	const std::uintptr_t begin,
	const std::size_t size,
	const std::uintptr_t slot) noexcept
{
	if (begin == 0 || slot < begin || size < sizeof(std::uintptr_t))
		return false;
	const std::size_t offset = static_cast<std::size_t>(slot - begin);
	return offset <= size - sizeof(std::uintptr_t);
}

std::string RejectionCode(const GlobalPointerCandidateEvidence& evidence)
{
	if (!evidence.AddressInImage)
		return "GLOBAL_POINTER_SLOT_OUTSIDE_IMAGE";
	if (!evidence.PointerAligned)
		return "GLOBAL_POINTER_SLOT_UNALIGNED";
	if (!evidence.WritableDataSection)
		return "GLOBAL_POINTER_SLOT_NOT_WRITABLE_DATA";
	if (!evidence.ExpectedTargetFromObjectArray)
		return "GLOBAL_POINTER_TARGET_NOT_FROM_OBJECT_ARRAY";
	if (!evidence.ExpectedTargetTypeValidated)
		return "GLOBAL_POINTER_TARGET_TYPE_INVALID";
	if (!evidence.ReadableTwice)
		return "GLOBAL_POINTER_SLOT_READ_FAILED";
	if (!evidence.Stable)
		return "GLOBAL_POINTER_TARGET_UNSTABLE";
	if (!evidence.MatchesExpectedTarget)
		return "GLOBAL_POINTER_TARGET_MISMATCH";
	if (evidence.ModuleOffset <= 0
		|| evidence.ModuleOffset > (std::numeric_limits<std::int32_t>::max)())
	{
		return "GLOBAL_POINTER_OFFSET_OUT_OF_RANGE";
	}
	return {};
}

} // namespace

const char* ToString(const GlobalPointerDiscoveryError error) noexcept
{
	switch (error)
	{
	case GlobalPointerDiscoveryError::None: return "NONE";
	case GlobalPointerDiscoveryError::InvalidImage: return "GLOBAL_POINTER_IMAGE_INVALID";
	case GlobalPointerDiscoveryError::ExpectedTypeUnavailable: return "GLOBAL_POINTER_EXPECTED_TYPE_UNAVAILABLE";
	case GlobalPointerDiscoveryError::CandidateLimitExceeded: return "GLOBAL_POINTER_CANDIDATE_LIMIT_EXCEEDED";
	case GlobalPointerDiscoveryError::NoCandidates: return "GLOBAL_POINTER_NO_CANDIDATES";
	case GlobalPointerDiscoveryError::NoValidatedCandidate: return "GLOBAL_POINTER_NO_VALIDATED_CANDIDATE";
	case GlobalPointerDiscoveryError::AmbiguousCandidates: return "GLOBAL_POINTER_CANDIDATES_AMBIGUOUS";
	case GlobalPointerDiscoveryError::AllocationFailed: return "GLOBAL_POINTER_ALLOCATION_FAILED";
	}
	return "GLOBAL_POINTER_UNKNOWN_ERROR";
}

GlobalPointerDiscoveryReport MakeGlobalPointerDiscoveryFailure(
	const GlobalPointerDiscoveryError error)
{
	GlobalPointerDiscoveryReport report;
	report.Error = error;
	return report;
}

GlobalPointerDiscoveryReport ResolveGlobalPointerCandidates(
	const UExplorer::Platform::PeImageView& image,
	const std::span<const GlobalPointerCandidateObservation> observations)
{
	GlobalPointerDiscoveryReport report;
	if (image.Base == 0 || image.Size < sizeof(std::uintptr_t) || image.Sections.empty())
	{
		report.Error = GlobalPointerDiscoveryError::InvalidImage;
		return report;
	}
	if (observations.empty())
		return report;
	if (observations.size() > kMaximumCandidateObservations)
	{
		report.Error = GlobalPointerDiscoveryError::CandidateLimitExceeded;
		return report;
	}

	try
	{
		report.Candidates.reserve(observations.size());
		std::vector<std::uintptr_t> acceptedAddresses;
		acceptedAddresses.reserve(observations.size());

		for (const GlobalPointerCandidateObservation& observation : observations)
		{
			GlobalPointerCandidateEvidence evidence{
				.SlotAddress = observation.SlotAddress,
				.ExpectedTarget = observation.ExpectedTarget,
				.FirstValue = observation.FirstValue,
				.SecondValue = observation.SecondValue,
				.ExpectedTargetFromObjectArray = observation.ExpectedTargetFromObjectArray,
				.ExpectedTargetTypeValidated = observation.ExpectedTargetTypeValidated,
				.ReadableTwice = observation.FirstReadSucceeded && observation.SecondReadSucceeded,
				.Stable = observation.FirstReadSucceeded
					&& observation.SecondReadSucceeded
					&& observation.FirstValue == observation.SecondValue,
				.MatchesExpectedTarget = observation.FirstReadSucceeded
					&& observation.SecondReadSucceeded
					&& observation.ExpectedTarget != 0
					&& observation.FirstValue == observation.ExpectedTarget
					&& observation.SecondValue == observation.ExpectedTarget
			};

			evidence.AddressInImage = ContainsPointerSlot(
				image.Base,
				image.Size,
				observation.SlotAddress);
			evidence.PointerAligned = observation.SlotAddress % alignof(std::uintptr_t) == 0;
			if (evidence.AddressInImage)
			{
				evidence.ModuleOffset = static_cast<std::int64_t>(observation.SlotAddress - image.Base);
				const auto section = std::ranges::find_if(
					image.Sections,
					[&observation](const UExplorer::Platform::PeSectionView& candidateSection) {
						return ContainsPointerSlot(
							candidateSection.Address,
							candidateSection.Size,
							observation.SlotAddress);
					});
				evidence.WritableDataSection = section != image.Sections.end()
					&& section->IsReadable()
					&& section->IsWritable()
					&& !section->IsExecutable();
			}

			evidence.RejectionCode = RejectionCode(evidence);
			evidence.Accepted = evidence.RejectionCode.empty();
			if (evidence.Accepted
				&& std::ranges::find(acceptedAddresses, evidence.SlotAddress)
					== acceptedAddresses.end())
			{
				acceptedAddresses.push_back(evidence.SlotAddress);
			}
			report.Candidates.push_back(std::move(evidence));
		}

		if (acceptedAddresses.empty())
		{
			report.Error = GlobalPointerDiscoveryError::NoValidatedCandidate;
			return report;
		}
		if (acceptedAddresses.size() != 1)
		{
			report.Error = GlobalPointerDiscoveryError::AmbiguousCandidates;
			return report;
		}

		report.Error = GlobalPointerDiscoveryError::None;
		report.SelectedAddress = acceptedAddresses.front();
		report.SelectedOffset = static_cast<std::int32_t>(report.SelectedAddress - image.Base);
		report.Confidence = "high";
		return report;
	}
	catch (...)
	{
		return MakeGlobalPointerDiscoveryFailure(GlobalPointerDiscoveryError::AllocationFailed);
	}
}

} // namespace OffsetFinder
