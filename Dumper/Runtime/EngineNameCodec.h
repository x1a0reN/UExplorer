#pragma once

#include "EngineContext.h"

#include <cstdint>
#include <string>

namespace UExplorer::Runtime
{

enum class EngineNameError : std::uint8_t
{
	None,
	InvalidProfile,
	InvalidAddress,
	IndexOutOfRange,
	LayoutInvalid,
	EntryUnavailable,
	HeaderInvalid,
	NameTooLong,
	EncodingInvalid,
	RedirectCycle,
	NumberInvalid
};

const char* ToString(EngineNameError error) noexcept;

struct EngineNameResult
{
	EngineNameError Error = EngineNameError::None;
	std::string Value;

	bool Ok() const noexcept { return Error == EngineNameError::None; }
};

bool IsEngineNameProfileLayoutValid(const EngineNameProfile& profile) noexcept;

// Decodes names only through a frozen EngineContext profile and checked memory reads.
class EngineNameCodec final
{
public:
	explicit EngineNameCodec(EngineNameProfile profile);

	bool IsConfigured() const noexcept { return m_Configured; }
	const EngineNameProfile& Profile() const noexcept { return m_Profile; }
	EngineNameResult DecodeFName(std::uintptr_t fnameAddress) const noexcept;
	EngineNameResult Decode(std::uint32_t comparisonIndex, std::uint32_t number = 0) const noexcept;

private:
	EngineNameResult DecodeNamePool(std::uint32_t comparisonIndex) const;
	EngineNameResult DecodeChunkedArray(std::uint32_t comparisonIndex) const;

	EngineNameProfile m_Profile;
	bool m_Configured = false;
};

} // namespace UExplorer::Runtime
