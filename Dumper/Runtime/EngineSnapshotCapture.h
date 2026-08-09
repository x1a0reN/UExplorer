#pragma once

#include "CallbackBarrier.h"
#include "EngineSnapshot.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace UExplorer::Runtime
{

enum class SnapshotSlotReadResult : std::uint8_t
{
	Captured,
	Empty,
	Failed
};

class IEngineSnapshotSource
{
public:
	virtual ~IEngineSnapshotSource() = default;
	virtual std::uint64_t ContextGeneration() const noexcept = 0;
	virtual bool IsCurrentExecutionThreadValid() const noexcept = 0;
	virtual bool TryGetObjectCount(std::int32_t& objectCount) = 0;
	virtual SnapshotSlotReadResult TryCaptureObject(
		std::int32_t index,
		EngineSnapshotObject& object) = 0;
	virtual bool ValidateSlot(
		std::int32_t index,
		const EngineSnapshotObject* expectedObject) = 0;
};

enum class SnapshotCaptureState : std::uint8_t
{
	Idle,
	Requested,
	Capturing,
	Validating,
	Publishing,
	Completed,
	Failed,
	Stopping,
	Stopped
};

const char* ToString(SnapshotCaptureState state) noexcept;

enum class SnapshotCaptureError : std::uint8_t
{
	None,
	Busy,
	Stopped,
	InvalidConfiguration,
	GenerationExhausted,
	InvalidPumpBudget,
	ExecutionThreadInvalid,
	SourceContextMismatch,
	SourceCountInvalid,
	SourceReadFailed,
	SourceValidationFailed,
	PublicationRejected,
	UnexpectedException
};

const char* ToString(SnapshotCaptureError error) noexcept;

enum class SnapshotPumpResult : std::uint8_t
{
	Idle,
	Progress,
	Published,
	Failed,
	Stopping,
	Busy,
	InvalidBudget
};

struct SnapshotCaptureRequestResult
{
	SnapshotCaptureError Error = SnapshotCaptureError::None;
	std::uint64_t Generation = 0;

	bool Ok() const noexcept { return Error == SnapshotCaptureError::None; }
};

struct SnapshotCaptureDiagnostics
{
	SnapshotCaptureState State = SnapshotCaptureState::Idle;
	SnapshotCaptureError Error = SnapshotCaptureError::None;
	std::uint64_t RequestedGeneration = 0;
	std::uint64_t ActiveGeneration = 0;
	std::int32_t SourceObjectCount = 0;
	std::int32_t NextSlot = 0;
	std::uint32_t CapturedObjects = 0;
	std::uint32_t SkippedSlots = 0;
	std::int32_t ErrorIndex = -1;
	std::uint32_t PumpInFlight = 0;
};

class EngineSnapshotCapture final
{
public:
	static constexpr std::size_t kDefaultPumpBudget = 256;
	static constexpr std::size_t kMaxPumpBudget = 4096;

	EngineSnapshotCapture(
		std::string sessionId,
		std::uint64_t contextGeneration,
		IEngineSnapshotSource& source,
		EngineSnapshotStore& store);
	EngineSnapshotCapture(const EngineSnapshotCapture&) = delete;
	EngineSnapshotCapture& operator=(const EngineSnapshotCapture&) = delete;

	bool IsConfigured() const noexcept;
	SnapshotCaptureRequestResult RequestCapture() noexcept;
	SnapshotPumpResult Pump(std::size_t workBudget = kDefaultPumpBudget) noexcept;
	SnapshotCaptureDiagnostics Diagnostics() const noexcept;
	bool StopAndDrain(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	struct WorkingCapture
	{
		std::uint64_t Generation = 0;
		std::uint64_t StartedAtMonotonicUs = 0;
		std::int32_t SourceObjectCount = 0;
		std::int32_t CaptureIndex = 0;
		std::int32_t ValidationIndex = 0;
		std::size_t ValidationRecordIndex = 0;
		std::size_t PublishRecordIndex = 0;
		std::uint32_t SkippedSlots = 0;
		std::deque<EngineSnapshotObject> CapturedObjects;
		std::vector<EngineSnapshotObject> PublishedObjects;
	};

	std::uint64_t AllocateGenerationLocked() noexcept;
	bool StartRequestedCapture();
	void Fail(SnapshotCaptureError error, std::int32_t index) noexcept;
	void PublishDiagnostics(const WorkingCapture& working) noexcept;
	bool StopRequested() const noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	IEngineSnapshotSource& m_Source;
	EngineSnapshotStore& m_Store;
	CallbackBarrier m_PumpBarrier;
	mutable std::mutex m_RequestMutex;
	std::optional<WorkingCapture> m_Working;
	std::uint64_t m_NextGeneration = 1;
	std::atomic<bool> m_StopRequested{false};
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic<SnapshotCaptureState> m_State{SnapshotCaptureState::Idle};
	std::atomic<SnapshotCaptureError> m_Error{SnapshotCaptureError::None};
	std::atomic<std::uint64_t> m_RequestedGeneration{0};
	std::atomic<std::uint64_t> m_ActiveGeneration{0};
	std::atomic<std::int32_t> m_SourceObjectCount{0};
	std::atomic<std::int32_t> m_NextSlot{0};
	std::atomic<std::uint32_t> m_CapturedObjects{0};
	std::atomic<std::uint32_t> m_SkippedSlots{0};
	std::atomic<std::int32_t> m_ErrorIndex{-1};
};

} // namespace UExplorer::Runtime
