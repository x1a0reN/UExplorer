#pragma once

#include "Runtime/CoreRuntime.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/WatchScheduler.h"
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

struct WatchCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct WatchCommandResult
{
	json Data = nullptr;
	std::optional<WatchCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

// Production sampling adapter for immutable object/type/property generations.
// Bind pins the exact snapshot/codec/property identity before publication;
// sampling stays binary on the game-thread path and never consults legacy
// Off/Settings state or serializes JSON from the hook callback.
class ObjectPropertyWatchSampleSource final : public Runtime::IWatchSampleSource
{
public:
	ObjectPropertyWatchSampleSource(
		Runtime::CoreRuntime& runtime,
		Runtime::EngineFacade& engine) noexcept;

	std::string_view SessionId() const noexcept override;
	std::uint64_t ContextGeneration() const noexcept override;
	bool IsCurrentExecutionThreadValid() const noexcept override;
	Runtime::WatchBindingResult Bind(
		const Runtime::WatchSubscriptionSpec& spec) override;
	Runtime::WatchSampleResult Sample(
		const Runtime::WatchSubscriptionSpec& spec,
		const std::shared_ptr<const Runtime::IWatchSampleBinding>& binding,
		std::size_t maxValueBytes) override;

private:
	Runtime::CoreRuntime& m_Runtime;
	Runtime::EngineFacade& m_Engine;
};

// Transport-neutral JSON boundary. CoreCommandService owns admission and
// capability gates; this service validates only the operation-specific data
// and delegates bounded state transitions to WatchScheduler.
class WatchCommandService final
{
public:
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;
	static constexpr std::size_t kMaxDrainEvents = 32;
	static constexpr std::size_t kMaxSnapshotHistory = 32;

	WatchCommandService(
		Runtime::WatchScheduler& scheduler,
		Runtime::CoreRuntime* runtime = nullptr,
		Runtime::EngineFacade* engine = nullptr) noexcept;

	WatchCommandResult Execute(
		std::string_view operation,
		const json& data) noexcept;

private:
	WatchCommandResult Add(const json& data) noexcept;
	WatchCommandResult List(const json& data) noexcept;
	WatchCommandResult Enable(const json& data) noexcept;
	WatchCommandResult Remove(const json& data) noexcept;
	WatchCommandResult Snapshot(const json& data) noexcept;
	WatchCommandResult DrainEvents(const json& data) noexcept;

	Runtime::WatchScheduler& m_Scheduler;
	Runtime::CoreRuntime* m_Runtime = nullptr;
	Runtime::EngineFacade* m_Engine = nullptr;
};

} // namespace UExplorer::Services
