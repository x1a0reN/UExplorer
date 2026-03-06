#include "EventsApi.h"
#include "ApiCommon.h"

#include <mutex>
#include <atomic>

namespace UExplorer::API
{

static std::mutex g_ServerMutex;
static HttpServer* g_Server = nullptr;

void SetServer(HttpServer* server)
{
	std::lock_guard<std::mutex> lk(g_ServerMutex);
	g_Server = server;
}

void RegisterEventsRoutes(HttpServer& server)
{
	server.Get("/api/v1/events/stream", [](const HttpRequest& req) -> HttpResponse {
		return { 200, "text/event-stream", "" };
	});

	server.Get("/api/v1/events/watches", [](const HttpRequest& req) -> HttpResponse {
		return { 200, "text/event-stream", "" };
	});

	server.Get("/api/v1/events/hooks", [](const HttpRequest& req) -> HttpResponse {
		return { 200, "text/event-stream", "" };
	});
}

void BroadcastHookEvent(const std::string& hookName, const std::string& data)
{
	std::lock_guard<std::mutex> lk(g_ServerMutex);
	if (g_Server)
	{
		g_Server->SendSSEEvent("hook", data);
	}
}

void BroadcastWatchEvent(int watchId, const std::string& data)
{
	std::lock_guard<std::mutex> lk(g_ServerMutex);
	if (g_Server)
	{
		g_Server->SendSSEEvent("watch", data);
	}
}

void BroadcastLogEvent(const std::string& level, const std::string& message)
{
	std::lock_guard<std::mutex> lk(g_ServerMutex);
	if (g_Server)
	{
		json evt;
		evt["level"] = level;
		evt["message"] = message;
		g_Server->SendSSEEvent("log", evt.dump());
	}
}

} // namespace UExplorer::API
