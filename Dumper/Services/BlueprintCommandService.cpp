#include "BlueprintCommandService.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kBytecode = "blueprint.bytecode";
constexpr std::string_view kDecompile = "blueprint.decompile";
constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSessionBytes = 128;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxProfileIdBytes = 256;
constexpr std::size_t kMaxSourceBytes = 1024;

struct BlueprintCommandInput
{
	Runtime::FunctionHandle Function;
	std::string FunctionPath;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
	std::string ProfileId;
};

BlueprintCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Error = BlueprintCommandError{
			.Code = std::move(code),
			.Message = std::move(message),
			.Details = std::move(details)
		}
	};
}

BlueprintCommandResult SuccessBounded(json data)
{
	if (data.dump().size() > BlueprintCommandService::kMaxResponseBytes)
	{
		return Failure(
			"BLUEPRINT_RESPONSE_LIMIT_EXCEEDED",
			"Serialized Blueprint response exceeds the 4 MiB command limit",
			{{"max_response_bytes", BlueprintCommandService::kMaxResponseBytes}});
	}
	return {.Data = std::move(data)};
}

bool IsBoundedText(const std::string_view value, const std::size_t maximum) noexcept
{
	return !value.empty() && value.size() <= maximum
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

bool IsUpperHex(const char character) noexcept
{
	return (character >= '0' && character <= '9')
		|| (character >= 'A' && character <= 'F');
}

bool TryCanonicalHex(
	const std::string_view encoded,
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
	const std::string_view digits = encoded.substr(prefix);
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
	if (!data.is_object() || data.size() != 6
		|| !data.contains("session_id")
		|| !data.contains("context_generation")
		|| !data.contains("index")
		|| !data.contains("serial")
		|| !data.contains("address")
		|| !data.contains("class_fingerprint")
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
	if (!data.is_object() || data.size() != 4
		|| !data.contains("function")
		|| !data.contains("owner")
		|| !data.contains("full_path")
		|| !data.contains("signature_fingerprint")
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

bool TryParseInput(
	const json& data,
	const bool requireProfile,
	BlueprintCommandInput& input)
{
	input = {};
	const std::size_t expectedFields = requireProfile ? 6 : 5;
	if (!data.is_object() || data.size() != expectedFields
		|| !data.contains("function")
		|| !data.contains("function_path")
		|| !data.contains("context_generation")
		|| !data.contains("object_snapshot_generation")
		|| !data.contains("type_snapshot_generation")
		|| (requireProfile && !data.contains("profile_id"))
		|| !data.at("function_path").is_string()
		|| (requireProfile && !data.at("profile_id").is_string())
		|| !TryParseFunctionHandle(data.at("function"), input.Function))
	{
		return false;
	}

	input.FunctionPath = data.at("function_path").get<std::string>();
	if (!IsBoundedText(input.FunctionPath, kMaxPathBytes)
		|| !TryUnsigned(
			data.at("context_generation"),
			1,
			kMaxProtocolInteger,
			input.ContextGeneration)
		|| !TryUnsigned(
			data.at("object_snapshot_generation"),
			1,
			kMaxProtocolInteger,
			input.ObjectSnapshotGeneration)
		|| !TryUnsigned(
			data.at("type_snapshot_generation"),
			1,
			kMaxProtocolInteger,
			input.TypeSnapshotGeneration))
	{
		return false;
	}

	if (requireProfile)
	{
		input.ProfileId = data.at("profile_id").get<std::string>();
		if (!IsBoundedText(input.ProfileId, kMaxProfileIdBytes))
			return false;
	}
	return true;
}

bool SameObjectHandle(
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
	return SameObjectHandle(left.Function, right.Function)
		&& SameObjectHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
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

std::string EncodeHex(const std::span<const std::uint8_t> bytes)
{
	constexpr char digits[] = "0123456789ABCDEF";
	std::string encoded;
	encoded.resize(bytes.size() * 2);
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		encoded[index * 2] = digits[bytes[index] >> 4U];
		encoded[index * 2 + 1] = digits[bytes[index] & 0x0FU];
	}
	return encoded;
}

const char* DisassemblyStatusName(
	const BlueprintDecompiler::DisassemblyStatus status) noexcept
{
	switch (status)
	{
	case BlueprintDecompiler::DisassemblyStatus::Complete: return "complete";
	case BlueprintDecompiler::DisassemblyStatus::Incomplete: return "incomplete";
	case BlueprintDecompiler::DisassemblyStatus::Error: return "error";
	}
	return "error";
}

const char* DisassemblyErrorName(
	const BlueprintDecompiler::DisassemblyErrorCode error) noexcept
{
	using Error = BlueprintDecompiler::DisassemblyErrorCode;
	switch (error)
	{
	case Error::None: return "NONE";
	case Error::ProfileRequired: return "BYTECODE_PROFILE_REQUIRED";
	case Error::InvalidProfile: return "BYTECODE_PROFILE_INVALID";
	case Error::InputLimitExceeded: return "BYTECODE_INPUT_LIMIT_EXCEEDED";
	case Error::TotalByteLimitExceeded: return "BYTECODE_TOTAL_BYTE_LIMIT_EXCEEDED";
	case Error::InstructionLimitExceeded: return "BYTECODE_INSTRUCTION_LIMIT_EXCEEDED";
	case Error::RecursionLimitExceeded: return "BYTECODE_RECURSION_LIMIT_EXCEEDED";
	case Error::StringLimitExceeded: return "BYTECODE_STRING_LIMIT_EXCEEDED";
	case Error::TruncatedOperand: return "BYTECODE_TRUNCATED_OPERAND";
	case Error::UnterminatedString: return "BYTECODE_UNTERMINATED_STRING";
	case Error::UnknownOpcode: return "BYTECODE_UNKNOWN_OPCODE";
	case Error::UnsupportedOpcode: return "BYTECODE_UNSUPPORTED_OPCODE";
	case Error::UnexpectedTerminator: return "BYTECODE_UNEXPECTED_TERMINATOR";
	case Error::ExpectedTerminator: return "BYTECODE_EXPECTED_TERMINATOR";
	case Error::InvalidOperand: return "BYTECODE_INVALID_OPERAND";
	case Error::EndOfScriptMissing: return "BYTECODE_END_OF_SCRIPT_MISSING";
	case Error::TrailingBytes: return "BYTECODE_TRAILING_BYTES";
	}
	return "BYTECODE_UNKNOWN_ERROR";
}

const char* OpcodeSemanticName(
	const BlueprintDecompiler::OpcodeSemantic semantic) noexcept
{
	switch (semantic)
	{
	case BlueprintDecompiler::OpcodeSemantic::Unknown: return "unknown";
	case BlueprintDecompiler::OpcodeSemantic::ExprToken: return "expr_token";
	case BlueprintDecompiler::OpcodeSemantic::PrimitiveCast: return "primitive_cast";
	}
	return "unknown";
}

json SerializeDisassembly(const BlueprintDecompiler::DisassemblyResult& result)
{
	json instructions = json::array();
	instructions.get_ref<json::array_t&>().reserve(result.Instructions.size());
	for (const BlueprintDecompiler::DisassembledInstruction& instruction
		: result.Instructions)
	{
		const bool hasToken = instruction.Semantic
			== BlueprintDecompiler::OpcodeSemantic::ExprToken
			&& instruction.Token != EExprToken::EX_Max;
		instructions.push_back({
			{"offset", instruction.Offset},
			{"size", instruction.Size},
			{"depth", instruction.Depth},
			{"raw_opcode", std::format("0x{:02X}", instruction.RawOpcode)},
			{"semantic", OpcodeSemanticName(instruction.Semantic)},
			{"token", hasToken ? json(GetExprTokenName(instruction.Token)) : json(nullptr)},
			{"token_value", hasToken
				? json(std::format("0x{:02X}", static_cast<std::uint8_t>(instruction.Token)))
				: json(nullptr)},
			{"text", instruction.Text}
		});
	}

	json firstError = nullptr;
	if (result.FirstError.Present)
	{
		firstError = {
			{"offset", result.FirstError.Offset},
			{"code", DisassemblyErrorName(result.FirstError.Code)},
			{"message", result.FirstError.Message}
		};
	}
	return {
		{"status", DisassemblyStatusName(result.Status)},
		{"profile_id", result.ProfileId},
		{"input_size", result.InputSize},
		{"bytes_consumed", result.BytesConsumed},
		{"coverage", result.Coverage},
		{"unknown_count", result.UnknownCount},
		{"saw_end_of_script", result.SawEndOfScript},
		{"first_error", std::move(firstError)},
		{"instructions", std::move(instructions)},
		{"pseudocode", result.Pseudocode}
	};
}

Runtime::BlueprintEvidenceBinding MakeBinding(
	const Runtime::TypeSnapshot& snapshot)
{
	return {
		.SessionId = snapshot.SessionId(),
		.ContextGeneration = snapshot.ContextGeneration(),
		.ObjectSnapshotGeneration = snapshot.ObjectSnapshotGeneration(),
		.TypeSnapshotGeneration = snapshot.Generation()
	};
}

bool ValidateCapture(
	const Runtime::BlueprintBytecodeCapture& capture,
	const Runtime::BlueprintBytecodeCaptureRequest& request) noexcept
{
	return Runtime::SameBlueprintEvidenceBinding(capture.Binding, request.Binding)
		&& SameFunctionHandle(capture.Function, request.Function)
		&& capture.FunctionPath == request.FunctionPath
		&& IsBoundedText(capture.Source, kMaxSourceBytes)
		&& capture.ScriptFieldOffset > 0
		&& capture.ScriptFieldOffset
			<= Runtime::BlueprintBytecodeEvidenceStore::kMaxScriptFieldOffset
		&& capture.ScriptDataAddress != 0
		&& capture.ScriptNum > 0
		&& capture.ScriptMax >= capture.ScriptNum
		&& capture.ScriptMax
			<= Runtime::BlueprintBytecodeEvidenceStore::kMaxScriptCapacity
		&& !capture.Script.empty()
		&& capture.Script.size() <= request.MaxScriptBytes
		&& capture.Script.size() == static_cast<std::size_t>(capture.ScriptNum)
		&& capture.ScriptDataAddress
			<= (std::numeric_limits<std::uintptr_t>::max)() - capture.Script.size()
		&& capture.CapturedAtMonotonicUs != 0
		&& capture.HeaderWitnessFingerprint != 0
		&& capture.HeaderWitnessFingerprint
			== Runtime::ComputeBlueprintScriptHeaderFingerprint(capture)
		&& capture.EvidenceFingerprint != 0
		&& capture.EvidenceFingerprint
			== Runtime::ComputeBlueprintBytecodeCaptureFingerprint(capture);
}

bool ValidateProfile(
	const Runtime::BlueprintBytecodeProfileRecord& profile,
	const Runtime::BlueprintBytecodeProfileRequest& request) noexcept
{
	const auto& limits = profile.Definition.Limits;
	return Runtime::SameBlueprintEvidenceBinding(profile.Binding, request.Binding)
		&& profile.Definition.Id == request.ProfileId
		&& IsBoundedText(profile.Source, kMaxSourceBytes)
		&& limits.MaxInputBytes > 0
		&& limits.MaxInputBytes <= Runtime::BlueprintBytecodeEvidenceStore::kMaxScriptBytes
		&& limits.MaxBytesConsumed > 0
		&& limits.MaxBytesConsumed <= Runtime::BlueprintBytecodeEvidenceStore::kMaxScriptBytes
		&& limits.MaxInstructions > 0
		&& limits.MaxInstructions <= Runtime::BlueprintBytecodeEvidenceStore::kMaxInstructions
		&& limits.MaxStringCodeUnits > 0
		&& limits.MaxStringCodeUnits
			<= Runtime::BlueprintBytecodeEvidenceStore::kMaxStringCodeUnits
		&& limits.MaxRecursionDepth > 0
		&& limits.MaxRecursionDepth
			<= Runtime::BlueprintBytecodeEvidenceStore::kMaxRecursionDepth
		&& profile.EvidenceFingerprint != 0
		&& profile.EvidenceFingerprint
			== Runtime::ComputeBlueprintBytecodeProfileFingerprint(profile);
}

BlueprintCommandResult CaptureSourceFailure(
	const Runtime::BlueprintEvidenceSourceError error,
	const std::string& functionPath)
{
	using SourceError = Runtime::BlueprintEvidenceSourceError;
	const json details = {
		{"function_path", functionPath},
		{"source_error", Runtime::ToString(error)}
	};
	switch (error)
	{
	case SourceError::Unavailable:
	case SourceError::Stopped:
		return Failure(
			"BYTECODE_CAPTURE_UNAVAILABLE",
			"No witnessed bounded bytecode capture is available for the exact function generation",
			details);
	case SourceError::InvalidRequest:
	case SourceError::InvalidEvidence:
		return Failure(
			"BYTECODE_CAPTURE_INVALID",
			"The bytecode source rejected or returned inconsistent capture evidence",
			details);
	case SourceError::DependencyChanged:
		return Failure(
			"BLUEPRINT_DEPENDENCY_CHANGED",
			"Blueprint capture dependencies changed before an immutable byte copy was produced",
			details);
	case SourceError::FunctionHandleStale:
		return Failure(
			"BLUEPRINT_FUNCTION_HANDLE_STALE",
			"The exact function identity changed before bytecode capture",
			details);
	case SourceError::ExecutionThreadInvalid:
		return Failure(
			"BLUEPRINT_EXECUTION_THREAD_INVALID",
			"The bytecode source could not validate the required execution thread",
			details);
	case SourceError::LimitExceeded:
		return Failure(
			"BYTECODE_CAPTURE_LIMIT_EXCEEDED",
			"The exact script exceeds the bounded bytecode capture limit",
			details);
	case SourceError::AllocationFailed:
		return Failure(
			"BLUEPRINT_ALLOCATION_FAILED",
			"The bytecode source could not allocate a bounded immutable capture",
			details);
	case SourceError::InternalError:
		return Failure(
			"BYTECODE_CAPTURE_INTERNAL_ERROR",
			"The bytecode source failed without producing immutable evidence",
			details);
	case SourceError::None:
		break;
	}
	return Failure(
		"BYTECODE_CAPTURE_INVALID",
		"The bytecode source reported success without immutable capture evidence",
		details);
}

BlueprintCommandResult ProfileSourceFailure(
	const Runtime::BlueprintEvidenceSourceError error,
	const std::string& profileId)
{
	using SourceError = Runtime::BlueprintEvidenceSourceError;
	const json details = {
		{"profile_id", profileId},
		{"source_error", Runtime::ToString(error)}
	};
	switch (error)
	{
	case SourceError::Unavailable:
	case SourceError::Stopped:
		return Failure(
			"BYTECODE_PROFILE_REQUIRED",
			"No immutable witnessed bytecode profile matches the requested session and generations",
			details);
	case SourceError::DependencyChanged:
		return Failure(
			"BLUEPRINT_DEPENDENCY_CHANGED",
			"The bytecode profile belongs to a different dependency generation",
			details);
	case SourceError::AllocationFailed:
		return Failure(
			"BLUEPRINT_ALLOCATION_FAILED",
			"The profile source could not allocate its bounded result",
			details);
	case SourceError::LimitExceeded:
	case SourceError::InvalidRequest:
	case SourceError::InvalidEvidence:
	case SourceError::FunctionHandleStale:
	case SourceError::ExecutionThreadInvalid:
	case SourceError::InternalError:
		return Failure(
			"BYTECODE_PROFILE_INVALID",
			"The profile source rejected or returned inconsistent witnessed profile evidence",
			details);
	case SourceError::None:
		break;
	}
	return Failure(
		"BYTECODE_PROFILE_INVALID",
		"The profile source reported success without immutable profile evidence",
		details);
}

json BaseResult(
	const Runtime::ReflectedFunction& function,
	const Runtime::BlueprintBytecodeCapture& capture)
{
	return {
		{"function", SerializeFunctionHandle(function.Handle)},
		{"function_path", function.FullPath},
		{"context_generation", capture.Binding.ContextGeneration},
		{"object_snapshot_generation", capture.Binding.ObjectSnapshotGeneration},
		{"type_snapshot_generation", capture.Binding.TypeSnapshotGeneration},
		{"byte_length", capture.Script.size()},
		{"capture_source", capture.Source},
		{"captured_at_monotonic_us", std::to_string(capture.CapturedAtMonotonicUs)},
		{"script_field_offset", capture.ScriptFieldOffset},
		{"script_data_address", std::format("0x{:X}", capture.ScriptDataAddress)},
		{"script_num", capture.ScriptNum},
		{"script_max", capture.ScriptMax},
		{"header_witness_fingerprint", std::format(
			"{:016X}",
			capture.HeaderWitnessFingerprint)},
		{"capture_fingerprint", std::format("{:016X}", capture.EvidenceFingerprint)}
	};
}

} // namespace

bool BlueprintCommandService::Handles(const std::string_view operation) noexcept
{
	return operation == kBytecode || operation == kDecompile;
}

BlueprintCommandResult BlueprintCommandService::Execute(
	std::shared_ptr<const Runtime::TypeSnapshot> snapshot,
	const std::string_view operation,
	const json& data,
	const std::string_view expectedSessionId,
	const std::uint64_t expectedContextGeneration,
	const std::uint64_t expectedObjectSnapshotGeneration,
	Runtime::IBlueprintBytecodeCaptureSource* captureSource,
	const Runtime::IBlueprintBytecodeProfileSource* profileSource) noexcept
{
	try
	{
		if (!Handles(operation))
		{
			return Failure(
				"OPERATION_NOT_SUPPORTED",
				"No Blueprint command is registered for the requested operation",
				{{"operation", operation}});
		}
		if (!snapshot)
		{
			return Failure(
				"TYPE_SNAPSHOT_UNAVAILABLE",
				"No complete immutable TypeSnapshot is published for Blueprint lookup");
		}
		if (expectedSessionId.empty()
			|| expectedContextGeneration == 0
			|| expectedObjectSnapshotGeneration == 0
			|| snapshot->SessionId() != expectedSessionId
			|| snapshot->ContextGeneration() != expectedContextGeneration
			|| snapshot->ObjectSnapshotGeneration() != expectedObjectSnapshotGeneration
			|| !snapshot->IsConfigured(expectedContextGeneration))
		{
			return Failure(
				"BLUEPRINT_SNAPSHOT_CONTEXT_MISMATCH",
				"Published type metadata does not match the active session and dependency generations",
				{
					{"snapshot_session_id", snapshot->SessionId()},
					{"snapshot_context_generation", snapshot->ContextGeneration()},
					{"snapshot_object_generation", snapshot->ObjectSnapshotGeneration()},
					{"active_session_id", expectedSessionId},
					{"active_context_generation", expectedContextGeneration},
					{"active_object_generation", expectedObjectSnapshotGeneration}
				});
		}

		const bool decompile = operation == kDecompile;
		BlueprintCommandInput input;
		if (!TryParseInput(data, decompile, input))
		{
			return Failure(
				"INVALID_ARGUMENT",
				decompile
					? "blueprint.decompile requires exactly function, function_path, context_generation, object_snapshot_generation, type_snapshot_generation, and profile_id"
					: "blueprint.bytecode requires exactly function, function_path, context_generation, object_snapshot_generation, and type_snapshot_generation");
		}
		if (input.ContextGeneration != snapshot->ContextGeneration()
			|| input.ObjectSnapshotGeneration != snapshot->ObjectSnapshotGeneration()
			|| input.TypeSnapshotGeneration != snapshot->Generation())
		{
			return Failure(
				"BLUEPRINT_SNAPSHOT_GENERATION_MISMATCH",
				"The request does not bind the current immutable context, object, and type generations",
				{
					{"requested_context_generation", input.ContextGeneration},
					{"current_context_generation", snapshot->ContextGeneration()},
					{"requested_object_generation", input.ObjectSnapshotGeneration},
					{"current_object_generation", snapshot->ObjectSnapshotGeneration()},
					{"requested_type_generation", input.TypeSnapshotGeneration},
					{"current_type_generation", snapshot->Generation()}
				});
		}

		const Runtime::ReflectedFunctionLookup lookup =
			snapshot->FindFunctionByFullPath(input.FunctionPath);
		if (!lookup.Found()
			|| lookup.DeclaringType->Kind != Runtime::ReflectedTypeKind::Class)
		{
			return Failure(
				"BLUEPRINT_FUNCTION_NOT_FOUND",
				"No class function in the exact TypeSnapshot generation has the requested full path",
				{{"function_path", input.FunctionPath}});
		}
		if (!SameFunctionHandle(lookup.Function->Handle, input.Function)
			|| !SameObjectHandle(lookup.DeclaringType->Handle, input.Function.Owner))
		{
			return Failure(
				"BLUEPRINT_FUNCTION_HANDLE_MISMATCH",
				"The supplied FunctionHandle does not match the exact TypeSnapshot function identity",
				{{"function_path", input.FunctionPath}});
		}
		if (lookup.Function->Implementation == Runtime::ReflectedFunctionImplementation::Native
			|| (lookup.Function->Implementation
					== Runtime::ReflectedFunctionImplementation::Unavailable
				&& lookup.Function->ReasonCode != "FUNCTION_BYTECODE_NOT_CAPTURED"))
		{
			return Failure(
				"BYTECODE_CAPTURE_UNAVAILABLE",
				"The exact TypeSnapshot does not identify a bytecode implementation for this function",
				{
					{"function_path", input.FunctionPath},
					{"implementation", Runtime::ToString(lookup.Function->Implementation)},
					{"reason_code", lookup.Function->ReasonCode.empty()
						? json(nullptr)
						: json(lookup.Function->ReasonCode)}
				});
		}

		const Runtime::BlueprintEvidenceBinding binding = MakeBinding(*snapshot);
		std::shared_ptr<const Runtime::BlueprintBytecodeProfileRecord> profile;
		if (decompile)
		{
			if (!profileSource)
			{
				return Failure(
					"BYTECODE_PROFILE_REQUIRED",
					"blueprint.decompile requires an immutable witnessed bytecode profile",
					{{"profile_id", input.ProfileId}});
			}
			const Runtime::BlueprintBytecodeProfileRequest profileRequest{
				.Binding = binding,
				.ProfileId = input.ProfileId
			};
			Runtime::BlueprintBytecodeProfileResult resolved;
			try
			{
				resolved = profileSource->ResolveProfile(profileRequest);
			}
			catch (const std::bad_alloc&)
			{
				return ProfileSourceFailure(
					Runtime::BlueprintEvidenceSourceError::AllocationFailed,
					input.ProfileId);
			}
			catch (...)
			{
				return ProfileSourceFailure(
					Runtime::BlueprintEvidenceSourceError::InternalError,
					input.ProfileId);
			}
			if (!resolved.Ok())
				return ProfileSourceFailure(resolved.Error, input.ProfileId);
			if (!ValidateProfile(*resolved.Profile, profileRequest))
			{
				return ProfileSourceFailure(
					Runtime::BlueprintEvidenceSourceError::InvalidEvidence,
					input.ProfileId);
			}
			profile = std::move(resolved.Profile);
		}

		if (!captureSource)
		{
			return Failure(
				"BYTECODE_CAPTURE_UNAVAILABLE",
				"No bytecode capture source is installed for the active session",
				{{"function_path", input.FunctionPath}});
		}
		const Runtime::BlueprintBytecodeCaptureRequest captureRequest{
			.Binding = binding,
			.Function = input.Function,
			.FunctionPath = input.FunctionPath,
			.MaxScriptBytes = kMaxScriptBytes
		};
		Runtime::BlueprintBytecodeCaptureResult captured;
		try
		{
			captured = captureSource->Capture(captureRequest);
		}
		catch (const std::bad_alloc&)
		{
			return CaptureSourceFailure(
				Runtime::BlueprintEvidenceSourceError::AllocationFailed,
				input.FunctionPath);
		}
		catch (...)
		{
			return CaptureSourceFailure(
				Runtime::BlueprintEvidenceSourceError::InternalError,
				input.FunctionPath);
		}
		if (!captured.Ok())
			return CaptureSourceFailure(captured.Error, input.FunctionPath);
		if (!ValidateCapture(*captured.Capture, captureRequest))
		{
			return CaptureSourceFailure(
				Runtime::BlueprintEvidenceSourceError::InvalidEvidence,
				input.FunctionPath);
		}

		json response = BaseResult(*lookup.Function, *captured.Capture);
		if (!decompile)
		{
			response["encoding"] = "hex_upper";
			response["bytecode"] = EncodeHex(captured.Capture->Script);
			return SuccessBounded(std::move(response));
		}

		const BlueprintDecompiler::DisassemblyResult disassembly =
			BlueprintDecompiler::Disassemble(
				captured.Capture->Script,
				profile->Definition);
		if (disassembly.FirstError.Present
			&& (disassembly.FirstError.Code
					== BlueprintDecompiler::DisassemblyErrorCode::ProfileRequired
				|| disassembly.FirstError.Code
					== BlueprintDecompiler::DisassemblyErrorCode::InvalidProfile))
		{
			return Failure(
				DisassemblyErrorName(disassembly.FirstError.Code),
				"The witnessed bytecode profile failed strict decoder validation",
				{
					{"profile_id", input.ProfileId},
					{"reason", disassembly.FirstError.Message}
				});
		}

		response["profile"] = {
			{"id", profile->Definition.Id},
			{"source", profile->Source},
			{"fingerprint", std::format("{:016X}", profile->EvidenceFingerprint)}
		};
		response["disassembly"] = SerializeDisassembly(disassembly);
		return SuccessBounded(std::move(response));
	}
	catch (const std::bad_alloc&)
	{
		return Failure(
			"BLUEPRINT_ALLOCATION_FAILED",
			"The bounded Blueprint command could not allocate its result");
	}
	catch (...)
	{
		return Failure(
			"BLUEPRINT_INTERNAL_ERROR",
			"The Blueprint command failed without returning partial or guessed data");
	}
}

} // namespace UExplorer::Services
