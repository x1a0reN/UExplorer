#include "WorldCommandService.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kInspect = "world.inspect";
constexpr std::string_view kLevels = "world.levels";
constexpr std::string_view kActors = "world.actors.list";
constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxSearchBytes = 1024;
constexpr std::size_t kMaxPathBytes = 4096;

struct Cursor
{
	std::uint64_t Generation = 0;
	std::size_t AfterOrdinal = 0;
	std::string QueryFingerprint;
};

WorldCommandError Error(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Code = std::move(code),
		.Message = std::move(message),
		.Details = std::move(details)
	};
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
		parsed = value.get<std::uint64_t>();
		return parsed >= minimum && parsed <= maximum;
	}
	const std::int64_t candidate = value.get<std::int64_t>();
	if (candidate < 0)
		return false;
	parsed = static_cast<std::uint64_t>(candidate);
	return parsed >= minimum && parsed <= maximum;
}

bool IsFingerprint(const std::string& value) noexcept
{
	return value.size() == 16
		&& std::all_of(value.begin(), value.end(), [](const char character) {
			return (character >= '0' && character <= '9')
				|| (character >= 'A' && character <= 'F');
		});
}

bool TryCursor(const json& value, Cursor& cursor)
{
	cursor = {};
	if (value.is_null())
		return true;
	if (!value.is_object() || value.size() != 3
		|| !value.contains("generation")
		|| !value.contains("after_ordinal")
		|| !value.contains("query_fingerprint")
		|| !value.at("query_fingerprint").is_string())
	{
		return false;
	}
	std::uint64_t generation = 0;
	std::uint64_t ordinal = 0;
	const std::string fingerprint = value.at("query_fingerprint").get<std::string>();
	if (!TryUnsigned(value.at("generation"), 1, kMaxProtocolInteger, generation)
		|| !TryUnsigned(
			value.at("after_ordinal"),
			0,
			Runtime::WorldSnapshotStore::kMaxActors - 1,
			ordinal)
		|| !IsFingerprint(fingerprint))
	{
		return false;
	}
	cursor = {
		.Generation = generation,
		.AfterOrdinal = static_cast<std::size_t>(ordinal),
		.QueryFingerprint = fingerprint
	};
	return true;
}

std::uint64_t HashBytes(std::uint64_t hash, const std::string_view value) noexcept
{
	constexpr std::uint64_t prime = 1099511628211ULL;
	for (const unsigned char character : value)
	{
		hash ^= character;
		hash *= prime;
	}
	hash ^= 0xFFu;
	hash *= prime;
	return hash;
}

std::string QueryFingerprint(
	const Runtime::WorldSnapshot& snapshot,
	const std::string_view operation,
	const std::string_view search,
	const std::string_view classSearch,
	const std::string_view levelPath)
{
	std::uint64_t hash = 1469598103934665603ULL;
	hash = HashBytes(hash, snapshot.SessionId);
	hash = HashBytes(hash, std::to_string(snapshot.ContextGeneration));
	hash = HashBytes(hash, std::to_string(snapshot.Generation));
	hash = HashBytes(hash, operation);
	hash = HashBytes(hash, search);
	hash = HashBytes(hash, classSearch);
	hash = HashBytes(hash, levelPath);
	return std::format("{:016X}", hash);
}

json SerializeHandle(const Runtime::ObjectHandle& handle)
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

json SerializeObject(const Runtime::WorldSnapshotObject& object)
{
	return {
		{"handle", SerializeHandle(object.Handle)},
		{"index", object.Handle.Index},
		{"name", object.Name},
		{"full_path", object.FullPath},
		{"class_path", object.ClassPath},
		{"class", object.ClassPath},
		{"address", std::format("0x{:X}", object.Handle.Address)}
	};
}

bool ContainsAsciiInsensitive(
	const std::string_view value,
	const std::string_view query) noexcept
{
	if (query.empty())
		return true;
	const auto lower = [](const unsigned char character) {
		return static_cast<unsigned char>(std::tolower(character));
	};
	return std::search(
		value.begin(),
		value.end(),
		query.begin(),
		query.end(),
		[&lower](const unsigned char left, const unsigned char right) {
			return lower(left) == lower(right);
		}) != value.end();
}

bool TryOptionalText(
	const json& value,
	const std::size_t maximum,
	std::string& parsed)
{
	parsed.clear();
	if (value.is_null())
		return true;
	if (!value.is_string())
		return false;
	parsed = value.get<std::string>();
	return parsed.size() <= maximum
		&& std::none_of(parsed.begin(), parsed.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

WorldCommandResult CheckSize(json data)
{
	if (data.dump().size() > WorldCommandService::kMaxSerializedDataBytes)
	{
		return {.Error = Error(
			"WORLD_RESPONSE_TOO_LARGE",
			"The immutable world response exceeds the 4 MiB command-data ceiling")};
	}
	return {.Data = std::move(data)};
}

WorldCommandResult Inspect(
	const json& data,
	const Runtime::WorldSnapshot& snapshot)
{
	if (!data.is_object() || !data.empty())
	{
		return {.Error = Error(
			"WORLD_REQUEST_INVALID",
			"world.inspect data must be an empty object")};
	}
	return CheckSize({
		{"generation", snapshot.Generation},
		{"context_generation", snapshot.ContextGeneration},
		{"object_snapshot_generation", snapshot.ObjectSnapshotGeneration},
		{"type_snapshot_generation", snapshot.TypeSnapshotGeneration},
		{"captured_at_monotonic_us", snapshot.CapturedAtMonotonicUs},
		{"capture_duration_us", snapshot.CaptureDurationUs},
		{"world", SerializeObject(snapshot.World)},
		{"level_count", snapshot.Levels.size()},
		{"actor_count", snapshot.Actors.size()}
	});
}

WorldCommandResult Levels(
	const json& data,
	const Runtime::WorldSnapshot& snapshot)
{
	if (!data.is_object() || data.size() != 2
		|| !data.contains("cursor") || !data.contains("limit"))
	{
		return {.Error = Error(
			"WORLD_LEVEL_QUERY_INVALID",
			"world.levels requires exactly cursor and limit")};
	}
	Cursor cursor;
	std::uint64_t limit = 0;
	if (!TryCursor(data.at("cursor"), cursor)
		|| !TryUnsigned(data.at("limit"), 1, WorldCommandService::kMaxPageRecords, limit))
	{
		return {.Error = Error(
			"WORLD_LEVEL_QUERY_INVALID",
			"The world level cursor or limit is invalid")};
	}
	const std::string fingerprint = QueryFingerprint(snapshot, kLevels, {}, {}, {});
	if (cursor.Generation != 0
		&& (cursor.Generation != snapshot.Generation
			|| cursor.QueryFingerprint != fingerprint))
	{
		return {.Error = Error(
			"WORLD_CURSOR_STALE",
			"The world level cursor does not match the current snapshot/query")};
	}
	if (cursor.Generation != 0
		&& (snapshot.Levels.empty()
			|| cursor.AfterOrdinal >= snapshot.Levels.size() - 1))
	{
		return {.Error = Error(
			"WORLD_CURSOR_INVALID",
			"The world level cursor was not issued for a page with remaining records")};
	}
	const std::size_t begin = cursor.Generation == 0 ? 0 : cursor.AfterOrdinal + 1;
	const std::size_t end = (std::min)(
		snapshot.Levels.size(),
		begin + static_cast<std::size_t>(limit));
	json items = json::array();
	items.get_ref<json::array_t&>().reserve(end - begin);
	for (std::size_t index = begin; index < end; ++index)
	{
		json item = SerializeObject(snapshot.Levels[index].Object);
		item["source"] = snapshot.Levels[index].Object.FullPath;
		item["actor_count"] = snapshot.Levels[index].ActorCount;
		items.push_back(std::move(item));
	}
	const bool hasMore = end < snapshot.Levels.size();
	json nextCursor = nullptr;
	if (hasMore && end > begin)
	{
		nextCursor = {
			{"generation", snapshot.Generation},
			{"after_ordinal", end - 1},
			{"query_fingerprint", fingerprint}
		};
	}
	return CheckSize({
		{"generation", snapshot.Generation},
		{"context_generation", snapshot.ContextGeneration},
		{"object_snapshot_generation", snapshot.ObjectSnapshotGeneration},
		{"type_snapshot_generation", snapshot.TypeSnapshotGeneration},
		{"world", SerializeObject(snapshot.World)},
		{"levels", std::move(items)},
		{"count", snapshot.Levels.size()},
		{"limit", limit},
		{"has_more", hasMore},
		{"next_cursor", std::move(nextCursor)}
	});
}

WorldCommandResult Actors(
	const json& data,
	const Runtime::WorldSnapshot& snapshot)
{
	if (!data.is_object() || data.size() != 5
		|| !data.contains("cursor") || !data.contains("limit")
		|| !data.contains("search") || !data.contains("class_search")
		|| !data.contains("level_path"))
	{
		return {.Error = Error(
			"WORLD_ACTOR_QUERY_INVALID",
			"world.actors.list requires cursor, limit, search, class_search, and level_path")};
	}
	Cursor cursor;
	std::uint64_t limit = 0;
	std::string search;
	std::string classSearch;
	std::string levelPath;
	if (!TryCursor(data.at("cursor"), cursor)
		|| !TryUnsigned(data.at("limit"), 1, WorldCommandService::kMaxPageRecords, limit)
		|| !TryOptionalText(data.at("search"), kMaxSearchBytes, search)
		|| !TryOptionalText(data.at("class_search"), kMaxSearchBytes, classSearch)
		|| !TryOptionalText(data.at("level_path"), kMaxPathBytes, levelPath))
	{
		return {.Error = Error(
			"WORLD_ACTOR_QUERY_INVALID",
			"The world actor cursor, limit, or filter is invalid")};
	}
	if (!levelPath.empty())
	{
		const bool levelExists = std::ranges::any_of(
			snapshot.Levels,
			[&levelPath](const Runtime::WorldSnapshotLevel& level) {
				return level.Object.FullPath == levelPath;
			});
		if (!levelExists)
		{
			return {.Error = Error(
				"WORLD_LEVEL_NOT_FOUND",
				"The exact level path is absent from the current world snapshot",
				{{"level_path", levelPath}})};
		}
	}
	const std::string fingerprint = QueryFingerprint(
		snapshot,
		kActors,
		search,
		classSearch,
		levelPath);
	if (cursor.Generation != 0
		&& (cursor.Generation != snapshot.Generation
			|| cursor.QueryFingerprint != fingerprint
			|| cursor.AfterOrdinal >= snapshot.Actors.size()))
	{
		return {.Error = Error(
			"WORLD_CURSOR_STALE",
			"The world actor cursor does not match the current snapshot/query")};
	}

	const std::size_t begin = cursor.Generation == 0 ? 0 : cursor.AfterOrdinal + 1;
	json items = json::array();
	items.get_ref<json::array_t&>().reserve(static_cast<std::size_t>(limit));
	std::size_t matched = 0;
	std::size_t lastOrdinal = 0;
	bool hasMore = false;
	bool cursorRecordMatched = cursor.Generation == 0;
	bool matchExistsAfterCursor = cursor.Generation == 0;
	for (std::size_t index = 0; index < snapshot.Actors.size(); ++index)
	{
		const Runtime::WorldSnapshotActor& actor = snapshot.Actors[index];
		const Runtime::WorldSnapshotLevel* level = snapshot.FindLevelByIndex(actor.Level.Index);
		if (!level
			|| (!levelPath.empty() && level->Object.FullPath != levelPath)
			|| (!search.empty()
				&& !ContainsAsciiInsensitive(actor.Object.Name, search)
				&& !ContainsAsciiInsensitive(actor.Object.FullPath, search))
			|| (!classSearch.empty()
				&& !ContainsAsciiInsensitive(actor.Object.ClassPath, classSearch)))
		{
			continue;
		}
		++matched;
		if (cursor.Generation != 0 && index == cursor.AfterOrdinal)
			cursorRecordMatched = true;
		if (cursor.Generation != 0 && index > cursor.AfterOrdinal)
			matchExistsAfterCursor = true;
		if (index < begin)
			continue;
		if (items.size() >= limit)
		{
			hasMore = true;
			continue;
		}
		json item = SerializeObject(actor.Object);
		item["level"] = SerializeObject(level->Object);
		items.push_back(std::move(item));
		lastOrdinal = index;
	}
	if (cursor.Generation != 0
		&& (!cursorRecordMatched || !matchExistsAfterCursor))
	{
		return {.Error = Error(
			"WORLD_CURSOR_INVALID",
			"The world actor cursor was not issued for this filtered result page")};
	}
	json nextCursor = nullptr;
	if (hasMore && !items.empty())
	{
		nextCursor = {
			{"generation", snapshot.Generation},
			{"after_ordinal", lastOrdinal},
			{"query_fingerprint", fingerprint}
		};
	}
	return CheckSize({
		{"generation", snapshot.Generation},
		{"context_generation", snapshot.ContextGeneration},
		{"object_snapshot_generation", snapshot.ObjectSnapshotGeneration},
		{"type_snapshot_generation", snapshot.TypeSnapshotGeneration},
		{"items", std::move(items)},
		{"matched", matched},
		{"limit", limit},
		{"has_more", hasMore},
		{"next_cursor", std::move(nextCursor)}
	});
}

} // namespace

bool WorldCommandService::Handles(const std::string_view operation) noexcept
{
	return operation == kInspect || operation == kLevels || operation == kActors;
}

WorldCommandResult WorldCommandService::Execute(
	const std::string_view operation,
	const json& data,
	std::shared_ptr<const Runtime::WorldSnapshot> snapshot) noexcept
{
	try
	{
		if (!snapshot)
		{
			return {.Error = Error(
				"WORLD_SNAPSHOT_UNAVAILABLE",
				"No immutable current-world snapshot has been published")};
		}
		if (operation == kInspect)
			return Inspect(data, *snapshot);
		if (operation == kLevels)
			return Levels(data, *snapshot);
		if (operation == kActors)
			return Actors(data, *snapshot);
		return {.Error = Error(
			"OPERATION_NOT_SUPPORTED",
			"The world command is not registered")};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = Error(
			"WORLD_ALLOCATION_FAILED",
			"The bounded world query could not allocate response storage")};
	}
	catch (...)
	{
		return {.Error = Error(
			"WORLD_QUERY_FAILED",
			"The immutable world query failed unexpectedly")};
	}
}

} // namespace UExplorer::Services
