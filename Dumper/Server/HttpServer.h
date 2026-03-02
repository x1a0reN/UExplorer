#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace UExplorer
{

struct HttpRequest
{
	std::string Method;
	std::string Path;
	std::string Query;
	std::string Body;
	std::unordered_map<std::string, std::string> Headers;
};

struct HttpResponse
{
	int StatusCode = 200;
	std::string ContentType = "application/json";
	std::string Body;
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;
using SSEConnectHandler = std::function<void(const std::string& clientId)>;
using SSEEventHandler = std::function<void(const std::string& clientId, const std::string& event, const std::string& data)>;
using SSEDisconnectHandler = std::function<void(const std::string& clientId)>;

class HttpServer
{
public:
	HttpServer(uint16_t port, const std::string& token);
	~HttpServer();

	bool Start();
	void Stop();
	bool IsRunning() const;
	uint16_t GetPort() const;

	// Route registration
	void Get(const std::string& path, RouteHandler handler);
	void Post(const std::string& path, RouteHandler handler);
	void Patch(const std::string& path, RouteHandler handler);
	void Delete(const std::string& path, RouteHandler handler);

	// SSE support
	void SetSSEHandlers(SSEConnectHandler onConnect, SSEEventHandler onEvent, SSEDisconnectHandler onDisconnect);
	void SendSSEEvent(const std::string& event, const std::string& data);
	void SendSSEEventTo(const std::string& clientId, const std::string& event, const std::string& data);

private:
	class Impl;
	std::unique_ptr<Impl> m_Impl;
};

} // namespace UExplorer
