#pragma once

#include <cstdint>
#include <string>

namespace UExplorer::Services
{

struct ScriptOffsetDiagnostics
{
	std::int32_t SelectedOffset = -1;
	std::int32_t SelectedScore = 0;
	std::int32_t ScoreGapTop2 = 0;
	std::int32_t BpEndHits = 0;
	std::int32_t WeightedBpEndHits = 0;
	std::int32_t GenericScriptHits = 0;
	std::int32_t VerifyProbed = 0;
	std::int32_t VerifyHeaderValid = 0;
	std::int32_t VerifyEndHits = 0;
	std::int32_t VerifyFirstOpcodeValid = 0;
	std::int32_t VerifySizeSane = 0;
	std::int32_t VerifyEndRate = 0;
	std::int32_t VerifyOpcodeRate = 0;
	std::string Confidence;
	std::string AnomalyTags;
};

class ICoreStatusDiagnosticsSource
{
public:
	virtual ~ICoreStatusDiagnosticsSource() = default;
	virtual std::string ProcessArchitecture() const = 0;
	virtual ScriptOffsetDiagnostics CaptureScriptOffsetDiagnostics() const = 0;
};

class EngineCoreStatusDiagnosticsSource final : public ICoreStatusDiagnosticsSource
{
public:
	std::string ProcessArchitecture() const override;
	ScriptOffsetDiagnostics CaptureScriptOffsetDiagnostics() const override;
};

} // namespace UExplorer::Services
