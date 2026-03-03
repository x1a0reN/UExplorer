#include "WinMemApi.h"

#include "ObjectsApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/UnrealContainers.h"
#include "Unreal/Enums.h"
#include "Platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

namespace UExplorer::API
{

struct PropertyReadResult
{
	json value;
	const char* state = "unknown"; // ok / empty / unknown
};

static PropertyReadResult MakeUnknown(const std::string& typeName, const std::string& reason = "")
{
	json v;
	v["kind"] = "unknown";
	v["type"] = typeName;
	if (!reason.empty())
		v["reason"] = reason;
	return { std::move(v), "unknown" };
}

static PropertyReadResult MakeEmpty(json value = nullptr)
{
	return { std::move(value), "empty" };
}

static PropertyReadResult MakeOk(json value)
{
	return { std::move(value), "ok" };
}

static json SerializeObjectRef(void* ptr)
{
	json out;
	out["kind"] = "object_ref";
	if (!ptr)
	{
		out["is_null"] = true;
		out["address"] = nullptr;
		out["name"] = nullptr;
		out["class"] = nullptr;
		out["index"] = nullptr;
		return out;
	}

	out["is_null"] = false;
	out["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(ptr));

	UEObject ref(ptr);
	if (!ref)
	{
		out["name"] = nullptr;
		out["class"] = nullptr;
		out["index"] = nullptr;
		return out;
	}

	try { out["name"] = ref.GetName(); } catch (...) { out["name"] = nullptr; }
	try
	{
		UEObject cls = ref.GetClass();
		out["class"] = cls ? json(cls.GetName()) : json(nullptr);
	}
	catch (...) { out["class"] = nullptr; }
	try { out["index"] = ref.GetIndex(); } catch (...) { out["index"] = nullptr; }
	return out;
}

static bool TryReadArrayHeader(uint8* addr, uintptr_t& outData, int32& outNum, int32& outMax)
{
	if (!addr)
		return false;

	__try
	{
		outData = *reinterpret_cast<uintptr_t*>(addr);
		outNum = *reinterpret_cast<int32*>(addr + sizeof(void*));
		outMax = *reinterpret_cast<int32*>(addr + sizeof(void*) + sizeof(int32));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}

	if (outNum < 0 || outMax < outNum || outMax > 0x400000)
		return false;

	if (outNum > 0)
	{
		if (outData == 0 || Platform::IsBadReadPtr(reinterpret_cast<void*>(outData)))
			return false;
	}

	return true;
}

static PropertyReadResult ReadPropertyValueAt(uint8* addr, const UEProperty& prop, int depth = 0)
{
	if (!addr)
		return MakeUnknown(prop.GetCppType(), "null_address");

	const EClassCastFlags type = prop.GetCastFlags();

	try
	{
		if (type & EClassCastFlags::BoolProperty)
		{
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			bool val = (*(addr + bp.GetByteOffset()) & bp.GetFieldMask()) != 0;
			return MakeOk(val);
		}
		if (type & EClassCastFlags::ByteProperty)
			return MakeOk(static_cast<int32>(*reinterpret_cast<uint8*>(addr)));
		if (type & EClassCastFlags::Int8Property)
			return MakeOk(static_cast<int32>(*reinterpret_cast<int8*>(addr)));
		if (type & EClassCastFlags::Int16Property)
			return MakeOk(static_cast<int32>(*reinterpret_cast<int16*>(addr)));
		if (type & EClassCastFlags::UInt16Property)
			return MakeOk(static_cast<uint32>(*reinterpret_cast<uint16*>(addr)));
		if (type & EClassCastFlags::IntProperty)
			return MakeOk(*reinterpret_cast<int32*>(addr));
		if (type & EClassCastFlags::UInt32Property)
			return MakeOk(*reinterpret_cast<uint32*>(addr));
		if (type & EClassCastFlags::Int64Property)
			return MakeOk(*reinterpret_cast<int64*>(addr));
		if (type & EClassCastFlags::UInt64Property)
			return MakeOk(*reinterpret_cast<uint64*>(addr));
		if (type & EClassCastFlags::FloatProperty)
		{
			const float v = *reinterpret_cast<float*>(addr);
			if (!std::isfinite(v))
				return MakeUnknown(prop.GetCppType(), "non_finite_float");
			return MakeOk(v);
		}
		if (type & EClassCastFlags::DoubleProperty)
		{
			const double v = *reinterpret_cast<double*>(addr);
			if (!std::isfinite(v))
				return MakeUnknown(prop.GetCppType(), "non_finite_double");
			return MakeOk(v);
		}
		if (type & EClassCastFlags::NameProperty)
		{
			FName* fn = reinterpret_cast<FName*>(addr);
			return MakeOk(fn->ToString());
		}
		if (type & EClassCastFlags::StrProperty)
		{
			UC::FString* fs = reinterpret_cast<UC::FString*>(addr);
			if (!fs->IsValid())
				return MakeEmpty("");
			return MakeOk(fs->ToString());
		}
		if (type & EClassCastFlags::TextProperty)
		{
			json out;
			out["kind"] = "text";
			out["value"] = "(FText unresolved)";
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::ClassProperty)
		{
			void* ptr = *reinterpret_cast<void**>(addr);
			json out = SerializeObjectRef(ptr);
			try
			{
				UEClassProperty cp = prop.Cast<UEClassProperty>();
				UEClass meta = cp.GetMetaClass();
				out["meta_class"] = meta ? json(meta.GetName()) : json(nullptr);
			}
			catch (...) {}
			return ptr ? MakeOk(std::move(out)) : MakeEmpty(std::move(out));
		}

		if ((type & EClassCastFlags::ObjectProperty) || (type & EClassCastFlags::ObjectPropertyBase)
			|| (type & EClassCastFlags::InterfaceProperty))
		{
			void* ptr = *reinterpret_cast<void**>(addr);
			json out = SerializeObjectRef(ptr);
			return ptr ? MakeOk(std::move(out)) : MakeEmpty(std::move(out));
		}

		if ((type & EClassCastFlags::WeakObjectProperty) || (type & EClassCastFlags::LazyObjectProperty))
		{
			json out;
			out["kind"] = "weak_object_ref";
			out["object_index"] = *reinterpret_cast<int32*>(addr);
			out["serial_number"] = *reinterpret_cast<int32*>(addr + sizeof(int32));
			if (out["object_index"].get<int32>() <= 0)
				return MakeEmpty(std::move(out));
			return MakeOk(std::move(out));
		}

		if ((type & EClassCastFlags::SoftObjectProperty) || (type & EClassCastFlags::SoftClassProperty))
		{
			json out;
			out["kind"] = "soft_object_ref";
			out["note"] = "soft reference parsing is partial";
			out["object_index_hint"] = *reinterpret_cast<int32*>(addr);
			out["serial_hint"] = *reinterpret_cast<int32*>(addr + sizeof(int32));
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::EnumProperty)
		{
			UEEnumProperty ep = prop.Cast<UEEnumProperty>();
			UEProperty under = ep.GetUnderlayingProperty();
			PropertyReadResult raw = under ? ReadPropertyValueAt(addr, under, depth + 1)
				: MakeUnknown(prop.GetCppType(), "enum_underlying_missing");

			json out;
			out["kind"] = "enum";
			out["underlying_type"] = under ? under.GetCppType() : "Unknown";
			out["raw"] = raw.value;
			try
			{
				UEEnum en = ep.GetEnum();
				if (en)
				{
					out["enum_name"] = en.GetName();
					if (raw.value.is_number_integer() || raw.value.is_number_unsigned())
					{
						const int64 rawVal = raw.value.get<int64>();
						for (const auto& [n, v] : en.GetNameValuePairs())
						{
							if (v == rawVal)
							{
								out["value_name"] = n.ToString();
								break;
							}
						}
					}
				}
			}
			catch (...) {}
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::ArrayProperty)
		{
			UEArrayProperty ap = prop.Cast<UEArrayProperty>();
			UEProperty inner = ap.GetInnerProperty();

			uintptr_t data = 0;
			int32 num = 0;
			int32 max = 0;
			if (!TryReadArrayHeader(addr, data, num, max))
				return MakeUnknown(prop.GetCppType(), "invalid_array_header");

			json out;
			out["kind"] = "array";
			out["num"] = num;
			out["max"] = max;
			out["data_ptr"] = data ? json(std::format("0x{:X}", data)) : json(nullptr);
			out["inner_type"] = inner ? inner.GetCppType() : "Unknown";

			if (num == 0)
			{
				out["preview"] = json::array();
				return MakeEmpty(std::move(out));
			}

			if (!inner)
				return MakeUnknown(prop.GetCppType(), "array_inner_missing");

			const int32 elemSize = inner.GetSize();
			if (elemSize <= 0 || elemSize > 0x4000)
				return MakeUnknown(prop.GetCppType(), "invalid_array_element_size");

			const int32 previewCount = (std::min)(num, 8);
			json preview = json::array();
			int32 unknownCount = 0;
			for (int32 i = 0; i < previewCount; i++)
			{
				uint8* elemAddr = reinterpret_cast<uint8*>(data + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(elemSize));
				PropertyReadResult elem = ReadPropertyValueAt(elemAddr, inner, depth + 1);
				preview.push_back(elem.value);
				if (std::strcmp(elem.state, "unknown") == 0)
					unknownCount++;
			}
			out["preview"] = std::move(preview);
			if (num > previewCount)
				out["truncated"] = true;

			if (unknownCount == previewCount)
				return MakeUnknown(prop.GetCppType(), "array_preview_unresolved");
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::StructProperty)
		{
			UEStructProperty sp = prop.Cast<UEStructProperty>();
			UEStruct st = sp.GetUnderlayingStruct();

			json out;
			out["kind"] = "struct";
			out["struct_type"] = st ? json(st.GetName()) : json("Unknown");
			out["size"] = prop.GetSize();

			if (!st)
				return MakeUnknown(prop.GetCppType(), "struct_type_missing");

			if (depth >= 2)
				return MakeOk(std::move(out));

			json fields = json::array();
			int32 unknownCount = 0;
			int32 totalCount = 0;
			for (const auto& member : st.GetProperties())
			{
				if (totalCount >= 16)
				{
					out["truncated"] = true;
					break;
				}

				totalCount++;
				json field;
				field["name"] = member.GetName();
				field["type"] = member.GetCppType();
				field["offset"] = member.GetOffset();
				field["size"] = member.GetSize();

				PropertyReadResult fv = ReadPropertyValueAt(addr + member.GetOffset(), member, depth + 1);
				field["value"] = std::move(fv.value);
				field["value_state"] = fv.state;
				if (std::strcmp(fv.state, "unknown") == 0)
					unknownCount++;
				fields.push_back(std::move(field));
			}

			out["fields"] = std::move(fields);
			if (totalCount == 0)
				return MakeEmpty(std::move(out));
			if (unknownCount == totalCount)
				return MakeUnknown(prop.GetCppType(), "struct_fields_unresolved");
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::MapProperty)
		{
			UEMapProperty mp = prop.Cast<UEMapProperty>();
			json out;
			out["kind"] = "map";
			out["key_type"] = mp.GetKeyProperty() ? mp.GetKeyProperty().GetCppType() : "Unknown";
			out["value_type"] = mp.GetValueProperty() ? mp.GetValueProperty().GetCppType() : "Unknown";
			out["note"] = "map serialization preview not implemented";
			return MakeOk(std::move(out));
		}

		if (type & EClassCastFlags::SetProperty)
		{
			UESetProperty sp = prop.Cast<UESetProperty>();
			json out;
			out["kind"] = "set";
			out["element_type"] = sp.GetElementProperty() ? sp.GetElementProperty().GetCppType() : "Unknown";
			out["note"] = "set serialization preview not implemented";
			return MakeOk(std::move(out));
		}

		if ((type & EClassCastFlags::DelegateProperty)
			|| (type & EClassCastFlags::MulticastDelegateProperty)
			|| (type & EClassCastFlags::MulticastInlineDelegateProperty))
		{
			json out;
			out["kind"] = "delegate";
			out["note"] = "delegate value preview not implemented";
			return MakeOk(std::move(out));
		}
	}
	catch (...)
	{
		return MakeUnknown(prop.GetCppType(), "exception");
	}

	return MakeUnknown(prop.GetCppType(), "unsupported_property_type");
}

// Helper: read a single property value from a live object as JSON
static PropertyReadResult ReadPropertyValue(uint8* objAddr, const UEProperty& prop)
{
	return ReadPropertyValueAt(objAddr + prop.GetOffset(), prop, 0);
}

json ReadPropertyValueUnified(uint8* objAddr, const UEProperty& prop, std::string& outState)
{
	PropertyReadResult result = ReadPropertyValue(objAddr, prop);
	outState = result.state ? result.state : "unknown";
	return std::move(result.value);
}

json SerializePropertyUnified(const UEProperty& prop)
{
	json j;
	j["name"] = prop.GetName();
	j["type"] = prop.GetCppType();
	j["offset"] = prop.GetOffset();
	j["size"] = prop.GetSize();
	j["array_dim"] = prop.GetArrayDim();
	j["flags"] = prop.StringifyFlags();
	return j;
}

json SerializeFunctionUnified(const UEFunction& func)
{
	json j;
	j["name"] = func.GetName();
	j["full_name"] = func.GetFullName();
	j["flags"] = func.StringifyFlags();
	j["param_size"] = func.GetStructSize();
	j["has_script"] = func.HasScript();
	j["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(func.GetExecFunction()));

	json params = json::array();
	for (const auto& prop : func.GetProperties())
	{
		json p;
		p["name"] = prop.GetName();
		p["type"] = prop.GetCppType();
		p["offset"] = prop.GetOffset();
		p["size"] = prop.GetSize();
		p["flags"] = prop.StringifyFlags();
		params.push_back(std::move(p));
	}
	j["params"] = std::move(params);
	return j;
}

// Helper: write a property value from JSON
static bool WritePropertyValue(uint8* objAddr, const UEProperty& prop, const json& value)
{
	uint8* addr = objAddr + prop.GetOffset();
	EClassCastFlags type = prop.GetCastFlags();

	DWORD oldProtect = 0;
	VirtualProtect(addr, prop.GetSize(), PAGE_EXECUTE_READWRITE, &oldProtect);

	bool ok = true;
	try {
		if (type & EClassCastFlags::BoolProperty)
		{
			UEBoolProperty bp = prop.Cast<UEBoolProperty>();
			uint8* byteAddr = addr + bp.GetByteOffset();
			if (value.get<bool>())
				*byteAddr |= bp.GetFieldMask();
			else
				*byteAddr &= ~bp.GetFieldMask();
		}
		else if (type & EClassCastFlags::ByteProperty)
			*reinterpret_cast<uint8*>(addr) = static_cast<uint8>(value.get<int>());
		else if (type & EClassCastFlags::IntProperty)
			*reinterpret_cast<int32*>(addr) = value.get<int32>();
		else if (type & EClassCastFlags::Int64Property)
			*reinterpret_cast<int64*>(addr) = value.get<int64>();
		else if (type & EClassCastFlags::FloatProperty)
			*reinterpret_cast<float*>(addr) = value.get<float>();
		else if (type & EClassCastFlags::DoubleProperty)
			*reinterpret_cast<double*>(addr) = value.get<double>();
		else
			ok = false;
	}
	catch (...) {
		ok = false;
	}

	VirtualProtect(addr, prop.GetSize(), oldProtect, &oldProtect);
	return ok;
}

static bool TryParseObjectIndex(const std::string& idxStr, int32& outIdx)
{
	if (idxStr.empty())
		return false;
	if (!std::all_of(idxStr.begin(), idxStr.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
		return false;

	try {
		outIdx = std::stoi(idxStr);
	}
	catch (...) {
		return false;
	}

	return outIdx >= 0 && outIdx < ObjectArray::Num();
}

static bool TryFindProperty(const UEClass& cls, const std::string& propName, UEProperty& outProp)
{
	for (const auto& prop : cls.GetProperties())
	{
		if (prop.GetName() == propName)
		{
			outProp = prop;
			return true;
		}
	}
	return false;
}

static json BuildOuterChain(UEObject obj)
{
	json outers = json::array();
	for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
	{
		json item;
		item["name"] = outer.GetName();
		item["full_name"] = outer.GetFullName();
		item["index"] = outer.GetIndex();
		item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(outer.GetAddress()));
		outers.push_back(std::move(item));
	}
	return outers;
}

void RegisterObjectsRoutes(HttpServer& server)
{
	// GET /api/v1/objects/count
	server.Get("/api/v1/objects/count", [](const HttpRequest&) -> HttpResponse {
		int32 total = ObjectArray::Num();
		int classes = 0, structs = 0, enums = 0, functions = 0, packages = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;

			if (obj.IsA(EClassCastFlags::Class)) classes++;
			else if (obj.IsA(EClassCastFlags::Function)) functions++;
			else if (obj.IsA(EClassCastFlags::Struct)) structs++;
			else if (obj.IsA(EClassCastFlags::Enum)) enums++;

			if (obj.IsA(EClassCastFlags::Package)) packages++;
		}

		json data;
		data["total"] = total;
		data["classes"] = classes;
		data["structs"] = structs;
		data["enums"] = enums;
		data["functions"] = functions;
		data["packages"] = packages;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects — paginated object list
	server.Get("/api/v1/objects", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = 0;
		int limit = 50;
		if (params.count("offset")) offset = std::stoi(params["offset"]);
		if (params.count("limit")) limit = std::stoi(params["limit"]);
		if (limit > 500) limit = 500;
		if (offset < 0) offset = 0;

		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int count = 0;
		int skipped = 0;

		for (int32 i = 0; i < total && count < limit; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;

			std::string name;
			try { name = obj.GetName(); }
			catch (...) { continue; }

			if (!filter.empty() && name.find(filter) == std::string::npos)
				continue;

			if (skipped < offset) { skipped++; continue; }

			json item;
			item["index"] = i;
			item["name"] = name;
			try {
				UEObject cls = obj.GetClass();
				item["class"] = cls ? cls.GetName() : "Unknown";
			}
			catch (...) {
				item["class"] = "Unknown";
			}
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
			count++;
		}

		json data;
		data["items"] = items;
		data["total"] = total;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects/search — multi-condition search
	server.Get("/api/v1/objects/search", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			std::string nameFilter = params.count("q") ? params["q"] : "";
			std::string classFilter = params.count("class") ? params["class"] : "";
			std::string packageFilter = params.count("package") ? params["package"] : "";
			int offset = 0;
			int limit = 50;
			if (params.count("offset")) offset = std::stoi(params["offset"]);
			if (params.count("limit")) limit = std::stoi(params["limit"]);
			if (limit > 500) limit = 500;
			if (offset < 0) offset = 0;

			json items = json::array();
			int32 total = ObjectArray::Num();
			int matched = 0;
			int count = 0;
			int skipped = 0;

			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;

				std::string name;
				try { name = obj.GetName(); }
				catch (...) { continue; }

				if (!nameFilter.empty() && name.find(nameFilter) == std::string::npos)
					continue;

				std::string className;
				try {
					UEObject cls = obj.GetClass();
					className = cls ? cls.GetName() : "Unknown";
				}
				catch (...) {
					className = "Unknown";
				}

				if (!classFilter.empty() && className.find(classFilter) == std::string::npos)
					continue;

				if (!packageFilter.empty())
				{
					bool found = false;
					for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
					{
						std::string outerName;
						try { outerName = outer.GetName(); }
						catch (...) { break; }
						if (outerName == packageFilter || outerName.find(packageFilter) != std::string::npos)
						{
							found = true;
							break;
						}
					}
					if (!found && name != packageFilter)
						continue;
				}

				matched++;
				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue;

				json item;
				item["index"] = i;
				item["name"] = name;
				item["class"] = className;
				item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				items.push_back(std::move(item));
				count++;
			}

			json data;
			data["items"] = items;
			data["matched"] = matched;
			data["offset"] = offset;
			data["limit"] = limit;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const std::exception& e) {
			return { 500, "application/json", MakeError(std::string("Search error: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Search error: unknown") };
		}
	});

	// GET /api/v1/objects/by-address/:addr — find object by address
	server.Get("/api/v1/objects/by-address/:addr", [](const HttpRequest& req) -> HttpResponse {
		std::string addrStr = GetPathSegment(req.Path, 4);
		if (addrStr.empty())
			return { 400, "application/json", MakeError("Missing address") };

		uintptr_t targetAddr = 0;
		try {
			targetAddr = std::stoull(addrStr, nullptr, 16);
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid address") };
		}
		if (!targetAddr)
			return { 400, "application/json", MakeError("Invalid address") };

		int32 total = ObjectArray::Num();
		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			if (reinterpret_cast<uintptr_t>(obj.GetAddress()) != targetAddr) continue;

			json data;
			data["index"] = i;
			data["name"] = obj.GetName();
			data["full_name"] = obj.GetFullName();
			data["address"] = std::format("0x{:X}", targetAddr);
			data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			return { 200, "application/json", MakeResponse(data) };
		}

		return { 404, "application/json", MakeError("No object found at address " + addrStr) };
	});

	// GET /api/v1/objects/by-path/:path — find object by full path
	server.Get("/api/v1/objects/by-path/:path", [](const HttpRequest& req) -> HttpResponse {
		std::string targetPath = GetPathSegment(req.Path, 4);
		if (targetPath.empty())
			return { 400, "application/json", MakeError("Missing path") };

		int32 total = ObjectArray::Num();
		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			try {
				if (obj.GetFullName() != targetPath && obj.GetName() != targetPath)
					continue;

				json data;
				data["index"] = i;
				data["name"] = obj.GetName();
				data["full_name"] = obj.GetFullName();
				data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
				return { 200, "application/json", MakeResponse(data) };
			}
			catch (...) {}
		}

		return { 404, "application/json", MakeError("No object found with path " + targetPath) };
	});

	// GET /api/v1/objects/:index/outer-chain — full outer chain
	server.Get("/api/v1/objects/:index/outer-chain", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		json data;
		data["index"] = idx;
		data["name"] = obj.GetName();
		data["outer_chain"] = BuildOuterChain(obj);
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/objects/:index/properties — read live property values
	server.Get("/api/v1/objects/:index/properties", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			json props = json::array();
			for (const auto& prop : cls.GetProperties())
			{
				try {
					json p;
					p["name"] = prop.GetName();
					p["type"] = prop.GetCppType();
					p["offset"] = prop.GetOffset();
					p["size"] = prop.GetSize();
					PropertyReadResult result = ReadPropertyValue(objAddr, prop);
					p["value"] = std::move(result.value);
					p["value_state"] = result.state;
					props.push_back(std::move(p));
				}
				catch (...) {}
			}

			return { 200, "application/json", MakeResponse(props) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read properties") };
		}
	});

	// GET /api/v1/objects/:index/property/:name — read a single property value
	server.Get("/api/v1/objects/:index/property/:name", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		std::string propName = GetPathSegment(req.Path, 5);
		if (propName.empty())
			return { 400, "application/json", MakeError("Missing property name") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			UEProperty prop;
			if (!TryFindProperty(cls, propName, prop))
				return { 404, "application/json", MakeError("Property not found: " + propName) };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			json data;
			data["object_index"] = idx;
			data["property"] = prop.GetName();
			data["type"] = prop.GetCppType();
			data["offset"] = prop.GetOffset();
			data["size"] = prop.GetSize();
			PropertyReadResult result = ReadPropertyValue(objAddr, prop);
			data["value"] = std::move(result.value);
			data["value_state"] = result.state;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read property") };
		}
	});

	// POST /api/v1/objects/:index/property/:name — write a single property value
	server.Post("/api/v1/objects/:index/property/:name", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 400, "application/json", MakeError("Invalid object index") };

		std::string propName = GetPathSegment(req.Path, 5);
		if (propName.empty())
			return { 400, "application/json", MakeError("Missing property name") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			json body = json::parse(req.Body);
			if (!body.contains("value"))
				return { 400, "application/json", MakeError("Missing 'value' field") };

			UEClass cls = obj.GetClass();
			if (!cls)
				return { 500, "application/json", MakeError("Object has no class") };

			UEProperty prop;
			if (!TryFindProperty(cls, propName, prop))
				return { 404, "application/json", MakeError("Property not found: " + propName) };

			uint8* objAddr = reinterpret_cast<uint8*>(obj.GetAddress());
			if (!WritePropertyValue(objAddr, prop, body["value"]))
				return { 400, "application/json", MakeError("Unsupported property type for writing") };

			json data;
			data["property"] = propName;
			data["written"] = true;
			PropertyReadResult result = ReadPropertyValue(objAddr, prop);
			data["new_value"] = std::move(result.value);
			data["new_value_state"] = result.state;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Property write failed") };
		}
	});

	// GET /api/v1/objects/:index — single object detail
	server.Get("/api/v1/objects/:index", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseObjectIndex(GetPathSegment(req.Path, 3), idx))
			return { 404, "application/json", MakeError("Not found") };

		UEObject obj = ObjectArray::GetByIndex(idx);
		if (!obj)
			return { 404, "application/json", MakeError("Object is null") };

		try {
			json data;
			data["index"] = idx;
			data["name"] = obj.GetName();
			data["full_name"] = obj.GetFullName();
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			data["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			data["outer_chain"] = BuildOuterChain(obj);
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read object") };
		}
	});

	// GET /api/v1/packages — list all packages
	server.Get("/api/v1/packages", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		if (offset < 0) offset = 0;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0;
		int count = 0;
		int skipped = 0;
		std::set<std::string> seen;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Package)) continue;

			std::string name;
			try { name = obj.GetName(); }
			catch (...) { continue; }

			if (!filter.empty() && name.find(filter) == std::string::npos)
				continue;
			if (seen.count(name))
				continue;
			seen.insert(name);

			matched++;
			if (skipped < offset) { skipped++; continue; }
			if (count >= limit) continue;

			json item;
			item["name"] = name;
			item["index"] = i;
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
			count++;
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/packages/:name/contents — list all types in a package
	server.Get("/api/v1/packages/:name/contents", [](const HttpRequest& req) -> HttpResponse {
		std::string packageName = GetPathSegment(req.Path, 3);
		if (packageName.empty())
			return { 400, "application/json", MakeError("Missing package name") };

		json items = json::array();
		int32 total = ObjectArray::Num();

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			std::string objName;
			try { objName = obj.GetName(); }
			catch (...) { continue; }

			bool belongs = false;
			for (UEObject outer = obj.GetOuter(); outer; outer = outer.GetOuter())
			{
				std::string outerName;
				try { outerName = outer.GetName(); }
				catch (...) { break; }
				if (outerName == packageName)
				{
					belongs = true;
					break;
				}
			}

			if (!belongs && objName != packageName)
				continue;
			if (obj.IsA(EClassCastFlags::Package))
				continue;

			json item;
			item["index"] = i;
			item["name"] = objName;
			item["class"] = obj.GetClass() ? obj.GetClass().GetName() : "Unknown";
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
		}

		json data;
		data["package"] = packageName;
		data["items"] = items;
		data["count"] = items.size();
		return { 200, "application/json", MakeResponse(data) };
	});
}

} // namespace UExplorer::API
