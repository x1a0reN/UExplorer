#include "CoreStatusDiagnostics.h"

#include <Windows.h>

extern "C" const char* UExplorer_GetScriptOffsetConfidence();
extern "C" const char* UExplorer_GetScriptOffsetAnomalyTags();
extern "C" std::int32_t UExplorer_GetScriptOffsetSelectedOffset();
extern "C" std::int32_t UExplorer_GetScriptOffsetSelectedScore();
extern "C" std::int32_t UExplorer_GetScriptOffsetScoreGapTop2();
extern "C" std::int32_t UExplorer_GetScriptOffsetBpEndHits();
extern "C" std::int32_t UExplorer_GetScriptOffsetWeightedBpEndHits();
extern "C" std::int32_t UExplorer_GetScriptOffsetGenericScriptHits();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyProbed();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyHeaderValid();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyEndHits();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyFirstOpcodeValid();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifySizeSane();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyEndRate();
extern "C" std::int32_t UExplorer_GetScriptOffsetVerifyOpcodeRate();

namespace UExplorer::Services
{
namespace
{

std::string SafeString(const char* value)
{
	return value ? value : "";
}

} // namespace

std::string EngineCoreStatusDiagnosticsSource::ProcessArchitecture() const
{
#ifdef _WIN64
	return "x64";
#else
	BOOL isWow64 = FALSE;
	if (IsWow64Process(GetCurrentProcess(), &isWow64) && isWow64)
		return "x86 (WOW64)";
	return "x86";
#endif
}

ScriptOffsetDiagnostics EngineCoreStatusDiagnosticsSource::CaptureScriptOffsetDiagnostics() const
{
	return {
		.SelectedOffset = UExplorer_GetScriptOffsetSelectedOffset(),
		.SelectedScore = UExplorer_GetScriptOffsetSelectedScore(),
		.ScoreGapTop2 = UExplorer_GetScriptOffsetScoreGapTop2(),
		.BpEndHits = UExplorer_GetScriptOffsetBpEndHits(),
		.WeightedBpEndHits = UExplorer_GetScriptOffsetWeightedBpEndHits(),
		.GenericScriptHits = UExplorer_GetScriptOffsetGenericScriptHits(),
		.VerifyProbed = UExplorer_GetScriptOffsetVerifyProbed(),
		.VerifyHeaderValid = UExplorer_GetScriptOffsetVerifyHeaderValid(),
		.VerifyEndHits = UExplorer_GetScriptOffsetVerifyEndHits(),
		.VerifyFirstOpcodeValid = UExplorer_GetScriptOffsetVerifyFirstOpcodeValid(),
		.VerifySizeSane = UExplorer_GetScriptOffsetVerifySizeSane(),
		.VerifyEndRate = UExplorer_GetScriptOffsetVerifyEndRate(),
		.VerifyOpcodeRate = UExplorer_GetScriptOffsetVerifyOpcodeRate(),
		.Confidence = SafeString(UExplorer_GetScriptOffsetConfidence()),
		.AnomalyTags = SafeString(UExplorer_GetScriptOffsetAnomalyTags())
	};
}

} // namespace UExplorer::Services
