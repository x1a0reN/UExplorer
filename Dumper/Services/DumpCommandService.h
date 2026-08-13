#pragma once

#include "Runtime/DumpJobCoordinator.h"
#include "Utils/Json/json.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct DumpCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct DumpCommandResult
{
	json Data = nullptr;
	std::optional<DumpCommandError> Error;

	bool Ok() const noexcept { return !Error.has_value(); }
};

// Transport-neutral boundary for owned dump jobs. A null coordinator means
// every operation fails closed. A non-null coordinator must outlive this
// service; start requests also require a caller-pinned immutable input.
class DumpCommandService final
{
public:
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;
	static constexpr std::size_t kMaxListJobs = 64;
	static constexpr std::size_t kMaxGetEvents = 512;

	explicit DumpCommandService(Runtime::DumpJobCoordinator* coordinator) noexcept;

	static bool Handles(std::string_view operation) noexcept;
	DumpCommandResult Execute(
		std::string_view operation,
		const json& data,
		std::shared_ptr<const Runtime::IDumpJobInput> input = nullptr) noexcept;

private:
	DumpCommandResult Start(
		std::string_view operation,
		const json& data,
		std::shared_ptr<const Runtime::IDumpJobInput> input);
	DumpCommandResult List(const json& data);
	DumpCommandResult Get(const json& data);
	DumpCommandResult Cancel(const json& data);

	Runtime::DumpJobCoordinator* m_Coordinator = nullptr;
};

} // namespace UExplorer::Services
