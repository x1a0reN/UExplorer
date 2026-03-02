#include "EventsApi.h"
#include "ApiCommon.h"

#include <mutex>
#include <set>
#include <atomic>

namespace UExplorer::API
{

// Server instance for broadcasting
static HttpServer* g_Server = nullptr;

void SetServer(HttpServer* server)
{
	g_Server = server;
}

void RegisterEventsRoutes(HttpServer& server)
{
	// GET /api/v1/events/stream — all events (hook + watch)
	server.Get("/api/v1/events/stream", [](const HttpRequest& req) -> HttpResponse {
		// SSE is handled at HTTP server level, this just returns a placeholder
		// The actual SSE connection is managed by HttpServer
		return { 200, "text/event-stream", "" };
	});

	// GET /api/v1/events/watches — watch change events only
	server.Get("/api/v1/events/watches", [](const HttpRequest& req) -> HttpResponse {
		return { 200, "text/event-stream", "" };
	});

	// GET /api/v1/events/hooks — hook hit events only
	server.Get("/api/v1/events/hooks", [](const HttpRequest& req) -> HttpResponse {
		return { 200, "text/event-stream", "" };
	});
}

void BroadcastHookEvent(const std::string& hookName, const std::string& data)
{
	if (g_Server)
	{
		g_Server->SendSSEEvent("hook", data);
	}
}

void BroadcastWatchEvent(int watchId, const std::string& data)
{
	if (g_Server)
	{
		g_Server->SendSSEEvent("watch", data);
	}
}

void BroadcastLogEvent(const std::string& level, const std::string& message)
{
	if (g_Server)
	{
		std::string data = "{\"level\":\"" + level + "\",\"message\":\"" + message + "\"}";
		g_Server->SendSSEEvent("log", data);
	}
}

} // namespace UExplorer::API
