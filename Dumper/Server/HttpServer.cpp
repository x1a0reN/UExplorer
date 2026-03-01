#include "HttpServer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <iostream>
#include <regex>
#include <sstream>
#include <algorithm>

namespace UExplorer
{

struct RouteEntry
{
	std::string Method;
	std::string Pattern;
	std::regex Regex;
	RouteHandler Handler;
};

// ---- Impl class (WinSock2) ----
class HttpServer::Impl
{
public:
	uint16_t Port;
	std::string Token;
	std::vector<RouteEntry> Routes;
	std::mutex RoutesMutex;

	SOCKET ListenSocket = INVALID_SOCKET;
	std::thread ServerThread;
	std::atomic<bool> Running{ false };

	Impl(uint16_t port, const std::string& token)
		: Port(port), Token(token) {}

	void AddRoute(const std::string& method, const std::string& pattern, RouteHandler handler)
	{
		std::lock_guard<std::mutex> lock(RoutesMutex);
		std::string regexStr = std::regex_replace(pattern, std::regex(":([a-zA-Z_]+)"), "([^/]+)");
		regexStr = "^" + regexStr + "$";
		Routes.push_back({ method, pattern, std::regex(regexStr), std::move(handler) });
	}

	bool ValidateToken(const std::string& headerValue) const
	{
		return headerValue == Token;
	}

	HttpResponse MatchAndHandle(const HttpRequest& req)
	{
		// Find matching handler under lock, then execute OUTSIDE the lock
		// so a blocking handler doesn't starve all other requests
		RouteHandler handler;
		{
			std::lock_guard<std::mutex> lock(RoutesMutex);
			for (const auto& route : Routes)
			{
				if (route.Method != req.Method) continue;
				std::smatch match;
				if (std::regex_match(req.Path, match, route.Regex))
				{
					handler = route.Handler;
					break;
				}
			}
		}

		if (!handler)
			return { 404, "application/json", R"({"success":false,"error":"Not Found"})" };

		try {
			return handler(req);
		}
		catch (const std::exception& e) {
			std::string err = std::string(R"({"success":false,"error":"Internal error: )") + e.what() + R"("})";
			return { 500, "application/json", err };
		}
		catch (...) {
			return { 500, "application/json", R"({"success":false,"error":"Unknown internal error"})" };
		}
	}

	// Read all data from socket until Content-Length is satisfied or connection closes
	std::string RecvAll(SOCKET sock, int contentLength)
	{
		std::string result;
		result.reserve(contentLength);
		char buf[4096];
		while ((int)result.size() < contentLength)
		{
			int toRead = std::min((int)sizeof(buf), contentLength - (int)result.size());
			int n = recv(sock, buf, toRead, 0);
			if (n <= 0) break;
			result.append(buf, n);
		}
		return result;
	}

	// Parse raw HTTP request header block
	HttpRequest ParseRequest(const std::string& raw, SOCKET sock)
	{
		HttpRequest req;
		std::istringstream stream(raw);
		std::string line;

		// Request line: "GET /path?query HTTP/1.1"
		if (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			auto sp1 = line.find(' ');
			auto sp2 = line.rfind(' ');
			if (sp1 != std::string::npos && sp2 != sp1)
			{
				req.Method = line.substr(0, sp1);
				std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
				auto qpos = target.find('?');
				if (qpos != std::string::npos)
				{
					req.Path = target.substr(0, qpos);
					req.Query = target.substr(qpos + 1);
				}
				else
				{
					req.Path = target;
				}
			}
		}

		// Headers
		int contentLength = 0;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) break;
			auto colon = line.find(':');
			if (colon != std::string::npos)
			{
				std::string key = line.substr(0, colon);
				std::string val = line.substr(colon + 1);
				// trim leading space
				if (!val.empty() && val[0] == ' ') val = val.substr(1);
				req.Headers[key] = val;
				// case-insensitive Content-Length check
				std::string keyLower = key;
				std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
				if (keyLower == "content-length")
					contentLength = std::stoi(val);
			}
		}

		// Body
		if (contentLength > 0)
		{
			// Some body bytes may already be in the raw buffer after headers
			auto headerEnd = raw.find("\r\n\r\n");
			std::string partial;
			if (headerEnd != std::string::npos)
				partial = raw.substr(headerEnd + 4);

			if ((int)partial.size() >= contentLength)
			{
				req.Body = partial.substr(0, contentLength);
			}
			else
			{
				req.Body = partial;
				int remaining = contentLength - (int)partial.size();
				req.Body += RecvAll(sock, remaining);
			}
		}

		return req;
	}

	std::string BuildResponse(const HttpResponse& resp)
	{
		std::ostringstream oss;
		oss << "HTTP/1.1 " << resp.StatusCode << " ";
		switch (resp.StatusCode)
		{
		case 200: oss << "OK"; break;
		case 204: oss << "No Content"; break;
		case 400: oss << "Bad Request"; break;
		case 401: oss << "Unauthorized"; break;
		case 404: oss << "Not Found"; break;
		case 500: oss << "Internal Server Error"; break;
		default:  oss << "Unknown"; break;
		}
		oss << "\r\n";
		oss << "Content-Type: " << resp.ContentType << "\r\n";
		oss << "Content-Length: " << resp.Body.size() << "\r\n";
		oss << "Access-Control-Allow-Origin: *\r\n";
		oss << "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
		oss << "Access-Control-Allow-Headers: Content-Type, X-UExplorer-Token\r\n";
		oss << "Connection: close\r\n";
		oss << "\r\n";
		oss << resp.Body;
		return oss.str();
	}

	void HandleClient(SOCKET clientSock)
	{
		try {
			// Read request (up to 64KB header)
			char buf[65536];
			int total = 0;
			std::string raw;

			while (total < (int)sizeof(buf) - 1)
			{
				int n = recv(clientSock, buf + total, (int)sizeof(buf) - 1 - total, 0);
				if (n <= 0) break;
				total += n;
				buf[total] = '\0';
				if (strstr(buf, "\r\n\r\n")) break;
			}
			raw.assign(buf, total);

			if (raw.empty())
			{
				closesocket(clientSock);
				return;
			}

			HttpRequest req = ParseRequest(raw, clientSock);

			// OPTIONS preflight
			if (req.Method == "OPTIONS")
			{
				HttpResponse resp{ 204, "text/plain", "" };
				std::string out = BuildResponse(resp);
				send(clientSock, out.c_str(), (int)out.size(), 0);
				closesocket(clientSock);
				return;
			}

			// Token auth (skip for health)
			if (req.Path != "/api/v1/status/health")
			{
				auto it = req.Headers.find("X-UExplorer-Token");
				if (it == req.Headers.end() || !ValidateToken(it->second))
				{
					HttpResponse resp{ 401, "application/json", R"({"success":false,"error":"Unauthorized"})" };
					std::string out = BuildResponse(resp);
					send(clientSock, out.c_str(), (int)out.size(), 0);
					closesocket(clientSock);
					return;
				}
			}

			// Dispatch
			HttpResponse resp = MatchAndHandle(req);
			std::string out = BuildResponse(resp);
			send(clientSock, out.c_str(), (int)out.size(), 0);
			closesocket(clientSock);
		}
		catch (...) {
			// Safety net: never let an exception escape and crash the game
			try { closesocket(clientSock); } catch (...) {}
		}
	}

	bool Bind()
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			std::cerr << "[UExplorer] WSAStartup failed\n";
			return false;
		}

		ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (ListenSocket == INVALID_SOCKET)
		{
			std::cerr << "[UExplorer] socket() failed\n";
			WSACleanup();
			return false;
		}

		int opt = 1;
		setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

		// Try requested port, then fallback to a few alternatives
		uint16_t ports[] = { Port, 27015, 27016, 27017, 27018, 0 }; // 0 = OS picks
		bool bound = false;

		for (uint16_t p : ports)
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = htons(p);

			if (bind(ListenSocket, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR)
			{
				// If OS picked (port 0), read back the actual port
				if (p == 0)
				{
					sockaddr_in bound_addr{};
					int len = sizeof(bound_addr);
					getsockname(ListenSocket, (sockaddr*)&bound_addr, &len);
					Port = ntohs(bound_addr.sin_port);
				}
				else
				{
					Port = p;
				}
				bound = true;
				break;
			}
			std::cerr << "[UExplorer] bind() port " << p << " failed: 0x"
				<< std::hex << WSAGetLastError() << std::dec << "\n";
		}

		if (!bound)
		{
			std::cerr << "[UExplorer] All ports failed\n";
			closesocket(ListenSocket);
			ListenSocket = INVALID_SOCKET;
			WSACleanup();
			return false;
		}

		if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR)
		{
			std::cerr << "[UExplorer] listen() failed\n";
			closesocket(ListenSocket);
			ListenSocket = INVALID_SOCKET;
			WSACleanup();
			return false;
		}

		std::cerr << "[UExplorer] HTTP server listening on 127.0.0.1:" << Port << "\n";
		return true;
	}

	void AcceptLoop()
	{
		u_long nonBlocking = 1;
		ioctlsocket(ListenSocket, FIONBIO, &nonBlocking);

		while (Running.load())
		{
			SOCKET client = accept(ListenSocket, nullptr, nullptr);
			if (client == INVALID_SOCKET)
			{
				if (WSAGetLastError() == WSAEWOULDBLOCK)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				continue;
			}

			std::thread([this, client]() {
				HandleClient(client);
			}).detach();
		}

		closesocket(ListenSocket);
		ListenSocket = INVALID_SOCKET;
		WSACleanup();
	}
}; // end Impl

// ---- Public API ----
HttpServer::HttpServer(uint16_t port, const std::string& token)
	: m_Impl(std::make_unique<Impl>(port, token)) {}

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start()
{
	if (m_Impl->Running.load()) return true;
	if (!m_Impl->Bind()) return false;
	m_Impl->Running.store(true);
	m_Impl->ServerThread = std::thread([this]() { m_Impl->AcceptLoop(); });
	return true;
}

void HttpServer::Stop()
{
	m_Impl->Running.store(false);
	if (m_Impl->ListenSocket != INVALID_SOCKET)
		closesocket(m_Impl->ListenSocket);
	if (m_Impl->ServerThread.joinable())
		m_Impl->ServerThread.join();
}

bool HttpServer::IsRunning() const { return m_Impl->Running.load(); }
uint16_t HttpServer::GetPort() const { return m_Impl->Port; }

void HttpServer::Get(const std::string& path, RouteHandler handler)
{ m_Impl->AddRoute("GET", path, std::move(handler)); }

void HttpServer::Post(const std::string& path, RouteHandler handler)
{ m_Impl->AddRoute("POST", path, std::move(handler)); }

void HttpServer::Patch(const std::string& path, RouteHandler handler)
{ m_Impl->AddRoute("PATCH", path, std::move(handler)); }

void HttpServer::Delete(const std::string& path, RouteHandler handler)
{ m_Impl->AddRoute("DELETE", path, std::move(handler)); }

} // namespace UExplorer
