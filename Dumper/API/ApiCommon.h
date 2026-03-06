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

inline std::string MakeError(const std::string& error)
{
	json envelope;
	envelope["success"] = false;
	envelope["data"] = nullptr;
	envelope["error"] = error;
	return envelope.dump();
}

inline int SafeParseInt(const std::string& s, int defaultVal = 0)
{
	try { size_t pos = 0; int v = std::stoi(s, &pos); return (pos > 0) ? v : defaultVal; }
	catch (...) { return defaultVal; }
}

inline uint64_t SafeParseUInt64(const std::string& s, uint64_t defaultVal = 0)
{
	try { return std::stoull(s, nullptr, 0); }
	catch (...) { return defaultVal; }
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

constexpr int32_t kMaxVTableIndex = 512;

// Shared extern declarations for script offset diagnostics (defined in OffsetFinder.cpp)
extern "C" const char* UExplorer_GetScriptOffsetConfidence();
extern "C" const char* UExplorer_GetScriptOffsetAnomalyTags();
extern "C" int32_t UExplorer_GetScriptOffsetSelectedOffset();
extern "C" int32_t UExplorer_GetScriptOffsetSelectedScore();
extern "C" int32_t UExplorer_GetScriptOffsetScoreGapTop2();
extern "C" int32_t UExplorer_GetScriptOffsetBpEndHits();
extern "C" int32_t UExplorer_GetScriptOffsetWeightedBpEndHits();
extern "C" int32_t UExplorer_GetScriptOffsetGenericScriptHits();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyProbed();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyHeaderValid();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyEndHits();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyFirstOpcodeValid();
extern "C" int32_t UExplorer_GetScriptOffsetVerifySizeSane();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyEndRate();
extern "C" int32_t UExplorer_GetScriptOffsetVerifyOpcodeRate();

inline json BuildScriptOffsetDiagnosticsJson()
{
	json diag;
	diag["selected_offset"] = UExplorer_GetScriptOffsetSelectedOffset();
	diag["selected_score"] = UExplorer_GetScriptOffsetSelectedScore();
	diag["score_gap_top2"] = UExplorer_GetScriptOffsetScoreGapTop2();
	diag["bp_end_hits"] = UExplorer_GetScriptOffsetBpEndHits();
	diag["weighted_bp_end_hits"] = UExplorer_GetScriptOffsetWeightedBpEndHits();
	diag["generic_script_hits"] = UExplorer_GetScriptOffsetGenericScriptHits();
	diag["verify_probed"] = UExplorer_GetScriptOffsetVerifyProbed();
	diag["verify_header_valid"] = UExplorer_GetScriptOffsetVerifyHeaderValid();
	diag["verify_end_hits"] = UExplorer_GetScriptOffsetVerifyEndHits();
	diag["verify_first_opcode_valid"] = UExplorer_GetScriptOffsetVerifyFirstOpcodeValid();
	diag["verify_size_sane"] = UExplorer_GetScriptOffsetVerifySizeSane();
	diag["verify_end_rate"] = UExplorer_GetScriptOffsetVerifyEndRate();
	diag["verify_opcode_rate"] = UExplorer_GetScriptOffsetVerifyOpcodeRate();
	diag["confidence"] = UExplorer_GetScriptOffsetConfidence();
	diag["anomaly_tags"] = UExplorer_GetScriptOffsetAnomalyTags();
	return diag;
}

} // namespace UExplorer::API
