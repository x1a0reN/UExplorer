#pragma once

#include "FunctionCallCommandService.h"
#include "Runtime/FunctionCallBatchCoordinator.h"
#include "Utils/Json/json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UExplorer::Services
{

using json = nlohmann::json;

struct FunctionCallBatchCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct FunctionCallBatchCommandResult
{
	json Data = nullptr;
	std::optional<FunctionCallBatchCommandError> Error;

	bool Ok() const noexcept { return !Error.has_value(); }
};

// This is the immutable, already identity-checked input presented to the
// exact single-call adapter. CallData has the closed call.invoke shape accepted
// by FunctionCallCommandService. The adapter must not retain any reference.
struct FunctionCallBatchInvokeRequest
{
	Runtime::FunctionCallBatchBinding Binding;
	Runtime::ObjectHandle Target;
	Runtime::FunctionHandle Function;
	std::string FunctionPath;
	json CallData = json::object();
	std::chrono::steady_clock::time_point Deadline;
};

// Core owns the concrete adapter. It must route InvokeExact through the same
// FunctionCallCommandService PrepareInvoke/game-thread/CompleteInvoke path as
// call.invoke; direct ProcessEvent or raw-memory fallbacks are forbidden.
class IFunctionCallBatchInvokeAdapter
{
public:
	virtual ~IFunctionCallBatchInvokeAdapter() = default;

	virtual FunctionCallCommandResult InvokeExact(
		const FunctionCallBatchInvokeRequest& request,
		Runtime::IFunctionCallBatchExecutionContext& context) noexcept = 0;
};

// Coordinator-owned worker bridge. The coordinator retains this object, which
// in turn retains the adapter, until StopAndDrain/destruction joins its worker.
class FunctionCallBatchCommandWorker final
	: public Runtime::IFunctionCallBatchWorker
{
public:
	explicit FunctionCallBatchCommandWorker(
		std::shared_ptr<IFunctionCallBatchInvokeAdapter> adapter) noexcept;

	bool IsConfigured() const noexcept { return static_cast<bool>(m_Adapter); }

	Runtime::FunctionCallBatchWorkerResult Execute(
		const std::shared_ptr<const Runtime::FunctionCallBatchRequest>& request,
		std::size_t itemIndex,
		Runtime::IFunctionCallBatchExecutionContext& context) override;

private:
	std::shared_ptr<IFunctionCallBatchInvokeAdapter> m_Adapter;
};

// Transport-neutral call.batch command boundary. The coordinator must outlive
// this service. Only batches admitted through this service appear in List;
// callers must not submit directly to the coordinator behind this boundary.
class FunctionCallBatchCommandService final
{
public:
	static constexpr std::size_t kMaxItems = 64;
	static constexpr std::size_t kMaxListedBatches = 64;
	static constexpr std::size_t kMaxSerializedRequestBytesPerItem = 256 * 1024;
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;

	explicit FunctionCallBatchCommandService(
		Runtime::FunctionCallBatchCoordinator* coordinator) noexcept;

	FunctionCallBatchCommandService(const FunctionCallBatchCommandService&) = delete;
	FunctionCallBatchCommandService& operator=(const FunctionCallBatchCommandService&) = delete;

	bool IsConfigured() const noexcept;
	static bool Handles(std::string_view operation) noexcept;
	FunctionCallBatchCommandResult Execute(
		std::string_view operation,
		const json& data) noexcept;

private:
	FunctionCallBatchCommandResult Submit(const json& data);
	FunctionCallBatchCommandResult Get(const json& data) const;
	FunctionCallBatchCommandResult Cancel(const json& data);
	FunctionCallBatchCommandResult List(const json& data) const;
	void RememberAdmission(Runtime::FunctionCallBatchId id) noexcept;
	std::vector<Runtime::FunctionCallBatchId> AdmissionIdsNewestFirst() const;

	Runtime::FunctionCallBatchCoordinator* m_Coordinator = nullptr;
	mutable std::mutex m_AdmissionsMutex;
	std::array<Runtime::FunctionCallBatchId, kMaxListedBatches> m_AdmissionIds{};
	std::size_t m_AdmissionCount = 0;
	std::size_t m_NextAdmissionSlot = 0;
};

} // namespace UExplorer::Services
