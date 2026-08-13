#pragma once

#include "Runtime/BlueprintBytecodeEvidence.h"
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

struct BlueprintCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct BlueprintCommandResult
{
	json Data = nullptr;
	std::optional<BlueprintCommandError> Error;

	bool Ok() const noexcept { return !Error.has_value(); }
};

// The service accepts only generation-bound immutable metadata and injected
// byte/profile evidence. It never reads a UFunction, Off:: value, or global UE state.
class BlueprintCommandService final
{
public:
	static constexpr std::size_t kMaxScriptBytes = 1024 * 1024;
	static constexpr std::size_t kMaxResponseBytes = 4 * 1024 * 1024;

	static bool Handles(std::string_view operation) noexcept;
	static BlueprintCommandResult Execute(
		std::shared_ptr<const Runtime::TypeSnapshot> snapshot,
		std::string_view operation,
		const json& data,
		std::string_view expectedSessionId,
		std::uint64_t expectedContextGeneration,
		std::uint64_t expectedObjectSnapshotGeneration,
		Runtime::IBlueprintBytecodeCaptureSource* captureSource,
		const Runtime::IBlueprintBytecodeProfileSource* profileSource) noexcept;
};

} // namespace UExplorer::Services
