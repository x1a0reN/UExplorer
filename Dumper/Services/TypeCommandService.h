#pragma once

#include "Runtime/TypeSnapshot.h"
#include "Utils/Json/json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct TypeCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct TypeCommandResult
{
	json Data = nullptr;
	std::optional<TypeCommandError> Error;

	bool Ok() const noexcept { return !Error.has_value(); }
};

// Serializes only an already-published immutable TypeSnapshot. No command in
// this service reads live UE memory or enters the game-thread queue.
class TypeCommandService final
{
public:
	static constexpr std::size_t kMaxPageRecords = 128;
	static constexpr std::size_t kMaxFunctionParameters = 128;
	static constexpr std::size_t kMaxResponseBytes = 4 * 1024 * 1024;

	static bool Handles(std::string_view operation) noexcept;
	static TypeCommandResult Execute(
		std::shared_ptr<const Runtime::TypeSnapshot> snapshot,
		std::string_view operation,
		const json& data,
		std::string_view expectedSessionId,
		std::uint64_t expectedContextGeneration,
		std::uint64_t expectedObjectSnapshotGeneration) noexcept;
};

} // namespace UExplorer::Services
