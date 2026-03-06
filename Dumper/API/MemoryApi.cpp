#include "WinMemApi.h"

#include "MemoryApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Platform.h"

#include <format>
#include <sstream>
#include <iomanip>

namespace UExplorer::API
{

static std::string BytesToHex(const uint8* data, size_t size)
{
	std::ostringstream ss;
	for (size_t i = 0; i < size; i++)
	{
		if (i > 0) ss << ' ';
		ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)data[i];
	}
	return ss.str();
}

static bool IsReadable(const void* addr, size_t size = 1)
{
	if (!addr) return false;
	if (Platform::IsBadReadPtr(addr)) return false;
	if (size > 1 && Platform::IsBadReadPtr(reinterpret_cast<const uint8*>(addr) + size - 1)) return false;
	return true;
}

template<typename T>
static bool SafeRead(uintptr_t addr, T& out)
{
	if (!IsReadable(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
	__try
	{
		out = *reinterpret_cast<const T*>(addr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool SafeReadBytes(uintptr_t addr, uint8* out, size_t size)
{
	if (!IsReadable(reinterpret_cast<const void*>(addr), size)) return false;
	__try
	{
		memcpy(out, reinterpret_cast<const void*>(addr), size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static std::string SafeBytesToHex(uintptr_t addr, size_t size)
{
	std::vector<uint8> buf(size, 0);
	if (!SafeReadBytes(addr, buf.data(), size))
		return "";
	return BytesToHex(buf.data(), size);
}

void RegisterMemoryRoutes(HttpServer& server)
{
	server.Post("/api/v1/memory/read", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			int size = body.value("size", 64);
			if (size < 1) size = 1;
			if (size > 4096) size = 4096;

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };

			std::string hexStr = SafeBytesToHex(addr, static_cast<size_t>(size));
			if (hexStr.empty())
				return { 400, "application/json", MakeError("Access violation: cannot read at " + addrStr) };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["size"] = size;
			data["hex"] = hexStr;

			json interp;
			uint8 u8v; int32 i32v; float fv; int64 i64v; double dv; uintptr_t pv;
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

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["type"] = type;

			bool ok = false;
			if (type == "byte" || type == "uint8") {
				uint8 v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "int32") {
				int32 v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "uint32") {
				uint32 v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "int64") {
				int64 v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
			} else if (type == "uint64") {
				uint64 v; ok = SafeRead(addr, v); if (ok) data["value"] = v;
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

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };
			if (bytes.empty())
				return { 400, "application/json", MakeError("No bytes to write") };
			if (bytes.size() > 4096)
				return { 400, "application/json", MakeError("Too many bytes (max 4096)") };

			if (!IsReadable(reinterpret_cast<const void*>(addr), bytes.size()))
				return { 400, "application/json", MakeError("Access violation: cannot write at " + addrStr) };

			uint8* ptr = reinterpret_cast<uint8*>(addr);
			DWORD oldProtect;
			VirtualProtect(ptr, bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect);
			for (size_t i = 0; i < bytes.size(); i++)
				ptr[i] = static_cast<uint8>(bytes[i] & 0xFF);
			VirtualProtect(ptr, bytes.size(), oldProtect, &oldProtect);

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

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };

			size_t typeSize = 0;
			if (type == "byte" || type == "uint8") typeSize = 1;
			else if (type == "int32" || type == "float") typeSize = 4;
			else if (type == "double") typeSize = 8;
			else return { 400, "application/json", MakeError("Unknown type: " + type) };

			if (!IsReadable(reinterpret_cast<const void*>(addr), typeSize))
				return { 400, "application/json", MakeError("Access violation: cannot write at " + addrStr) };

			DWORD oldProtect;
			VirtualProtect(reinterpret_cast<void*>(addr), typeSize, PAGE_EXECUTE_READWRITE, &oldProtect);
			if (type == "byte" || type == "uint8")
				*reinterpret_cast<uint8*>(addr) = static_cast<uint8>(body.value("value", 0));
			else if (type == "int32")
				*reinterpret_cast<int32*>(addr) = body.value("value", 0);
			else if (type == "float")
				*reinterpret_cast<float*>(addr) = body.value("value", 0.0f);
			else if (type == "double")
				*reinterpret_cast<double*>(addr) = body.value("value", 0.0);
			VirtualProtect(reinterpret_cast<void*>(addr), typeSize, oldProtect, &oldProtect);

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
			auto offsets = body.value("offsets", std::vector<int64>{});

			uintptr_t addr = std::stoull(baseStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid base address") };

			json steps = json::array();
			json step;
			step["address"] = std::format("0x{:X}", addr);
			step["offset"] = 0;
			steps.push_back(step);

			for (size_t i = 0; i < offsets.size(); i++)
			{
				uintptr_t deref = 0;
				if (!SafeRead(addr, deref))
					return { 200, "application/json", MakeResponse(json{
						{"error", "Access violation at step " + std::to_string(i) + " (0x" + std::format("{:X}", addr) + ")"},
						{"steps", steps}
					})};
				if (!deref)
					return { 200, "application/json", MakeResponse(json{
						{"error", "Null pointer at step " + std::to_string(i)},
						{"steps", steps}
					})};

				addr = deref + offsets[i];
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
			int32 i32v; float fv; uintptr_t pv;
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