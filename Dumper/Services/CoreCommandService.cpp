#include "CoreCommandService.h"

#include "Runtime/ObjectSnapshotReflectionCandidateSource.h"
#include "Runtime/ObjectSnapshotTypeCandidateSource.h"
#include "ObjectPropertyCommandService.h"
#include "TypeCommandService.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kStatusInspect = "status.inspect";
constexpr std::string_view kStatusEngine = "status.engine";
constexpr std::string_view kStatusHealth = "status.health";
constexpr std::string_view kStatusReconnect = "status.reconnect";
constexpr std::string_view kObjectSnapshotPage = "objects.snapshot.page";
constexpr std::string_view kObjectHandleIssue = "objects.handle.issue";
constexpr std::string_view kFunctionHandleIssue = "functions.handle.issue";
constexpr std::string_view kObjectPropertyRead = "objects.property.read";
constexpr std::size_t kMaxSnapshotPageRecords = 128;

std::uint64_t ElapsedMicroseconds(const std::chrono::steady_clock::time_point started) noexcept
{
	const auto finished = std::chrono::steady_clock::now();
	return finished >= started
		? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			finished - started).count())
		: 0;
}

json SerializeCapabilities(const Runtime::CoreRuntimeSnapshot& snapshot)
{
	json capabilities = json::object();
	if (!snapshot.Capabilities)
		return capabilities;
	for (const auto& [name, capability] : snapshot.Capabilities->All())
	{
		capabilities[name] = {
			{"available", capability.Available},
			{"reason_code", capability.ReasonCode.empty() ? json(nullptr) : json(capability.ReasonCode)},
			{"reason", capability.Reason.empty() ? json(nullptr) : json(capability.Reason)},
			{"dependencies", capability.Dependencies}
		};
	}
	return capabilities;
}

json SerializeRuntime(const Runtime::CoreRuntimeSnapshot& snapshot)
{
	return {
		{"state", Runtime::ToString(snapshot.State)},
		{"session_id", snapshot.SessionId.empty() ? json(nullptr) : json(snapshot.SessionId)},
		{"liveness", snapshot.IsLive()},
		{"readiness", snapshot.IsReady()},
		{"active_requests", snapshot.ActiveRequests},
		{"readiness_blockers", snapshot.ReadinessBlockers},
		{"failure_code", snapshot.FailureCode.empty() ? json(nullptr) : json(snapshot.FailureCode)},
		{"failure_message", snapshot.FailureMessage.empty() ? json(nullptr) : json(snapshot.FailureMessage)},
		{"context_generation", snapshot.Context
			? json(snapshot.Context->Generation())
			: json(nullptr)}
	};
}

json SerializeGameThreadDiagnostics(const Runtime::GameThreadExecutor& executor)
{
	const Runtime::GameThreadDiagnostics diagnostics = executor.GetDiagnostics();
	return {
		{"enabled", diagnostics.Enabled},
		{"processing", diagnostics.Processing},
		{"pump_observed", diagnostics.PumpObserved},
		{"pump_thread_stable", diagnostics.PumpThreadStable},
		{"pump_thread_id", diagnostics.PumpThreadId},
		{"last_tick_monotonic_us", diagnostics.LastPumpTickMonotonicUs},
		{"tick_count", diagnostics.PumpTickCount},
		{"last_task_duration_us", diagnostics.LastTaskDurationUs},
		{"queue_depth", diagnostics.QueueDepth},
		{"queue_capacity", diagnostics.Capacity},
		{"pump_backend", "post_render_vtable"}
	};
}

json SerializeSnapshotDiagnostics(const Runtime::EngineFacade& engine)
{
	const Runtime::EngineSnapshotStore& store = engine.Snapshots();
	const std::shared_ptr<const Runtime::EngineSnapshot> snapshot = store.Current();
	json data;
	if (snapshot)
	{
		data = {
			{"published", true},
			{"generation", snapshot->Generation},
			{"context_generation", snapshot->ContextGeneration},
			{"captured_at_monotonic_us", snapshot->CapturedAtMonotonicUs},
			{"capture_duration_us", snapshot->CaptureDurationUs},
			{"source_object_count", snapshot->SourceObjectCount},
			{"object_count", snapshot->Objects.size()},
			{"skipped_slots", snapshot->SkippedSlots},
			{"stopped", store.IsStopped()}
		};
	}
	else
	{
		data = {
			{"published", false},
			{"generation", nullptr},
			{"stopped", store.IsStopped()}
		};
	}
	data["retired_snapshot_count"] = store.RetiredSnapshotCount();

	const Runtime::EngineSnapshotCapture* capture = engine.SnapshotCapture();
	data["capture_configured"] = capture != nullptr;
	if (capture)
	{
		const Runtime::SnapshotCaptureDiagnostics diagnostics = capture->Diagnostics();
		data["capture"] = {
			{"state", Runtime::ToString(diagnostics.State)},
			{"error_code", diagnostics.Error == Runtime::SnapshotCaptureError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.Error))},
			{"requested_generation", diagnostics.RequestedGeneration},
			{"active_generation", diagnostics.ActiveGeneration},
			{"source_object_count", diagnostics.SourceObjectCount},
			{"next_slot", diagnostics.NextSlot},
			{"captured_objects", diagnostics.CapturedObjects},
			{"skipped_slots", diagnostics.SkippedSlots},
			{"error_index", diagnostics.ErrorIndex},
			{"pump_in_flight", diagnostics.PumpInFlight},
			{"retired_captures", diagnostics.RetiredCaptures}
		};
	}
	else
	{
		data["capture"] = nullptr;
	}
	return data;
}

json SerializeReflectionDiagnostics(
	const Runtime::EngineFacade& engine,
	const Runtime::ObjectSnapshotReflectionCandidateSource* source)
{
	const std::shared_ptr<const Runtime::ReflectionRuntimeSnapshot> reflection =
		engine.Reflection();
	json data = {
		{"layout_published", reflection && reflection->Layout},
		{"property_codec_published", reflection && reflection->Properties}
	};
	if (reflection && reflection->Layout)
	{
		data["property_system"] = Runtime::ToString(
			reflection->Layout->PropertySystem());
		data["layout_fingerprint"] = std::format(
			"0x{:016X}", reflection->Layout->Fingerprint());
	}
	else
	{
		data["property_system"] = nullptr;
		data["layout_fingerprint"] = nullptr;
	}

	const Runtime::ReflectionLayoutCapture* capture = engine.ReflectionCapture();
	data["capture_configured"] = capture != nullptr;
	if (capture)
	{
		const Runtime::ReflectionLayoutCaptureDiagnostics diagnostics =
			capture->Diagnostics();
		data["capture"] = {
			{"state", Runtime::ToString(diagnostics.State)},
			{"error_code", diagnostics.Error == Runtime::ReflectionLayoutCaptureError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.Error))},
			{"source_error_code", diagnostics.SourceError
					== Runtime::ReflectionCandidateSourceError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.SourceError))},
			{"validation_error_code", diagnostics.ValidationError
					== Runtime::ReflectionValidationError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.ValidationError))},
			{"captured_fields", diagnostics.CapturedFields},
			{"captured_witnesses", diagnostics.CapturedWitnesses},
			{"source_steps", diagnostics.SourceSteps},
			{"pump_in_flight", diagnostics.PumpInFlight}
		};
	}
	else
	{
		data["capture"] = nullptr;
	}

	if (source)
	{
		const Runtime::ObjectSnapshotReflectionSourceDiagnostics diagnostics =
			source->Diagnostics();
		data["source"] = {
			{"preparation_error_code", diagnostics.PreparationError
					== Runtime::ReflectionCandidatePreparationError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.PreparationError))},
			{"source_error_code", diagnostics.SourceError
					== Runtime::ReflectionCandidateSourceError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.SourceError))},
			{"prepared_snapshot_generation", diagnostics.PreparedSnapshotGeneration},
			{"property_system", Runtime::ToString(diagnostics.PropertySystem)},
			{"discovery_phase", diagnostics.DiscoveryPhase},
			{"source_steps", diagnostics.SourceSteps},
			{"emitted_fields", diagnostics.EmittedFields},
			{"active", diagnostics.Active}
		};
	}
	else
	{
		data["source"] = nullptr;
	}
	return data;
}

json SerializeTypeSnapshotDiagnostics(
	const Runtime::EngineFacade& engine,
	const Runtime::ObjectSnapshotTypeCandidateSource* source)
{
	const Runtime::TypeSnapshotStore& store = engine.Types();
	const std::shared_ptr<const Runtime::TypeSnapshot> snapshot = store.Current();
	json data;
	if (snapshot)
	{
		data = {
			{"published", true},
			{"generation", snapshot->Generation()},
			{"context_generation", snapshot->ContextGeneration()},
			{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
			{"reflection_layout_fingerprint", std::format(
				"0x{:016X}", snapshot->ReflectionLayoutFingerprint())},
			{"captured_at_monotonic_us", snapshot->CapturedAtMonotonicUs()},
			{"capture_duration_us", snapshot->CaptureDurationUs()},
			{"source", snapshot->Source()},
			{"type_count", snapshot->Types().size()},
			{"stopped", store.IsStopped()}
		};
	}
	else
	{
		data = {
			{"published", false},
			{"generation", nullptr},
			{"stopped", store.IsStopped()}
		};
	}

	const Runtime::TypeSnapshotCapture* capture = engine.TypeCapture();
	data["capture_configured"] = capture != nullptr;
	if (capture)
	{
		const Runtime::TypeSnapshotCaptureDiagnostics diagnostics =
			capture->Diagnostics();
		data["capture"] = {
			{"state", Runtime::ToString(diagnostics.State)},
			{"error_code", diagnostics.Error == Runtime::TypeSnapshotCaptureError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.Error))},
			{"source_error_code", diagnostics.SourceError
					== Runtime::TypeSnapshotSourceError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.SourceError))},
			{"publish_error_code", diagnostics.PublishError
					== Runtime::TypeSnapshotPublishError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.PublishError))},
			{"requested_generation", diagnostics.RequestedGeneration},
			{"active_generation", diagnostics.ActiveGeneration},
			{"object_snapshot_generation", diagnostics.ObjectSnapshotGeneration},
			{"reflection_layout_fingerprint", diagnostics.ReflectionLayoutFingerprint == 0
				? json(nullptr)
				: json(std::format(
					"0x{:016X}", diagnostics.ReflectionLayoutFingerprint))},
			{"captured_types", diagnostics.CapturedTypes},
			{"captured_functions", diagnostics.CapturedFunctions},
			{"captured_members", diagnostics.CapturedMembers},
			{"source_steps", diagnostics.SourceSteps},
			{"validation_steps", diagnostics.ValidationSteps},
			{"work_in_flight", diagnostics.WorkInFlight},
			{"retired_candidates", diagnostics.RetiredCandidates}
		};
	}
	else
	{
		data["capture"] = nullptr;
	}
	if (source)
	{
		const Runtime::ObjectSnapshotTypeSourceDiagnostics diagnostics =
			source->Diagnostics();
		data["source"] = {
			{"preparation_error_code", diagnostics.PreparationError
					== Runtime::TypeCandidatePreparationError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.PreparationError))},
			{"source_error_code", diagnostics.SourceError
					== Runtime::TypeSnapshotSourceError::None
				? json(nullptr)
				: json(Runtime::ToString(diagnostics.SourceError))},
			{"prepared_snapshot_generation", diagnostics.PreparedSnapshotGeneration},
			{"reflection_layout_fingerprint", diagnostics.ReflectionLayoutFingerprint == 0
				? json(nullptr)
				: json(std::format(
					"0x{:016X}", diagnostics.ReflectionLayoutFingerprint))},
			{"prepared_types", diagnostics.PreparedTypes},
			{"prepared_functions", diagnostics.PreparedFunctions},
			{"capture_phase", diagnostics.CapturePhase},
			{"source_steps", diagnostics.SourceSteps},
			{"validation_steps", diagnostics.ValidationSteps},
			{"captured_evidence", diagnostics.CapturedEvidence},
			{"active", diagnostics.Active}
		};
	}
	else
	{
		data["source"] = nullptr;
	}
	return data;
}

json SerializeScriptOffsetDiagnostics(const ScriptOffsetDiagnostics& diagnostics)
{
	return {
		{"selected_offset", diagnostics.SelectedOffset},
		{"selected_score", diagnostics.SelectedScore},
		{"score_gap_top2", diagnostics.ScoreGapTop2},
		{"bp_end_hits", diagnostics.BpEndHits},
		{"weighted_bp_end_hits", diagnostics.WeightedBpEndHits},
		{"generic_script_hits", diagnostics.GenericScriptHits},
		{"verify_probed", diagnostics.VerifyProbed},
		{"verify_header_valid", diagnostics.VerifyHeaderValid},
		{"verify_end_hits", diagnostics.VerifyEndHits},
		{"verify_first_opcode_valid", diagnostics.VerifyFirstOpcodeValid},
		{"verify_size_sane", diagnostics.VerifySizeSane},
		{"verify_end_rate", diagnostics.VerifyEndRate},
		{"verify_opcode_rate", diagnostics.VerifyOpcodeRate},
		{"confidence", diagnostics.Confidence},
		{"anomaly_tags", diagnostics.AnomalyTags}
	};
}

json OffsetValue(const Runtime::EngineContext& context, const std::string& name)
{
	const Runtime::OffsetReport* report = context.FindOffset(name);
	return report ? json(report->Value) : json(nullptr);
}

json SerializeOffsetReports(const Runtime::EngineContext& context)
{
	json reports = json::object();
	for (const auto& [name, report] : context.Offsets())
	{
		reports[name] = {
			{"value", report.Value},
			{"required", report.Required},
			{"state", Runtime::ToString(report.State)},
			{"source", report.Source},
			{"confidence", report.Confidence},
			{"candidates", report.Candidates},
			{"checks", report.Checks},
			{"reason_code", report.ReasonCode.empty() ? json(nullptr) : json(report.ReasonCode)},
			{"reason", report.Reason.empty() ? json(nullptr) : json(report.Reason)}
		};
	}
	return reports;
}

json SerializeNameProfile(const Runtime::EngineNameProfile& profile)
{
	return {
		{"storage", Runtime::ToString(profile.Storage)},
		{"storage_address", profile.StorageAddress == 0
			? json(nullptr)
			: json(std::format("0x{:X}", profile.StorageAddress))},
		{"validated", profile.Validated},
		{"fname_size", profile.FNameSize},
		{"comparison_index_offset", profile.ComparisonIndexOffset},
		{"number_offset", profile.NumberOffset},
		{"uses_outline_number", profile.UsesOutlineNumber},
		{"block_offset_bits", profile.BlockOffsetBits},
		{"entry_stride", profile.EntryStride},
		{"chunks_start", profile.ChunksStart},
		{"max_chunk_index_offset", profile.MaxChunkIndexOffset},
		{"num_elements_offset", profile.NumElementsOffset},
		{"byte_cursor_offset", profile.ByteCursorOffset},
		{"entry_string_offset", profile.EntryStringOffset},
		{"entry_header_offset", profile.EntryHeaderOffset},
		{"entry_index_offset", profile.EntryIndexOffset},
		{"entry_length_shift", profile.EntryLengthShift},
		{"source", profile.Source},
		{"checks", profile.Checks},
		{"reason_code", profile.ReasonCode.empty() ? json(nullptr) : json(profile.ReasonCode)},
		{"reason", profile.Reason.empty() ? json(nullptr) : json(profile.Reason)}
	};
}

json ModuleRelativeAddress(
	const Runtime::EngineContext& context,
	const std::string& offsetName)
{
	const Runtime::OffsetReport* report = context.FindOffset(offsetName);
	if (!report || !report->IsValidated() || report->Value < 0)
		return nullptr;
	const auto offset = static_cast<std::uintptr_t>(report->Value);
	if (offset > (std::numeric_limits<std::uintptr_t>::max)() - context.ModuleBase())
		return nullptr;
	return std::format("0x{:X}", context.ModuleBase() + offset);
}

json SerializeObjectHandle(const Runtime::ObjectHandle& handle)
{
	return {
		{"session_id", handle.SessionId},
		{"context_generation", handle.ContextGeneration},
		{"index", handle.Index},
		{"serial", handle.SerialNumber},
		{"address", std::format("0x{:X}", handle.Address)},
		{"class_fingerprint", std::format("{:016X}", handle.ClassFingerprint)}
	};
}

json SerializeFunctionHandle(const Runtime::FunctionHandle& handle)
{
	return {
		{"function", SerializeObjectHandle(handle.Function)},
		{"owner", SerializeObjectHandle(handle.Owner)},
		{"full_path", handle.FullPath},
		{"signature_fingerprint", std::format("{:016X}", handle.SignatureFingerprint)}
	};
}

bool TryParseStrictIndex(const json& data, std::int32_t& index)
{
	index = -1;
	if (!data.is_object() || data.size() != 1 || !data.contains("index")
		|| !data.at("index").is_number_integer())
	{
		return false;
	}
	const json& encodedIndex = data.at("index");
	if (encodedIndex.is_number_unsigned())
	{
		const std::uint64_t candidate = encodedIndex.get<std::uint64_t>();
		if (candidate > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))
			return false;
		index = static_cast<std::int32_t>(candidate);
		return true;
	}

	const std::int64_t candidate = encodedIndex.get<std::int64_t>();
	if (candidate < 0 || candidate > (std::numeric_limits<std::int32_t>::max)())
		return false;
	index = static_cast<std::int32_t>(candidate);
	return true;
}

struct SnapshotPageInput
{
	std::optional<std::uint64_t> Generation;
	std::optional<std::int32_t> AfterIndex;
	std::size_t Limit = 0;
};

bool TryParseBoundedUnsigned(
	const json& value,
	const std::uint64_t minimum,
	const std::uint64_t maximum,
	std::uint64_t& parsed)
{
	parsed = 0;
	if (!value.is_number_integer())
		return false;

	if (value.is_number_unsigned())
	{
		const std::uint64_t candidate = value.get<std::uint64_t>();
		if (candidate < minimum || candidate > maximum)
			return false;
		parsed = candidate;
		return true;
	}

	const std::int64_t candidate = value.get<std::int64_t>();
	if (candidate < 0)
		return false;
	const auto unsignedCandidate = static_cast<std::uint64_t>(candidate);
	if (unsignedCandidate < minimum || unsignedCandidate > maximum)
		return false;
	parsed = unsignedCandidate;
	return true;
}

bool TryParseSnapshotPageInput(const json& data, SnapshotPageInput& input)
{
	input = {};
	if (!data.is_object()
		|| data.size() != 2
		|| !data.contains("cursor")
		|| !data.contains("limit"))
	{
		return false;
	}

	std::uint64_t limit = 0;
	if (!TryParseBoundedUnsigned(data.at("limit"), 1, kMaxSnapshotPageRecords, limit))
		return false;
	input.Limit = static_cast<std::size_t>(limit);

	const json& cursor = data.at("cursor");
	if (cursor.is_null())
		return true;
	if (!cursor.is_object()
		|| cursor.size() != 2
		|| !cursor.contains("generation")
		|| !cursor.contains("after_index"))
	{
		return false;
	}

	std::uint64_t generation = 0;
	std::uint64_t afterIndex = 0;
	if (!TryParseBoundedUnsigned(
		cursor.at("generation"),
		1,
		Runtime::EngineSnapshotStore::kMaxProtocolGeneration,
		generation)
		|| !TryParseBoundedUnsigned(
			cursor.at("after_index"),
			0,
			static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()),
			afterIndex))
	{
		return false;
	}

	input.Generation = generation;
	input.AfterIndex = static_cast<std::int32_t>(afterIndex);
	return true;
}

class IssueObjectHandleWork final : public Runtime::IGameThreadWork
{
public:
	IssueObjectHandleWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		const std::int32_t index)
		: m_Lease(std::move(lease)),
		  m_Engine(engine),
		  m_Index(index)
	{
	}

	bool Execute() override
	{
		if (!m_Lease.Context())
			return false;
		if (m_Engine.ContextGeneration() != m_Lease.Context()->Generation())
			return false;
		m_Result = m_Engine.IssueObjectHandle(m_Index);
		return true;
	}

	const Runtime::ObjectHandleResult& Result() const noexcept { return m_Result; }

private:
	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	std::int32_t m_Index;
	Runtime::ObjectHandleResult m_Result;
};

class IssueFunctionHandleWork final : public Runtime::IGameThreadWork
{
public:
	IssueFunctionHandleWork(
		Runtime::CoreRuntime::RequestLease lease,
		Runtime::EngineFacade& engine,
		const std::int32_t index)
		: m_Lease(std::move(lease)),
		  m_Engine(engine),
		  m_Index(index)
	{
	}

	bool Execute() override
	{
		if (!m_Lease.Context())
			return false;
		if (m_Engine.ContextGeneration() != m_Lease.Context()->Generation())
			return false;
		m_Result = m_Engine.IssueFunctionHandle(m_Index);
		return true;
	}

	const Runtime::FunctionHandleResult& Result() const noexcept { return m_Result; }

private:
	Runtime::CoreRuntime::RequestLease m_Lease;
	Runtime::EngineFacade& m_Engine;
	std::int32_t m_Index;
	Runtime::FunctionHandleResult m_Result;
};

CoreCommandTiming ToCommandTiming(const Runtime::GameThreadTaskTiming& timing) noexcept
{
	return {.QueuedUs = timing.QueuedUs, .ExecuteUs = timing.ExecuteUs};
}

} // namespace

CoreCommandService::CoreCommandService(
	Runtime::CoreRuntime& runtime,
	Runtime::GameThreadExecutor& gameThread,
	Runtime::EngineFacade& engine,
	ICoreStatusDiagnosticsSource& statusDiagnostics,
	const Runtime::ObjectSnapshotReflectionCandidateSource* reflectionSource,
	const Runtime::ObjectSnapshotTypeCandidateSource* typeSource)
	: m_Runtime(runtime),
	  m_GameThread(gameThread),
	  m_Engine(engine),
	  m_StatusDiagnostics(statusDiagnostics),
	  m_ReflectionSource(reflectionSource),
	  m_TypeSource(typeSource)
{
	const Runtime::CoreRuntimeSnapshot snapshot = m_Runtime.Snapshot();
	m_SessionId = snapshot.SessionId;
	m_ContextGeneration = snapshot.Context ? snapshot.Context->Generation() : 0;
}

bool CoreCommandService::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= 128
		&& m_ContextGeneration != 0
		&& m_Engine.IsConfigured()
		&& m_Engine.SessionId() == m_SessionId
		&& m_Engine.ContextGeneration() == m_ContextGeneration
		&& (!m_ReflectionSource
			|| m_ReflectionSource->ContextGeneration() == m_ContextGeneration)
		&& (!m_TypeSource
			|| m_TypeSource->ContextGeneration() == m_ContextGeneration);
}

CoreCommandResponse CoreCommandService::Execute(
	const CoreCommandRequest& request,
	const GameThreadQueuedCallback& onGameThreadQueued) noexcept
{
	try
	{
		if (!IsConfigured())
			return Failure(request, "COMMAND_SERVICE_NOT_CONFIGURED", "Core command service is not configured");
		const Runtime::CoreRuntimeSnapshot snapshot = m_Runtime.Snapshot();
		if (snapshot.SessionId != m_SessionId
			|| !snapshot.Context
			|| snapshot.Context->Generation() != m_ContextGeneration)
		{
			return Failure(
				request,
				"COMMAND_SERVICE_STALE",
				"Core command service does not match the active runtime generation");
		}
		if (request.RequestId == 0)
			return Failure(request, "REQUEST_ID_INVALID", "request_id must be positive");
		if (request.Operation.empty() || request.Operation.size() > 128)
			return Failure(request, "OPERATION_INVALID", "operation must be 1..128 characters");
		for (const char value : request.Operation)
		{
			const bool valid = (value >= 'a' && value <= 'z')
				|| (value >= '0' && value <= '9')
				|| value == '_'
				|| value == '-'
				|| value == '.';
			if (!valid)
				return Failure(request, "OPERATION_INVALID", "operation contains an invalid character");
		}
		if (request.SessionId != m_SessionId)
		{
			return Failure(
				request,
				"SESSION_MISMATCH",
				"Request session does not match the active Core session");
		}
		if (request.TimeoutMs <= 0 || request.TimeoutMs > Runtime::GameThreadExecutor::kMaxTimeoutMs)
		{
			return Failure(
				request,
				"TIMEOUT_INVALID",
				"timeout_ms must be in range 1..120000");
		}

		if (request.Operation == kStatusInspect
			|| request.Operation == kStatusEngine
			|| request.Operation == kStatusHealth
			|| request.Operation == kStatusReconnect)
		{
			return ExecuteStatus(request);
		}
		if (request.Operation == kObjectSnapshotPage)
			return ExecuteSnapshotPage(request);
		if (request.Operation == kObjectHandleIssue)
			return ExecuteHandleIssue(request, false, onGameThreadQueued);
		if (request.Operation == kFunctionHandleIssue)
			return ExecuteHandleIssue(request, true, onGameThreadQueued);
		if (request.Operation == kObjectPropertyRead)
			return ExecuteObjectPropertyRead(request, onGameThreadQueued);
		if (TypeCommandService::Handles(request.Operation))
			return ExecuteTypeCommand(request);
		return Failure(
			request,
			"OPERATION_NOT_SUPPORTED",
			"No Core domain command is registered for the requested operation",
			{{"operation", request.Operation}});
	}
	catch (const std::exception& error)
	{
		return Failure(request, "COMMAND_INTERNAL_ERROR", error.what());
	}
	catch (...)
	{
		return Failure(request, "COMMAND_INTERNAL_ERROR", "Core command raised an unknown exception");
	}
}

CoreCommandResponse CoreCommandService::ExecuteObjectPropertyRead(
	const CoreCommandRequest& request,
	const GameThreadQueuedCallback& onGameThreadQueued)
{
	std::string admissionError;
	auto lease = m_Runtime.TryAcquireRequest(&admissionError);
	if (!lease)
	{
		return Failure(
			request,
			admissionError.empty() ? "CORE_NOT_READY" : admissionError,
			"CoreRuntime is not accepting property commands");
	}
	constexpr const char* capabilityName = "objects.properties";
	const Runtime::CapabilityStatus* capability = lease->Capabilities()
		? lease->Capabilities()->Find(capabilityName)
		: nullptr;
	if (!capability || !capability->Available)
	{
		return Failure(
			request,
			capability && !capability->ReasonCode.empty()
				? capability->ReasonCode
				: "OBJECT_PROPERTY_CAPABILITY_UNAVAILABLE",
			capability && !capability->Reason.empty()
				? capability->Reason
				: "Validated reflected property reads are unavailable",
			{{"capability", capabilityName}});
	}

	ObjectPropertyReadPreparation prepared = ObjectPropertyCommandService::PrepareRead(
		request.Data,
		std::move(*lease),
		m_Engine);
	if (!prepared.Ok())
	{
		return Failure(
			request,
			prepared.Error ? std::move(prepared.Error->Code) : "PROPERTY_PREPARATION_FAILED",
			prepared.Error ? std::move(prepared.Error->Message) : "Property read preparation failed",
			prepared.Error ? std::move(prepared.Error->Details) : json::object());
	}

	Runtime::GameThreadTicket ticket;
	const Runtime::GameThreadQueueResult queued = m_GameThread.Enqueue(
		prepared.Work,
		std::chrono::steady_clock::now() + std::chrono::milliseconds(request.TimeoutMs),
		ticket);
	if (queued != Runtime::GameThreadQueueResult::Accepted)
	{
		switch (queued)
		{
		case Runtime::GameThreadQueueResult::Disabled:
			return Failure(request, "GAME_THREAD_UNAVAILABLE", "Game-thread executor is disabled");
		case Runtime::GameThreadQueueResult::QueueBusy:
			return Failure(request, "GAME_THREAD_QUEUE_BUSY", "Game-thread queue capacity is exhausted");
		case Runtime::GameThreadQueueResult::Invalid:
			return Failure(request, "GAME_THREAD_TASK_INVALID", "Game-thread task or deadline is invalid");
		case Runtime::GameThreadQueueResult::Accepted:
			break;
		}
	}

	if (onGameThreadQueued)
	{
		try
		{
			onGameThreadQueued(ticket);
		}
		catch (...)
		{
			m_GameThread.Cancel(ticket);
			return Failure(request, "COMMAND_OBSERVER_FAILED", "Request task observer raised an exception");
		}
	}

	const Runtime::GameThreadSubmitResult submitted = m_GameThread.Wait(ticket);
	Runtime::GameThreadTaskTiming taskTiming;
	m_GameThread.TryGetTiming(ticket, taskTiming);
	const CoreCommandTiming timing = ToCommandTiming(taskTiming);
	switch (submitted)
	{
	case Runtime::GameThreadSubmitResult::Completed:
		break;
	case Runtime::GameThreadSubmitResult::Disabled:
		return Failure(request, "GAME_THREAD_UNAVAILABLE", "Game-thread executor is disabled", json::object(), timing);
	case Runtime::GameThreadSubmitResult::Cancelled:
		return Failure(request, "REQUEST_CANCELLED", "Queued property command was cancelled", json::object(), timing);
	case Runtime::GameThreadSubmitResult::PumpThreadWaitDenied:
		return Failure(request, "GAME_THREAD_REENTRANT_WAIT_DENIED", "Synchronous waits are forbidden on the pump thread", json::object(), timing);
	case Runtime::GameThreadSubmitResult::QueueBusy:
		return Failure(request, "GAME_THREAD_QUEUE_BUSY", "Game-thread queue capacity is exhausted", json::object(), timing);
	case Runtime::GameThreadSubmitResult::TimedOutBeforeStart:
		return Failure(request, "GAME_THREAD_TIMEOUT_BEFORE_START", "Property command expired before execution", json::object(), timing);
	case Runtime::GameThreadSubmitResult::TimedOutWhileRunning:
		return Failure(request, "GAME_THREAD_TIMEOUT_WHILE_RUNNING", "Property command is still running; completion is unknown", json::object(), timing);
	case Runtime::GameThreadSubmitResult::ExecutionFailed:
		return Failure(request, "GAME_THREAD_EXECUTION_FAILED", "Property command failed during guarded execution", json::object(), timing);
	}

	ObjectPropertyCommandResult result =
		ObjectPropertyCommandService::CompleteRead(*prepared.Work);
	if (!result.Ok())
	{
		return Failure(
			request,
			std::move(result.Error->Code),
			std::move(result.Error->Message),
			std::move(result.Error->Details),
			timing);
	}
	return Success(request, std::move(result.Data), timing);
}

CoreCommandResponse CoreCommandService::ExecuteTypeCommand(
	const CoreCommandRequest& request)
{
	const auto started = std::chrono::steady_clock::now();
	const auto timing = [&started]() noexcept {
		return CoreCommandTiming{.ExecuteUs = ElapsedMicroseconds(started)};
	};

	std::string admissionError;
	auto lease = m_Runtime.TryAcquireRequest(&admissionError);
	if (!lease)
	{
		return Failure(
			request,
			admissionError.empty() ? "CORE_NOT_READY" : admissionError,
			"CoreRuntime is not accepting domain commands",
			json::object(),
			timing());
	}

	constexpr const char* capabilityName = "types.inspect";
	const Runtime::CapabilityStatus* capability = lease->Capabilities()
		? lease->Capabilities()->Find(capabilityName)
		: nullptr;
	if (!capability || !capability->Available)
	{
		return Failure(
			request,
			capability && !capability->ReasonCode.empty()
				? capability->ReasonCode
				: "TYPE_SNAPSHOT_UNAVAILABLE",
			capability && !capability->Reason.empty()
				? capability->Reason
				: "No complete immutable type snapshot is available",
			{{"capability", capabilityName}},
			timing());
	}

	const std::shared_ptr<const Runtime::EngineSnapshot> objects =
		m_Engine.Snapshots().Current();
	const std::shared_ptr<const Runtime::TypeSnapshot> types =
		m_Engine.Types().Current();
	if (!objects || !types || !lease->Context())
	{
		return Failure(
			request,
			"TYPE_SNAPSHOT_UNAVAILABLE",
			"The type capability is available but its immutable dependencies are not published",
			{{"capability", capabilityName}},
			timing());
	}

	TypeCommandResult result = TypeCommandService::Execute(
		types,
		request.Operation,
		request.Data,
		m_SessionId,
		lease->Context()->Generation(),
		objects->Generation);
	if (!result.Ok())
	{
		return Failure(
			request,
			std::move(result.Error->Code),
			std::move(result.Error->Message),
			std::move(result.Error->Details),
			timing());
	}
	return Success(request, std::move(result.Data), timing());
}

CoreCommandResponse CoreCommandService::ExecuteSnapshotPage(const CoreCommandRequest& request)
{
	const auto started = std::chrono::steady_clock::now();
	const auto timing = [&started]() noexcept {
		return CoreCommandTiming{.ExecuteUs = ElapsedMicroseconds(started)};
	};

	SnapshotPageInput input;
	if (!TryParseSnapshotPageInput(request.Data, input))
	{
		return Failure(
			request,
			"INVALID_ARGUMENT",
			"Snapshot page data must contain exactly cursor and limit; limit must be 1..128 and a non-null cursor must contain a valid generation and after_index",
			json::object(),
			timing());
	}

	std::string admissionError;
	auto lease = m_Runtime.TryAcquireRequest(&admissionError);
	if (!lease)
	{
		return Failure(
			request,
			admissionError.empty() ? "CORE_NOT_READY" : admissionError,
			"CoreRuntime is not accepting domain commands",
			json::object(),
			timing());
	}

	constexpr const char* capabilityName = "objects.snapshot";
	const Runtime::CapabilityStatus* capability = lease->Capabilities()
		? lease->Capabilities()->Find(capabilityName)
		: nullptr;
	if (!capability || !capability->Available)
	{
		return Failure(
			request,
			capability && !capability->ReasonCode.empty()
				? capability->ReasonCode
				: "OBJECT_SNAPSHOT_UNAVAILABLE",
			capability && !capability->Reason.empty()
				? capability->Reason
				: "No complete immutable object snapshot is available",
			{{"capability", capabilityName}},
			timing());
	}

	const std::shared_ptr<const Runtime::EngineSnapshot> snapshot = m_Engine.Snapshots().Current();
	if (!snapshot)
	{
		return Failure(
			request,
			"OBJECT_SNAPSHOT_UNAVAILABLE",
			"The snapshot capability is available but no immutable snapshot is published",
			{{"capability", capabilityName}},
			timing());
	}
	if (!lease->Context()
		|| snapshot->SessionId != m_SessionId
		|| snapshot->SessionId != request.SessionId
		|| snapshot->ContextGeneration != lease->Context()->Generation()
		|| snapshot->ContextGeneration != m_ContextGeneration)
	{
		return Failure(
			request,
			"SNAPSHOT_CONTEXT_MISMATCH",
			"Published snapshot does not belong to the active Core session and context generation",
			{
				{"snapshot_session_id", snapshot->SessionId},
				{"snapshot_context_generation", snapshot->ContextGeneration},
				{"active_session_id", m_SessionId},
				{"active_context_generation", m_ContextGeneration}
			},
			timing());
	}
	if (input.Generation && *input.Generation != snapshot->Generation)
	{
		return Failure(
			request,
			"SNAPSHOT_GENERATION_MISMATCH",
			"Snapshot generation changed; restart paging with a null cursor",
			{
				{"requested_generation", *input.Generation},
				{"current_generation", snapshot->Generation}
			},
			timing());
	}

	const std::int32_t afterIndex = input.AfterIndex.value_or(-1);
	const auto first = std::upper_bound(
		snapshot->Objects.begin(),
		snapshot->Objects.end(),
		afterIndex,
		[](const std::int32_t index, const Runtime::EngineSnapshotObject& record) {
			return index < record.Handle.Index;
		});
	const std::size_t remaining = static_cast<std::size_t>(
		std::distance(first, snapshot->Objects.end()));
	const std::size_t pageSize = (std::min)(remaining, input.Limit);
	const auto last = first + static_cast<std::ptrdiff_t>(pageSize);

	json items = json::array();
	items.get_ref<json::array_t&>().reserve(pageSize);
	for (auto current = first; current != last; ++current)
	{
		items.push_back({
			{"handle", SerializeObjectHandle(current->Handle)},
			{"name", current->Name},
			{"full_path", current->FullPath},
			{"class_path", current->ClassPath},
			{"package_path", current->PackagePath},
			{"kind", Runtime::ToString(current->Kind)}
		});
	}

	const bool hasMore = last != snapshot->Objects.end();
	json nextCursor = nullptr;
	if (hasMore)
	{
		nextCursor = {
			{"generation", snapshot->Generation},
			{"after_index", (last - 1)->Handle.Index}
		};
	}

	json data = {
		{"generation", snapshot->Generation},
		{"context_generation", snapshot->ContextGeneration},
		{"captured_at_monotonic_us", snapshot->CapturedAtMonotonicUs},
		{"capture_duration_us", snapshot->CaptureDurationUs},
		{"source_object_count", snapshot->SourceObjectCount},
		{"record_count", snapshot->Objects.size()},
		{"skipped_slots", snapshot->SkippedSlots},
		{"items", std::move(items)},
		{"has_more", hasMore},
		{"next_cursor", std::move(nextCursor)}
	};
	return Success(request, std::move(data), timing());
}

CoreCommandResponse CoreCommandService::ExecuteStatus(const CoreCommandRequest& request)
{
	const auto started = std::chrono::steady_clock::now();
	if (!request.Data.is_object() || !request.Data.empty())
		return Failure(request, "INVALID_ARGUMENT", "Status command data must be an empty object");
	if (request.Operation == kStatusReconnect)
	{
		return Failure(
			request,
			"RECONNECT_DISABLED",
			"Engine globals cannot be mutated without an exclusive CoreRuntime transition");
	}

	const Runtime::CoreRuntimeSnapshot snapshot = m_Runtime.Snapshot();
	if (request.Operation == kStatusHealth)
	{
		json data = SerializeRuntime(snapshot);
		data["alive"] = snapshot.IsLive();
		data["game_thread"] = SerializeGameThreadDiagnostics(m_GameThread);
		data["object_snapshot"] = SerializeSnapshotDiagnostics(m_Engine);
		data["reflection"] = SerializeReflectionDiagnostics(
			m_Engine, m_ReflectionSource);
		data["type_snapshot"] = SerializeTypeSnapshotDiagnostics(
			m_Engine, m_TypeSource);
		return Success(request, std::move(data), {.ExecuteUs = ElapsedMicroseconds(started)});
	}
	if (!snapshot.Context)
		return Failure(request, "CORE_CONTEXT_UNAVAILABLE", "Immutable EngineContext is unavailable");

	const Runtime::EngineContext& context = *snapshot.Context;
	const ScriptOffsetDiagnostics scriptDiagnostics =
		m_StatusDiagnostics.CaptureScriptOffsetDiagnostics();
	if (request.Operation == kStatusInspect)
	{
		json data;
		data["game_name"] = context.GameName();
		data["game_version"] = context.GameVersion();
		data["object_count"] = context.ObjectCount();
		data["gobjects_address"] = std::format("0x{:X}", context.ObjectArrayAddress());
		data["pid"] = context.ProcessId();
		data["architecture"] = m_StatusDiagnostics.ProcessArchitecture();
		data["script_offset_diagnostics"] = SerializeScriptOffsetDiagnostics(scriptDiagnostics);
		data["name_profile"] = SerializeNameProfile(context.NameProfile());
		data["runtime"] = SerializeRuntime(snapshot);
		data["capabilities"] = SerializeCapabilities(snapshot);
		data["game_thread"] = SerializeGameThreadDiagnostics(m_GameThread);
		data["object_snapshot"] = SerializeSnapshotDiagnostics(m_Engine);
		data["reflection"] = SerializeReflectionDiagnostics(
			m_Engine, m_ReflectionSource);
		data["type_snapshot"] = SerializeTypeSnapshotDiagnostics(
			m_Engine, m_TypeSource);
		return Success(request, std::move(data), {.ExecuteUs = ElapsedMicroseconds(started)});
	}

	json offsets;
	offsets["gobjects"] = OffsetValue(context, "gobjects");
	offsets["gnames"] = OffsetValue(context, "gnames");
	offsets["gworld"] = OffsetValue(context, "gworld");
	offsets["gengine"] = OffsetValue(context, "gengine");
	offsets["process_event_index"] = OffsetValue(context, "process_event.index");
	offsets["process_event_offset"] = OffsetValue(context, "process_event.offset");
	offsets["fuobjectitem_serial"] = OffsetValue(context, "fuobjectitem.serial_number");
	offsets["ulevel_actors"] = OffsetValue(context, "ulevel.actors");
	offsets["ufunction_script"] = OffsetValue(context, "ufunction.script");

	json addresses;
	addresses["module_base"] = std::format("0x{:X}", context.ModuleBase());
	addresses["gobjects"] = std::format("0x{:X}", context.ObjectArrayAddress());
	addresses["gnames"] = ModuleRelativeAddress(context, "gnames");
	addresses["gworld_ptr"] = ModuleRelativeAddress(context, "gworld");
	addresses["gengine_ptr"] = ModuleRelativeAddress(context, "gengine");

	json internals;
	internals["use_fproperty"] = context.Profile().UsesFProperty;
	internals["use_namepool"] = context.Profile().UsesNamePool;
	internals["use_large_world_coordinates"] = context.Profile().UsesLargeWorldCoordinates;
	internals["use_case_preserving_name"] = context.Profile().UsesCasePreservingName;
	internals["is_enum_name_only"] = context.Profile().EnumNameOnly;
	internals["is_small_enum_value"] = context.Profile().SmallEnumValue;

	json data;
	data["game_name"] = context.GameName();
	data["game_version"] = context.GameVersion();
	data["architecture"] = m_StatusDiagnostics.ProcessArchitecture();
	data["pid"] = context.ProcessId();
	data["object_count"] = context.ObjectCount();
	data["offsets"] = std::move(offsets);
	data["offset_reports"] = SerializeOffsetReports(context);
	data["name_profile"] = SerializeNameProfile(context.NameProfile());
	data["addresses"] = std::move(addresses);
	data["internals"] = std::move(internals);
	data["script_offset_diagnostics"] = SerializeScriptOffsetDiagnostics(scriptDiagnostics);
	data["runtime"] = SerializeRuntime(snapshot);
	data["capabilities"] = SerializeCapabilities(snapshot);
	data["game_thread"] = SerializeGameThreadDiagnostics(m_GameThread);
	data["object_snapshot"] = SerializeSnapshotDiagnostics(m_Engine);
	data["reflection"] = SerializeReflectionDiagnostics(
		m_Engine, m_ReflectionSource);
	data["type_snapshot"] = SerializeTypeSnapshotDiagnostics(
		m_Engine, m_TypeSource);
	return Success(request, std::move(data), {.ExecuteUs = ElapsedMicroseconds(started)});
}

CoreCommandResponse CoreCommandService::ExecuteHandleIssue(
	const CoreCommandRequest& request,
	const bool functionHandle,
	const GameThreadQueuedCallback& onGameThreadQueued)
{
	std::int32_t index = -1;
	if (!TryParseStrictIndex(request.Data, index))
	{
		return Failure(
			request,
			"INVALID_ARGUMENT",
			"Handle issue data must contain exactly one non-negative int32 index");
	}

	std::string admissionError;
	auto lease = m_Runtime.TryAcquireRequest(&admissionError);
	if (!lease)
	{
		return Failure(
			request,
			admissionError.empty() ? "CORE_NOT_READY" : admissionError,
			"CoreRuntime is not accepting domain commands");
	}
	const char* capabilityName = functionHandle ? "functions.handles" : "objects.handles";
	const Runtime::CapabilityStatus* capability = lease->Capabilities()
		? lease->Capabilities()->Find(capabilityName)
		: nullptr;
	if (!capability || !capability->Available)
	{
		return Failure(
			request,
			capability && !capability->ReasonCode.empty()
				? capability->ReasonCode
				: "OBJECT_HANDLE_CAPABILITY_UNAVAILABLE",
			capability && !capability->Reason.empty()
				? capability->Reason
				: "Stable execution handles are unavailable",
			{{"capability", capabilityName}});
	}

	std::shared_ptr<Runtime::IGameThreadWork> work;
	std::shared_ptr<IssueObjectHandleWork> objectWork;
	std::shared_ptr<IssueFunctionHandleWork> functionWork;
	if (functionHandle)
	{
		functionWork = std::make_shared<IssueFunctionHandleWork>(
			std::move(*lease),
			m_Engine,
			index);
		work = functionWork;
	}
	else
	{
		objectWork = std::make_shared<IssueObjectHandleWork>(
			std::move(*lease),
			m_Engine,
			index);
		work = objectWork;
	}

	Runtime::GameThreadTicket ticket;
	const Runtime::GameThreadQueueResult queued = m_GameThread.Enqueue(
		work,
		std::chrono::steady_clock::now() + std::chrono::milliseconds(request.TimeoutMs),
		ticket);
	if (queued != Runtime::GameThreadQueueResult::Accepted)
	{
		switch (queued)
		{
		case Runtime::GameThreadQueueResult::Disabled:
			return Failure(request, "GAME_THREAD_UNAVAILABLE", "Game-thread executor is disabled");
		case Runtime::GameThreadQueueResult::QueueBusy:
			return Failure(request, "GAME_THREAD_QUEUE_BUSY", "Game-thread queue capacity is exhausted");
		case Runtime::GameThreadQueueResult::Invalid:
			return Failure(request, "GAME_THREAD_TASK_INVALID", "Game-thread task or deadline is invalid");
		case Runtime::GameThreadQueueResult::Accepted:
			break;
		}
	}

	if (onGameThreadQueued)
	{
		try
		{
			onGameThreadQueued(ticket);
		}
		catch (...)
		{
			m_GameThread.Cancel(ticket);
			return Failure(request, "COMMAND_OBSERVER_FAILED", "Request task observer raised an exception");
		}
	}

	const Runtime::GameThreadSubmitResult submitted = m_GameThread.Wait(ticket);
	Runtime::GameThreadTaskTiming taskTiming;
	m_GameThread.TryGetTiming(ticket, taskTiming);
	const CoreCommandTiming timing = ToCommandTiming(taskTiming);
	switch (submitted)
	{
	case Runtime::GameThreadSubmitResult::Completed:
		break;
	case Runtime::GameThreadSubmitResult::Disabled:
		return Failure(request, "GAME_THREAD_UNAVAILABLE", "Game-thread executor is disabled", json::object(), timing);
	case Runtime::GameThreadSubmitResult::Cancelled:
		return Failure(request, "REQUEST_CANCELLED", "Queued game-thread command was cancelled", json::object(), timing);
	case Runtime::GameThreadSubmitResult::PumpThreadWaitDenied:
		return Failure(request, "GAME_THREAD_REENTRANT_WAIT_DENIED", "Synchronous waits are forbidden on the pump thread", json::object(), timing);
	case Runtime::GameThreadSubmitResult::QueueBusy:
		return Failure(request, "GAME_THREAD_QUEUE_BUSY", "Game-thread queue capacity is exhausted", json::object(), timing);
	case Runtime::GameThreadSubmitResult::TimedOutBeforeStart:
		return Failure(request, "GAME_THREAD_TIMEOUT_BEFORE_START", "Game-thread command expired before execution", json::object(), timing);
	case Runtime::GameThreadSubmitResult::TimedOutWhileRunning:
		return Failure(request, "GAME_THREAD_TIMEOUT_WHILE_RUNNING", "Game-thread command is still running; completion is unknown", json::object(), timing);
	case Runtime::GameThreadSubmitResult::ExecutionFailed:
		return Failure(request, "GAME_THREAD_EXECUTION_FAILED", "Game-thread command failed during guarded execution", json::object(), timing);
	}

	if (functionHandle)
	{
		const Runtime::FunctionHandleResult& result = functionWork->Result();
		if (!result.Ok())
		{
			return Failure(
				request,
				Runtime::ToString(result.Error),
				"Function handle identity validation failed",
				{{"index", index}},
				timing);
		}
		return Success(request, SerializeFunctionHandle(result.Value), timing);
	}

	const Runtime::ObjectHandleResult& result = objectWork->Result();
	if (!result.Ok())
	{
		return Failure(
			request,
			Runtime::ToString(result.Error),
			"Object handle identity validation failed",
			{{"index", index}},
			timing);
	}
	return Success(request, SerializeObjectHandle(result.Value), timing);
}

CoreCommandResponse CoreCommandService::Success(
	const CoreCommandRequest& request,
	json data,
	const CoreCommandTiming timing) const
{
	return {
		.Ok = true,
		.RequestId = request.RequestId,
		.SessionId = m_SessionId,
		.Data = std::move(data),
		.Timing = timing
	};
}

CoreCommandResponse CoreCommandService::Failure(
	const CoreCommandRequest& request,
	std::string code,
	std::string message,
	json details,
	const CoreCommandTiming timing) const
{
	return {
		.Ok = false,
		.RequestId = request.RequestId,
		.SessionId = m_SessionId,
		.Data = nullptr,
		.Error = CoreCommandError{
			.Code = std::move(code),
			.Message = std::move(message),
			.Details = std::move(details)
		},
		.Timing = timing
	};
}

} // namespace UExplorer::Services
