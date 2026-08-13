#pragma once

#include "Runtime/HookEventCollector.h"
#include "Runtime/ObjectHandle.h"
#include "Utils/Json/json.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UExplorer::Runtime
{
class CoreRuntime;
class EngineFacade;
class GameThreadExecutor;
}

namespace UExplorer::Services
{

using json = nlohmann::json;
using HookSubscriptionId = std::uint64_t;

enum class HookCaptureMode : std::uint8_t
{
	FixedMetadata,
	PreEncodedPayload
};

const char* ToString(HookCaptureMode mode) noexcept;

// Neither policy authorizes reading or decoding a ProcessEvent parameter frame.
// PreEncodedPayload only permits a producer-owned, already encoded byte span.
struct HookCapturePolicy
{
	HookCaptureMode Mode = HookCaptureMode::FixedMetadata;
	std::size_t MaxPayloadBytes = 0;
};

struct HookSubscriptionSpec
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	Runtime::FunctionHandle Function;
	std::string FunctionPath;
	HookCapturePolicy Capture;
};

enum class HookSubscriptionState : std::uint8_t
{
	Enabled,
	Disabled,
	Terminal
};

const char* ToString(HookSubscriptionState state) noexcept;

enum class HookSubscriptionError : std::uint8_t
{
	None,
	InvalidConfiguration,
	InvalidSpec,
	SessionMismatch,
	ContextGenerationMismatch,
	SnapshotGenerationInvalid,
	CurrentSnapshotUnavailable,
	SnapshotGenerationMismatch,
	FunctionSnapshotMismatch,
	CapturePolicyInvalid,
	Duplicate,
	CapacityExceeded,
	IdExhausted,
	SnapshotGenerationExhausted,
	NotFound,
	Terminal,
	InvalidLimit,
	AllocationFailed,
	CollectorDrainFailed,
	CaptureModeUnavailable
};

const char* ToString(HookSubscriptionError error) noexcept;

struct HookSubscription
{
	HookSubscriptionId Id = 0;
	HookSubscriptionSpec Spec;
	HookSubscriptionState State = HookSubscriptionState::Disabled;
	std::uint64_t CreatedAtMonotonicUs = 0;
	std::uint64_t HitCount = 0;
	std::uint64_t LastEventSequence = 0;
	std::uint64_t LastCorrelation = 0;
	std::size_t LogCount = 0;
	std::size_t LogBytes = 0;
	std::uint64_t LogDropCount = 0;
	std::string TerminalReasonCode;
	std::string TerminalReason;
};

// Immutable producer entry. Spec retains the complete session/context,
// object/type generation, function serial/address/class, owner identity,
// canonical path, display path, and signature proof admitted by the command.
struct HookProducerSubscription
{
	HookSubscriptionId Id = 0;
	HookSubscriptionSpec Spec;
};

// Open-addressed immutable lookup table. Reads allocate nothing, acquire no
// service lock, and perform at most BucketCount probes.
class HookEnabledSnapshot final
{
public:
	std::uint64_t Generation() const noexcept { return m_Generation; }
	std::size_t Size() const noexcept { return m_Entries.size(); }
	std::size_t BucketCount() const noexcept { return m_Buckets.size(); }
	const std::vector<HookProducerSubscription>& Entries() const noexcept
	{
		return m_Entries;
	}
	const HookProducerSubscription* Find(
		const Runtime::FunctionHandle& function,
		std::uint64_t objectSnapshotGeneration,
		std::uint64_t typeSnapshotGeneration) const noexcept;
	const HookProducerSubscription* FindByFunctionAddress(
		std::uintptr_t functionAddress) const noexcept;

private:
	friend class HookCommandService;
	std::uint64_t m_Generation = 0;
	std::vector<HookProducerSubscription> m_Entries;
	// Zero is empty; populated values are one-based indexes into m_Entries.
	std::vector<std::uint32_t> m_Buckets;
	std::vector<std::uint32_t> m_AddressBuckets;
};

struct HookCommandError
{
	std::string Code;
	std::string Message;
	json Details = json::object();
};

struct HookCommandResult
{
	json Data = nullptr;
	std::optional<HookCommandError> Error;

	bool Ok() const noexcept { return !Error; }
};

struct HookMutationResult
{
	HookSubscriptionError Error = HookSubscriptionError::None;
	HookSubscriptionId Id = 0;

	bool Ok() const noexcept { return Error == HookSubscriptionError::None; }
};

struct HookCommandLimits
{
	std::size_t MaxSubscriptions = 256;
	std::size_t MaxLogEntriesPerSubscription = 128;
	std::size_t MaxLogBytesPerSubscription = 256 * 1024;
	std::size_t MaxDrainBatch = 256;
	bool AllowPreEncodedPayload = true;
};

// Transport-neutral subscription registry and worker-side collector drain.
// This class never installs/restores a hook and deliberately has no producer
// availability fallback. An upper layer must keep hook capability false until
// a real owner publishes through the exact enabled-snapshot contract.
class HookCommandService final
{
public:
	static constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
	static constexpr std::size_t kHardMaxSubscriptions = 1024;
	static constexpr std::size_t kHardMaxLogEntries = 4096;
	static constexpr std::size_t kHardMaxLogBytes = 4 * 1024 * 1024;
	static constexpr std::size_t kHardMaxDrainBatch = 4096;
	static constexpr std::size_t kMaxSerializedDataBytes = 4 * 1024 * 1024;

	HookCommandService(
		std::string sessionId,
		std::uint64_t contextGeneration,
		Runtime::HookEventCollector& collector,
		HookCommandLimits limits = {},
		Runtime::CoreRuntime* runtime = nullptr,
		Runtime::EngineFacade* engine = nullptr,
		Runtime::GameThreadExecutor* gameThread = nullptr);
	~HookCommandService();

	HookCommandService(const HookCommandService&) = delete;
	HookCommandService& operator=(const HookCommandService&) = delete;

	bool IsConfigured() const noexcept { return m_Configured; }
	static bool Handles(std::string_view operation) noexcept;
	HookCommandResult Execute(std::string_view operation, const json& data) noexcept;

	std::shared_ptr<const HookEnabledSnapshot> AcquireEnabledSnapshot() const noexcept;
	HookMutationResult MarkTerminal(
		HookSubscriptionId id,
		std::string reasonCode,
		std::string reason) noexcept;

private:
	struct SubscriptionRecord;
	struct LogRecord;
	struct CollectorDrainSummary;

	HookCommandResult Add(const json& data) noexcept;
	HookCommandResult List(const json& data) noexcept;
	HookCommandResult Enable(const json& data) noexcept;
	HookCommandResult Remove(const json& data) noexcept;
	HookCommandResult Log(const json& data) noexcept;

	HookSubscriptionError ValidateSpec(const HookSubscriptionSpec& spec) const noexcept;
	HookSubscriptionError ValidateCurrentSpec(
		const HookSubscriptionSpec& spec) const noexcept;
	SubscriptionRecord* FindLocked(HookSubscriptionId id) const noexcept;
	std::shared_ptr<const HookEnabledSnapshot> BuildEnabledSnapshotLocked(
		const SubscriptionRecord* overrideRecord = nullptr,
		bool overrideEnabled = false,
		const SubscriptionRecord* excludedRecord = nullptr) const;
	void PublishEnabledSnapshotLocked(
		std::shared_ptr<const HookEnabledSnapshot> snapshot) noexcept;
	HookSubscription CopySubscriptionLocked(const SubscriptionRecord& record) const;
	CollectorDrainSummary DrainCollectorWorker(std::size_t maximum) noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	Runtime::HookEventCollector& m_Collector;
	HookCommandLimits m_Limits;
	Runtime::CoreRuntime* m_Runtime = nullptr;
	Runtime::EngineFacade* m_Engine = nullptr;
	Runtime::GameThreadExecutor* m_GameThread = nullptr;
	bool m_Configured = false;
	std::shared_ptr<const HookEnabledSnapshot> m_EmptyEnabledSnapshot;
	std::atomic<std::shared_ptr<const HookEnabledSnapshot>> m_EnabledSnapshot;
	mutable std::mutex m_Mutex;
	std::mutex m_DrainMutex;
	std::vector<std::unique_ptr<SubscriptionRecord>> m_Subscriptions;
	HookSubscriptionId m_NextId = 1;
	std::uint64_t m_EnabledSnapshotGeneration = 0;
	std::uint64_t m_UnmatchedEventCount = 0;
	std::uint64_t m_PolicyRejectedEventCount = 0;
	std::uint64_t m_DrainFailureCount = 0;
};

} // namespace UExplorer::Services
