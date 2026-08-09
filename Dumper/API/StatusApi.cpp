#include "StatusApi.h"
#include "ApiCommon.h"

#include "Services/CoreCommandServiceAccess.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace UExplorer::API
{
namespace
{

std::atomic<std::uint64_t> g_LegacyStatusRequestId{1};

Services::CoreCommandResponse ExecuteStatusCommand(const std::string& operation)
{
	Services::CoreCommandService* service = Services::GetCoreCommandService();
	if (!service)
	{
		return {
			.Ok = false,
			.Error = Services::CoreCommandError{
				.Code = "CORE_COMMAND_SERVICE_UNAVAILABLE",
				.Message = "Core command service is unavailable"
			}
		};
	}

	std::uint64_t requestId = g_LegacyStatusRequestId.fetch_add(1, std::memory_order_relaxed);
	if (requestId == 0)
		requestId = g_LegacyStatusRequestId.fetch_add(1, std::memory_order_relaxed);
	return service->Execute({
		.RequestId = requestId,
		.Operation = operation,
		.SessionId = service->SessionId(),
		.TimeoutMs = 5000,
		.Data = Services::json::object()
	});
}

int ErrorHttpStatus(const std::string& code)
{
	if (code == "RECONNECT_DISABLED" || code == "SESSION_MISMATCH")
		return 409;
	if (code == "INVALID_ARGUMENT"
		|| code == "REQUEST_ID_INVALID"
		|| code == "OPERATION_INVALID"
		|| code == "TIMEOUT_INVALID")
	{
		return 400;
	}
	if (code == "CORE_COMMAND_SERVICE_UNAVAILABLE"
		|| code == "COMMAND_SERVICE_NOT_CONFIGURED"
		|| code == "COMMAND_SERVICE_STALE"
		|| code == "CORE_CONTEXT_UNAVAILABLE"
		|| code == "CORE_NOT_READY"
		|| code == "CORE_STOPPING")
	{
		return 503;
	}
	return 500;
}

HttpResponse AdaptStatusResponse(
	const Services::CoreCommandResponse& response,
	const bool health = false)
{
	if (response.Ok)
	{
		const int status = health && !response.Data.value("liveness", false) ? 503 : 200;
		return {status, "application/json", MakeResponse(response.Data)};
	}

	const Services::CoreCommandError error = response.Error.value_or(
		Services::CoreCommandError{
			.Code = "COMMAND_RESPONSE_INVALID",
			.Message = "Core command failed without an error payload"
		});
	json details = error.Details.is_object() ? error.Details : json::object();
	if (!error.Message.empty())
		details["message"] = error.Message;
	return {
		ErrorHttpStatus(error.Code),
		"application/json",
		MakeError(error.Code, details)
	};
}

} // namespace

void RegisterStatusRoutes(HttpServer& server)
{
	server.Get("/api/v1/status/engine", [](const HttpRequest&) -> HttpResponse {
		return AdaptStatusResponse(ExecuteStatusCommand("status.engine"));
	});

	server.Post("/api/v1/status/reconnect", [](const HttpRequest&) -> HttpResponse {
		return AdaptStatusResponse(ExecuteStatusCommand("status.reconnect"));
	});

	server.Get("/api/v1/status/health", [](const HttpRequest&) -> HttpResponse {
		return AdaptStatusResponse(ExecuteStatusCommand("status.health"), true);
	});

	server.Get("/api/v1/status", [](const HttpRequest&) -> HttpResponse {
		return AdaptStatusResponse(ExecuteStatusCommand("status.inspect"));
	});
}

} // namespace UExplorer::API
