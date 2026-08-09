#include "MemoryApi.h"
#include "ApiCommon.h"

#include "Runtime/SafeMemory.h"

#include <charconv>
#include <cstring>
#include <cstdint>
#include <format>
#include <array>
#include <limits>
#include <sstream>
#include <iomanip>
#include <utility>
#include <vector>

namespace UExplorer::API
{

static std::string BytesToHex(const std::uint8_t* data, size_t size)
{
	std::ostringstream ss;
	for (size_t i = 0; i < size; i++)
	{
		if (i > 0) ss << ' ';
		ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)data[i];
	}
	return ss.str();
}

static bool TryParseHexValue(std::string text, uintptr_t& output, const bool allowZero)
{
	if (text.starts_with("0x") || text.starts_with("0X"))
		text.erase(0, 2);
	if (text.empty())
		return false;

	uintptr_t parsed = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
		|| (!allowZero && parsed == 0))
		return false;
	output = parsed;
	return true;
}

static bool TryParseAddress(std::string text, uintptr_t& outAddress)
{
	return TryParseHexValue(std::move(text), outAddress, false);
}

template<typename T>
static bool SafeRead(uintptr_t addr, T& out)
{
	return Runtime::ReadValue(addr, out).Ok();
}

static bool SafeReadBytes(uintptr_t addr, std::uint8_t* out, size_t size)
{
	return Runtime::ReadMemory(
		addr,
		std::span<std::byte>(reinterpret_cast<std::byte*>(out), size)).Ok();
}

static std::string SafeBytesToHex(uintptr_t addr, size_t size)
{
	std::vector<std::uint8_t> buf(size, 0);
	if (!SafeReadBytes(addr, buf.data(), size))
		return "";
	return BytesToHex(buf.data(), size);
}

static Runtime::MemoryResult SafeWriteBytes(uintptr_t addr, const std::uint8_t* data, size_t size)
{
	if (!data)
		return {.Error = Runtime::MemoryError::InvalidRange};
	return Runtime::WriteMemory(
		addr,
		std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
}

static bool CheckedAddOffset(uintptr_t base, std::int64_t offset, uintptr_t& result)
{
	if (offset >= 0)
	{
		const auto positive = static_cast<std::uint64_t>(offset);
		if (positive > (std::numeric_limits<uintptr_t>::max)() - base)
			return false;
		result = base + static_cast<uintptr_t>(positive);
		return true;
	}
	const std::uint64_t magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
	if (magnitude > base)
		return false;
	result = base - static_cast<uintptr_t>(magnitude);
	return true;
}

void RegisterMemoryRoutes(HttpServer& server)
{
	server.Post("/api/v1/memory/read", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			int size = body.value("size", 64);
			if (size < 1 || size > 4096)
				return { 400, "application/json", MakeError("size must be in range 1..4096") };

			uintptr_t addr = 0;
			if (!TryParseAddress(addrStr, addr))
				return { 400, "application/json", MakeError("Invalid address") };

			std::string hexStr = SafeBytesToHex(addr, static_cast<size_t>(size));
			if (hexStr.empty())
				return { 400, "application/json", MakeError("Access violation: cannot read at " + addrStr) };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["size"] = size;
			data["hex"] = hexStr;

			json interp;
			std::uint8_t u8v; std::int32_t i32v; float fv; std::int64_t i64v; double dv; uintptr_t pv;
			if (size >= 1 && SafeRead(addr, u8v)) interp["uint8"] = u8v;
			if (size >= 4 && SafeRead(addr, i32v)) interp["int32"] = i32v;
			if (size >= 4 && SafeRead(addr, fv))   interp["float"] = fv;
			if (size >= 8 && SafeRead(addr, i64v)) interp["int64"] = i64v;
			if (size >= 8 && SafeRead(addr, dv))   interp["double"] = dv;
			if (size >= 8 && SafeRead(addr, pv))   interp["pointer"] = std::format("0x{:X}", pv);
			data["interpret"] = interp;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory read failed") };
		}
	});

	server.Post("/api/v1/memory/read-typed", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			std::string type = body.value("type", "int32");

			uintptr_t addr = 0;
			if (!TryParseAddress(addrStr, addr))
				return { 400, "application/json", MakeError("Invalid address") };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["type"] = type;

			bool ok = false;
			if (type == "byte" || type == "uint8") {
				std::uint8_t v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "int32") {
				std::int32_t v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "uint32") {
				std::uint32_t v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "int64") {
				std::int64_t v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "uint64") {
				std::uint64_t v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "float") {
				float v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "double") {
				double v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "pointer") {
				uintptr_t v; ok = SafeRead(addr, v); if (ok) data["value"] = std::format("0x{:X}", v);
			} else {
				return { 400, "application/json", MakeError("Unknown type: " + type) };
			}

			if (!ok)
				return { 400, "application/json", MakeError("Access violation: cannot read at " + addrStr) };

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory read failed") };
		}
	});

	server.Post("/api/v1/memory/write", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			auto bytes = body.value("bytes", std::vector<int>{});

			uintptr_t addr = 0;
			if (!TryParseAddress(addrStr, addr))
				return { 400, "application/json", MakeError("Invalid address") };
			if (bytes.empty())
				return { 400, "application/json", MakeError("No bytes to write") };
			if (bytes.size() > 4096)
				return { 400, "application/json", MakeError("Too many bytes (max 4096)") };

			std::vector<std::uint8_t> writeBytes;
			writeBytes.reserve(bytes.size());
			for (const int value : bytes)
			{
				if (value < 0 || value > 0xFF)
					return { 400, "application/json", MakeError("Byte values must be in range 0..255") };
				writeBytes.push_back(static_cast<std::uint8_t>(value));
			}

			const Runtime::MemoryResult writeResult =
				SafeWriteBytes(addr, writeBytes.data(), writeBytes.size());
			if (!writeResult.Ok())
				return { 400, "application/json",
					MakeError(std::string("MEMORY_WRITE_") + Runtime::ToString(writeResult.Error)) };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["bytes_written"] = bytes.size();
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory write failed") };
		}
	});

	server.Post("/api/v1/memory/write-typed", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			std::string type = body.value("type", "int32");

			uintptr_t addr = 0;
			if (!TryParseAddress(addrStr, addr))
				return { 400, "application/json", MakeError("Invalid address") };
			if (!body.contains("value"))
				return { 400, "application/json", MakeError("Missing value") };

			size_t typeSize = 0;
			if (type == "byte" || type == "uint8") typeSize = 1;
			else if (type == "int32" || type == "uint32" || type == "float") typeSize = 4;
			else if (type == "int64" || type == "uint64" || type == "double" || type == "pointer") typeSize = 8;
			else return { 400, "application/json", MakeError("Unknown type: " + type) };

			std::array<std::uint8_t, sizeof(double)> encoded{};
			if (type == "byte" || type == "uint8")
			{
				const int value = body.at("value").get<int>();
				if (value < 0 || value > 0xFF)
					return { 400, "application/json", MakeError("Byte value must be in range 0..255") };
				encoded[0] = static_cast<std::uint8_t>(value);
			}
			else if (type == "int32")
			{
				const std::int32_t value = body.at("value").get<std::int32_t>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "uint32")
			{
				const std::uint32_t value = body.at("value").get<std::uint32_t>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "int64")
			{
				const std::int64_t value = body.at("value").get<std::int64_t>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "uint64")
			{
				const std::uint64_t value = body.at("value").get<std::uint64_t>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "float")
			{
				const float value = body.at("value").get<float>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "double")
			{
				const double value = body.at("value").get<double>();
				memcpy(encoded.data(), &value, sizeof(value));
			}
			else if (type == "pointer")
			{
				if (!body.at("value").is_string())
					return { 400, "application/json", MakeError("Pointer value must be a hexadecimal string") };
				uintptr_t value = 0;
				if (!TryParseHexValue(body.at("value").get<std::string>(), value, true))
					return { 400, "application/json", MakeError("Invalid pointer value") };
				memcpy(encoded.data(), &value, sizeof(value));
			}

			const Runtime::MemoryResult writeResult = SafeWriteBytes(addr, encoded.data(), typeSize);
			if (!writeResult.Ok())
				return { 400, "application/json",
					MakeError(std::string("MEMORY_WRITE_") + Runtime::ToString(writeResult.Error)) };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["type"] = type;
			data["written"] = true;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory write failed") };
		}
	});

	server.Post("/api/v1/memory/pointer-chain", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string baseStr = body.value("base", "");
			auto offsets = body.value("offsets", std::vector<std::int64_t>{});

			uintptr_t addr = 0;
			if (!TryParseAddress(baseStr, addr))
				return { 400, "application/json", MakeError("Invalid base address") };
			if (offsets.size() > 64)
				return { 400, "application/json", MakeError("Too many offsets (max 64)") };

			json steps = json::array();
			json step;
			step["address"] = std::format("0x{:X}", addr);
			step["offset"] = 0;
			steps.push_back(step);

			for (size_t i = 0; i < offsets.size(); i++)
			{
				uintptr_t deref = 0;
				if (!SafeRead(addr, deref))
					return { 400, "application/json", MakeError(
						"POINTER_CHAIN_READ_FAILED",
						{{"failed_step", i}, {"steps", steps}}) };
				if (!deref)
					return { 400, "application/json", MakeError(
						"POINTER_CHAIN_NULL",
						{{"failed_step", i}, {"steps", steps}}) };

				if (!CheckedAddOffset(deref, offsets[i], addr))
					return { 400, "application/json", MakeError(
						"POINTER_CHAIN_OVERFLOW",
						{{"failed_step", i}, {"steps", steps}}) };
				json s;
				s["deref"] = std::format("0x{:X}", deref);
				s["offset"] = offsets[i];
				s["address"] = std::format("0x{:X}", addr);
				steps.push_back(s);
			}

			json data;
			data["final_address"] = std::format("0x{:X}", addr);
			data["steps"] = steps;

			json val;
			std::int32_t i32v; float fv; uintptr_t pv;
			if (SafeRead(addr, i32v)) val["int32"] = i32v;
			if (SafeRead(addr, fv))   val["float"] = fv;
			if (SafeRead(addr, pv))   val["pointer"] = std::format("0x{:X}", pv);
			data["value"] = val;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Pointer chain failed") };
		}
	});
}

} // namespace UExplorer::API
