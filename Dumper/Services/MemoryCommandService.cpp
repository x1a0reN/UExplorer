#include "MemoryCommandService.h"

#include "Runtime/SafeMemory.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kRawRead = "memory.raw.read";
constexpr std::string_view kRawWrite = "memory.raw.write";
constexpr std::string_view kTypedRead = "memory.typed.read";
constexpr std::string_view kTypedWrite = "memory.typed.write";
constexpr std::string_view kPointerChain = "memory.pointer_chain.resolve";

MemoryCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {.Error = MemoryCommandError{
		.Code = std::move(code),
		.Message = std::move(message),
		.Details = std::move(details)}};
}

MemoryCommandResult MemoryFailure(
	const std::string_view operation,
	const std::string_view action,
	const Runtime::MemoryResult& result,
	json details = json::object())
{
	details["operation"] = operation;
	details["memory_error"] = Runtime::ToString(result.Error);
	details["native_error"] = result.NativeError;
	details["bytes_processed"] = result.BytesProcessed;
	return Failure(
		std::format("MEMORY_{}_{}", action, Runtime::ToString(result.Error)),
		std::format("Bounded memory {} failed: {}", action, Runtime::ToString(result.Error)),
		std::move(details));
}

bool IsUpperHex(const char value) noexcept
{
	return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}

bool TryAddress(const json& value, const bool allowZero, std::uintptr_t& parsed) noexcept
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!encoded.starts_with("0x")
		|| encoded.size() < 3
		|| encoded.size() > 18
		|| (encoded.size() > 3 && encoded[2] == '0')
		|| !std::all_of(encoded.begin() + 2, encoded.end(), IsUpperHex))
	{
		return false;
	}
	std::uint64_t wide = 0;
	const auto result = std::from_chars(
		encoded.data() + 2,
		encoded.data() + encoded.size(),
		wide,
		16);
	if (result.ec != std::errc{}
		|| result.ptr != encoded.data() + encoded.size()
		|| (!allowZero && wide == 0)
		|| wide > (std::numeric_limits<std::uintptr_t>::max)())
	{
		return false;
	}
	parsed = static_cast<std::uintptr_t>(wide);
	return true;
}

bool TryUnsignedCount(
	const json& value,
	const std::size_t minimum,
	const std::size_t maximum,
	std::size_t& parsed) noexcept
{
	parsed = 0;
	if (!value.is_number_integer())
		return false;
	try
	{
		if (value.is_number_unsigned())
		{
			const std::uint64_t candidate = value.get<std::uint64_t>();
			if (candidate < minimum || candidate > maximum)
				return false;
			parsed = static_cast<std::size_t>(candidate);
			return true;
		}
		const std::int64_t candidate = value.get<std::int64_t>();
		if (candidate < 0
			|| static_cast<std::uint64_t>(candidate) < minimum
			|| static_cast<std::uint64_t>(candidate) > maximum)
		{
			return false;
		}
		parsed = static_cast<std::size_t>(candidate);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool IsCanonicalSignedDecimal(const std::string_view encoded) noexcept
{
	if (encoded.empty())
		return false;
	std::size_t cursor = encoded.front() == '-' ? 1 : 0;
	if (cursor == encoded.size())
		return false;
	if (encoded[cursor] == '0')
		return cursor == 0 && encoded.size() == 1;
	if (encoded[cursor] < '1' || encoded[cursor] > '9')
		return false;
	for (++cursor; cursor < encoded.size(); ++cursor)
	{
		if (encoded[cursor] < '0' || encoded[cursor] > '9')
			return false;
	}
	return true;
}

bool IsCanonicalUnsignedDecimal(const std::string_view encoded) noexcept
{
	if (encoded.empty())
		return false;
	if (encoded.front() == '0')
		return encoded.size() == 1;
	return encoded.front() >= '1' && encoded.front() <= '9'
		&& std::all_of(encoded.begin() + 1, encoded.end(), [](const char value) {
			return value >= '0' && value <= '9';
		});
}

bool TrySigned(const json& value, std::int64_t& parsed) noexcept
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalSignedDecimal(encoded))
		return false;
	const auto result = std::from_chars(
		encoded.data(), encoded.data() + encoded.size(), parsed, 10);
	return result.ec == std::errc{} && result.ptr == encoded.data() + encoded.size();
}

bool TryUnsigned(const json& value, std::uint64_t& parsed) noexcept
{
	parsed = 0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (!IsCanonicalUnsignedDecimal(encoded))
		return false;
	const auto result = std::from_chars(
		encoded.data(), encoded.data() + encoded.size(), parsed, 10);
	return result.ec == std::errc{} && result.ptr == encoded.data() + encoded.size();
}

bool TryFiniteDouble(const json& value, double& parsed) noexcept
{
	parsed = 0.0;
	if (!value.is_string())
		return false;
	const std::string& encoded = value.get_ref<const std::string&>();
	if (encoded.empty() || encoded.size() > 128 || encoded.front() == '+'
		|| std::any_of(encoded.begin(), encoded.end(), [](const unsigned char value) {
			return value <= 0x20 || value == 0x7F;
		}))
	{
		return false;
	}
	const auto result = std::from_chars(
		encoded.data(),
		encoded.data() + encoded.size(),
		parsed,
		std::chars_format::general);
	return result.ec == std::errc{}
		&& result.ptr == encoded.data() + encoded.size()
		&& std::isfinite(parsed);
}

std::string BytesToHex(const std::span<const std::byte> bytes)
{
	static constexpr char kDigits[] = "0123456789ABCDEF";
	std::string encoded;
	encoded.reserve(bytes.size() * 3 - (bytes.empty() ? 0 : 1));
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		if (index != 0)
			encoded.push_back(' ');
		const auto value = static_cast<std::uint8_t>(bytes[index]);
		encoded.push_back(kDigits[value >> 4]);
		encoded.push_back(kDigits[value & 0x0F]);
	}
	return encoded;
}

constexpr Runtime::MemoryWriteOptions kDataWriteOptions{
	.AllowProtectionChange = true,
	.AllowExecutableWrite = false,
	.FlushInstructionCache = false
};

json SerializeMemoryResult(const Runtime::MemoryResult& result)
{
	return {
		{"error", Runtime::ToString(result.Error)},
		{"native_error", result.NativeError},
		{"bytes_processed", result.BytesProcessed}
	};
}

enum class RollbackState : std::uint8_t
{
	NotNeeded,
	Restored,
	RestoredWithError,
	Failed
};

const char* ToString(const RollbackState state) noexcept
{
	switch (state)
	{
	case RollbackState::NotNeeded: return "not_needed";
	case RollbackState::Restored: return "restored";
	case RollbackState::RestoredWithError: return "restored_with_error";
	case RollbackState::Failed: return "failed";
	}
	return "failed";
}

struct RollbackAttempt
{
	RollbackState State = RollbackState::Failed;
	bool PrecheckMatches = false;
	bool WriteAttempted = false;
	bool VerifyMatches = false;
	Runtime::MemoryResult Precheck;
	Runtime::MemoryResult Write;
	Runtime::MemoryResult Verify;

	bool Complete() const noexcept
	{
		return State == RollbackState::NotNeeded
			|| State == RollbackState::Restored;
	}

	json Serialize() const
	{
		json data = {
			{"state", ToString(State)},
			{"complete", Complete()},
			{"scope", "content_bytes_only"},
			{"atomic", false},
			{"contents_restored", VerifyMatches || PrecheckMatches},
			{"write_attempted", WriteAttempted},
			{"precheck_matches", Precheck.Ok() ? json(PrecheckMatches) : json(nullptr)},
			{"precheck", SerializeMemoryResult(Precheck)},
			{"verify_matches", Verify.Ok() ? json(VerifyMatches) : json(nullptr)},
			{"verify", SerializeMemoryResult(Verify)}
		};
		data["write"] = WriteAttempted
			? SerializeMemoryResult(Write)
			: json(nullptr);
		return data;
	}
};

RollbackAttempt RestorePreimage(
	const std::uintptr_t address,
	const std::span<const std::byte> preimage,
	const std::span<std::byte> observed)
{
	RollbackAttempt attempt;
	if (observed.size() != preimage.size())
		return attempt;
	std::fill(observed.begin(), observed.end(), std::byte{0});
	attempt.Precheck = Runtime::ReadMemory(address, observed);
	attempt.PrecheckMatches = attempt.Precheck.Ok()
		&& std::equal(preimage.begin(), preimage.end(), observed.begin());
	if (attempt.PrecheckMatches)
	{
		attempt.State = RollbackState::NotNeeded;
		attempt.Verify = attempt.Precheck;
		attempt.VerifyMatches = true;
		return attempt;
	}

	attempt.WriteAttempted = true;
	attempt.Write = Runtime::WriteMemory(address, preimage, kDataWriteOptions);
	std::fill(observed.begin(), observed.end(), std::byte{0});
	attempt.Verify = Runtime::ReadMemory(address, observed);
	attempt.VerifyMatches = attempt.Verify.Ok()
		&& std::equal(preimage.begin(), preimage.end(), observed.begin());
	if (attempt.Write.Ok() && attempt.VerifyMatches)
		attempt.State = RollbackState::Restored;
	else if (attempt.VerifyMatches)
		attempt.State = RollbackState::RestoredWithError;
	else
		attempt.State = RollbackState::Failed;
	return attempt;
}

MemoryCommandResult WriteFailureWithRollback(
	const std::string_view operation,
	const std::string_view errorPrefix,
	const std::string_view label,
	const std::string_view stage,
	const Runtime::MemoryResult& primary,
	const std::uintptr_t address,
	const std::span<const std::byte> preimage,
	const std::span<std::byte> rollbackScratch,
	json details)
{
	RollbackAttempt rollback = RestorePreimage(address, preimage, rollbackScratch);
	details["operation"] = operation;
	details["address"] = std::format("0x{:X}", address);
	details["failure_stage"] = stage;
	details["primary"] = SerializeMemoryResult(primary);
	details["rollback"] = rollback.Serialize();
	details["atomic"] = false;
	details["restore_scope"] = "bounded_content_bytes";
	return Failure(
		std::format(
			"{}_{}",
			errorPrefix,
			rollback.Complete() ? "FAILED_CONTENT_RESTORED" : "ROLLBACK_FAILED"),
		rollback.Complete()
			? std::format(
				"{} failed during {}; the bounded preimage contents are restored",
				label,
				stage)
			: std::format(
				"{} failed during {}; the bounded preimage restore could not be proven",
				label,
				stage),
		std::move(details));
}

MemoryCommandResult ExecuteVerifiedWrite(
	const std::string_view operation,
	const std::string_view errorPrefix,
	const std::string_view label,
	const std::uintptr_t address,
	const std::span<const std::byte> input,
	json details)
{
	std::vector<std::byte> preimage(input.size());
	std::vector<std::byte> observed(input.size());
	const Runtime::MemoryResult preimageRead = Runtime::ReadMemory(address, preimage);
	if (!preimageRead.Ok())
	{
		details["operation"] = operation;
		details["address"] = std::format("0x{:X}", address);
		details["preimage"] = SerializeMemoryResult(preimageRead);
		details["mutation_attempted"] = false;
		return Failure(
			std::format("{}_PREIMAGE_UNAVAILABLE", errorPrefix),
			std::format("{} requires a complete readable preimage before mutation", label),
			std::move(details));
	}

	const Runtime::MemoryResult write =
		Runtime::WriteMemory(address, input, kDataWriteOptions);
	if (!write.Ok())
	{
		return WriteFailureWithRollback(
			operation,
			errorPrefix,
			label,
			"write",
			write,
			address,
			preimage,
			observed,
			std::move(details));
	}

	std::fill(observed.begin(), observed.end(), std::byte{0});
	const Runtime::MemoryResult verify = Runtime::ReadMemory(address, observed);
	if (!verify.Ok())
	{
		return WriteFailureWithRollback(
			operation,
			errorPrefix,
			label,
			"verify_read",
			verify,
			address,
			preimage,
			observed,
			std::move(details));
	}
	if (!std::equal(input.begin(), input.end(), observed.begin()))
	{
		return WriteFailureWithRollback(
			operation,
			errorPrefix,
			label,
			"verify_mismatch",
			Runtime::MemoryResult{
				.Error = Runtime::MemoryError::ValueMismatch,
				.BytesProcessed = observed.size()
			},
			address,
			preimage,
			observed,
			std::move(details));
	}

	return {.Data = {
		{"address", std::format("0x{:X}", address)},
		{"bytes_written", input.size()},
		{"verified", true},
		{"atomic", false},
		{"failure_restore", "bounded_content_preimage"}
	}};
}

template<typename T>
bool CopyFromBytes(const std::span<const std::byte> bytes, T& value) noexcept
{
	static_assert(std::is_trivially_copyable_v<T>);
	if (bytes.size() < sizeof(T))
		return false;
	std::memcpy(&value, bytes.data(), sizeof(T));
	return true;
}

json InterpretBytes(const std::span<const std::byte> bytes)
{
	json values = json::object();
	std::uint8_t u8 = 0;
	std::int32_t i32 = 0;
	std::int64_t i64 = 0;
	float f32 = 0.0F;
	double f64 = 0.0;
	std::uintptr_t pointer = 0;
	if (CopyFromBytes(bytes, u8))
		values["uint8"] = u8;
	if (CopyFromBytes(bytes, i32))
		values["int32"] = i32;
	if (CopyFromBytes(bytes, f32) && std::isfinite(f32))
		values["float"] = f32;
	if (CopyFromBytes(bytes, i64))
		values["int64"] = std::to_string(i64);
	if (CopyFromBytes(bytes, f64) && std::isfinite(f64))
		values["double"] = f64;
	if (CopyFromBytes(bytes, pointer))
		values["pointer"] = std::format("0x{:X}", pointer);
	return values;
}

bool CheckedAddOffset(
	const std::uintptr_t base,
	const std::int64_t offset,
	std::uintptr_t& result) noexcept
{
	if (offset >= 0)
	{
		const auto positive = static_cast<std::uint64_t>(offset);
		if (positive > (std::numeric_limits<std::uintptr_t>::max)() - base)
			return false;
		result = base + static_cast<std::uintptr_t>(positive);
		return true;
	}
	const std::uint64_t magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
	if (magnitude > base)
		return false;
	result = base - static_cast<std::uintptr_t>(magnitude);
	return true;
}

enum class ScalarType : std::uint8_t
{
	Int8,
	UInt8,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Float,
	Double,
	Pointer
};

bool TryScalarType(const json& value, ScalarType& type, std::string& name) noexcept
{
	if (!value.is_string())
		return false;
	name = value.get_ref<const std::string&>();
	if (name == "int8") type = ScalarType::Int8;
	else if (name == "byte" || name == "uint8") type = ScalarType::UInt8;
	else if (name == "int16") type = ScalarType::Int16;
	else if (name == "uint16") type = ScalarType::UInt16;
	else if (name == "int32") type = ScalarType::Int32;
	else if (name == "uint32") type = ScalarType::UInt32;
	else if (name == "int64") type = ScalarType::Int64;
	else if (name == "uint64") type = ScalarType::UInt64;
	else if (name == "float") type = ScalarType::Float;
	else if (name == "double") type = ScalarType::Double;
	else if (name == "pointer") type = ScalarType::Pointer;
	else return false;
	return true;
}

std::size_t ScalarSize(const ScalarType type) noexcept
{
	switch (type)
	{
	case ScalarType::Int8:
	case ScalarType::UInt8: return 1;
	case ScalarType::Int16:
	case ScalarType::UInt16: return 2;
	case ScalarType::Int32:
	case ScalarType::UInt32:
	case ScalarType::Float: return 4;
	case ScalarType::Int64:
	case ScalarType::UInt64:
	case ScalarType::Double:
	case ScalarType::Pointer: return 8;
	}
	return 0;
}

template<typename T>
bool EncodeSigned(const json& value, std::array<std::byte, 8>& bytes) noexcept
{
	std::int64_t parsed = 0;
	if (!TrySigned(value, parsed)
		|| parsed < static_cast<std::int64_t>((std::numeric_limits<T>::min)())
		|| parsed > static_cast<std::int64_t>((std::numeric_limits<T>::max)()))
	{
		return false;
	}
	const T narrowed = static_cast<T>(parsed);
	std::memcpy(bytes.data(), &narrowed, sizeof(narrowed));
	return true;
}

template<typename T>
bool EncodeUnsigned(const json& value, std::array<std::byte, 8>& bytes) noexcept
{
	std::uint64_t parsed = 0;
	if (!TryUnsigned(value, parsed)
		|| parsed > static_cast<std::uint64_t>((std::numeric_limits<T>::max)()))
	{
		return false;
	}
	const T narrowed = static_cast<T>(parsed);
	std::memcpy(bytes.data(), &narrowed, sizeof(narrowed));
	return true;
}

bool EncodeScalar(
	const ScalarType type,
	const json& value,
	std::array<std::byte, 8>& bytes) noexcept
{
	bytes.fill(std::byte{0});
	switch (type)
	{
	case ScalarType::Int8: return EncodeSigned<std::int8_t>(value, bytes);
	case ScalarType::UInt8: return EncodeUnsigned<std::uint8_t>(value, bytes);
	case ScalarType::Int16: return EncodeSigned<std::int16_t>(value, bytes);
	case ScalarType::UInt16: return EncodeUnsigned<std::uint16_t>(value, bytes);
	case ScalarType::Int32: return EncodeSigned<std::int32_t>(value, bytes);
	case ScalarType::UInt32: return EncodeUnsigned<std::uint32_t>(value, bytes);
	case ScalarType::Int64: return EncodeSigned<std::int64_t>(value, bytes);
	case ScalarType::UInt64: return EncodeUnsigned<std::uint64_t>(value, bytes);
	case ScalarType::Float:
	{
		double parsed = 0.0;
		if (!TryFiniteDouble(value, parsed))
			return false;
		const float narrowed = static_cast<float>(parsed);
		if (!std::isfinite(narrowed))
			return false;
		std::memcpy(bytes.data(), &narrowed, sizeof(narrowed));
		return true;
	}
	case ScalarType::Double:
	{
		double parsed = 0.0;
		if (!TryFiniteDouble(value, parsed))
			return false;
		std::memcpy(bytes.data(), &parsed, sizeof(parsed));
		return true;
	}
	case ScalarType::Pointer:
	{
		std::uintptr_t pointer = 0;
		if (!TryAddress(value, true, pointer))
			return false;
		std::memcpy(bytes.data(), &pointer, sizeof(pointer));
		return true;
	}
	}
	return false;
}

json DecodeScalar(const ScalarType type, const std::span<const std::byte> bytes)
{
	switch (type)
	{
	case ScalarType::Int8:
	{
		std::int8_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::UInt8:
	{
		std::uint8_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::Int16:
	{
		std::int16_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::UInt16:
	{
		std::uint16_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::Int32:
	{
		std::int32_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::UInt32:
	{
		std::uint32_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::Int64:
	{
		std::int64_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::UInt64:
	{
		std::uint64_t value = 0; CopyFromBytes(bytes, value); return std::to_string(value);
	}
	case ScalarType::Float:
	{
		float value = 0.0F; CopyFromBytes(bytes, value);
		return std::isfinite(value) ? json(value) : json(nullptr);
	}
	case ScalarType::Double:
	{
		double value = 0.0; CopyFromBytes(bytes, value);
		return std::isfinite(value) ? json(value) : json(nullptr);
	}
	case ScalarType::Pointer:
	{
		std::uintptr_t value = 0; CopyFromBytes(bytes, value);
		return std::format("0x{:X}", value);
	}
	}
	return nullptr;
}

MemoryCommandResult ExecuteRawRead(const json& data)
{
	if (!data.is_object() || data.size() != 2
		|| !data.contains("address") || !data.contains("size"))
	{
		return Failure("MEMORY_READ_REQUEST_INVALID", "Raw read requires exactly address and size");
	}
	std::uintptr_t address = 0;
	std::size_t size = 0;
	if (!TryAddress(data.at("address"), false, address)
		|| !TryUnsignedCount(data.at("size"), 1, MemoryCommandService::kMaxReadBytes, size))
	{
		return Failure(
			"MEMORY_READ_REQUEST_INVALID",
			"address must be canonical uppercase hex and size must be in range 1..4096");
	}
	std::vector<std::byte> bytes(size);
	const Runtime::MemoryResult read = Runtime::ReadMemory(address, bytes);
	if (!read.Ok())
		return MemoryFailure(kRawRead, "READ", read, {{"address", data.at("address")}, {"size", size}});
	return {.Data = {
		{"address", std::format("0x{:X}", address)},
		{"size", size},
		{"hex", BytesToHex(bytes)},
		{"interpret", InterpretBytes(bytes)}}};
}

MemoryCommandResult ExecuteRawWrite(const json& data)
{
	if (!data.is_object() || data.size() != 2
		|| !data.contains("address") || !data.contains("bytes")
		|| !data.at("bytes").is_array()
		|| data.at("bytes").empty()
		|| data.at("bytes").size() > MemoryCommandService::kMaxWriteBytes)
	{
		return Failure(
			"MEMORY_WRITE_REQUEST_INVALID",
			"Raw write requires canonical address and 1..4096 byte values");
	}
	std::uintptr_t address = 0;
	if (!TryAddress(data.at("address"), false, address))
		return Failure("MEMORY_WRITE_REQUEST_INVALID", "address must be canonical uppercase hex");
	std::vector<std::byte> bytes;
	bytes.reserve(data.at("bytes").size());
	for (const json& item : data.at("bytes"))
	{
		std::size_t value = 0;
		if (!TryUnsignedCount(item, 0, 255, value))
			return Failure("MEMORY_WRITE_REQUEST_INVALID", "Every raw byte must be an integer in range 0..255");
		bytes.push_back(static_cast<std::byte>(value));
	}
	return ExecuteVerifiedWrite(
		kRawWrite,
		"MEMORY_WRITE",
		"Raw write",
		address,
		bytes,
		{{"requested_bytes", bytes.size()}});
}

MemoryCommandResult ExecuteTypedRead(const json& data)
{
	if (!data.is_object() || data.size() != 2
		|| !data.contains("address") || !data.contains("type"))
	{
		return Failure("MEMORY_TYPED_READ_REQUEST_INVALID", "Typed read requires exactly address and type");
	}
	std::uintptr_t address = 0;
	ScalarType type{};
	std::string typeName;
	if (!TryAddress(data.at("address"), false, address)
		|| !TryScalarType(data.at("type"), type, typeName))
	{
		return Failure("MEMORY_TYPED_READ_REQUEST_INVALID", "The address or scalar type is invalid");
	}
	std::array<std::byte, 8> bytes{};
	const std::size_t size = ScalarSize(type);
	const Runtime::MemoryResult read = Runtime::ReadMemory(
		address,
		std::span<std::byte>(bytes).first(size));
	if (!read.Ok())
		return MemoryFailure(kTypedRead, "TYPED_READ", read, {{"type", typeName}});
	json value = DecodeScalar(type, std::span<const std::byte>(bytes).first(size));
	if (value.is_null() && (type == ScalarType::Float || type == ScalarType::Double))
		return Failure("MEMORY_TYPED_NON_FINITE", "The selected floating-point value is not finite");
	return {.Data = {
		{"address", std::format("0x{:X}", address)},
		{"type", typeName},
		{"value", std::move(value)}}};
}

MemoryCommandResult ExecuteTypedWrite(const json& data)
{
	if (!data.is_object() || data.size() != 3
		|| !data.contains("address") || !data.contains("type") || !data.contains("value"))
	{
		return Failure("MEMORY_TYPED_WRITE_REQUEST_INVALID", "Typed write requires address, type, and value");
	}
	std::uintptr_t address = 0;
	ScalarType type{};
	std::string typeName;
	std::array<std::byte, 8> bytes{};
	if (!TryAddress(data.at("address"), false, address)
		|| !TryScalarType(data.at("type"), type, typeName)
		|| !EncodeScalar(type, data.at("value"), bytes))
	{
		return Failure(
			"MEMORY_TYPED_WRITE_REQUEST_INVALID",
			"The address, scalar type, or canonical string value is invalid or out of range");
	}
	const std::size_t size = ScalarSize(type);
	const auto input = std::span<const std::byte>(bytes).first(size);
	MemoryCommandResult result = ExecuteVerifiedWrite(
		kTypedWrite,
		"MEMORY_TYPED_WRITE",
		"Typed write",
		address,
		input,
		{{"type", typeName}});
	if (!result.Ok())
		return result;
	result.Data["type"] = typeName;
	result.Data["written"] = true;
	return result;
}

MemoryCommandResult ExecutePointerChain(const json& data)
{
	if (!data.is_object() || data.size() != 2
		|| !data.contains("base") || !data.contains("offsets")
		|| !data.at("offsets").is_array()
		|| data.at("offsets").size() > MemoryCommandService::kMaxPointerOffsets)
	{
		return Failure(
			"POINTER_CHAIN_REQUEST_INVALID",
			"Pointer-chain resolution requires base and at most 64 signed decimal offsets");
	}
	std::uintptr_t address = 0;
	if (!TryAddress(data.at("base"), false, address))
		return Failure("POINTER_CHAIN_REQUEST_INVALID", "base must be canonical uppercase hex");
	json steps = json::array({{{"address", std::format("0x{:X}", address)}, {"offset", "0"}}});
	for (std::size_t index = 0; index < data.at("offsets").size(); ++index)
	{
		std::int64_t offset = 0;
		if (!TrySigned(data.at("offsets").at(index), offset))
		{
			return Failure(
				"POINTER_CHAIN_REQUEST_INVALID",
				"Every pointer-chain offset must be a canonical signed decimal string",
				{{"failed_step", index}});
		}
		std::uintptr_t dereferenced = 0;
		const Runtime::MemoryResult read = Runtime::ReadValue(address, dereferenced);
		if (!read.Ok())
		{
			return MemoryFailure(
				kPointerChain,
				"POINTER_READ",
				read,
				{{"failed_step", index}, {"steps", std::move(steps)}});
		}
		if (dereferenced == 0)
		{
			return Failure(
				"POINTER_CHAIN_NULL",
				"Pointer-chain resolution encountered a null pointer",
				{{"failed_step", index}, {"steps", std::move(steps)}});
		}
		if (!CheckedAddOffset(dereferenced, offset, address))
		{
			return Failure(
				"POINTER_CHAIN_OVERFLOW",
				"Pointer-chain address arithmetic overflowed",
				{{"failed_step", index}, {"steps", std::move(steps)}});
		}
		steps.push_back({
			{"deref", std::format("0x{:X}", dereferenced)},
			{"offset", std::to_string(offset)},
			{"address", std::format("0x{:X}", address)}});
	}

	json value = nullptr;
	std::array<std::byte, sizeof(std::uintptr_t)> bytes{};
	const Runtime::MemoryResult read = Runtime::ReadMemory(address, bytes);
	if (read.Ok())
		value = InterpretBytes(bytes);
	return {.Data = {
		{"final_address", std::format("0x{:X}", address)},
		{"steps", std::move(steps)},
		{"value", std::move(value)}}};
}

} // namespace

bool MemoryCommandService::Handles(const std::string_view operation) noexcept
{
	return operation == kRawRead
		|| operation == kRawWrite
		|| operation == kTypedRead
		|| operation == kTypedWrite
		|| operation == kPointerChain;
}

MemoryCommandResult MemoryCommandService::Execute(
	const std::string_view operation,
	const json& data) noexcept
{
	try
	{
		if (operation == kRawRead)
			return ExecuteRawRead(data);
		if (operation == kRawWrite)
			return ExecuteRawWrite(data);
		if (operation == kTypedRead)
			return ExecuteTypedRead(data);
		if (operation == kTypedWrite)
			return ExecuteTypedWrite(data);
		if (operation == kPointerChain)
			return ExecutePointerChain(data);
		return Failure(
			"MEMORY_OPERATION_NOT_SUPPORTED",
			"MemoryCommandService does not handle the requested operation",
			{{"operation", operation}});
	}
	catch (const std::bad_alloc&)
	{
		return Failure("MEMORY_ALLOCATION_FAILED", "The bounded memory command could not allocate its owned buffer");
	}
	catch (const json::exception& error)
	{
		return Failure("MEMORY_REQUEST_INVALID", error.what());
	}
	catch (...)
	{
		return Failure("MEMORY_COMMAND_FAILED", "The bounded memory command raised an unexpected exception");
	}
}

} // namespace UExplorer::Services
