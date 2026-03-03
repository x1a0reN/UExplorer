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
#include <Wincrypt.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Advapi32.lib")

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

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
	struct SSEClientInfo
	{
		std::string ClientId;
		std::string Path;
	};

	struct WSClientInfo
	{
		std::string ClientId;
		std::string Path;
	};

public:
	uint16_t Port;
	std::string Token;
	std::vector<RouteEntry> Routes;
	std::mutex RoutesMutex;

	SOCKET ListenSocket = INVALID_SOCKET;
	std::thread ServerThread;
	std::atomic<bool> Running{ false };

	// SSE support
	std::mutex SSEClientsMutex;
	std::unordered_map<SOCKET, SSEClientInfo> SSEClients; // socket -> info
	std::atomic<int> SSEClientCounter{ 0 };
	SSEConnectHandler OnSSEConnect;
	SSEEventHandler OnSSEEvent;
	SSEDisconnectHandler OnSSEDisconnect;

	// WebSocket support
	std::mutex WSClientsMutex;
	std::unordered_map<SOCKET, WSClientInfo> WSClients;
	std::atomic<int> WSClientCounter{ 0 };

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

	static std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return s;
	}

	static std::string EscapeJson(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 16);
		for (char ch : s)
		{
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(ch) < 0x20)
				{
					char tmp[7] = { 0 };
					sprintf_s(tmp, "\\u%04X", static_cast<unsigned char>(ch));
					out += tmp;
				}
				else
				{
					out.push_back(ch);
				}
				break;
			}
		}
		return out;
	}

	HttpResponse MatchAndHandle(const HttpRequest& req)
	{
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

	std::string RecvAll(SOCKET sock, int contentLength)
	{
		std::string result;
		result.reserve(contentLength);
		char buf[4096];
		while (static_cast<int>(result.size()) < contentLength)
		{
			int toRead = std::min(static_cast<int>(sizeof(buf)), contentLength - static_cast<int>(result.size()));
			int n = recv(sock, buf, toRead, 0);
			if (n <= 0) break;
			result.append(buf, n);
		}
		return result;
	}

	HttpRequest ParseRequest(const std::string& raw, SOCKET sock)
	{
		HttpRequest req;
		std::istringstream stream(raw);
		std::string line;

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
				if (!val.empty() && val[0] == ' ') val = val.substr(1);
				req.Headers[key] = val;

				std::string keyLower = ToLower(key);
				if (keyLower == "content-length")
					contentLength = std::stoi(val);
			}
		}

		if (contentLength > 0)
		{
			auto headerEnd = raw.find("\r\n\r\n");
			std::string partial;
			if (headerEnd != std::string::npos)
				partial = raw.substr(headerEnd + 4);

			if (static_cast<int>(partial.size()) >= contentLength)
			{
				req.Body = partial.substr(0, contentLength);
			}
			else
			{
				req.Body = partial;
				int remaining = contentLength - static_cast<int>(partial.size());
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
		case 101: oss << "Switching Protocols"; break;
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

	std::string GetHeaderValue(const HttpRequest& req, const std::string& key) const
	{
		const std::string needle = ToLower(key);
		for (const auto& [k, v] : req.Headers)
		{
			if (ToLower(k) == needle)
				return v;
		}
		return "";
	}

	static std::string GetQueryValue(const std::string& query, const std::string& key)
	{
		size_t start = 0;
		while (start < query.size())
		{
			size_t amp = query.find('&', start);
			std::string pair = (amp == std::string::npos)
				? query.substr(start)
				: query.substr(start, amp - start);

			size_t eq = pair.find('=');
			if (eq != std::string::npos)
			{
				std::string k = pair.substr(0, eq);
				if (k == key)
					return pair.substr(eq + 1);
			}

			if (amp == std::string::npos) break;
			start = amp + 1;
		}
		return "";
	}

	bool IsWebSocketUpgrade(const HttpRequest& req) const
	{
		std::string upgrade = ToLower(GetHeaderValue(req, "Upgrade"));
		std::string connection = ToLower(GetHeaderValue(req, "Connection"));
		return req.Method == "GET"
			&& upgrade == "websocket"
			&& connection.find("upgrade") != std::string::npos;
	}

	static bool SendAll(SOCKET sock, const char* data, int len)
	{
		int sent = 0;
		while (sent < len)
		{
			int n = send(sock, data + sent, len - sent, 0);
			if (n <= 0) return false;
			sent += n;
		}
		return true;
	}

	static bool RecvExact(SOCKET sock, uint8_t* out, size_t len)
	{
		size_t got = 0;
		while (got < len)
		{
			int n = recv(sock, reinterpret_cast<char*>(out + got), static_cast<int>(len - got), 0);
			if (n <= 0) return false;
			got += static_cast<size_t>(n);
		}
		return true;
	}

	static std::string ComputeWebSocketAccept(const std::string& clientKey)
	{
		const std::string source = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

		HCRYPTPROV prov = 0;
		HCRYPTHASH hash = 0;
		std::array<BYTE, 20> digest{};
		DWORD digestLen = static_cast<DWORD>(digest.size());
		std::string out;

		if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
			return out;
		if (!CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash))
		{
			CryptReleaseContext(prov, 0);
			return out;
		}
		if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(source.data()), static_cast<DWORD>(source.size()), 0))
		{
			CryptDestroyHash(hash);
			CryptReleaseContext(prov, 0);
			return out;
		}
		if (!CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestLen, 0))
		{
			CryptDestroyHash(hash);
			CryptReleaseContext(prov, 0);
			return out;
		}

		DWORD b64Len = 0;
		if (CryptBinaryToStringA(digest.data(), digestLen,
			CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len))
		{
			out.resize(b64Len);
			if (CryptBinaryToStringA(digest.data(), digestLen,
				CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &b64Len))
			{
				if (!out.empty() && out.back() == '\0') out.pop_back();
			}
			else
			{
				out.clear();
			}
		}

		CryptDestroyHash(hash);
		CryptReleaseContext(prov, 0);
		return out;
	}

	bool SendWebSocketFrame(SOCKET sock, uint8_t opcode, const std::string& payload)
	{
		std::vector<uint8_t> frame;
		frame.reserve(payload.size() + 16);
		frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F))); // FIN + opcode

		const uint64_t len = payload.size();
		if (len <= 125)
		{
			frame.push_back(static_cast<uint8_t>(len));
		}
		else if (len <= 0xFFFF)
		{
			frame.push_back(126);
			frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
			frame.push_back(static_cast<uint8_t>(len & 0xFF));
		}
		else
		{
			frame.push_back(127);
			for (int i = 7; i >= 0; --i)
			{
				frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
			}
		}

		frame.insert(frame.end(), payload.begin(), payload.end());
		return SendAll(sock, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()));
	}

	bool ReadWebSocketFrame(SOCKET sock, uint8_t& opcode, std::string& payload)
	{
		uint8_t h[2] = { 0 };
		if (!RecvExact(sock, h, 2))
			return false;

		opcode = static_cast<uint8_t>(h[0] & 0x0F);
		const bool masked = (h[1] & 0x80) != 0;
		uint64_t len = static_cast<uint64_t>(h[1] & 0x7F);

		if (len == 126)
		{
			uint8_t ext[2] = { 0 };
			if (!RecvExact(sock, ext, 2)) return false;
			len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
		}
		else if (len == 127)
		{
			uint8_t ext[8] = { 0 };
			if (!RecvExact(sock, ext, 8)) return false;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | ext[i];
		}

		if (len > 4 * 1024 * 1024)
			return false;

		uint8_t mask[4] = { 0 };
		if (masked)
		{
			if (!RecvExact(sock, mask, 4)) return false;
		}

		payload.assign(static_cast<size_t>(len), '\0');
		if (len > 0)
		{
			if (!RecvExact(sock, reinterpret_cast<uint8_t*>(payload.data()), static_cast<size_t>(len)))
				return false;
		}

		if (masked)
		{
			for (size_t i = 0; i < payload.size(); i++)
				payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
		}

		return true;
	}

	void BroadcastWebSocketEvent(const std::string& event, const std::string& data)
	{
		std::lock_guard<std::mutex> lk(WSClientsMutex);
		const std::string payload = "{\"event\":\"" + EscapeJson(event)
			+ "\",\"data\":\"" + EscapeJson(data) + "\"}";

		for (const auto& [sock, info] : WSClients)
		{
			if (info.Path != "/api/v1/ws/events") continue;
			SendWebSocketFrame(sock, 0x1, payload);
		}
	}

	void HandleWebSocketClient(SOCKET clientSock, const HttpRequest& req)
	{
		const std::string path = req.Path;
		if (path != "/api/v1/ws/console" && path != "/api/v1/ws/events")
		{
			HttpResponse resp{ 404, "application/json", R"({"success":false,"error":"WebSocket path not found"})" };
			std::string out = BuildResponse(resp);
			send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
			closesocket(clientSock);
			return;
		}

		const std::string key = GetHeaderValue(req, "Sec-WebSocket-Key");
		if (key.empty())
		{
			HttpResponse resp{ 400, "application/json", R"({"success":false,"error":"Missing Sec-WebSocket-Key"})" };
			std::string out = BuildResponse(resp);
			send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
			closesocket(clientSock);
			return;
		}

		const std::string accept = ComputeWebSocketAccept(key);
		if (accept.empty())
		{
			HttpResponse resp{ 500, "application/json", R"({"success":false,"error":"WebSocket handshake failed"})" };
			std::string out = BuildResponse(resp);
			send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
			closesocket(clientSock);
			return;
		}

		std::ostringstream hs;
		hs << "HTTP/1.1 101 Switching Protocols\r\n";
		hs << "Upgrade: websocket\r\n";
		hs << "Connection: Upgrade\r\n";
		hs << "Sec-WebSocket-Accept: " << accept << "\r\n";
		hs << "Access-Control-Allow-Origin: *\r\n";
		hs << "\r\n";

		const std::string hsResp = hs.str();
		if (!SendAll(clientSock, hsResp.c_str(), static_cast<int>(hsResp.size())))
		{
			closesocket(clientSock);
			return;
		}

		const int wsIdNum = ++WSClientCounter;
		const std::string clientId = "ws-" + std::to_string(wsIdNum);
		{
			std::lock_guard<std::mutex> lk(WSClientsMutex);
			WSClients[clientSock] = WSClientInfo{ clientId, path };
		}

		std::string connected = "{\"type\":\"connected\",\"clientId\":\"" + EscapeJson(clientId)
			+ "\",\"path\":\"" + EscapeJson(path) + "\"}";
		SendWebSocketFrame(clientSock, 0x1, connected);

		while (Running.load())
		{
			fd_set readfds;
			timeval tv;
			FD_ZERO(&readfds);
			FD_SET(clientSock, &readfds);
			tv.tv_sec = 0;
			tv.tv_usec = 500000;
			int sel = select(0, &readfds, nullptr, nullptr, &tv);
			if (sel < 0) break;
			if (sel == 0) continue;

			uint8_t opcode = 0;
			std::string payload;
			if (!ReadWebSocketFrame(clientSock, opcode, payload))
				break;

			if (opcode == 0x8)
			{
				SendWebSocketFrame(clientSock, 0x8, "");
				break;
			}
			if (opcode == 0x9)
			{
				SendWebSocketFrame(clientSock, 0xA, payload);
				continue;
			}
			if (opcode != 0x1)
				continue;

			if (path == "/api/v1/ws/console")
			{
				std::string response = "{\"type\":\"console\",\"ok\":true,\"input\":\""
					+ EscapeJson(payload)
					+ "\",\"output\":\"Console bridge connected\"}";
				SendWebSocketFrame(clientSock, 0x1, response);
			}
			else if (path == "/api/v1/ws/events")
			{
				if (payload == "ping")
					SendWebSocketFrame(clientSock, 0x1, "{\"type\":\"pong\"}");
			}
		}

		{
			std::lock_guard<std::mutex> lk(WSClientsMutex);
			WSClients.erase(clientSock);
		}
		closesocket(clientSock);
	}

	void HandleClient(SOCKET clientSock)
	{
		try {
			char buf[65536];
			int total = 0;
			std::string raw;

			while (total < static_cast<int>(sizeof(buf)) - 1)
			{
				int n = recv(clientSock, buf + total, static_cast<int>(sizeof(buf)) - 1 - total, 0);
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

			if (req.Method == "OPTIONS")
			{
				HttpResponse resp{ 204, "text/plain", "" };
				std::string out = BuildResponse(resp);
				send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
				closesocket(clientSock);
				return;
			}

			if (req.Path != "/api/v1/status/health")
			{
				bool authorized = false;
				auto it = req.Headers.find("X-UExplorer-Token");
				if (it != req.Headers.end() && ValidateToken(it->second))
					authorized = true;

				// Browser WebSocket clients usually cannot set custom headers; allow token query fallback.
				if (!authorized && req.Path.rfind("/api/v1/ws/", 0) == 0)
				{
					std::string tokenInQuery = GetQueryValue(req.Query, "token");
					if (!tokenInQuery.empty() && ValidateToken(tokenInQuery))
						authorized = true;
				}

				if (!authorized)
				{
					HttpResponse resp{ 401, "application/json", R"({"success":false,"error":"Unauthorized"})" };
					std::string out = BuildResponse(resp);
					send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
					closesocket(clientSock);
					return;
				}
			}

			if (IsWebSocketUpgrade(req))
			{
				HandleWebSocketClient(clientSock, req);
				return;
			}

			if (req.Path.rfind("/api/v1/events/", 0) == 0)
			{
				int clientIdNum = ++SSEClientCounter;
				std::string clientId = "sse-" + std::to_string(clientIdNum);

				{
					std::lock_guard<std::mutex> lk(SSEClientsMutex);
					SSEClients[clientSock] = SSEClientInfo{ clientId, req.Path };
				}

				if (OnSSEConnect)
					OnSSEConnect(clientId);

				std::ostringstream hdr;
				hdr << "HTTP/1.1 200 OK\r\n";
				hdr << "Content-Type: text/event-stream\r\n";
				hdr << "Cache-Control: no-cache\r\n";
				hdr << "Connection: keep-alive\r\n";
				hdr << "Access-Control-Allow-Origin: *\r\n";
				hdr << "\r\n";
				send(clientSock, hdr.str().c_str(), static_cast<int>(hdr.str().size()), 0);

				std::string init = "event: connected\ndata: {\"clientId\":\"" + clientId + "\"}\n\n";
				send(clientSock, init.c_str(), static_cast<int>(init.size()), 0);

				char rbuf[64];
				fd_set readfds;
				timeval tv;

				while (Running.load())
				{
					FD_ZERO(&readfds);
					FD_SET(clientSock, &readfds);
					tv.tv_sec = 0;
					tv.tv_usec = 500000;
					int sel = select(0, &readfds, nullptr, nullptr, &tv);
					if (sel < 0) break;
					if (sel == 0) continue;

					int n = recv(clientSock, rbuf, sizeof(rbuf) - 1, 0);
					if (n <= 0) break;
				}

				{
					std::lock_guard<std::mutex> lk(SSEClientsMutex);
					SSEClients.erase(clientSock);
				}
				if (OnSSEDisconnect)
					OnSSEDisconnect(clientId);
				closesocket(clientSock);
				return;
			}

			HttpResponse resp = MatchAndHandle(req);
			std::string out = BuildResponse(resp);
			send(clientSock, out.c_str(), static_cast<int>(out.size()), 0);
			closesocket(clientSock);
		}
		catch (...) {
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
		setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

		uint16_t ports[] = { Port, 27015, 27016, 27017, 27018, 0 };
		bool bound = false;

		for (uint16_t p : ports)
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = htons(p);

			if (bind(ListenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR)
			{
				if (p == 0)
				{
					sockaddr_in boundAddr{};
					int len = sizeof(boundAddr);
					getsockname(ListenSocket, reinterpret_cast<sockaddr*>(&boundAddr), &len);
					Port = ntohs(boundAddr.sin_port);
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

void HttpServer::SetSSEHandlers(SSEConnectHandler onConnect, SSEEventHandler onEvent, SSEDisconnectHandler onDisconnect)
{
	m_Impl->OnSSEConnect = std::move(onConnect);
	m_Impl->OnSSEEvent = std::move(onEvent);
	m_Impl->OnSSEDisconnect = std::move(onDisconnect);
}

void HttpServer::SendSSEEvent(const std::string& event, const std::string& data)
{
	{
		std::lock_guard<std::mutex> lk(m_Impl->SSEClientsMutex);
		for (const auto& [sock, info] : m_Impl->SSEClients)
		{
			const bool isAllStream = (info.Path == "/api/v1/events/stream");
			const bool isWatchStream = (info.Path == "/api/v1/events/watches" && event == "watch");
			const bool isHookStream = (info.Path == "/api/v1/events/hooks" && event == "hook");
			if (!isAllStream && !isWatchStream && !isHookStream)
				continue;

			std::string msg = "event: " + event + "\ndata: " + data + "\n\n";
			send(sock, msg.c_str(), static_cast<int>(msg.size()), 0);
		}
	}

	m_Impl->BroadcastWebSocketEvent(event, data);
}

void HttpServer::SendSSEEventTo(const std::string& clientId, const std::string& event, const std::string& data)
{
	std::lock_guard<std::mutex> lk(m_Impl->SSEClientsMutex);
	for (const auto& [sock, info] : m_Impl->SSEClients)
	{
		if (info.ClientId == clientId)
		{
			std::string msg = "event: " + event + "\ndata: " + data + "\n\n";
			send(sock, msg.c_str(), static_cast<int>(msg.size()), 0);
			break;
		}
	}
}

} // namespace UExplorer
