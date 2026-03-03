#include "BlueprintApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"
#include "Blueprint/BlueprintDecompiler.h"
#include "Platform.h"

#include <cctype>
#include <cstring>
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

	const uint8_t* dataPtr = nullptr;
	int32 num = 0;
	int32 max = 0;

	// Guard raw field reads: malformed offsets/pointers must not crash the host process.
	__try
	{
		dataPtr = *reinterpret_cast<uint8_t* const*>(obj + Off::UFunction::Script);
		num = *reinterpret_cast<int32*>(obj + Off::UFunction::Script + sizeof(void*));
		max = *reinterpret_cast<int32*>(obj + Off::UFunction::Script + sizeof(void*) + sizeof(int32));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}

	if (!dataPtr || num <= 0 || num > 1024 * 1024) return false;
	if (max < num || max > 4 * 1024 * 1024) return false;
	if (Platform::IsBadReadPtr(dataPtr)) return false;

	out.resize(static_cast<size_t>(num));
	__try
	{
		std::memcpy(out.data(), dataPtr, static_cast<size_t>(num));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out.clear();
		return false;
	}

	return true;
}

static std::string UrlDecode(std::string s)
{
	std::string out;
	out.reserve(s.size());

	for (size_t i = 0; i < s.size(); i++)
	{
		char c = s[i];
		if (c == '+' )
		{
			out.push_back(' ');
			continue;
		}
		if (c == '%' && i + 2 < s.size())
		{
			char h1 = s[i + 1];
			char h2 = s[i + 2];
			auto hex = [](char ch) -> int {
				if (ch >= '0' && ch <= '9') return ch - '0';
				if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
				if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
				return -1;
			};
			int hi = hex(h1);
			int lo = hex(h2);
			if (hi >= 0 && lo >= 0)
			{
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(c);
	}

	return out;
}

static bool ResolveFunctionByIndex(int32 idx, UEFunction& outFunc, UEObject& outObj)
{
	if (idx < 0 || idx >= ObjectArray::Num())
		return false;

	UEObject obj = ObjectArray::GetByIndex(idx);
	if (!obj || !obj.IsA(EClassCastFlags::Function))
		return false;

	outFunc = obj.Cast<UEFunction>();
	outObj = obj;
	return static_cast<bool>(outFunc);
}

static bool ResolveFunctionByPath(const std::string& rawFuncPath, UEFunction& outFunc, UEObject& outObj)
{
	std::string funcPath = UrlDecode(rawFuncPath);
	if (funcPath.empty())
		return false;

	bool numeric = true;
	for (char ch : funcPath)
	{
		if (!std::isdigit(static_cast<unsigned char>(ch)))
		{
			numeric = false;
			break;
		}
	}
	if (numeric)
	{
		try {
			int32 idx = std::stoi(funcPath);
			if (ResolveFunctionByIndex(idx, outFunc, outObj))
				return true;
		}
		catch (...) {}
	}

	int32 total = ObjectArray::Num();
	for (int32 i = 0; i < total; i++)
	{
		UEObject obj = ObjectArray::GetByIndex(i);
		if (!obj || !obj.IsA(EClassCastFlags::Function))
			continue;

		std::string name;
		std::string full;
		std::string path;
		std::string outerName;
		try {
			name = obj.GetName();
			full = obj.GetFullName();
			path = obj.GetPathName();
			UEObject outer = obj.GetOuter();
			if (outer) outerName = outer.GetName();
		}
		catch (...) {
			continue;
		}

		const std::string classDotName = outerName.empty() ? name : (outerName + "." + name);
		const std::string classScopeName = outerName.empty() ? name : (outerName + "::" + name);

		if (funcPath == name || funcPath == full || funcPath == path
			|| funcPath == classDotName || funcPath == classScopeName)
		{
			outFunc = obj.Cast<UEFunction>();
			outObj = obj;
			return static_cast<bool>(outFunc);
		}
	}

	return false;
}

static std::string BuildClassNameFromFunctionObject(const UEObject& funcObj)
{
	try {
		UEObject outer = funcObj.GetOuter();
		if (outer) return outer.GetName();
	}
	catch (...) {}
	return "";
}

static HttpResponse HandleBlueprintDecompile(const UEFunction& func, const UEObject& funcObj)
{
	if (!func.HasScript())
		return { 400, "application/json", MakeError("Function has no script bytecode") };

	std::vector<uint8_t> scriptData;
	if (!SafeGetScript(func, scriptData))
		return { 400, "application/json", MakeError("Script data pointer is invalid") };

	auto result = BlueprintDecompiler::DecompileBytes(scriptData);

	json data;
	data["function"] = func.GetName();
	data["class"] = BuildClassNameFromFunctionObject(funcObj);
	data["flags"] = func.StringifyFlags();
	data["script_size"] = static_cast<int>(scriptData.size());
	data["pseudocode"] = result;
	return { 200, "application/json", MakeResponse(data) };
}

static HttpResponse HandleBlueprintBytecode(const UEFunction& func)
{
	if (!func.HasScript())
		return { 400, "application/json", MakeError("Function has no script bytecode") };

	std::vector<uint8_t> script;
	if (!SafeGetScript(func, script))
		return { 400, "application/json", MakeError("Script data pointer is invalid") };

	std::ostringstream ss;
	for (size_t i = 0; i < script.size(); i++)
	{
		if (i > 0) ss << ' ';
		ss << std::hex << std::uppercase << std::setfill('0')
		   << std::setw(2) << static_cast<int>(script[i]);
	}

	json data;
	data["function"] = func.GetName();
	data["size"] = static_cast<int>(script.size());
	data["hex"] = ss.str();
	return { 200, "application/json", MakeResponse(data) };
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
			UEFunction func;
			UEObject funcObj;
			if (!ResolveFunctionByIndex(idx, func, funcObj))
				return { 400, "application/json", MakeError("Object is not a UFunction") };

			return HandleBlueprintDecompile(func, funcObj);
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
			UEFunction func;
			UEObject funcObj;
			if (!ResolveFunctionByIndex(idx, func, funcObj))
				return { 400, "application/json", MakeError("Object is not a UFunction") };

			return HandleBlueprintBytecode(func);
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read bytecode") };
		}
	});

	// GET /api/v1/blueprint/:funcpath/decompile — decompile by function path
	server.Get("/api/v1/blueprint/:funcpath/decompile", [](const HttpRequest& req) -> HttpResponse {
		try {
			std::string funcPath = GetPathSegment(req.Path, 3);
			if (funcPath.empty())
				return { 400, "application/json", MakeError("Missing function path") };

			UEFunction func;
			UEObject funcObj;
			if (!ResolveFunctionByPath(funcPath, func, funcObj))
				return { 404, "application/json", MakeError("Function not found: " + UrlDecode(funcPath)) };

			return HandleBlueprintDecompile(func, funcObj);
		}
		catch (...) {
			return { 500, "application/json", MakeError("Decompilation failed") };
		}
	});

	// GET /api/v1/blueprint/:funcpath/bytecode — bytecode by function path
	server.Get("/api/v1/blueprint/:funcpath/bytecode", [](const HttpRequest& req) -> HttpResponse {
		try {
			std::string funcPath = GetPathSegment(req.Path, 3);
			if (funcPath.empty())
				return { 400, "application/json", MakeError("Missing function path") };

			UEFunction func;
			UEObject funcObj;
			if (!ResolveFunctionByPath(funcPath, func, funcObj))
				return { 404, "application/json", MakeError("Function not found: " + UrlDecode(funcPath)) };

			return HandleBlueprintBytecode(func);
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read bytecode") };
		}
	});
}

} // namespace UExplorer::API
