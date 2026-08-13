#include "DumpCommandService.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kSdkStart = "dump.sdk.start";
constexpr std::string_view kUsmapStart = "dump.usmap.start";
constexpr std::string_view kDumpspaceStart = "dump.dumpspace.start";
constexpr std::string_view kIdaStart = "dump.ida.start";
constexpr std::string_view kJobsList = "dump.jobs.list";
constexpr std::string_view kJobsGet = "dump.jobs.get";
constexpr std::string_view kJobsCancel = "dump.jobs.cancel";
constexpr std::string_view kOptionsEnvelopeSchema = "uexplorer.dump.start.v1";
constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxOutputIdentityBytes = 128;

struct RequestScope
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
};

DumpCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {.Error = DumpCommandError{
		.Code = std::move(code),
		.Message = std::move(message),
		.Details = std::move(details)}};
}

DumpCommandResult CoordinatorFailure(
	const Runtime::DumpJobError error,
	const std::string_view action)
{
	return Failure(
		Runtime::ToString(error),
		std::string("Dump coordinator rejected ") + std::string(action) + ".",
		{{"coordinator_error", Runtime::ToString(error)}});
}

bool IsBoundedText(const std::string& value, const std::size_t maximum) noexcept
{
	return !value.empty()
		&& value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

bool IsOutputIdentity(const std::string& value) noexcept
{
	if (value.empty()
		|| value.size() > kMaxOutputIdentityBytes
		|| value == "."
		|| value == "..")
	{
		return false;
	}
	const unsigned char first = static_cast<unsigned char>(value.front());
	if (!((first >= 'a' && first <= 'z')
		|| (first >= 'A' && first <= 'Z')
		|| (first >= '0' && first <= '9')))
	{
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
		return (character >= 'a' && character <= 'z')
			|| (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9')
			|| character == '-'
			|| character == '_'
			|| character == '.';
	});
}

bool HasExactKeys(
	const json& data,
	const std::initializer_list<std::string_view> keys)
{
	if (!data.is_object() || data.size() != keys.size())
		return false;
	return std::all_of(keys.begin(), keys.end(), [&data](const std::string_view key) {
		return data.contains(std::string(key));
	});
}

bool TryUnsigned(
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
	const auto converted = static_cast<std::uint64_t>(candidate);
	if (converted < minimum || converted > maximum)
		return false;
	parsed = converted;
	return true;
}

bool TrySize(
	const json& value,
	const std::size_t minimum,
	const std::size_t maximum,
	std::size_t& parsed)
{
	std::uint64_t converted = 0;
	if (!TryUnsigned(value, minimum, maximum, converted))
		return false;
	parsed = static_cast<std::size_t>(converted);
	return true;
}

bool IsCanonicalUnsignedDecimal(const std::string_view encoded) noexcept
{
	if (encoded.empty())
		return false;
	if (encoded.front() == '0')
		return encoded.size() == 1;
	return encoded.front() >= '1'
		&& encoded.front() <= '9'
		&& std::all_of(encoded.begin() + 1, encoded.end(), [](const char character) {
			return character >= '0' && character <= '9';
		});
}

bool TryJobId(const json& value, Runtime::DumpJobId& parsed) noexcept
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalUnsignedDecimal(encoded))
		return false;
	const auto converted = std::from_chars(
		encoded.data(),
		encoded.data() + encoded.size(),
		parsed,
		10);
	return converted.ec == std::errc{}
		&& converted.ptr == encoded.data() + encoded.size()
		&& parsed > 0
		&& parsed <= kMaxProtocolInteger;
}

bool TryScope(const json& data, RequestScope& scope)
{
	scope = {};
	if (!data.at("session_id").is_string())
		return false;
	scope.SessionId = data.at("session_id").get<std::string>();
	return IsBoundedText(scope.SessionId, kMaxSessionBytes)
		&& TryUnsigned(
			data.at("context_generation"),
			1,
			kMaxProtocolInteger,
			scope.ContextGeneration)
		&& TryUnsigned(
			data.at("object_snapshot_generation"),
			1,
			kMaxProtocolInteger,
			scope.ObjectSnapshotGeneration)
		&& TryUnsigned(
			data.at("type_snapshot_generation"),
			1,
			kMaxProtocolInteger,
			scope.TypeSnapshotGeneration);
}

bool ScopeMatches(
	const RequestScope& scope,
	const Runtime::DumpJobRequest& request) noexcept
{
	return request.Spec.SessionId == scope.SessionId
		&& request.Spec.ContextGeneration == scope.ContextGeneration
		&& request.Spec.ObjectSnapshotGeneration == scope.ObjectSnapshotGeneration
		&& request.Spec.TypeSnapshotGeneration == scope.TypeSnapshotGeneration;
}

json SerializeScope(const RequestScope& scope)
{
	return {
		{"session_id", scope.SessionId},
		{"context_generation", scope.ContextGeneration},
		{"object_snapshot_generation", scope.ObjectSnapshotGeneration},
		{"type_snapshot_generation", scope.TypeSnapshotGeneration}
	};
}

std::string_view FormatForOperation(const std::string_view operation) noexcept
{
	if (operation == kSdkStart)
		return "sdk";
	if (operation == kUsmapStart)
		return "usmap";
	if (operation == kDumpspaceStart)
		return "dumpspace";
	if (operation == kIdaStart)
		return "ida-script";
	return {};
}

bool IsKnownFormat(const std::string_view format) noexcept
{
	return format == "sdk"
		|| format == "usmap"
		|| format == "dumpspace"
		|| format == "ida-script";
}

std::vector<std::byte> EncodeOptionsEnvelope(const std::string_view format)
{
	const std::string encoded = json{
		{"format", format},
		{"options", json::object()},
		{"schema", kOptionsEnvelopeSchema}
	}.dump();
	std::vector<std::byte> bytes;
	bytes.reserve(encoded.size());
	for (const unsigned char character : encoded)
		bytes.push_back(static_cast<std::byte>(character));
	return bytes;
}

bool TryDecodeOptionsEnvelope(
	const std::vector<std::byte>& bytes,
	std::string& format)
{
	format.clear();
	std::string encoded;
	encoded.reserve(bytes.size());
	for (const std::byte value : bytes)
		encoded.push_back(static_cast<char>(value));
	json envelope;
	try
	{
		envelope = json::parse(encoded);
	}
	catch (const json::exception&)
	{
		return false;
	}
	if (!HasExactKeys(envelope, {"format", "options", "schema"})
		|| !envelope.at("format").is_string()
		|| !envelope.at("schema").is_string()
		|| envelope.at("schema").get_ref<const std::string&>() != kOptionsEnvelopeSchema
		|| !envelope.at("options").is_object()
		|| !envelope.at("options").empty())
	{
		return false;
	}
	format = envelope.at("format").get<std::string>();
	return IsKnownFormat(format);
}

std::uint64_t MonotonicMicroseconds(
	const std::chrono::steady_clock::time_point value) noexcept
{
	const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
		value.time_since_epoch()).count();
	return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

const char* ToString(const Runtime::DumpJobCancelDisposition disposition) noexcept
{
	switch (disposition)
	{
	case Runtime::DumpJobCancelDisposition::None: return "none";
	case Runtime::DumpJobCancelDisposition::CancelledBeforeStart: return "cancelled_before_start";
	case Runtime::DumpJobCancelDisposition::CancellationRequested: return "cancellation_requested";
	}
	return "unknown";
}

json SerializeSummary(const Runtime::DumpJobSummary& summary)
{
	if (!summary.Request)
		throw std::invalid_argument("dump summary has no immutable request");
	std::string format;
	if (!TryDecodeOptionsEnvelope(summary.Request->Spec.OpaqueOptions, format))
		throw std::invalid_argument("dump summary has an invalid options envelope");

	json error = nullptr;
	if (!summary.ErrorCode.empty() || !summary.ErrorMessage.empty())
	{
		error = {
			{"code", summary.ErrorCode},
			{"message", summary.ErrorMessage}
		};
	}
	return {
		{"job_id", std::to_string(summary.Request->Id)},
		{"format", format},
		{"state", Runtime::ToString(summary.State)},
		{"scope", {
			{"session_id", summary.Request->Spec.SessionId},
			{"context_generation", summary.Request->Spec.ContextGeneration},
			{"object_snapshot_generation", summary.Request->Spec.ObjectSnapshotGeneration},
			{"type_snapshot_generation", summary.Request->Spec.TypeSnapshotGeneration}
		}},
		{"output_path_identity", summary.Request->Spec.OutputPathIdentity},
		{"submitted_at_monotonic_us", MonotonicMicroseconds(summary.Request->SubmittedAt)},
		{"deadline_at_monotonic_us", MonotonicMicroseconds(summary.Request->Deadline)},
		{"started_at_monotonic_us", summary.StartedAtMonotonicUs},
		{"finished_at_monotonic_us", summary.FinishedAtMonotonicUs},
		{"cancellation_requested", summary.CancellationRequested},
		{"deadline_exceeded", summary.DeadlineExceeded},
		{"error", std::move(error)},
		{"retained_event_count", summary.RetainedEventCount},
		{"retained_event_bytes", summary.RetainedEventBytes},
		{"dropped_event_count", summary.DroppedEventCount},
		{"last_event_sequence", summary.LastEventSequence}
	};
}

json SerializeEvent(const Runtime::DumpJobEvent& event)
{
	json payload;
	if (event.Kind == Runtime::DumpJobEventKind::Progress)
	{
		payload = {
			{"phase", event.Progress.Phase},
			{"completed", event.Progress.Completed},
			{"total", event.Progress.Total},
			{"message", event.Progress.Message}
		};
	}
	else
	{
		payload = {
			{"severity", Runtime::ToString(event.Diagnostic.Severity)},
			{"code", event.Diagnostic.Code},
			{"message", event.Diagnostic.Message}
		};
	}
	return {
		{"sequence", event.Sequence},
		{"recorded_at_monotonic_us", event.RecordedAtMonotonicUs},
		{"kind", Runtime::ToString(event.Kind)},
		{"payload", std::move(payload)}
	};
}

DumpCommandResult ScopeMismatch(
	const Runtime::DumpJobId id,
	const RequestScope& requested)
{
	return Failure(
		"DUMP_JOB_SCOPE_MISMATCH",
		"The job is not bound to the requested immutable session/snapshot scope.",
		{{"job_id", std::to_string(id)}, {"requested_scope", SerializeScope(requested)}});
}

DumpCommandResult EnsureResponseBound(DumpCommandResult result)
{
	json envelope;
	if (result.Ok())
	{
		envelope = {{"data", result.Data}};
	}
	else
	{
		envelope = {{"error", {
			{"code", result.Error->Code},
			{"message", result.Error->Message},
			{"details", result.Error->Details}
		}}};
	}
	if (envelope.dump().size() <= DumpCommandService::kMaxSerializedDataBytes)
		return result;
	return Failure(
		"DUMP_RESPONSE_TOO_LARGE",
		"The serialized dump command response exceeds the 4 MiB service limit.");
}

} // namespace

DumpCommandService::DumpCommandService(
	Runtime::DumpJobCoordinator* const coordinator) noexcept
	: m_Coordinator(coordinator)
{
}

bool DumpCommandService::Handles(const std::string_view operation) noexcept
{
	return !FormatForOperation(operation).empty()
		|| operation == kJobsList
		|| operation == kJobsGet
		|| operation == kJobsCancel;
}

DumpCommandResult DumpCommandService::Execute(
	const std::string_view operation,
	const json& data,
	std::shared_ptr<const Runtime::IDumpJobInput> input) noexcept
{
	try
	{
		DumpCommandResult result;
		if (!FormatForOperation(operation).empty())
			result = Start(operation, data, std::move(input));
		else if (operation == kJobsList)
			result = List(data);
		else if (operation == kJobsGet)
			result = Get(data);
		else if (operation == kJobsCancel)
			result = Cancel(data);
		else
		{
			result = Failure(
				"OPERATION_NOT_SUPPORTED",
				"The dump command service does not implement this operation.");
		}
		return EnsureResponseBound(std::move(result));
	}
	catch (const std::invalid_argument& exception)
	{
		return Failure(
			"DUMP_JOB_RECORD_INVALID",
			"A retained dump job violated the command service record contract.",
			{{"reason", exception.what()}});
	}
	catch (...)
	{
		return Failure(
			"DUMP_ALLOCATION_FAILED",
			"The dump command could not allocate its bounded response state.");
	}
}

DumpCommandResult DumpCommandService::Start(
	const std::string_view operation,
	const json& data,
	std::shared_ptr<const Runtime::IDumpJobInput> input)
{
	if (!m_Coordinator || !m_Coordinator->IsConfigured())
	{
		return Failure(
			"DUMP_WORKER_UNAVAILABLE",
			"No owned dump worker is injected; starts are unavailable.");
	}
	if (!input)
	{
		return Failure(
			"DUMP_INPUT_SNAPSHOT_UNAVAILABLE",
			"Dump start requires a caller-pinned immutable execution input.");
	}
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"format",
		"output_path_identity",
		"deadline_ms",
		"options"}))
	{
		return Failure(
			"DUMP_REQUEST_SCHEMA_INVALID",
			"Dump start data must contain only the required v1 fields.");
	}

	RequestScope scope;
	if (!TryScope(data, scope))
	{
		return Failure(
			"DUMP_SCOPE_INVALID",
			"Dump start requires a valid session/context/object/type generation scope.");
	}
	if (!data.at("format").is_string())
		return Failure("DUMP_FORMAT_INVALID", "Dump format must be a string discriminator.");
	const std::string& format = data.at("format").get_ref<const std::string&>();
	const std::string_view expectedFormat = FormatForOperation(operation);
	if (!IsKnownFormat(format) || format != expectedFormat)
	{
		return Failure(
			"DUMP_FORMAT_OPERATION_MISMATCH",
			"The explicit format discriminator does not match the start operation.",
			{{"expected_format", expectedFormat}});
	}
	if (!data.at("output_path_identity").is_string()
		|| !IsOutputIdentity(
			data.at("output_path_identity").get_ref<const std::string&>()))
	{
		return Failure(
			"DUMP_OUTPUT_IDENTITY_INVALID",
			"output_path_identity must be a 1..128 byte ASCII token; path separators and traversal are forbidden.");
	}
	std::uint64_t deadlineMs = 0;
	if (!TryUnsigned(
		data.at("deadline_ms"),
		1,
		static_cast<std::uint64_t>(Runtime::DumpJobCoordinator::kMaxDeadlineMs),
		deadlineMs))
	{
		return Failure(
			"DUMP_DEADLINE_INVALID",
			"deadline_ms must be an integer from 1 through 86400000.");
	}
	if (!data.at("options").is_object())
	{
		return Failure(
			"DUMP_OPTIONS_SCHEMA_INVALID",
			"Dump options must be an object with the closed v1 schema.");
	}
	if (!data.at("options").empty())
	{
		return Failure(
			"DUMP_OPTION_NOT_SUPPORTED",
			"No per-format dump option is implemented by the v1 command service.");
	}

	Runtime::DumpJobSpec spec;
	spec.SessionId = std::move(scope.SessionId);
	spec.ContextGeneration = scope.ContextGeneration;
	spec.ObjectSnapshotGeneration = scope.ObjectSnapshotGeneration;
	spec.TypeSnapshotGeneration = scope.TypeSnapshotGeneration;
	spec.OutputPathIdentity = data.at("output_path_identity").get<std::string>();
	const std::string outputIdentity = spec.OutputPathIdentity;
	spec.OpaqueOptions = EncodeOptionsEnvelope(format);
	spec.Input = std::move(input);
	const Runtime::DumpJobSubmitResult submitted = m_Coordinator->Submit(
		std::move(spec),
		std::chrono::milliseconds(deadlineMs));
	if (!submitted.Ok())
		return CoordinatorFailure(submitted.Error, "job submission");
	return {.Data = {
		{"job_id", std::to_string(submitted.Id)},
		{"format", format},
		{"output_path_identity", outputIdentity},
		{"admission", "accepted"}
	}};
}

DumpCommandResult DumpCommandService::List(const json& data)
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"max_jobs"}))
	{
		return Failure(
			"DUMP_REQUEST_SCHEMA_INVALID",
			"Dump job list data must contain only the required v1 fields.");
	}
	RequestScope scope;
	std::size_t maxJobs = 0;
	if (!TryScope(data, scope)
		|| !TrySize(data.at("max_jobs"), 1, kMaxListJobs, maxJobs))
	{
		return Failure(
			"DUMP_LIST_REQUEST_INVALID",
			"Dump job list requires a valid immutable scope and max_jobs from 1 through 64.");
	}

	if (!m_Coordinator || !m_Coordinator->IsConfigured())
	{
		return Failure(
			"DUMP_WORKER_UNAVAILABLE",
			"No owned dump coordinator/worker is injected; an empty job store is not fabricated.",
			{{"scope", SerializeScope(scope)}});
	}

	const Runtime::DumpJobCoordinatorSnapshot state = m_Coordinator->Snapshot();
	const std::size_t coordinatorLimit = (std::max)(
		std::size_t{1},
		state.RetainedJobCount);
	const Runtime::DumpJobListResult listed = m_Coordinator->List(coordinatorLimit);
	if (!listed.Ok())
		return CoordinatorFailure(listed.Error, "job listing");

	json jobs = json::array();
	std::size_t matchingJobs = 0;
	for (const Runtime::DumpJobSummary& summary : listed.Jobs)
	{
		if (!summary.Request || !ScopeMatches(scope, *summary.Request))
			continue;
		++matchingJobs;
		if (jobs.size() < maxJobs)
			jobs.push_back(SerializeSummary(summary));
	}
	return {.Data = {
		{"scope", SerializeScope(scope)},
		{"jobs", std::move(jobs)},
		{"more_jobs_available", matchingJobs > maxJobs}
	}};
}

DumpCommandResult DumpCommandService::Get(const json& data)
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"job_id",
		"after_event_sequence",
		"max_events"}))
	{
		return Failure(
			"DUMP_REQUEST_SCHEMA_INVALID",
			"Dump job get data must contain only the required v1 fields.");
	}

	RequestScope scope;
	Runtime::DumpJobId id = 0;
	std::uint64_t afterSequence = 0;
	std::size_t maxEvents = 0;
	if (!TryScope(data, scope)
		|| !TryJobId(data.at("job_id"), id)
		|| !TryUnsigned(
			data.at("after_event_sequence"),
			0,
			kMaxProtocolInteger,
			afterSequence)
		|| !TrySize(data.at("max_events"), 1, kMaxGetEvents, maxEvents))
	{
		return Failure(
			"DUMP_GET_REQUEST_INVALID",
			"Dump job get requires a valid scope, canonical job_id, event sequence, and bounded event limit.");
	}
	if (!m_Coordinator || !m_Coordinator->IsConfigured())
		return CoordinatorFailure(Runtime::DumpJobError::NotFound, "job lookup");

	Runtime::DumpJobSnapshotResult snapshot = m_Coordinator->Snapshot(id, afterSequence, 1);
	if (!snapshot.Ok())
		return CoordinatorFailure(snapshot.Error, "job lookup");
	if (!snapshot.Summary.Request || !ScopeMatches(scope, *snapshot.Summary.Request))
		return ScopeMismatch(id, scope);

	const std::size_t boundedEventCount = (std::max)(
		std::size_t{1},
		(std::min)(maxEvents, snapshot.Summary.RetainedEventCount));
	if (boundedEventCount != 1)
	{
		snapshot = m_Coordinator->Snapshot(id, afterSequence, boundedEventCount);
		if (!snapshot.Ok())
			return CoordinatorFailure(snapshot.Error, "job event lookup");
		if (!snapshot.Summary.Request || !ScopeMatches(scope, *snapshot.Summary.Request))
			return ScopeMismatch(id, scope);
	}

	json events = json::array();
	for (const Runtime::DumpJobEvent& event : snapshot.Events)
		events.push_back(SerializeEvent(event));
	const bool eventGap = !snapshot.Events.empty()
		&& snapshot.Events.front().Sequence > afterSequence
		&& snapshot.Events.front().Sequence - afterSequence > 1;
	return {.Data = {
		{"job", SerializeSummary(snapshot.Summary)},
		{"events", std::move(events)},
		{"more_events_available", snapshot.MoreEventsAvailable},
		{"event_gap_detected", eventGap}
	}};
}

DumpCommandResult DumpCommandService::Cancel(const json& data)
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"job_id"}))
	{
		return Failure(
			"DUMP_REQUEST_SCHEMA_INVALID",
			"Dump job cancel data must contain only the required v1 fields.");
	}
	RequestScope scope;
	Runtime::DumpJobId id = 0;
	if (!TryScope(data, scope) || !TryJobId(data.at("job_id"), id))
	{
		return Failure(
			"DUMP_CANCEL_REQUEST_INVALID",
			"Dump job cancel requires a valid immutable scope and canonical job_id.");
	}
	if (!m_Coordinator || !m_Coordinator->IsConfigured())
		return CoordinatorFailure(Runtime::DumpJobError::NotFound, "job cancellation");

	const Runtime::DumpJobSnapshotResult snapshot = m_Coordinator->Snapshot(id, 0, 1);
	if (!snapshot.Ok())
		return CoordinatorFailure(snapshot.Error, "job cancellation lookup");
	if (!snapshot.Summary.Request || !ScopeMatches(scope, *snapshot.Summary.Request))
		return ScopeMismatch(id, scope);
	const Runtime::DumpJobCancelResult cancelled = m_Coordinator->Cancel(id);
	if (!cancelled.Ok())
		return CoordinatorFailure(cancelled.Error, "job cancellation");
	return {.Data = {
		{"job_id", std::to_string(id)},
		{"disposition", ToString(cancelled.Disposition)}
	}};
}

} // namespace UExplorer::Services
