#pragma once

#include "Runtime/WorldSnapshot.h"
#include "Utils/Json/json.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct WorldCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct WorldCommandResult
{
	json Data = nullptr;
	std::optional<WorldCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

class WorldCommandService final
{
public:
	static constexpr std::size_t kMaxPageRecords = 128;
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;

	static bool Handles(std::string_view operation) noexcept;
	static WorldCommandResult Execute(
		std::string_view operation,
		const json& data,
		std::shared_ptr<const Runtime::WorldSnapshot> snapshot) noexcept;
};

} // namespace UExplorer::Services
