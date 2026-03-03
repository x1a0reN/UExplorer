#pragma once

#include <string>
#include <chrono>
#include "Json/json.hpp"

namespace UExplorer::API
{

using json = nlohmann::json;

// Standard JSON response envelope
inline std::string MakeResponse(const json& data)
{
	auto now = std::chrono::system_clock::now();
	auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
		now.time_since_epoch()).count();

	json envelope;
	envelope["success"] = true;
	envelope["data"] = data;
	envelope["error"] = nullptr;
	envelope["timestamp"] = epoch;
	return envelope.dump();
}

inline std::string MakeError(const std::string& error, int code = 400)
{
	json envelope;
	envelope["success"] = false;
	envelope["data"] = nullptr;
	envelope["error"] = error;
	return envelope.dump();
}

// Parse query string "key1=val1&key2=val2" into map
inline std::unordered_map<std::string, std::string> ParseQuery(const std::string& query)
{
	std::unordered_map<std::string, std::string> params;
	size_t pos = 0;
	std::string q = query;

	while (!q.empty())
	{
		auto amp = q.find('&');
		std::string pair = (amp != std::string::npos) ? q.substr(0, amp) : q;
		q = (amp != std::string::npos) ? q.substr(amp + 1) : "";

		auto eq = pair.find('=');
		if (eq != std::string::npos)
			params[pair.substr(0, eq)] = pair.substr(eq + 1);
	}
	return params;
}

// Extract path segment by index: "/api/v1/classes/Foo" -> GetPathSegment(path, 3) = "Foo"
inline std::string GetPathSegment(const std::string& path, int index)
{
	int cur = 0;
	size_t start = 0;
	for (size_t i = 0; i <= path.size(); i++)
	{
		if (i == path.size() || path[i] == '/')
		{
			if (i > start)
			{
				if (cur == index) return path.substr(start, i - start);
				cur++;
			}
			start = i + 1;
		}
	}
	return "";
}

} // namespace UExplorer::API
