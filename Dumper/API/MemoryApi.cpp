#include "WinMemApi.h"

#include "MemoryApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"

#include <format>
#include <sstream>
#include <iomanip>

namespace UExplorer::API
{

// Helper: format bytes as hex string
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

void RegisterMemoryRoutes(HttpServer& server)
{
	// POST /api/v1/memory/read — read raw bytes
	server.Post("/api/v1/memory/read", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			int size = body.value("size", 64);
			if (size > 4096) size = 4096;

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };

			const uint8* ptr = reinterpret_cast<const uint8*>(addr);

			json data;
			data["address"] = std::format("0x{:X}", addr);
			data["size"] = size;
			data["hex"] = BytesToHex(ptr, size);

			// Also provide typed interpretations at offset 0
			json interp;
			if (size >= 1) interp["uint8"] = *reinterpret_cast<const uint8*>(ptr);
			if (size >= 4) interp["int32"] = *reinterpret_cast<const int32*>(ptr);
			if (size >= 4) interp["float"] = *reinterpret_cast<const float*>(ptr);
			if (size >= 8) interp["int64"] = *reinterpret_cast<const int64*>(ptr);
			if (size >= 8) interp["double"] = *reinterpret_cast<const double*>(ptr);
			if (size >= 8) interp["pointer"] = std::format("0x{:X}", *reinterpret_cast<const uintptr_t*>(ptr));
			data["interpret"] = interp;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory read failed (access violation?)") };
		}
	});

	// POST /api/v1/memory/read-typed — typed memory read
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

			if (type == "byte" || type == "uint8")
				data["value"] = *reinterpret_cast<uint8*>(addr);
			else if (type == "int32")
				data["value"] = *reinterpret_cast<int32*>(addr);
			else if (type == "uint32")
				data["value"] = *reinterpret_cast<uint32*>(addr);
			else if (type == "int64")
				data["value"] = *reinterpret_cast<int64*>(addr);
			else if (type == "uint64")
				data["value"] = *reinterpret_cast<uint64*>(addr);
			else if (type == "float")
				data["value"] = *reinterpret_cast<float*>(addr);
			else if (type == "double")
				data["value"] = *reinterpret_cast<double*>(addr);
			else if (type == "pointer")
				data["value"] = std::format("0x{:X}", *reinterpret_cast<uintptr_t*>(addr));
			else
				return { 400, "application/json", MakeError("Unknown type: " + type) };

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Memory read failed") };
		}
	});

	// POST /api/v1/memory/write — write raw bytes
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

	// POST /api/v1/memory/write-typed — typed memory write
	server.Post("/api/v1/memory/write-typed", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string addrStr = body.value("address", "");
			std::string type = body.value("type", "int32");

			uintptr_t addr = std::stoull(addrStr, nullptr, 16);
			if (!addr)
				return { 400, "application/json", MakeError("Invalid address") };

			DWORD oldProtect;
			if (type == "byte" || type == "uint8") {
				VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &oldProtect);
				*reinterpret_cast<uint8*>(addr) = static_cast<uint8>(body.value("value", 0));
				VirtualProtect(reinterpret_cast<void*>(addr), 1, oldProtect, &oldProtect);
			}
			else if (type == "int32") {
				VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
				*reinterpret_cast<int32*>(addr) = body.value("value", 0);
				VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
			}
			else if (type == "float") {
				VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
				*reinterpret_cast<float*>(addr) = body.value("value", 0.0f);
				VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
			}
			else if (type == "double") {
				VirtualProtect(reinterpret_cast<void*>(addr), 8, PAGE_EXECUTE_READWRITE, &oldProtect);
				*reinterpret_cast<double*>(addr) = body.value("value", 0.0);
				VirtualProtect(reinterpret_cast<void*>(addr), 8, oldProtect, &oldProtect);
			}
			else {
				return { 400, "application/json", MakeError("Unknown type: " + type) };
			}

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

	// POST /api/v1/memory/pointer-chain — follow pointer chain
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
				// Dereference pointer
				uintptr_t deref = *reinterpret_cast<uintptr_t*>(addr);
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

			// Read value at final address
			json val;
			val["int32"] = *reinterpret_cast<int32*>(addr);
			val["float"] = *reinterpret_cast<float*>(addr);
			val["pointer"] = std::format("0x{:X}", *reinterpret_cast<uintptr_t*>(addr));
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