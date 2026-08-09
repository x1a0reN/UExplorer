#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace UExplorer::Platform
{

inline bool TryParseBytePattern(
	const std::string_view text,
	std::vector<int>& pattern) noexcept
{
	pattern.clear();
	try
	{
		std::vector<int> parsedPattern;
		std::size_t cursor = 0;
		while (cursor < text.size())
		{
			while (cursor < text.size() && text[cursor] == ' ')
				++cursor;
			if (cursor == text.size())
				break;

			if (text[cursor] == '?')
			{
				++cursor;
				if (cursor < text.size() && text[cursor] == '?')
					++cursor;
				parsedPattern.push_back(-1);
			}
			else
			{
				if (text.size() - cursor < 2)
					return false;
				unsigned int value = 0;
				const char* begin = text.data() + cursor;
				const char* end = begin + 2;
				const auto parsed = std::from_chars(begin, end, value, 16);
				if (parsed.ec != std::errc{} || parsed.ptr != end || value > 0xFF)
					return false;
				parsedPattern.push_back(static_cast<int>(value));
				cursor += 2;
			}

			if (cursor < text.size() && text[cursor] != ' ')
				return false;
		}
		if (parsedPattern.empty())
			return false;
		pattern = std::move(parsedPattern);
		return true;
	}
	catch (...)
	{
		pattern.clear();
		return false;
	}
}

inline std::optional<std::size_t> FindBytePatternOffset(
	const std::span<const std::uint8_t> bytes,
	const std::span<const int> pattern,
	const std::size_t skipCount = 0) noexcept
{
	if (pattern.empty() || pattern.size() > bytes.size())
		return std::nullopt;
	for (const int value : pattern)
	{
		if (value < -1 || value > 0xFF)
			return std::nullopt;
	}

	std::size_t remainingSkips = skipCount;
	const std::size_t lastStart = bytes.size() - pattern.size();
	for (std::size_t offset = 0; offset <= lastStart; ++offset)
	{
		bool matches = true;
		for (std::size_t index = 0; index < pattern.size(); ++index)
		{
			if (pattern[index] != -1
				&& bytes[offset + index] != static_cast<std::uint8_t>(pattern[index]))
			{
				matches = false;
				break;
			}
		}
		if (!matches)
			continue;
		if (remainingSkips != 0)
		{
			--remainingSkips;
			continue;
		}
		return offset;
	}
	return std::nullopt;
}

} // namespace UExplorer::Platform
