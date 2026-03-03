#include "ClassesApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/Enums.h"
#include "Platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>

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

// Helper: read a single property value from an object as JSON
static PropertyReadResult ReadPropertyValue(uint8* objAddr, const UEProperty& prop)
{
	return ReadPropertyValueAt(objAddr + prop.GetOffset(), prop, 0);
}

// Helper: serialize a single property to JSON
static json SerializeProperty(const UEProperty& prop)
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

// Helper: serialize a single function to JSON
static json SerializeFunction(const UEFunction& func)
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
	j["params"] = params;
	return j;
}

void RegisterClassesRoutes(HttpServer& server)
{
	// GET /api/v1/classes — paginated class list
	server.Get("/api/v1/classes", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj || !obj.IsA(EClassCastFlags::Class)) continue;

			try {
				std::string name = obj.GetName();
				if (!filter.empty() && name.find(filter) == std::string::npos) continue;
				matched++;

				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue; // keep counting matched

				UEStruct cls = obj.Cast<UEStruct>();
				json item;
				item["index"] = i;
				item["name"] = name;
				item["full_name"] = obj.GetFullName();
				item["size"] = cls.GetStructSize();
				item["super"] = cls.GetSuper() ? cls.GetSuper().GetName() : "";
				item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
				items.push_back(std::move(item));
				count++;
			} catch (...) { continue; }
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/classes/:name — class full info
	server.Get("/api/v1/classes/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		if (name.empty())
			return { 400, "application/json", MakeError("Missing class name") };

		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json data;
			data["name"] = cls.GetName();
			data["full_name"] = cls.GetFullName();
			data["cpp_name"] = cls.GetCppName();
			data["size"] = cls.GetStructSize();
			data["alignment"] = cls.GetMinAlignment();
			data["index"] = cls.GetIndex();
			data["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(cls.GetAddress()));
			data["super"] = cls.GetSuper() ? cls.GetSuper().GetName() : "";

			// Fields
			json fields = json::array();
			for (const auto& prop : cls.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			data["fields"] = fields;

			// Functions
			json funcs = json::array();
			for (const auto& func : cls.GetFunctions())
			{
				try { funcs.push_back(SerializeFunction(func)); }
				catch (...) {}
			}
			data["functions"] = funcs;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to serialize class") };
		}
	});

	// GET /api/v1/classes/:name/fields
	server.Get("/api/v1/classes/:name/fields", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json fields = json::array();
			for (const auto& prop : cls.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			return { 200, "application/json", MakeResponse(fields) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get fields") };
		}
	});

	// GET /api/v1/classes/:name/functions
	server.Get("/api/v1/classes/:name/functions", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			json funcs = json::array();
			for (const auto& func : cls.GetFunctions())
			{
				try { funcs.push_back(SerializeFunction(func)); }
				catch (...) {}
			}
			return { 200, "application/json", MakeResponse(funcs) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get functions") };
		}
	});

	// GET /api/v1/classes/:name/hierarchy — inheritance chain
	server.Get("/api/v1/classes/:name/hierarchy", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			// Walk up the inheritance chain
			json parents = json::array();
			UEStruct cur = cls.GetSuper();
			while (cur)
			{
				parents.push_back(cur.GetName());
				cur = cur.GetSuper();
			}

			// Find direct subclasses
			json children = json::array();
			int32 total = ObjectArray::Num();
			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj || !obj.IsA(EClassCastFlags::Class)) continue;
				try {
					UEStruct s = obj.Cast<UEStruct>();
					if (s.GetSuper() && s.GetSuper().GetName() == name)
						children.push_back(obj.GetName());
				} catch (...) {}
			}

			json data;
			data["name"] = name;
			data["parents"] = parents;
			data["children"] = children;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to get hierarchy") };
		}
	});

	// GET /api/v1/structs — paginated struct list (non-class, non-function)
	server.Get("/api/v1/structs", [](const HttpRequest& req) -> HttpResponse {
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;
		std::string filter = params.count("q") ? params["q"] : "";

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			if (!obj.IsA(EClassCastFlags::Struct)) continue;
			if (obj.IsA(EClassCastFlags::Class) || obj.IsA(EClassCastFlags::Function)) continue;

			try {
				std::string name = obj.GetName();
				if (!filter.empty() && name.find(filter) == std::string::npos) continue;
				matched++;
				if (skipped < offset) { skipped++; continue; }
				if (count >= limit) continue;

				UEStruct s = obj.Cast<UEStruct>();
				json item;
				item["index"] = i;
				item["name"] = name;
				item["size"] = s.GetStructSize();
				item["super"] = s.GetSuper() ? s.GetSuper().GetName() : "";
				items.push_back(std::move(item));
				count++;
			} catch (...) { continue; }
		}

		json data;
		data["items"] = items;
		data["total"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/structs/:name — struct detail
	server.Get("/api/v1/structs/:name", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEStruct s = ObjectArray::FindStructFast(name);
		if (!s)
			return { 404, "application/json", MakeError("Struct not found: " + name) };

		try {
			json data;
			data["name"] = s.GetName();
			data["full_name"] = s.GetFullName();
			data["size"] = s.GetStructSize();
			data["alignment"] = s.GetMinAlignment();
			data["super"] = s.GetSuper() ? s.GetSuper().GetName() : "";

			json fields = json::array();
			for (const auto& prop : s.GetProperties())
			{
				try { fields.push_back(SerializeProperty(prop)); }
				catch (...) {}
			}
			data["fields"] = fields;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to serialize struct") };
		}
	});

	// GET /api/v1/classes/:name/instances — live instances of a class
	server.Get("/api/v1/classes/:name/instances", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		auto params = ParseQuery(req.Query);
		int offset = params.count("offset") ? std::stoi(params["offset"]) : 0;
		int limit = params.count("limit") ? std::stoi(params["limit"]) : 50;
		if (limit > 500) limit = 500;

		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		json items = json::array();
		int32 total = ObjectArray::Num();
		int matched = 0, count = 0, skipped = 0;

		for (int32 i = 0; i < total; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;

			// Check if object is an instance of this class
			try {
				UEObject objClass = obj.GetClass();
				if (!objClass) continue;
				if (objClass.GetName() != name && !objClass.IsA(cls))
					continue;
			} catch (...) { continue; }

			// Skip CDO (Class Default Object)
			try {
				UEObject outer = obj.GetOuter();
				if (outer && outer.GetName() == name)
					continue; // Skip CDO
			} catch (...) {}

			matched++;
			if (skipped < offset) { skipped++; continue; }
			if (count >= limit) continue;

			json item;
			item["index"] = i;
			item["name"] = obj.GetName();
			item["address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(obj.GetAddress()));
			items.push_back(std::move(item));
			count++;
		}

		json data;
		data["class"] = name;
		data["items"] = items;
		data["matched"] = matched;
		data["offset"] = offset;
		data["limit"] = limit;
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/classes/:name/cdo — Class Default Object properties
	server.Get("/api/v1/classes/:name/cdo", [](const HttpRequest& req) -> HttpResponse {
		std::string name = GetPathSegment(req.Path, 3);
		UEClass cls = ObjectArray::FindClassFast(name);
		if (!cls)
			return { 404, "application/json", MakeError("Class not found: " + name) };

		try {
			UEObject cdo = cls.GetDefaultObject();
			if (!cdo)
				return { 500, "application/json", MakeError("Failed to get CDO") };

			uint8* cdoAddr = reinterpret_cast<uint8*>(cdo.GetAddress());
			json props = json::array();
			int32 totalProps = 0;
			int32 unknownCount = 0;
			int32 emptyCount = 0;

			for (const auto& prop : cls.GetProperties())
			{
				try {
					totalProps++;
					json p;
					p["name"] = prop.GetName();
					p["type"] = prop.GetCppType();
					p["offset"] = prop.GetOffset();
					p["size"] = prop.GetSize();
					PropertyReadResult result = ReadPropertyValue(cdoAddr, prop);
					p["value"] = std::move(result.value);
					p["value_state"] = result.state;
					if (std::strcmp(result.state, "unknown") == 0)
						unknownCount++;
					else if (std::strcmp(result.state, "empty") == 0)
						emptyCount++;
					props.push_back(std::move(p));
				}
				catch (...) {}
			}

			const int32 safeTotal = (std::max)(1, totalProps);
			json data;
			data["class"] = name;
			data["cdo_address"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(cdoAddr));
			data["properties"] = props;
			data["stats"] = {
				{"total_properties", totalProps},
				{"unknown_count", unknownCount},
				{"empty_count", emptyCount},
				{"unknown_ratio_percent", (unknownCount * 100) / safeTotal},
				{"empty_ratio_percent", (emptyCount * 100) / safeTotal}
			};

			std::cerr << "[UExplorer] CDO property stats: class=" << name
				<< " total=" << totalProps
				<< " unknown=" << unknownCount
				<< " empty=" << emptyCount
				<< " unknownRatio=" << ((unknownCount * 100) / safeTotal) << "%"
				<< " emptyRatio=" << ((emptyCount * 100) / safeTotal) << "%"
				<< std::endl;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read CDO") };
		}
	});
}

} // namespace UExplorer::API
