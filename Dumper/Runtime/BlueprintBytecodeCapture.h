#pragma once

#include "BlueprintBytecodeEvidence.h"
#include "CallbackBarrier.h"
#include "EngineFacade.h"
#include "GameThreadExecutor.h"
#include "SafeMemory.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace UExplorer::Runtime
{

enum class BlueprintScriptArrayByteOrder : std::uint8_t
{
	LittleEndian
};

// This record is supplied by a separate layout-proving boundary. The capture
// source validates the record and consumes only its explicit byte locations.
struct BlueprintScriptArrayLayoutWitness
{
	BlueprintEvidenceBinding Binding;
	std::string Source;
	bool Validated = false;
	std::uint32_t ScriptFieldOffset = 0;
	std::uint32_t HeaderByteWidth = 0;
	std::uint32_t DataPointerOffset = 0;
	std::uint8_t DataPointerWidth = 0;
	std::uint32_t NumOffset = 0;
	std::uint8_t NumWidth = 0;
	std::uint32_t MaxOffset = 0;
	std::uint8_t MaxWidth = 0;
	std::uint8_t ElementWidth = 0;
	bool CountsSigned = false;
	BlueprintScriptArrayByteOrder ByteOrder =
		BlueprintScriptArrayByteOrder::LittleEndian;
	std::uint64_t EvidenceFingerprint = 0;
};

std::uint64_t ComputeBlueprintScriptArrayLayoutWitnessFingerprint(
	const BlueprintScriptArrayLayoutWitness& witness) noexcept;
bool IsBlueprintScriptArrayLayoutWitnessValid(
	const BlueprintScriptArrayLayoutWitness& witness) noexcept;

enum class BlueprintBytecodeLiveCaptureError : std::uint8_t
{
	None,
	SourceInvalid,
	SourceStopped,
	RequestInvalid,
	EvidenceUnavailable,
	EvidenceStoreStopped,
	LayoutUnavailable,
	LayoutInvalid,
	BindingMismatch,
	DependencyUnavailable,
	DependencyChanged,
	ExecutionThreadInvalid,
	FunctionHandleStale,
	FunctionMetadataMismatch,
	HeaderAddressOverflow,
	HeaderReadFailed,
	HeaderInvalid,
	ScriptAddressInvalid,
	ScriptLimitExceeded,
	ScriptReadFailed,
	ScriptTerminatorInvalid,
	HeaderChangedDuringCopy,
	ExecutorDisabled,
	ExecutorCancelled,
	ExecutorQueueBusy,
	ExecutorWaitDenied,
	ExecutorTimedOut,
	ExecutorFailed,
	PublishFailed,
	AllocationFailed,
	InternalError
};

const char* ToString(BlueprintBytecodeLiveCaptureError error) noexcept;

struct BlueprintBytecodeLiveCaptureResult
{
	BlueprintBytecodeLiveCaptureError Error =
		BlueprintBytecodeLiveCaptureError::EvidenceUnavailable;
	BlueprintEvidenceSourceError SourceError =
		BlueprintEvidenceSourceError::Unavailable;
	HandleError HandleValidationError = HandleError::None;
	MemoryError MemoryReadError = MemoryError::None;
	BlueprintEvidencePublishError PublishError =
		BlueprintEvidencePublishError::None;
	GameThreadSubmitResult SubmitResult = GameThreadSubmitResult::Disabled;
	std::shared_ptr<const BlueprintBytecodeCapture> Capture;

	bool Ok() const noexcept
	{
		return Error == BlueprintBytecodeLiveCaptureError::None
			&& SourceError == BlueprintEvidenceSourceError::None
			&& static_cast<bool>(Capture);
	}
};

// On a cache miss this source owns one synchronous game-thread work item,
// copies the bounded Script payload, and publishes it to the generation store.
class BlueprintBytecodeCaptureSource final
	: public IBlueprintBytecodeCaptureSource
{
public:
	static constexpr std::uint32_t kMaxHeaderBytes = 256;
	static constexpr int kDefaultTimeoutMs = 5000;

	BlueprintBytecodeCaptureSource(
		EngineFacade& engine,
		GameThreadExecutor& gameThread,
		BlueprintBytecodeEvidenceStore& evidenceStore,
		BlueprintScriptArrayLayoutWitness layout,
		int timeoutMs = kDefaultTimeoutMs);
	BlueprintBytecodeCaptureSource(const BlueprintBytecodeCaptureSource&) = delete;
	BlueprintBytecodeCaptureSource& operator=(const BlueprintBytecodeCaptureSource&) = delete;

	bool IsConfigured() const noexcept;
	bool IsStopping() const noexcept { return m_Barrier.IsStopping(); }
	std::uint32_t InFlight() const noexcept { return m_Barrier.InFlight(); }
	const BlueprintScriptArrayLayoutWitness& Layout() const noexcept
	{
		return m_Layout;
	}

	BlueprintBytecodeCaptureResult Capture(
		const BlueprintBytecodeCaptureRequest& request) override;
	BlueprintBytecodeLiveCaptureResult CaptureWithDiagnostics(
		const BlueprintBytecodeCaptureRequest& request) noexcept;
	bool StopAndDrain(
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	EngineFacade& m_Engine;
	GameThreadExecutor& m_GameThread;
	BlueprintBytecodeEvidenceStore& m_EvidenceStore;
	BlueprintScriptArrayLayoutWitness m_Layout;
	int m_TimeoutMs = kDefaultTimeoutMs;
	CallbackBarrier m_Barrier;
};

} // namespace UExplorer::Runtime
