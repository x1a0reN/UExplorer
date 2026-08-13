#pragma once

#include "Utils/Json/json.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct MemoryCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct MemoryCommandResult
{
	json Data = nullptr;
	std::optional<MemoryCommandError> Error;

	bool Ok() const noexcept { return !Error.has_value(); }
};

// Transport-neutral bounded access to process memory. This service only uses
// SafeMemory and never interprets raw bytes as live UE object state.
class MemoryCommandService final
{
public:
	static constexpr std::size_t kMaxReadBytes = 4096;
	static constexpr std::size_t kMaxWriteBytes = 4096;
	static constexpr std::size_t kMaxPointerOffsets = 64;

	static bool Handles(std::string_view operation) noexcept;
	static MemoryCommandResult Execute(
		std::string_view operation,
		const json& data) noexcept;
};

} // namespace UExplorer::Services
