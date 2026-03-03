#include "WinMemApi.h"

#include "WorldApi.h"
#include "ApiCommon.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealContainers.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "Settings.h"

#include <cmath>
#include <format>
#include <set>
#include <vector>

namespace UExplorer::API
{

// Helper: get GWorld pointer
static void* GetGWorld()
{
	if (Off::InSDK::World::GWorld == 0) return nullptr;
	uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
	void** worldPtr = reinterpret_cast<void**>(base + Off::InSDK::World::GWorld);
	return *worldPtr;
}

static bool TryParseIndex(const std::string& raw, int32& out)
{
	try {
		out = std::stoi(raw);
	}
	catch (...) {
		return false;
	}
	return out >= 0 && out < ObjectArray::Num();
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

static bool TryReadPtrField(const void* base, int32 offset, void*& outPtr)
{
	outPtr = nullptr;
	if (!base || offset < 0)
		return false;

	const uint8* addr = reinterpret_cast<const uint8*>(base) + offset;
	if (Platform::IsBadReadPtr(addr))
		return false;

	__try
	{
		outPtr = *reinterpret_cast<void* const*>(addr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		outPtr = nullptr;
		return false;
	}
}

static bool TryReadInt32Field(const void* base, int32 offset, int32& outValue)
{
	outValue = 0;
	if (!base || offset < 0)
		return false;

	const uint8* addr = reinterpret_cast<const uint8*>(base) + offset;
	if (Platform::IsBadReadPtr(addr))
		return false;

	__try
	{
		outValue = *reinterpret_cast<const int32*>(addr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		outValue = 0;
		return false;
	}
}

static bool IsReadableObjectAddress(const void* objAddr)
{
	return objAddr
		&& !Platform::IsBadReadPtr(objAddr)
		&& Platform::IsAddressInProcessRange(objAddr);
}

static bool TryReadObjectNameByAddress(const void* objAddr, std::string& outName)
{
	outName.clear();
	if (!IsReadableObjectAddress(objAddr) || Off::UObject::Name < 0)
		return false;

	const uint8* fnameAddr = reinterpret_cast<const uint8*>(objAddr) + Off::UObject::Name;
	if (Platform::IsBadReadPtr(fnameAddr))
		return false;

	int32 compIdx = -1;
	if (!TryReadInt32Field(fnameAddr, Off::FName::CompIdx, compIdx))
		return false;
	if (compIdx < 0)
		return false;

	std::string name = NameArray::GetNameEntry(compIdx).GetString();
	if (name.empty() && compIdx != 0)
		return false;

	int32 rawNum = 0;
	if (TryReadInt32Field(fnameAddr, Off::FName::Number, rawNum))
	{
		const uint32 number = static_cast<uint32>(rawNum);
		if (!Settings::Internal::bUseOutlineNumberName && number > 0)
			name += "_" + std::to_string(number - 1);
	}

	outName = std::move(name);
	return true;
}

static bool TryReadObjectClassAddress(const void* objAddr, void*& outClassAddr)
{
	outClassAddr = nullptr;
	if (!IsReadableObjectAddress(objAddr) || Off::UObject::Class < 0)
		return false;

	if (!TryReadPtrField(objAddr, Off::UObject::Class, outClassAddr))
		return false;

	return IsReadableObjectAddress(outClassAddr);
}

static std::string GetSafeClassNameByAddress(const void* objAddr)
{
	void* classAddr = nullptr;
	if (!TryReadObjectClassAddress(objAddr, classAddr))
		return "Unknown";

	std::string className;
	if (!TryReadObjectNameByAddress(classAddr, className) || className.empty())
		return "Unknown";
	return className;
}

static std::string BuildSafeFullNameByAddress(const void* objAddr, const std::string& className, const std::string& objName)
{
	if (objName.empty())
		return className.empty() ? std::string("Unknown") : className;

	std::vector<std::string> pathParts;
	pathParts.push_back(objName);

	const void* cursor = objAddr;
	for (int i = 0; i < 32; i++)
	{
		if (!cursor || Off::UObject::Outer < 0)
			break;

		void* outerAddr = nullptr;
		if (!TryReadPtrField(cursor, Off::UObject::Outer, outerAddr))
			break;
		if (!outerAddr || outerAddr == cursor || !IsReadableObjectAddress(outerAddr))
			break;

		std::string outerName;
		if (!TryReadObjectNameByAddress(outerAddr, outerName) || outerName.empty())
			break;

		pathParts.push_back(std::move(outerName));
		cursor = outerAddr;
	}

	std::string path;
	for (auto it = pathParts.rbegin(); it != pathParts.rend(); ++it)
	{
		if (!path.empty())
			path += ".";
		path += *it;
	}

	if (!className.empty() && className != "Unknown")
		return className + " " + path;
	return path;
}

static json MakeObjectBrief(UEObject obj)
{
	json out;
	const void* objAddr = obj ? obj.GetAddress() : nullptr;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(objAddr);
	out["address"] = std::format("0x{:X}", addr);

	int32 index = -1;
	if (IsReadableObjectAddress(objAddr))
	{
		int32 tmp = 0;
		if (TryReadInt32Field(objAddr, Off::UObject::Index, tmp))
			index = tmp;
	}
	out["index"] = index;

	std::string name;
	if (!TryReadObjectNameByAddress(objAddr, name) || name.empty())
		name = "<invalid>";
	out["name"] = name;

	const std::string className = GetSafeClassNameByAddress(objAddr);
	out["class"] = className;
	out["full_name"] = BuildSafeFullNameByAddress(objAddr, className, name == "<invalid>" ? std::string() : name);

	return out;
}

static int GetLevelActorCount(UEObject level)
{
	if (!level || Off::InSDK::ULevel::Actors <= 0)
		return -1;

	try {
		uint8* levelAddr = reinterpret_cast<uint8*>(level.GetAddress());
		UC::TArray<void*>* actors = reinterpret_cast<UC::TArray<void*>*>(levelAddr + Off::InSDK::ULevel::Actors);
		if (!actors)
			return -1;
		int32 n = actors->Num();
		if (n < 0 || n > 500000)
			return -1;
		return n;
	}
	catch (...) {
		return -1;
	}
}

static UEObject GetActorRootComponent(UEObject actor)
{
	if (!actor || !actor.IsA(EClassCastFlags::Actor))
		return nullptr;

	UEClass cls = actor.GetClass();
	if (!cls)
		return nullptr;

	UEProperty prop;
	if (!TryFindProperty(cls, "RootComponent", prop))
		return nullptr;
	if (!(prop.GetCastFlags() & EClassCastFlags::ObjectProperty))
		return nullptr;

	uint8* actorAddr = reinterpret_cast<uint8*>(actor.GetAddress());
	void* ptr = *reinterpret_cast<void**>(actorAddr + prop.GetOffset());
	if (!ptr)
		return nullptr;

	return UEObject(ptr);
}

static bool ReadVec3Property(UEObject obj, const std::string& propName, json& out)
{
	UEClass cls = obj.GetClass();
	if (!cls) return false;

	UEProperty prop;
	if (!TryFindProperty(cls, propName, prop))
		return false;
	if (!(prop.GetCastFlags() & EClassCastFlags::StructProperty))
		return false;

	uint8* addr = reinterpret_cast<uint8*>(obj.GetAddress()) + prop.GetOffset();
	const bool useDouble = Settings::Internal::bUseLargeWorldCoordinates || prop.GetSize() >= 24;

	if (useDouble)
	{
		double* v = reinterpret_cast<double*>(addr);
		out["x"] = v[0];
		out["y"] = v[1];
		out["z"] = v[2];
	}
	else
	{
		float* v = reinterpret_cast<float*>(addr);
		out["x"] = v[0];
		out["y"] = v[1];
		out["z"] = v[2];
	}

	return true;
}

static bool ParseVec3(const json& input, double& x, double& y, double& z)
{
	try {
		if (input.is_array() && input.size() >= 3)
		{
			x = input[0].get<double>();
			y = input[1].get<double>();
			z = input[2].get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
				return false;
			return true;
		}
		if (input.is_object())
		{
			if (!input.contains("x") || !input.contains("y") || !input.contains("z"))
				return false;
			x = input.at("x").get<double>();
			y = input.at("y").get<double>();
			z = input.at("z").get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
				return false;
			return true;
		}
	}
	catch (...) {}
	return false;
}

static bool WriteVec3Property(UEObject obj, const std::string& propName, const json& input)
{
	UEClass cls = obj.GetClass();
	if (!cls) return false;

	UEProperty prop;
	if (!TryFindProperty(cls, propName, prop))
		return false;
	if (!(prop.GetCastFlags() & EClassCastFlags::StructProperty))
		return false;

	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	if (!ParseVec3(input, x, y, z))
		return false;

	uint8* addr = reinterpret_cast<uint8*>(obj.GetAddress()) + prop.GetOffset();
	DWORD oldProtect = 0;
	VirtualProtect(addr, prop.GetSize(), PAGE_EXECUTE_READWRITE, &oldProtect);

	const bool useDouble = Settings::Internal::bUseLargeWorldCoordinates || prop.GetSize() >= 24;
	if (useDouble)
	{
		double* v = reinterpret_cast<double*>(addr);
		v[0] = x;
		v[1] = y;
		v[2] = z;
	}
	else
	{
		float* v = reinterpret_cast<float*>(addr);
		v[0] = static_cast<float>(x);
		v[1] = static_cast<float>(y);
		v[2] = static_cast<float>(z);
	}

	VirtualProtect(addr, prop.GetSize(), oldProtect, &oldProtect);
	return true;
}

static void AddComponentIfValid(std::set<uintptr_t>& seen, json& out, void* ptr)
{
	if (!ptr) return;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	if (seen.count(addr)) return;

	UEObject comp(ptr);
	if (!comp) return;

	std::string clsName = "Unknown";
	try {
		UEObject cls = comp.GetClass();
		if (cls) clsName = cls.GetName();
	}
	catch (...) {}

	if (clsName.find("Component") == std::string::npos)
		return;

	seen.insert(addr);
	out.push_back(MakeObjectBrief(comp));
}

static json CollectActorComponents(UEObject actor)
{
	json components = json::array();
	std::set<uintptr_t> seen;

	UEObject root = GetActorRootComponent(actor);
	if (root)
		AddComponentIfValid(seen, components, root.GetAddress());

	UEClass cls = actor.GetClass();
	if (!cls)
		return components;

	uint8* actorAddr = reinterpret_cast<uint8*>(actor.GetAddress());
	for (const auto& prop : cls.GetProperties())
	{
		const std::string name = prop.GetName();
		const EClassCastFlags castFlags = prop.GetCastFlags();

		if ((castFlags & EClassCastFlags::ObjectProperty) && name.find("Component") != std::string::npos)
		{
			void* ptr = *reinterpret_cast<void**>(actorAddr + prop.GetOffset());
			AddComponentIfValid(seen, components, ptr);
			continue;
		}

		if ((castFlags & EClassCastFlags::ArrayProperty) && name.find("Component") != std::string::npos)
		{
			UC::TArray<void*>* arr = reinterpret_cast<UC::TArray<void*>*>(actorAddr + prop.GetOffset());
			if (!arr) continue;
			const int32 num = arr->Num();
			if (num < 0 || num > 4096) continue;

			for (int32 i = 0; i < num; i++)
			{
				void* ptr = nullptr;
				try { ptr = (*arr)[i]; }
				catch (...) { break; }
				AddComponentIfValid(seen, components, ptr);
			}
		}
	}

	return components;
}

void RegisterWorldRoutes(HttpServer& server)
{
	// GET /api/v1/world — current world info
	server.Get("/api/v1/world", [](const HttpRequest&) -> HttpResponse {
		try {
			void* world = GetGWorld();
			if (!world)
				return { 500, "application/json", MakeError("GWorld not available") };

			UEObject worldObj(world);
			json data;
			json worldBrief = MakeObjectBrief(worldObj);
			data["name"] = worldBrief["name"];
			data["address"] = worldBrief["address"];
			data["index"] = worldBrief["index"];

			int actorCount = 0;
			int32 total = ObjectArray::Num();
			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;
				if (obj.IsA(EClassCastFlags::Actor)) actorCount++;
			}
			data["actor_count"] = actorCount;

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read world") };
		}
	});

	// GET /api/v1/world/levels — loaded levels in current world
	server.Get("/api/v1/world/levels", [](const HttpRequest&) -> HttpResponse {
		try {
			void* world = GetGWorld();
			if (!world)
				return { 500, "application/json", MakeError("GWorld not available") };

			UEObject worldObj(world);
			if (!worldObj)
				return { 500, "application/json", MakeError("GWorld object is invalid") };

			std::set<uintptr_t> levelAddrs;
			json levels = json::array();

			auto addLevel = [&](UEObject levelObj, const std::string& source) {
				if (!levelObj) return;
				uintptr_t addr = reinterpret_cast<uintptr_t>(levelObj.GetAddress());
				if (levelAddrs.count(addr)) return;
				levelAddrs.insert(addr);

				json item = MakeObjectBrief(levelObj);
				item["source"] = source;
				int actorCount = GetLevelActorCount(levelObj);
				if (actorCount >= 0) item["actor_count"] = actorCount;
				levels.push_back(std::move(item));
			};

			UEClass worldClass = worldObj.GetClass();
			if (worldClass)
			{
				uint8* worldAddr = reinterpret_cast<uint8*>(worldObj.GetAddress());

				UEProperty persistentProp;
				if (TryFindProperty(worldClass, "PersistentLevel", persistentProp)
					&& (persistentProp.GetCastFlags() & EClassCastFlags::ObjectProperty))
				{
					void* ptr = *reinterpret_cast<void**>(worldAddr + persistentProp.GetOffset());
					addLevel(UEObject(ptr), "PersistentLevel");
				}

				for (const auto& prop : worldClass.GetProperties())
				{
					if (!(prop.GetCastFlags() & EClassCastFlags::ArrayProperty))
						continue;

					const std::string propName = prop.GetName();
					if (propName.find("Level") == std::string::npos)
						continue;

					UC::TArray<void*>* arr = reinterpret_cast<UC::TArray<void*>*>(worldAddr + prop.GetOffset());
					if (!arr) continue;
					int32 num = arr->Num();
					if (num < 0 || num > 2048) continue;

					for (int32 i = 0; i < num; i++)
					{
						void* ptr = nullptr;
						try { ptr = (*arr)[i]; }
						catch (...) { break; }
						if (!ptr) continue;

						UEObject lv(ptr);
						if (!lv) continue;

						std::string clsName = GetSafeClassNameByAddress(lv.GetAddress());
						if (clsName.find("LevelStreaming") != std::string::npos)
						{
							void* streamingClassAddr = nullptr;
							if (!TryReadObjectClassAddress(lv.GetAddress(), streamingClassAddr))
							{
								addLevel(lv, propName);
								continue;
							}

							UEClass streamingCls(streamingClassAddr);
							if (!streamingCls) continue;
							UEProperty loadedLevelProp;
							if (!TryFindProperty(streamingCls, "LoadedLevel", loadedLevelProp)) continue;
							if (!(loadedLevelProp.GetCastFlags() & EClassCastFlags::ObjectProperty)) continue;
							uint8* streamingAddr = reinterpret_cast<uint8*>(lv.GetAddress());
							void* loadedLevelPtr = *reinterpret_cast<void**>(streamingAddr + loadedLevelProp.GetOffset());
							addLevel(UEObject(loadedLevelPtr), propName + ":LoadedLevel");
						}
						else
						{
							addLevel(lv, propName);
						}
					}
				}
			}

			json data;
			data["world"] = MakeObjectBrief(worldObj);
			data["levels"] = std::move(levels);
			data["count"] = static_cast<int>(levelAddrs.size());
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to enumerate levels") };
		}
	});

	// GET /api/v1/world/actors — paginated actor list
	server.Get("/api/v1/world/actors", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			int offset = 0;
			int limit = 50;
			if (params.count("offset")) offset = std::stoi(params["offset"]);
			if (params.count("limit")) limit = std::stoi(params["limit"]);
			if (limit > 500) limit = 500;
			if (offset < 0) offset = 0;

			std::string filter = params.count("q") ? params["q"] : "";
			std::string classFilter = params.count("class") ? params["class"] : "";

			json items = json::array();
			int32 total = ObjectArray::Num();
			int count = 0;
			int skipped = 0;
			int matched = 0;

			for (int32 i = 0; i < total && count < limit; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;
				if (!obj.IsA(EClassCastFlags::Actor)) continue;

				const void* objAddr = obj.GetAddress();
				std::string name;
				if (!TryReadObjectNameByAddress(objAddr, name) || name.empty())
					continue;

				if (!filter.empty() && name.find(filter) == std::string::npos)
					continue;

				std::string className = GetSafeClassNameByAddress(objAddr);

				if (!classFilter.empty() && className.find(classFilter) == std::string::npos)
					continue;

				matched++;
				if (skipped < offset) { skipped++; continue; }

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
		catch (...) {
			return { 500, "application/json", MakeError("Failed to enumerate actors") };
		}
	});

	// GET /api/v1/world/actors/:index — actor detail (including transform/components)
	server.Get("/api/v1/world/actors/:index", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseIndex(GetPathSegment(req.Path, 4), idx))
			return { 400, "application/json", MakeError("Invalid actor index") };

		UEObject actor = ObjectArray::GetByIndex(idx);
		if (!actor)
			return { 404, "application/json", MakeError("Actor is null") };
		if (!actor.IsA(EClassCastFlags::Actor))
			return { 400, "application/json", MakeError("Object is not an Actor") };

		json data = MakeObjectBrief(actor);
		UEObject root = GetActorRootComponent(actor);
		data["root_component"] = root ? MakeObjectBrief(root) : json(nullptr);

		json transform;
		if (root)
		{
			json location;
			json rotation;
			json scale;
			if (ReadVec3Property(root, "RelativeLocation", location)) transform["location"] = std::move(location);
			if (ReadVec3Property(root, "RelativeRotation", rotation)) transform["rotation"] = std::move(rotation);
			if (ReadVec3Property(root, "RelativeScale3D", scale)) transform["scale"] = std::move(scale);
		}
		data["transform"] = transform;

		json components = CollectActorComponents(actor);
		data["components"] = components;
		data["component_count"] = static_cast<int>(components.size());
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/world/actors/:index/components — actor components list
	server.Get("/api/v1/world/actors/:index/components", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseIndex(GetPathSegment(req.Path, 4), idx))
			return { 400, "application/json", MakeError("Invalid actor index") };

		UEObject actor = ObjectArray::GetByIndex(idx);
		if (!actor)
			return { 404, "application/json", MakeError("Actor is null") };
		if (!actor.IsA(EClassCastFlags::Actor))
			return { 400, "application/json", MakeError("Object is not an Actor") };

		json components = CollectActorComponents(actor);
		json data;
		data["actor_index"] = idx;
		data["components"] = components;
		data["count"] = static_cast<int>(components.size());
		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/world/actors/:index/transform — modify actor transform
	server.Post("/api/v1/world/actors/:index/transform", [](const HttpRequest& req) -> HttpResponse {
		int32 idx = -1;
		if (!TryParseIndex(GetPathSegment(req.Path, 4), idx))
			return { 400, "application/json", MakeError("Invalid actor index") };

		UEObject actor = ObjectArray::GetByIndex(idx);
		if (!actor)
			return { 404, "application/json", MakeError("Actor is null") };
		if (!actor.IsA(EClassCastFlags::Actor))
			return { 400, "application/json", MakeError("Object is not an Actor") };

		json body;
		try {
			body = json::parse(req.Body);
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}

		UEObject root = GetActorRootComponent(actor);
		if (!root)
			return { 500, "application/json", MakeError("RootComponent not found") };

		struct PendingWrite
		{
			const char* requestField;
			const char* propertyName;
			json newValue;
			json originalValue;
		};

		std::vector<PendingWrite> writes;
		auto enqueueWrite = [&](const char* requestField, const char* propertyName) -> HttpResponse* {
			if (!body.contains(requestField))
				return nullptr;

			double x = 0.0, y = 0.0, z = 0.0;
			if (!ParseVec3(body[requestField], x, y, z))
			{
				static HttpResponse badInput;
				badInput = { 400, "application/json", MakeError(std::string("Invalid vector format for ") + requestField) };
				return &badInput;
			}

			json original;
			if (!ReadVec3Property(root, propertyName, original))
			{
				static HttpResponse readFail;
				readFail = { 500, "application/json", MakeError(std::string("Failed to capture original ") + propertyName) };
				return &readFail;
			}

			json normalized;
			normalized["x"] = x;
			normalized["y"] = y;
			normalized["z"] = z;
			writes.push_back(PendingWrite{ requestField, propertyName, std::move(normalized), std::move(original) });
			return nullptr;
		};

		if (HttpResponse* err = enqueueWrite("location", "RelativeLocation")) return *err;
		if (HttpResponse* err = enqueueWrite("rotation", "RelativeRotation")) return *err;
		if (HttpResponse* err = enqueueWrite("scale", "RelativeScale3D")) return *err;

		if (writes.empty())
			return { 400, "application/json", MakeError("Missing location/rotation/scale") };

		int32 applied = 0;
		for (int32 i = 0; i < static_cast<int32>(writes.size()); i++)
		{
			const auto& write = writes[i];
			if (!WriteVec3Property(root, write.propertyName, write.newValue))
			{
				bool rollbackOk = true;
				for (int32 j = i - 1; j >= 0; j--)
				{
					if (!WriteVec3Property(root, writes[j].propertyName, writes[j].originalValue))
						rollbackOk = false;
				}

				json err;
				err["success"] = false;
				err["error"] = std::string("Failed to write ") + write.propertyName;
				err["rolled_back"] = true;
				err["rollback_ok"] = rollbackOk;
				err["applied_count"] = applied;
				return { 400, "application/json", err.dump() };
			}
			applied++;
		}

		json transform;
		json location;
		json rotation;
		json scale;
		if (ReadVec3Property(root, "RelativeLocation", location)) transform["location"] = std::move(location);
		if (ReadVec3Property(root, "RelativeRotation", rotation)) transform["rotation"] = std::move(rotation);
		if (ReadVec3Property(root, "RelativeScale3D", scale)) transform["scale"] = std::move(scale);

		json data;
		data["actor_index"] = idx;
		data["updated"] = true;
		data["rolled_back"] = false;
		data["transform"] = std::move(transform);
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/world/shortcuts — quick access objects
	server.Get("/api/v1/world/shortcuts", [](const HttpRequest&) -> HttpResponse {
		try {
			json data;
			auto findFirst = [](const char* className) -> json {
				int32 total = ObjectArray::Num();
				for (int32 i = 0; i < total; i++)
				{
					UEObject obj = ObjectArray::GetByIndex(i);
					if (!obj) continue;
					try {
						UEObject cls = obj.GetClass();
						if (!cls) continue;
						std::string cn = cls.GetName();
						if (cn.find(className) != std::string::npos
							&& obj.GetName().find("Default__") == std::string::npos)
						{
							json r;
							r["index"] = i;
							r["name"] = obj.GetName();
							r["class"] = cn;
							r["address"] = std::format("0x{:X}",
								reinterpret_cast<uintptr_t>(obj.GetAddress()));
							return r;
						}
					}
					catch (...) {}
				}
				return nullptr;
			};

			data["game_mode"] = findFirst("GameMode");
			data["game_state"] = findFirst("GameState");
			data["player_controller"] = findFirst("PlayerController");
			data["pawn"] = findFirst("Pawn");

			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to find shortcuts") };
		}
	});
}

} // namespace UExplorer::API
