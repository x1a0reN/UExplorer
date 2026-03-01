#include "BlueprintApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"
#include "Blueprint/BlueprintDecompiler.h"
#include "Platform.h"

#include <format>
#include <sstream>
#include <iomanip>

namespace UExplorer::API
{

// Safe script read: validates pointer before copying
static bool SafeGetScript(const UEFunction& func, std::vector<uint8_t>& out)
{
	if (Off::UFunction::Script <= 0) return false;

	uint8_t* obj = reinterpret_cast<uint8_t*>(const_cast<void*>(func.GetAddress()));
	if (!obj) return false;

	const uint8_t* dataPtr = *reinterpret_cast<uint8_t* const*>(obj + Off::UFunction::Script);
	const int32 num = *reinterpret_cast<int32*>(obj + Off::UFunction::Script + sizeof(void*));

	if (!dataPtr || num <= 0 || num > 1024 * 1024) return false;
	if (Platform::IsBadReadPtr(dataPtr)) return false;
	if (Platform::IsBadReadPtr(dataPtr + num - 1)) return false;

	out.assign(dataPtr, dataPtr + num);
	return true;
}

void RegisterBlueprintRoutes(HttpServer& server)
{
	// GET /api/v1/blueprint/decompile?index=N — decompile a blueprint function
	server.Get("/api/v1/blueprint/decompile", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			if (!params.count("index"))
				return { 400, "application/json", MakeError("Missing 'index' param") };

			int32 idx = std::stoi(params["index"]);
			if (idx < 0 || idx >= ObjectArray::Num())
				return { 404, "application/json", MakeError("Index out of range") };

			UEObject obj = ObjectArray::GetByIndex(idx);
			if (!obj || !obj.IsA(EClassCastFlags::Function))
				return { 400, "application/json", MakeError("Object is not a UFunction") };

			UEFunction func = obj.Cast<UEFunction>();
			if (!func.HasScript())
				return { 400, "application/json", MakeError("Function has no script bytecode") };

			std::vector<uint8_t> scriptData;
			if (!SafeGetScript(func, scriptData))
				return { 400, "application/json", MakeError("Script data pointer is invalid") };

			auto result = BlueprintDecompiler::DecompileBytes(scriptData);

			json data;
			data["function"] = func.GetName();
			data["class"] = "";
			try {
				UEObject outer = ObjectArray::GetByIndex(idx);
				if (outer) {
					UEObject cls = outer.GetOuter();
					if (cls) data["class"] = cls.GetName();
				}
			} catch (...) {}
			data["flags"] = func.StringifyFlags();
			data["script_size"] = (int)scriptData.size();
			data["pseudocode"] = result;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Decompilation failed") };
		}
	});

	// GET /api/v1/blueprint/bytecode?index=N — raw bytecode hex dump
	server.Get("/api/v1/blueprint/bytecode", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			if (!params.count("index"))
				return { 400, "application/json", MakeError("Missing 'index' param") };

			int32 idx = std::stoi(params["index"]);
			if (idx < 0 || idx >= ObjectArray::Num())
				return { 404, "application/json", MakeError("Index out of range") };

			UEObject obj = ObjectArray::GetByIndex(idx);
			if (!obj || !obj.IsA(EClassCastFlags::Function))
				return { 400, "application/json", MakeError("Object is not a UFunction") };

			UEFunction func = obj.Cast<UEFunction>();
			if (!func.HasScript())
				return { 400, "application/json", MakeError("Function has no script bytecode") };

			std::vector<uint8_t> script;
			if (!SafeGetScript(func, script))
				return { 400, "application/json", MakeError("Script data pointer is invalid") };

			// Format as hex string
			std::ostringstream ss;
			for (size_t i = 0; i < script.size(); i++)
			{
				if (i > 0) ss << ' ';
				ss << std::hex << std::uppercase << std::setfill('0')
				   << std::setw(2) << (int)script[i];
			}

			json data;
			data["function"] = func.GetName();
			data["size"] = (int)script.size();
			data["hex"] = ss.str();
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read bytecode") };
		}
	});
}

} // namespace UExplorer::API
