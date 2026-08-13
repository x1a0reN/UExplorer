#include "FunctionCallBatchCommandService.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kSubmitOperation = "call.batch";
constexpr std::string_view kGetOperation = "call.batch.get";
constexpr std::string_view kCancelOperation = "call.batch.cancel";
constexpr std::string_view kListOperation = "call.batch.list";
constexpr std::uint64_t kMaxProtocolInteger =
	Runtime::FunctionCallBatchCoordinator::kMaxProtocolInteger;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxArgumentCount = 128;
constexpr std::size_t kMaxJsonNodes = 65'536;
constexpr std::size_t kMaxJsonDepth = 64;
constexpr std::size_t kMaxDiagnosticCodeBytes = 256;
constexpr std::size_t kMaxDiagnosticMessageBytes = 4096;

struct RequestScope
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
};

struct DecodedCallData
{
	Runtime::ObjectHandle Target;
	Runtime::FunctionHandle Function;
	std::string FunctionPath;
	json Data = json::object();
};

FunctionCallBatchCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {.Error = FunctionCallBatchCommandError{
		.Code = std::move(code),
		.Message = std::move(message),
		.Details = std::move(details)}};
}

FunctionCallBatchCommandResult CoordinatorFailure(
	const Runtime::FunctionCallBatchError error,
	const std::string_view action)
{
	return Failure(
		Runtime::ToString(error),
		std::string("The call batch coordinator rejected ") + std::string(action) + ".",
		{{"coordinator_error", Runtime::ToString(error)}});
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

bool IsBoundedText(const std::string& value, const std::size_t maximum) noexcept
{
	return !value.empty()
		&& value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
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

bool IsUpperHex(const char character) noexcept
{
	return (character >= '0' && character <= '9')
		|| (character >= 'A' && character <= 'F');
}

bool TryCanonicalHex(
	const std::string& encoded,
	const bool prefixed,
	const bool fixedWidth,
	std::uint64_t& parsed) noexcept
{
	parsed = 0;
	const std::size_t prefix = prefixed ? 2 : 0;
	if ((prefixed && !encoded.starts_with("0x"))
		|| encoded.size() <= prefix
		|| encoded.size() - prefix > 16
		|| (fixedWidth && encoded.size() - prefix != 16)
		|| (!fixedWidth && encoded.size() - prefix > 1 && encoded[prefix] == '0'))
	{
		return false;
	}
	const std::string_view digits(encoded.data() + prefix, encoded.size() - prefix);
	if (!std::all_of(digits.begin(), digits.end(), IsUpperHex))
		return false;
	const auto converted = std::from_chars(
		digits.data(), digits.data() + digits.size(), parsed, 16);
	return converted.ec == std::errc{}
		&& converted.ptr == digits.data() + digits.size();
}

bool TryParseObjectHandle(const json& data, Runtime::ObjectHandle& handle)
{
	handle = {};
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"index",
		"serial",
		"address",
		"class_fingerprint"})
		|| !data.at("session_id").is_string()
		|| !data.at("address").is_string()
		|| !data.at("class_fingerprint").is_string())
	{
		return false;
	}
	handle.SessionId = data.at("session_id").get<std::string>();
	std::uint64_t contextGeneration = 0;
	std::uint64_t index = 0;
	std::uint64_t serial = 0;
	std::uint64_t address = 0;
	std::uint64_t classFingerprint = 0;
	if (!IsBoundedText(handle.SessionId, kMaxSessionBytes)
		|| !TryUnsigned(
			data.at("context_generation"),
			1,
			kMaxProtocolInteger,
			contextGeneration)
		|| !TryUnsigned(
			data.at("index"),
			0,
			static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()),
			index)
		|| !TryUnsigned(
			data.at("serial"),
			1,
			static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()),
			serial)
		|| !TryCanonicalHex(
			data.at("address").get_ref<const std::string&>(),
			true,
			false,
			address)
		|| address == 0
		|| address > (std::numeric_limits<std::uintptr_t>::max)()
		|| !TryCanonicalHex(
			data.at("class_fingerprint").get_ref<const std::string&>(),
			false,
			true,
			classFingerprint)
		|| classFingerprint == 0)
	{
		return false;
	}
	handle.ContextGeneration = contextGeneration;
	handle.Index = static_cast<std::int32_t>(index);
	handle.SerialNumber = static_cast<std::int32_t>(serial);
	handle.Address = static_cast<std::uintptr_t>(address);
	handle.ClassFingerprint = classFingerprint;
	return true;
}

bool TryParseFunctionHandle(const json& data, Runtime::FunctionHandle& handle)
{
	handle = {};
	if (!HasExactKeys(data, {
		"function",
		"owner",
		"full_path",
		"signature_fingerprint"})
		|| !data.at("full_path").is_string()
		|| !data.at("signature_fingerprint").is_string()
		|| !TryParseObjectHandle(data.at("function"), handle.Function)
		|| !TryParseObjectHandle(data.at("owner"), handle.Owner))
	{
		return false;
	}
	handle.FullPath = data.at("full_path").get<std::string>();
	std::uint64_t signature = 0;
	if (!IsBoundedText(handle.FullPath, kMaxPathBytes)
		|| !TryCanonicalHex(
			data.at("signature_fingerprint").get_ref<const std::string&>(),
			false,
			true,
			signature)
		|| signature == 0)
	{
		return false;
	}
	handle.SignatureFingerprint = signature;
	return true;
}

bool SameHandle(
	const Runtime::ObjectHandle& left,
	const Runtime::ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameFunctionHandle(
	const Runtime::FunctionHandle& left,
	const Runtime::FunctionHandle& right) noexcept
{
	return SameHandle(left.Function, right.Function)
		&& SameHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
}

bool HandleMatchesScope(
	const Runtime::ObjectHandle& handle,
	const RequestScope& scope) noexcept
{
	return handle.SessionId == scope.SessionId
		&& handle.ContextGeneration == scope.ContextGeneration;
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

json SerializeScope(const RequestScope& scope)
{
	return {
		{"session_id", scope.SessionId},
		{"context_generation", scope.ContextGeneration},
		{"object_snapshot_generation", scope.ObjectSnapshotGeneration},
		{"type_snapshot_generation", scope.TypeSnapshotGeneration}
	};
}

bool ScopeMatches(
	const RequestScope& scope,
	const Runtime::FunctionCallBatchBinding& binding) noexcept
{
	return binding.SessionId == scope.SessionId
		&& binding.ContextGeneration == scope.ContextGeneration
		&& binding.ObjectSnapshotGeneration == scope.ObjectSnapshotGeneration
		&& binding.TypeSnapshotGeneration == scope.TypeSnapshotGeneration;
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

bool TryBatchId(const json& value, Runtime::FunctionCallBatchId& parsed) noexcept
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalUnsignedDecimal(encoded))
		return false;
	const auto converted = std::from_chars(
		encoded.data(), encoded.data() + encoded.size(), parsed, 10);
	return converted.ec == std::errc{}
		&& converted.ptr == encoded.data() + encoded.size()
		&& parsed > 0
		&& parsed <= kMaxProtocolInteger;
}

bool ValidateJsonTree(
	const json& value,
	std::size_t& nodes,
	const std::size_t depth) noexcept
{
	if (depth > kMaxJsonDepth || ++nodes > kMaxJsonNodes)
		return false;
	if (value.is_array())
	{
		for (const json& child : value)
		{
			if (!ValidateJsonTree(child, nodes, depth + 1))
				return false;
		}
	}
	else if (value.is_object())
	{
		for (const auto& [key, child] : value.items())
		{
			if (key.empty() || key.size() > kMaxPathBytes
				|| !ValidateJsonTree(child, nodes, depth + 1))
			{
				return false;
			}
		}
	}
	return true;
}

std::vector<std::byte> ToBytes(const std::string& encoded)
{
	std::vector<std::byte> bytes;
	bytes.reserve(encoded.size());
	for (const unsigned char value : encoded)
		bytes.push_back(static_cast<std::byte>(value));
	return bytes;
}

std::string FromBytes(const std::vector<std::byte>& bytes)
{
	std::string encoded;
	encoded.reserve(bytes.size());
	for (const std::byte value : bytes)
		encoded.push_back(static_cast<char>(value));
	return encoded;
}

bool TryDecodeCallData(
	const Runtime::FunctionCallBatchBinding& binding,
	const Runtime::FunctionCallBatchItem& item,
	DecodedCallData& decoded)
{
	decoded = {};
	json data;
	try
	{
		data = json::parse(FromBytes(item.SerializedRequest));
	}
	catch (const json::exception&)
	{
		return false;
	}
	if (!HasExactKeys(data, {
		"target",
		"function",
		"type_snapshot_generation",
		"function_path",
		"arguments"})
		|| !data.at("function_path").is_string()
		|| !data.at("arguments").is_object()
		|| data.at("arguments").size() > kMaxArgumentCount
		|| !TryParseObjectHandle(data.at("target"), decoded.Target)
		|| !TryParseFunctionHandle(data.at("function"), decoded.Function))
	{
		return false;
	}
	decoded.FunctionPath = data.at("function_path").get<std::string>();
	std::uint64_t typeGeneration = 0;
	std::size_t nodes = 0;
	if (!TryUnsigned(
		data.at("type_snapshot_generation"),
		1,
		kMaxProtocolInteger,
		typeGeneration)
		|| typeGeneration != binding.TypeSnapshotGeneration
		|| !IsBoundedText(decoded.FunctionPath, kMaxPathBytes)
		|| !ValidateJsonTree(data.at("arguments"), nodes, 0)
		|| !SameHandle(decoded.Target, item.Target)
		|| !SameFunctionHandle(decoded.Function, item.Function)
		|| item.ExpectedFunctionPath != item.Function.FullPath
		|| decoded.Target.SessionId != binding.SessionId
		|| decoded.Target.ContextGeneration != binding.ContextGeneration
		|| decoded.Function.Function.SessionId != binding.SessionId
		|| decoded.Function.Function.ContextGeneration != binding.ContextGeneration
		|| decoded.Function.Owner.SessionId != binding.SessionId
		|| decoded.Function.Owner.ContextGeneration != binding.ContextGeneration)
	{
		return false;
	}
	decoded.Data = std::move(data);
	return true;
}

std::uint64_t MonotonicMicroseconds(
	const std::chrono::steady_clock::time_point value) noexcept
{
	const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
		value.time_since_epoch()).count();
	if (count <= 0)
		return 0;
	return (std::min)(static_cast<std::uint64_t>(count), kMaxProtocolInteger);
}

const char* ToString(
	const Runtime::FunctionCallBatchCancelDisposition disposition) noexcept
{
	switch (disposition)
	{
	case Runtime::FunctionCallBatchCancelDisposition::None: return "none";
	case Runtime::FunctionCallBatchCancelDisposition::CancelledBeforeStart:
		return "cancelled_before_start";
	case Runtime::FunctionCallBatchCancelDisposition::CancellationRequested:
		return "cancellation_requested";
	}
	return "unknown";
}

std::uint64_t RequestFingerprint(const std::vector<std::byte>& bytes) noexcept
{
	std::uint64_t hash = 1469598103934665603ULL;
	for (const std::byte value : bytes)
	{
		hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(value));
		hash *= 1099511628211ULL;
	}
	return hash;
}

json SerializeBatchMetadata(const Runtime::FunctionCallBatchSummary& summary)
{
	if (!summary.Request)
		throw std::invalid_argument("call batch summary has no immutable request");
	std::size_t succeeded = 0;
	std::size_t failed = 0;
	std::size_t cancelled = 0;
	std::size_t pending = 0;
	for (const Runtime::FunctionCallBatchItemResult& item : summary.Items)
	{
		switch (item.State)
		{
		case Runtime::FunctionCallBatchItemState::Succeeded: ++succeeded; break;
		case Runtime::FunctionCallBatchItemState::Failed:
		case Runtime::FunctionCallBatchItemState::SkippedFailFast: ++failed; break;
		case Runtime::FunctionCallBatchItemState::Cancelled:
		case Runtime::FunctionCallBatchItemState::DeadlineExceeded: ++cancelled; break;
		case Runtime::FunctionCallBatchItemState::Pending:
		case Runtime::FunctionCallBatchItemState::Running: ++pending; break;
		}
	}
	return {
		{"batch_id", std::to_string(summary.Request->Id)},
		{"scope", {
			{"session_id", summary.Request->Spec.Binding.SessionId},
			{"context_generation", summary.Request->Spec.Binding.ContextGeneration},
			{"object_snapshot_generation", summary.Request->Spec.Binding.ObjectSnapshotGeneration},
			{"type_snapshot_generation", summary.Request->Spec.Binding.TypeSnapshotGeneration}
		}},
		{"policy", Runtime::ToString(summary.Request->Spec.Policy)},
		{"state", Runtime::ToString(summary.State)},
		{"submitted_at_monotonic_us", MonotonicMicroseconds(summary.Request->SubmittedAt)},
		{"deadline_at_monotonic_us", MonotonicMicroseconds(summary.Request->Deadline)},
		{"started_at_monotonic_us", summary.StartedAtMonotonicUs},
		{"finished_at_monotonic_us", summary.FinishedAtMonotonicUs},
		{"cancellation_requested", summary.CancellationRequested},
		{"deadline_exceeded", summary.DeadlineWasExceeded},
		{"shutdown_cancellation_requested", summary.ShutdownCancellationRequested},
		{"retained_result_bytes", summary.RetainedResultBytes},
		{"item_count", summary.Items.size()},
		{"item_counts", {
			{"succeeded", succeeded},
			{"failed", failed},
			{"cancelled_or_deadline", cancelled},
			{"pending_or_running", pending}
		}}
	};
}

json DecodeResponse(const std::vector<std::byte>& bytes)
{
	json response = json::parse(FromBytes(bytes));
	std::size_t nodes = 0;
	if (!ValidateJsonTree(response, nodes, 0))
		throw std::invalid_argument("call batch item response exceeds the JSON shape bound");
	return response;
}

json SerializeBatch(const Runtime::FunctionCallBatchSummary& summary)
{
	if (!summary.Request
		|| summary.Request->Spec.Items.size() != summary.Items.size())
	{
		throw std::invalid_argument("call batch summary request/result cardinality differs");
	}
	json serialized = SerializeBatchMetadata(summary);
	json items = json::array();
	items.get_ref<json::array_t&>().reserve(summary.Items.size());
	for (std::size_t index = 0; index < summary.Items.size(); ++index)
	{
		const Runtime::FunctionCallBatchItem& requestItem =
			summary.Request->Spec.Items[index];
		const Runtime::FunctionCallBatchItemResult& resultItem = summary.Items[index];
		DecodedCallData decoded;
		if (!TryDecodeCallData(summary.Request->Spec.Binding, requestItem, decoded)
			|| resultItem.Index != index)
		{
			throw std::invalid_argument("call batch item identity binding is invalid");
		}

		json error = nullptr;
		if (!resultItem.DiagnosticCode.empty() || !resultItem.DiagnosticMessage.empty())
		{
			error = {
				{"code", resultItem.DiagnosticCode},
				{"message", resultItem.DiagnosticMessage}
			};
		}
		json response = nullptr;
		if (resultItem.State == Runtime::FunctionCallBatchItemState::Succeeded)
			response = DecodeResponse(resultItem.SerializedResponse);
		else if (!resultItem.SerializedResponse.empty())
			throw std::invalid_argument("non-success call batch item retained a response");

		items.push_back({
			{"index", index},
			{"state", Runtime::ToString(resultItem.State)},
			{"target", SerializeObjectHandle(requestItem.Target)},
			{"function", SerializeFunctionHandle(requestItem.Function)},
			{"function_path", decoded.FunctionPath},
			{"request_bytes", requestItem.SerializedRequest.size()},
			{"request_fingerprint", std::format(
				"{:016X}",
				RequestFingerprint(requestItem.SerializedRequest))},
			{"started_at_monotonic_us", resultItem.StartedAtMonotonicUs},
			{"finished_at_monotonic_us", resultItem.FinishedAtMonotonicUs},
			{"response", std::move(response)},
			{"error", std::move(error)}
		});
	}
	serialized["items"] = std::move(items);
	return serialized;
}

FunctionCallBatchCommandResult ScopeMismatch(
	const Runtime::FunctionCallBatchId id,
	const RequestScope& scope)
{
	return Failure(
		"CALL_BATCH_SCOPE_MISMATCH",
		"The batch is not bound to the requested immutable session/snapshot scope.",
		{{"batch_id", std::to_string(id)}, {"requested_scope", SerializeScope(scope)}});
}

FunctionCallBatchCommandResult EnsureResponseBound(
	FunctionCallBatchCommandResult result)
{
	json envelope;
	if (result.Ok())
		envelope = {{"data", result.Data}};
	else
	{
		envelope = {{"error", {
			{"code", result.Error->Code},
			{"message", result.Error->Message},
			{"details", result.Error->Details}
		}}};
	}
	if (envelope.dump().size()
		<= FunctionCallBatchCommandService::kMaxSerializedDataBytes)
	{
		return result;
	}
	return Failure(
		"CALL_BATCH_RESPONSE_TOO_LARGE",
		"The serialized call batch response exceeds the 4 MiB service limit.");
}

Runtime::FunctionCallBatchWorkerResult WorkerFailure(
	std::string code,
	std::string message)
{
	return {
		.Status = Runtime::FunctionCallBatchWorkerStatus::Failed,
		.DiagnosticCode = std::move(code),
		.DiagnosticMessage = std::move(message)
	};
}

Runtime::FunctionCallBatchWorkerResult WorkerCancelled(
	std::string code,
	std::string message)
{
	return {
		.Status = Runtime::FunctionCallBatchWorkerStatus::Cancelled,
		.DiagnosticCode = std::move(code),
		.DiagnosticMessage = std::move(message)
	};
}

} // namespace

FunctionCallBatchCommandWorker::FunctionCallBatchCommandWorker(
	std::shared_ptr<IFunctionCallBatchInvokeAdapter> adapter) noexcept
	: m_Adapter(std::move(adapter))
{
}

Runtime::FunctionCallBatchWorkerResult FunctionCallBatchCommandWorker::Execute(
	const std::shared_ptr<const Runtime::FunctionCallBatchRequest>& request,
	const std::size_t itemIndex,
	Runtime::IFunctionCallBatchExecutionContext& context)
{
	if (!m_Adapter)
	{
		return WorkerFailure(
			"CALL_BATCH_ADAPTER_NOT_READY",
			"No exact single-call adapter is retained by the batch worker.");
	}
	if (!request || itemIndex >= request->Spec.Items.size())
	{
		return WorkerFailure(
			"CALL_BATCH_WORKER_REQUEST_INVALID",
			"The coordinator supplied no immutable batch item at this index.");
	}
	if (context.IsDeadlineExceeded())
	{
		return WorkerCancelled(
			"CALL_BATCH_DEADLINE_EXCEEDED",
			"The total batch deadline elapsed before exact-call admission.");
	}
	if (context.IsCancellationRequested())
	{
		return WorkerCancelled(
			"CALL_BATCH_CANCELLED",
			"Cancellation was requested before exact-call admission.");
	}

	DecodedCallData decoded;
	if (!TryDecodeCallData(
		request->Spec.Binding,
		request->Spec.Items[itemIndex],
		decoded))
	{
		return WorkerFailure(
			"CALL_BATCH_ITEM_BINDING_INVALID",
			"The owned item bytes do not match the exact target/function/generation binding.");
	}

	FunctionCallBatchInvokeRequest invocation{
		.Binding = request->Spec.Binding,
		.Target = std::move(decoded.Target),
		.Function = std::move(decoded.Function),
		.FunctionPath = std::move(decoded.FunctionPath),
		.CallData = std::move(decoded.Data),
		.Deadline = request->Deadline
	};
	FunctionCallCommandResult result = m_Adapter->InvokeExact(invocation, context);
	if (!result.Ok())
	{
		if (!result.Error
			|| !IsBoundedText(result.Error->Code, kMaxDiagnosticCodeBytes)
			|| !IsBoundedText(result.Error->Message, kMaxDiagnosticMessageBytes))
		{
			return WorkerFailure(
				"CALL_BATCH_ADAPTER_RESULT_INVALID",
				"The exact-call adapter returned an invalid diagnostic.");
		}
		return WorkerFailure(
			std::move(result.Error->Code),
			std::move(result.Error->Message));
	}

	const std::string encoded = result.Data.dump();
	if (encoded.size() > context.MaxResultBytesPerItem())
	{
		return WorkerFailure(
			"CALL_BATCH_ITEM_RESULT_TOO_LARGE",
			"The exact call result exceeds the configured per-item result budget.");
	}
	return {
		.Status = Runtime::FunctionCallBatchWorkerStatus::Succeeded,
		.SerializedResponse = ToBytes(encoded)
	};
}

FunctionCallBatchCommandService::FunctionCallBatchCommandService(
	Runtime::FunctionCallBatchCoordinator* const coordinator) noexcept
	: m_Coordinator(coordinator)
{
}

bool FunctionCallBatchCommandService::IsConfigured() const noexcept
{
	return m_Coordinator && m_Coordinator->IsConfigured();
}

bool FunctionCallBatchCommandService::Handles(
	const std::string_view operation) noexcept
{
	return operation == kSubmitOperation
		|| operation == kGetOperation
		|| operation == kCancelOperation
		|| operation == kListOperation;
}

FunctionCallBatchCommandResult FunctionCallBatchCommandService::Execute(
	const std::string_view operation,
	const json& data) noexcept
{
	if (!Handles(operation))
	{
		return Failure(
			"OPERATION_NOT_SUPPORTED",
			"The call batch command service does not implement this operation.");
	}
	if (!IsConfigured())
	{
		return Failure(
			"CALL_BATCH_ADAPTER_NOT_READY",
			"No configured coordinator with an owned exact-call adapter is available.");
	}
	try
	{
		FunctionCallBatchCommandResult result;
		if (operation == kSubmitOperation)
			result = Submit(data);
		else if (operation == kGetOperation)
			result = Get(data);
		else if (operation == kCancelOperation)
			result = Cancel(data);
		else
			result = List(data);
		return EnsureResponseBound(std::move(result));
	}
	catch (const std::invalid_argument& exception)
	{
		return Failure(
			"CALL_BATCH_RECORD_INVALID",
			"A retained batch violated the command boundary contract.",
			{{"reason", exception.what()}});
	}
	catch (...)
	{
		return Failure(
			"CALL_BATCH_ALLOCATION_FAILED",
			"The call batch command could not allocate bounded owned state.");
	}
}

FunctionCallBatchCommandResult FunctionCallBatchCommandService::Submit(
	const json& data)
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"policy",
		"deadline_ms",
		"items"}))
	{
		return Failure(
			"CALL_BATCH_REQUEST_SCHEMA_INVALID",
			"Batch submit data must contain only the required v1 fields.");
	}

	RequestScope scope;
	if (!TryScope(data, scope))
	{
		return Failure(
			"CALL_BATCH_SCOPE_INVALID",
			"Batch submit requires a valid session/context/object/type generation scope.");
	}
	if (!data.at("policy").is_string())
		return Failure("CALL_BATCH_POLICY_INVALID", "Batch policy must be a string.");
	Runtime::FunctionCallBatchPolicy policy;
	const std::string& encodedPolicy = data.at("policy").get_ref<const std::string&>();
	if (encodedPolicy == "continue_on_error")
		policy = Runtime::FunctionCallBatchPolicy::ContinueOnError;
	else if (encodedPolicy == "stop_on_first_failure")
		policy = Runtime::FunctionCallBatchPolicy::StopOnFirstFailure;
	else
	{
		return Failure(
			"CALL_BATCH_POLICY_INVALID",
			"Batch policy must be continue_on_error or stop_on_first_failure.");
	}

	std::uint64_t deadlineMs = 0;
	if (!TryUnsigned(
		data.at("deadline_ms"),
		1,
		static_cast<std::uint64_t>(Runtime::FunctionCallBatchCoordinator::kMaxDeadlineMs),
		deadlineMs))
	{
		return Failure(
			"CALL_BATCH_DEADLINE_INVALID",
			"deadline_ms must be an integer from 1 through 86400000.");
	}
	if (!data.at("items").is_array()
		|| data.at("items").empty()
		|| data.at("items").size() > kMaxItems)
	{
		return Failure(
			"CALL_BATCH_ITEM_LIMIT_EXCEEDED",
			"Batch items must be a non-empty array of at most 64 exact calls.");
	}

	Runtime::FunctionCallBatchSpec spec;
	spec.Binding = {
		.SessionId = scope.SessionId,
		.ContextGeneration = scope.ContextGeneration,
		.ObjectSnapshotGeneration = scope.ObjectSnapshotGeneration,
		.TypeSnapshotGeneration = scope.TypeSnapshotGeneration
	};
	spec.Policy = policy;
	spec.Items.reserve(data.at("items").size());
	std::size_t totalBytes = 0;
	for (std::size_t index = 0; index < data.at("items").size(); ++index)
	{
		const json& itemData = data.at("items").at(index);
		if (!HasExactKeys(itemData, {
			"target",
			"function",
			"function_path",
			"arguments"})
			|| !itemData.at("function_path").is_string()
			|| !itemData.at("arguments").is_object()
			|| itemData.at("arguments").size() > kMaxArgumentCount)
		{
			return Failure(
				"CALL_BATCH_ITEM_SCHEMA_INVALID",
				"Each batch item must contain only target, function, function_path, and arguments.",
				{{"item_index", index}});
		}

		Runtime::ObjectHandle target;
		Runtime::FunctionHandle function;
		const std::string functionPath = itemData.at("function_path").get<std::string>();
		std::size_t nodes = 0;
		if (!TryParseObjectHandle(itemData.at("target"), target)
			|| !TryParseFunctionHandle(itemData.at("function"), function)
			|| !IsBoundedText(functionPath, kMaxPathBytes)
			|| !HandleMatchesScope(target, scope)
			|| !HandleMatchesScope(function.Function, scope)
			|| !HandleMatchesScope(function.Owner, scope)
			|| !ValidateJsonTree(itemData.at("arguments"), nodes, 0))
		{
			return Failure(
				"CALL_BATCH_ITEM_BINDING_INVALID",
				"An item has a malformed or out-of-scope exact call identity/argument tree.",
				{{"item_index", index}});
		}

		const json callData = {
			{"target", SerializeObjectHandle(target)},
			{"function", SerializeFunctionHandle(function)},
			{"type_snapshot_generation", scope.TypeSnapshotGeneration},
			{"function_path", functionPath},
			{"arguments", itemData.at("arguments")}
		};
		const std::string encoded = callData.dump();
		if (encoded.size() > kMaxSerializedRequestBytesPerItem
			|| totalBytes > kMaxSerializedDataBytes - encoded.size())
		{
			return Failure(
				"CALL_BATCH_REQUEST_BYTES_LIMIT_EXCEEDED",
				"The normalized item or total batch request exceeds its owned-byte budget.",
				{{"item_index", index}});
		}
		totalBytes += encoded.size();
		std::string functionIdentity = function.FullPath;
		spec.Items.push_back({
			.Target = std::move(target),
			.Function = std::move(function),
			.ExpectedFunctionPath = std::move(functionIdentity),
			.SerializedRequest = ToBytes(encoded)
		});
	}

	const std::size_t itemCount = spec.Items.size();
	const Runtime::FunctionCallBatchSubmitResult submitted = m_Coordinator->Submit(
		std::move(spec),
		std::chrono::milliseconds(deadlineMs));
	if (!submitted.Ok())
		return CoordinatorFailure(submitted.Error, "batch submission");
	RememberAdmission(submitted.Id);
	return {.Data = {
		{"batch_id", std::to_string(submitted.Id)},
		{"admission", "accepted"},
		{"item_count", itemCount},
		{"policy", encodedPolicy},
		{"scope", SerializeScope(scope)}
	}};
}

FunctionCallBatchCommandResult FunctionCallBatchCommandService::Get(
	const json& data) const
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"batch_id"}))
	{
		return Failure(
			"CALL_BATCH_REQUEST_SCHEMA_INVALID",
			"Batch get data must contain only the immutable scope and batch_id.");
	}
	RequestScope scope;
	Runtime::FunctionCallBatchId id = 0;
	if (!TryScope(data, scope) || !TryBatchId(data.at("batch_id"), id))
	{
		return Failure(
			"CALL_BATCH_GET_REQUEST_INVALID",
			"Batch get requires a valid immutable scope and canonical decimal batch_id.");
	}
	const Runtime::FunctionCallBatchSnapshotResult snapshot = m_Coordinator->Snapshot(id);
	if (!snapshot.Ok())
		return CoordinatorFailure(snapshot.Error, "batch lookup");
	if (!snapshot.Summary.Request
		|| !ScopeMatches(scope, snapshot.Summary.Request->Spec.Binding))
	{
		return ScopeMismatch(id, scope);
	}
	return {.Data = {{"batch", SerializeBatch(snapshot.Summary)}}};
}

FunctionCallBatchCommandResult FunctionCallBatchCommandService::Cancel(
	const json& data)
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"batch_id"}))
	{
		return Failure(
			"CALL_BATCH_REQUEST_SCHEMA_INVALID",
			"Batch cancel data must contain only the immutable scope and batch_id.");
	}
	RequestScope scope;
	Runtime::FunctionCallBatchId id = 0;
	if (!TryScope(data, scope) || !TryBatchId(data.at("batch_id"), id))
	{
		return Failure(
			"CALL_BATCH_CANCEL_REQUEST_INVALID",
			"Batch cancel requires a valid immutable scope and canonical decimal batch_id.");
	}
	const Runtime::FunctionCallBatchSnapshotResult snapshot = m_Coordinator->Snapshot(id);
	if (!snapshot.Ok())
		return CoordinatorFailure(snapshot.Error, "batch cancellation lookup");
	if (!snapshot.Summary.Request
		|| !ScopeMatches(scope, snapshot.Summary.Request->Spec.Binding))
	{
		return ScopeMismatch(id, scope);
	}
	const Runtime::FunctionCallBatchCancelResult cancelled = m_Coordinator->Cancel(id);
	if (!cancelled.Ok())
		return CoordinatorFailure(cancelled.Error, "batch cancellation");
	return {.Data = {
		{"batch_id", std::to_string(id)},
		{"disposition", ToString(cancelled.Disposition)}
	}};
}

FunctionCallBatchCommandResult FunctionCallBatchCommandService::List(
	const json& data) const
{
	if (!HasExactKeys(data, {
		"session_id",
		"context_generation",
		"object_snapshot_generation",
		"type_snapshot_generation",
		"max_batches"}))
	{
		return Failure(
			"CALL_BATCH_REQUEST_SCHEMA_INVALID",
			"Batch list data must contain only the immutable scope and max_batches.");
	}
	RequestScope scope;
	std::size_t maximum = 0;
	if (!TryScope(data, scope)
		|| !TrySize(data.at("max_batches"), 1, kMaxListedBatches, maximum))
	{
		return Failure(
			"CALL_BATCH_LIST_REQUEST_INVALID",
			"Batch list requires a valid immutable scope and max_batches from 1 through 64.");
	}

	json batches = json::array();
	std::size_t matching = 0;
	for (const Runtime::FunctionCallBatchId id : AdmissionIdsNewestFirst())
	{
		const Runtime::FunctionCallBatchSnapshotResult snapshot = m_Coordinator->Snapshot(id);
		if (snapshot.Error == Runtime::FunctionCallBatchError::NotFound)
			continue;
		if (!snapshot.Ok())
			return CoordinatorFailure(snapshot.Error, "batch listing");
		if (!snapshot.Summary.Request
			|| !ScopeMatches(scope, snapshot.Summary.Request->Spec.Binding))
		{
			continue;
		}
		++matching;
		if (batches.size() < maximum)
			batches.push_back(SerializeBatchMetadata(snapshot.Summary));
	}

	return {.Data = {
		{"scope", SerializeScope(scope)},
		{"batches", std::move(batches)},
		{"more_batches_available", matching > maximum}
	}};
}

void FunctionCallBatchCommandService::RememberAdmission(
	const Runtime::FunctionCallBatchId id) noexcept
{
	std::lock_guard<std::mutex> lock(m_AdmissionsMutex);
	m_AdmissionIds[m_NextAdmissionSlot] = id;
	m_NextAdmissionSlot = (m_NextAdmissionSlot + 1) % m_AdmissionIds.size();
	if (m_AdmissionCount < m_AdmissionIds.size())
		++m_AdmissionCount;
}

std::vector<Runtime::FunctionCallBatchId>
FunctionCallBatchCommandService::AdmissionIdsNewestFirst() const
{
	std::lock_guard<std::mutex> lock(m_AdmissionsMutex);
	std::vector<Runtime::FunctionCallBatchId> ids;
	ids.reserve(m_AdmissionCount);
	for (std::size_t offset = 0; offset < m_AdmissionCount; ++offset)
	{
		const std::size_t index =
			(m_NextAdmissionSlot + m_AdmissionIds.size() - 1 - offset)
			% m_AdmissionIds.size();
		if (m_AdmissionIds[index] != 0)
			ids.push_back(m_AdmissionIds[index]);
	}
	return ids;
}

} // namespace UExplorer::Services
