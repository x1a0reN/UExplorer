#include "TypeCommandService.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace UExplorer::Services
{
namespace
{

constexpr std::string_view kClassGet = "types.classes.get";
constexpr std::string_view kClassFields = "types.classes.fields";
constexpr std::string_view kClassFunctions = "types.classes.functions";
constexpr std::string_view kClassHierarchy = "types.classes.hierarchy";
constexpr std::string_view kClassDefaultObject = "types.classes.cdo";
constexpr std::string_view kFunctionGet = "types.functions.get";
constexpr std::string_view kStructGet = "types.structs.get";
constexpr std::string_view kStructFields = "types.structs.fields";
constexpr std::string_view kEnumGet = "types.enums.get";
constexpr std::string_view kEnumValues = "types.enums.values";
constexpr std::uint64_t kMaxProtocolGeneration = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kMaxQueryOrdinal =
	Runtime::TypeSnapshotStore::kMaxTotalMembers - 1;

struct TypePathInput
{
	std::string Path;
};

struct TypeQueryCursor
{
	std::uint64_t Generation = 0;
	std::uint64_t AfterOrdinal = 0;
	std::string QueryFingerprint;
};

struct TypeMemberPageInput
{
	std::string Path;
	Runtime::TypeMemberScope Scope = Runtime::TypeMemberScope::Direct;
	std::optional<TypeQueryCursor> Cursor;
	std::size_t Limit = 0;
};

struct TypePathPageInput
{
	std::string Path;
	std::optional<TypeQueryCursor> Cursor;
	std::size_t Limit = 0;
};

TypeCommandResult Failure(
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Error = TypeCommandError{
			.Code = std::move(code),
			.Message = std::move(message),
			.Details = std::move(details)
		}
	};
}

TypeCommandResult SuccessBounded(json data)
{
	if (data.dump().size() > TypeCommandService::kMaxResponseBytes)
	{
		return Failure(
			"TYPE_RESPONSE_LIMIT_EXCEEDED",
			"Serialized type response exceeds the 4 MiB command limit",
			{{"max_response_bytes", TypeCommandService::kMaxResponseBytes}});
	}
	return {.Data = std::move(data)};
}

json NullableText(const std::string& value)
{
	return value.empty() ? json(nullptr) : json(value);
}

bool TryParseBoundedUnsigned(
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
		const std::uint64_t candidate = value.get<std::uint64_t>();
		if (candidate < minimum || candidate > maximum)
			return false;
		parsed = candidate;
		return true;
	}
	const std::int64_t candidate = value.get<std::int64_t>();
	if (candidate < 0)
		return false;
	const auto unsignedCandidate = static_cast<std::uint64_t>(candidate);
	if (unsignedCandidate < minimum || unsignedCandidate > maximum)
		return false;
	parsed = unsignedCandidate;
	return true;
}

bool IsCanonicalFingerprint(const std::string& value) noexcept
{
	if (value.size() != 16)
		return false;
	return std::all_of(value.begin(), value.end(), [](const char character) {
		return (character >= '0' && character <= '9')
			|| (character >= 'A' && character <= 'F');
	});
}

bool TryParsePath(const json& value, std::string& path)
{
	path.clear();
	if (!value.is_string())
		return false;
	path = value.get<std::string>();
	return !path.empty()
		&& path.size() <= Runtime::TypeSnapshotStore::kMaxTextBytes;
}

bool TryParseCursor(const json& value, std::optional<TypeQueryCursor>& cursor)
{
	cursor.reset();
	if (value.is_null())
		return true;
	if (!value.is_object()
		|| value.size() != 3
		|| !value.contains("generation")
		|| !value.contains("after_ordinal")
		|| !value.contains("query_fingerprint")
		|| !value.at("query_fingerprint").is_string())
	{
		return false;
	}
	TypeQueryCursor parsed;
	if (!TryParseBoundedUnsigned(
			value.at("generation"), 1, kMaxProtocolGeneration, parsed.Generation)
		|| !TryParseBoundedUnsigned(
			value.at("after_ordinal"), 0, kMaxQueryOrdinal, parsed.AfterOrdinal))
	{
		return false;
	}
	parsed.QueryFingerprint = value.at("query_fingerprint").get<std::string>();
	if (!IsCanonicalFingerprint(parsed.QueryFingerprint))
		return false;
	cursor = std::move(parsed);
	return true;
}

bool TryParsePathInput(const json& data, TypePathInput& input)
{
	input = {};
	return data.is_object()
		&& data.size() == 1
		&& data.contains("path")
		&& TryParsePath(data.at("path"), input.Path);
}

bool TryParseMemberPageInput(const json& data, TypeMemberPageInput& input)
{
	input = {};
	if (!data.is_object()
		|| data.size() != 4
		|| !data.contains("path")
		|| !data.contains("scope")
		|| !data.contains("cursor")
		|| !data.contains("limit")
		|| !TryParsePath(data.at("path"), input.Path)
		|| !data.at("scope").is_string()
		|| !TryParseCursor(data.at("cursor"), input.Cursor))
	{
		return false;
	}
	const std::string scope = data.at("scope").get<std::string>();
	if (scope == "direct")
		input.Scope = Runtime::TypeMemberScope::Direct;
	else if (scope == "include_inherited")
		input.Scope = Runtime::TypeMemberScope::IncludeInherited;
	else
		return false;
	std::uint64_t limit = 0;
	if (!TryParseBoundedUnsigned(
			data.at("limit"), 1, TypeCommandService::kMaxPageRecords, limit))
	{
		return false;
	}
	input.Limit = static_cast<std::size_t>(limit);
	return true;
}

bool TryParsePathPageInput(const json& data, TypePathPageInput& input)
{
	input = {};
	if (!data.is_object()
		|| data.size() != 3
		|| !data.contains("path")
		|| !data.contains("cursor")
		|| !data.contains("limit")
		|| !TryParsePath(data.at("path"), input.Path)
		|| !TryParseCursor(data.at("cursor"), input.Cursor))
	{
		return false;
	}
	std::uint64_t limit = 0;
	if (!TryParseBoundedUnsigned(
			data.at("limit"), 1, TypeCommandService::kMaxPageRecords, limit))
	{
		return false;
	}
	input.Limit = static_cast<std::size_t>(limit);
	return true;
}

std::string QueryFingerprint(
	const Runtime::TypeSnapshot& snapshot,
	const std::string_view operation,
	const std::string_view path,
	const std::string_view scope = {})
{
	std::uint64_t hash = 1469598103934665603ULL;
	const auto append = [&hash](const std::string_view value) {
		for (const unsigned char character : value)
		{
			hash ^= character;
			hash *= 1099511628211ULL;
		}
		hash ^= 0xFFU;
		hash *= 1099511628211ULL;
	};
	append(snapshot.SessionId());
	append(std::to_string(snapshot.ContextGeneration()));
	append(std::to_string(snapshot.ObjectSnapshotGeneration()));
	append(operation);
	append(path);
	append(scope);
	if (hash == 0)
		hash = 1;
	return std::format("{:016X}", hash);
}

bool SameHandle(
	const Runtime::ObjectHandle& left,
	const Runtime::ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

json SerializeObjectHandle(const Runtime::ObjectHandle& handle)
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

json SerializeFunctionHandle(const Runtime::FunctionHandle& handle)
{
	return {
		{"function", SerializeObjectHandle(handle.Function)},
		{"owner", SerializeObjectHandle(handle.Owner)},
		{"full_path", handle.FullPath},
		{"signature_fingerprint", std::format("{:016X}", handle.SignatureFingerprint)}
	};
}

json SerializeTypeReference(const Runtime::ReflectedType& type)
{
	return {
		{"handle", SerializeObjectHandle(type.Handle)},
		{"kind", Runtime::ToString(type.Kind)},
		{"name", type.Name},
		{"full_path", type.FullPath}
	};
}

json SerializeProperty(const Runtime::ReflectedProperty& property)
{
	return {
		{"name", property.Name},
		{"type_name", property.TypeName},
		{"kind", Runtime::ToString(property.Kind)},
		{"offset", property.Offset},
		{"size", property.Size},
		{"array_dim", property.ArrayDim},
		{"flags", std::format("0x{:016X}", property.Flags)},
		{"state", Runtime::ToString(property.State)},
		{"reason_code", NullableText(property.ReasonCode)},
		{"reason", NullableText(property.Reason)},
		{"descriptor_available", static_cast<bool>(property.Descriptor)}
	};
}

json SerializeTypeDetail(
	const Runtime::TypeSnapshot& snapshot,
	const Runtime::ReflectedType& type)
{
	json parent = nullptr;
	if (type.Super)
	{
		const Runtime::ReflectedType* resolved =
			snapshot.FindByObjectIndex(type.Super->Index);
		if (resolved && SameHandle(*type.Super, resolved->Handle))
			parent = SerializeTypeReference(*resolved);
	}

	json defaultObject = nullptr;
	if (type.Kind == Runtime::ReflectedTypeKind::Class)
	{
		defaultObject = {
			{"state", Runtime::ToString(type.DefaultObjectState)},
			{"handle", type.DefaultObject
				? SerializeObjectHandle(*type.DefaultObject)
				: json(nullptr)},
			{"reason_code", NullableText(type.DefaultObjectReasonCode)},
			{"reason", NullableText(type.DefaultObjectReason)}
		};
	}

	json enumMetadata = nullptr;
	if (type.Kind == Runtime::ReflectedTypeKind::Enum)
	{
		enumMetadata = {
			{"state", Runtime::ToString(type.EnumState)},
			{"underlying_kind", type.EnumUnderlyingKind == Runtime::PropertyKind::Unknown
				? json(nullptr)
				: json(Runtime::ToString(type.EnumUnderlyingKind))},
			{"reason_code", NullableText(type.EnumReasonCode)},
			{"reason", NullableText(type.EnumReason)},
			{"value_count", type.EnumEntries.size()}
		};
	}

	return {
		{"type_snapshot_generation", snapshot.Generation()},
		{"object_snapshot_generation", snapshot.ObjectSnapshotGeneration()},
		{"context_generation", snapshot.ContextGeneration()},
		{"handle", SerializeObjectHandle(type.Handle)},
		{"kind", Runtime::ToString(type.Kind)},
		{"name", type.Name},
		{"full_path", type.FullPath},
		{"package_path", type.PackagePath},
		{"properties_size", type.PropertiesSize},
		{"min_alignment", type.MinAlignment},
		{"super", std::move(parent)},
		{"direct_property_count", type.DirectProperties.size()},
		{"direct_function_count", type.DirectFunctions.size()},
		{"default_object", std::move(defaultObject)},
		{"enum", std::move(enumMetadata)}
	};
}

TypeCommandResult FindExpectedType(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const std::string& path,
	const Runtime::ReflectedTypeKind expectedKind,
	const Runtime::ReflectedType*& type)
{
	type = snapshot->FindByFullPath(path);
	if (!type)
	{
		return Failure(
			"TYPE_NOT_FOUND",
			"No type in the published generation has the requested full path",
			{{"path", path}});
	}
	if (type->Kind != expectedKind)
	{
		return Failure(
			"TYPE_KIND_INVALID",
			"The requested full path resolves to a different reflected type kind",
			{
				{"path", path},
				{"expected_kind", Runtime::ToString(expectedKind)},
				{"actual_kind", Runtime::ToString(type->Kind)}
			});
	}
	return {.Data = json::object()};
}

TypeCommandResult ValidateCursor(
	const std::optional<TypeQueryCursor>& cursor,
	const Runtime::TypeSnapshot& snapshot,
	const std::string& fingerprint,
	const std::size_t total,
	std::size_t& start)
{
	start = 0;
	if (!cursor)
		return {.Data = json::object()};
	if (cursor->Generation != snapshot.Generation())
	{
		return Failure(
			"TYPE_SNAPSHOT_GENERATION_MISMATCH",
			"Type snapshot generation changed; restart paging with a null cursor",
			{
				{"requested_generation", cursor->Generation},
				{"current_generation", snapshot.Generation()}
			});
	}
	if (cursor->QueryFingerprint != fingerprint)
	{
		return Failure(
			"TYPE_QUERY_CURSOR_MISMATCH",
			"Type query cursor belongs to a different session, context, path, scope, or operation",
			{
				{"requested_query_fingerprint", cursor->QueryFingerprint},
				{"current_query_fingerprint", fingerprint}
			});
	}
	// A continuation cursor can only point before at least one remaining item.
	if (total == 0 || cursor->AfterOrdinal >= total - 1)
	{
		return Failure(
			"TYPE_QUERY_CURSOR_INVALID",
			"Type query cursor ordinal is outside the selected immutable result set",
			{{"after_ordinal", cursor->AfterOrdinal}, {"total", total}});
	}
	start = static_cast<std::size_t>(cursor->AfterOrdinal) + 1;
	return {.Data = json::object()};
}

json NextCursor(
	const Runtime::TypeSnapshot& snapshot,
	const std::string& fingerprint,
	const std::size_t start,
	const std::size_t pageSize,
	const bool hasMore)
{
	if (!hasMore)
		return nullptr;
	return {
		{"generation", snapshot.Generation()},
		{"after_ordinal", start + pageSize - 1},
		{"query_fingerprint", fingerprint}
	};
}

template<typename TMember, typename TAccessor>
Runtime::TypeMemberQueryError CollectMemberPage(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const Runtime::ReflectedType& root,
	const Runtime::TypeMemberScope scope,
	const std::size_t start,
	const std::size_t limit,
	TAccessor accessor,
	std::size_t& total,
	std::vector<Runtime::TypeMemberView<TMember>>& page)
{
	total = 0;
	page.clear();
	page.reserve(limit);
	std::set<std::int32_t> visited;
	const Runtime::ReflectedType* current = &root;
	std::uint32_t depth = 0;
	while (current)
	{
		if (!visited.emplace(current->Handle.Index).second)
			return Runtime::TypeMemberQueryError::HierarchyCycle;
		const std::vector<TMember>& members = accessor(*current);
		if (members.size() > Runtime::TypeSnapshotStore::kMaxTotalMembers - total)
			return Runtime::TypeMemberQueryError::HierarchyReferenceInvalid;
		if (page.size() < limit && start < total + members.size())
		{
			const std::size_t localStart = start > total ? start - total : 0;
			const std::size_t take = (std::min)(
				members.size() - localStart, limit - page.size());
			for (std::size_t index = 0; index < take; ++index)
			{
				page.push_back({
					.Member = &members[localStart + index],
					.DeclaringType = current,
					.InheritanceDepth = depth
				});
			}
		}
		total += members.size();
		if (scope == Runtime::TypeMemberScope::Direct || !current->Super)
			break;
		if (++depth > Runtime::TypeSnapshotStore::kMaxHierarchyDepth)
			return Runtime::TypeMemberQueryError::HierarchyDepthExceeded;
		const Runtime::ReflectedType* parent =
			snapshot->FindByObjectIndex(current->Super->Index);
		if (!parent
			|| !SameHandle(*current->Super, parent->Handle)
			|| parent->Kind != current->Kind)
		{
			return Runtime::TypeMemberQueryError::HierarchyReferenceInvalid;
		}
		current = parent;
	}
	return Runtime::TypeMemberQueryError::None;
}

TypeCommandResult MemberQueryFailure(
	const Runtime::TypeMemberQueryError error,
	const std::string& path)
{
	return Failure(
		Runtime::ToString(error),
		"Published type hierarchy could not be traversed safely",
		{{"path", path}});
}

bool AppendBounded(json& items, json item, std::size_t& serializedBytes)
{
	const std::size_t itemBytes = item.dump().size();
	const std::size_t separatorBytes = items.empty() ? 0 : 1;
	if (serializedBytes > TypeCommandService::kMaxResponseBytes
		|| itemBytes > TypeCommandService::kMaxResponseBytes - serializedBytes
		|| separatorBytes
			> TypeCommandService::kMaxResponseBytes - serializedBytes - itemBytes)
		return false;
	serializedBytes += itemBytes + separatorBytes;
	items.push_back(std::move(item));
	return true;
}

TypeCommandResult ExecuteTypeDetail(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data,
	const Runtime::ReflectedTypeKind kind)
{
	TypePathInput input;
	if (!TryParsePathInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Type detail data must contain exactly one non-empty full path field named path");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(snapshot, input.Path, kind, type);
	if (!found.Ok())
		return found;
	return SuccessBounded(SerializeTypeDetail(*snapshot, *type));
}

TypeCommandResult ExecuteFields(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const std::string_view operation,
	const json& data,
	const Runtime::ReflectedTypeKind kind)
{
	TypeMemberPageInput input;
	if (!TryParseMemberPageInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Type field data must contain exactly path, scope, cursor, and limit; scope is direct or include_inherited and limit is 1..128");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(snapshot, input.Path, kind, type);
	if (!found.Ok())
		return found;
	const std::string fingerprint = QueryFingerprint(
		*snapshot, operation, input.Path, Runtime::ToString(input.Scope));

	// Collect once with the cursor-derived start after first validating its immutable
	// generation/query binding. The final ordinal bound is checked against exact total.
	if (input.Cursor
		&& (input.Cursor->Generation != snapshot->Generation()
			|| input.Cursor->QueryFingerprint != fingerprint))
	{
		std::size_t ignored = 0;
		return ValidateCursor(input.Cursor, *snapshot, fingerprint, 1, ignored);
	}
	const std::size_t requestedStart = input.Cursor
		? static_cast<std::size_t>(input.Cursor->AfterOrdinal) + 1
		: 0;
	std::size_t total = 0;
	std::vector<Runtime::TypeMemberView<Runtime::ReflectedProperty>> page;
	const Runtime::TypeMemberQueryError error = CollectMemberPage<Runtime::ReflectedProperty>(
		snapshot,
		*type,
		input.Scope,
		requestedStart,
		input.Limit,
		[](const Runtime::ReflectedType& value)
			-> const std::vector<Runtime::ReflectedProperty>& {
			return value.DirectProperties;
		},
		total,
		page);
	if (error != Runtime::TypeMemberQueryError::None)
		return MemberQueryFailure(error, input.Path);
	std::size_t start = 0;
	TypeCommandResult cursorResult = ValidateCursor(
		input.Cursor, *snapshot, fingerprint, total, start);
	if (!cursorResult.Ok())
		return cursorResult;

	json items = json::array();
	items.get_ref<json::array_t&>().reserve(page.size());
	std::size_t serializedBytes = 2;
	for (const auto& view : page)
	{
		json item = SerializeProperty(*view.Member);
		item["declaring_type"] = SerializeTypeReference(*view.DeclaringType);
		item["inheritance_depth"] = view.InheritanceDepth;
		if (!AppendBounded(items, std::move(item), serializedBytes))
		{
			return Failure(
				"TYPE_RESPONSE_LIMIT_EXCEEDED",
				"Serialized type field page exceeds the 4 MiB command limit",
				{{"path", input.Path}, {"max_response_bytes", TypeCommandService::kMaxResponseBytes}});
		}
	}
	const bool hasMore = start + page.size() < total;
	return SuccessBounded({
		{"type_snapshot_generation", snapshot->Generation()},
		{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
		{"context_generation", snapshot->ContextGeneration()},
		{"path", input.Path},
		{"kind", Runtime::ToString(kind)},
		{"scope", Runtime::ToString(input.Scope)},
		{"items", std::move(items)},
		{"total", total},
		{"matched", total},
		{"limit", input.Limit},
		{"has_more", hasMore},
		{"next_cursor", NextCursor(*snapshot, fingerprint, start, page.size(), hasMore)}
	});
}

TypeCommandResult BuildFunctionItem(
	const Runtime::ReflectedFunction& function,
	const Runtime::ReflectedType& declaringType,
	const std::uint32_t inheritanceDepth,
	json& item)
{
	item = nullptr;
	if (function.Parameters.size() > TypeCommandService::kMaxFunctionParameters)
	{
		return Failure(
			"TYPE_FUNCTION_PARAMETER_LIMIT_EXCEEDED",
			"A reflected function exceeds the bounded parameter serialization limit",
			{
				{"function_path", function.FullPath},
				{"parameter_count", function.Parameters.size()},
				{"max_parameters", TypeCommandService::kMaxFunctionParameters}
			});
	}
	json parameters = json::array();
	parameters.get_ref<json::array_t&>().reserve(function.Parameters.size());
	for (const Runtime::ReflectedParameter& parameter : function.Parameters)
	{
		json serialized = SerializeProperty(parameter.Property);
		serialized["direction"] = Runtime::ToString(parameter.Direction);
		parameters.push_back(std::move(serialized));
	}
	item = {
		{"handle", SerializeFunctionHandle(function.Handle)},
		{"name", function.Name},
		{"full_path", function.FullPath},
		{"flags", std::format("0x{:016X}", function.Flags)},
		{"parameter_size", function.ParameterSize},
		{"parameter_count", function.Parameters.size()},
		{"native_address", function.NativeAddress == 0
			? json(nullptr)
			: json(std::format("0x{:X}", function.NativeAddress))},
		{"implementation", Runtime::ToString(function.Implementation)},
		{"reason_code", NullableText(function.ReasonCode)},
		{"reason", NullableText(function.Reason)},
		{"parameters", std::move(parameters)},
		{"declaring_type", SerializeTypeReference(declaringType)},
		{"inheritance_depth", inheritanceDepth}
	};
	return {.Data = json::object()};
}

TypeCommandResult ExecuteFunctionDetail(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data)
{
	TypePathInput input;
	if (!TryParsePathInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Function detail data must contain exactly one non-empty full path field named path");
	}
	const Runtime::ReflectedFunctionLookup lookup =
		snapshot->FindFunctionByFullPath(input.Path);
	if (!lookup.Found())
	{
		return Failure(
			"FUNCTION_NOT_FOUND",
			"No function in the published type generation has the requested full path",
			{{"path", input.Path}});
	}
	json item;
	TypeCommandResult serialized = BuildFunctionItem(
		*lookup.Function, *lookup.DeclaringType, 0, item);
	if (!serialized.Ok())
		return serialized;
	item["type_snapshot_generation"] = snapshot->Generation();
	item["object_snapshot_generation"] = snapshot->ObjectSnapshotGeneration();
	item["context_generation"] = snapshot->ContextGeneration();
	return SuccessBounded(std::move(item));
}

TypeCommandResult ExecuteFunctions(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data)
{
	TypeMemberPageInput input;
	if (!TryParseMemberPageInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Class function data must contain exactly path, scope, cursor, and limit; scope is direct or include_inherited and limit is 1..128");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(
		snapshot, input.Path, Runtime::ReflectedTypeKind::Class, type);
	if (!found.Ok())
		return found;
	const std::string fingerprint = QueryFingerprint(
		*snapshot, kClassFunctions, input.Path, Runtime::ToString(input.Scope));
	if (input.Cursor
		&& (input.Cursor->Generation != snapshot->Generation()
			|| input.Cursor->QueryFingerprint != fingerprint))
	{
		std::size_t ignored = 0;
		return ValidateCursor(input.Cursor, *snapshot, fingerprint, 1, ignored);
	}
	const std::size_t requestedStart = input.Cursor
		? static_cast<std::size_t>(input.Cursor->AfterOrdinal) + 1
		: 0;
	std::size_t total = 0;
	std::vector<Runtime::TypeMemberView<Runtime::ReflectedFunction>> page;
	const Runtime::TypeMemberQueryError error = CollectMemberPage<Runtime::ReflectedFunction>(
		snapshot,
		*type,
		input.Scope,
		requestedStart,
		input.Limit,
		[](const Runtime::ReflectedType& value)
			-> const std::vector<Runtime::ReflectedFunction>& {
			return value.DirectFunctions;
		},
		total,
		page);
	if (error != Runtime::TypeMemberQueryError::None)
		return MemberQueryFailure(error, input.Path);
	std::size_t start = 0;
	TypeCommandResult cursorResult = ValidateCursor(
		input.Cursor, *snapshot, fingerprint, total, start);
	if (!cursorResult.Ok())
		return cursorResult;

	json items = json::array();
	items.get_ref<json::array_t&>().reserve(page.size());
	std::size_t serializedBytes = 2;
	for (const auto& view : page)
	{
		const Runtime::ReflectedFunction& function = *view.Member;
		json item;
		TypeCommandResult serialized = BuildFunctionItem(
			function, *view.DeclaringType, view.InheritanceDepth, item);
		if (!serialized.Ok())
			return serialized;
		if (!AppendBounded(items, std::move(item), serializedBytes))
		{
			return Failure(
				"TYPE_RESPONSE_LIMIT_EXCEEDED",
				"Serialized class function page exceeds the 4 MiB command limit",
				{{"path", input.Path}, {"max_response_bytes", TypeCommandService::kMaxResponseBytes}});
		}
	}
	const bool hasMore = start + page.size() < total;
	return SuccessBounded({
		{"type_snapshot_generation", snapshot->Generation()},
		{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
		{"context_generation", snapshot->ContextGeneration()},
		{"path", input.Path},
		{"kind", "class"},
		{"scope", Runtime::ToString(input.Scope)},
		{"items", std::move(items)},
		{"total", total},
		{"matched", total},
		{"limit", input.Limit},
		{"has_more", hasMore},
		{"next_cursor", NextCursor(*snapshot, fingerprint, start, page.size(), hasMore)}
	});
}

TypeCommandResult ExecuteHierarchy(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data)
{
	TypePathPageInput input;
	if (!TryParsePathPageInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Class hierarchy data must contain exactly path, cursor, and limit; limit is 1..128");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(
		snapshot, input.Path, Runtime::ReflectedTypeKind::Class, type);
	if (!found.Ok())
		return found;
	const std::string fingerprint = QueryFingerprint(
		*snapshot, kClassHierarchy, input.Path);
	const std::vector<std::size_t>* childIndices =
		snapshot->FindDirectChildIndices(type->Handle.Index);
	const std::size_t total = childIndices ? childIndices->size() : 0;
	std::size_t start = 0;
	TypeCommandResult cursorResult = ValidateCursor(
		input.Cursor, *snapshot, fingerprint, total, start);
	if (!cursorResult.Ok())
		return cursorResult;
	const std::size_t pageSize = (std::min)(input.Limit, total - start);

	json parents = json::array();
	std::set<std::int32_t> visited;
	const Runtime::ReflectedType* current = type;
	std::uint32_t depth = 0;
	std::size_t serializedBytes = 2;
	while (current->Super)
	{
		if (!visited.emplace(current->Handle.Index).second)
			return MemberQueryFailure(Runtime::TypeMemberQueryError::HierarchyCycle, input.Path);
		if (++depth > Runtime::TypeSnapshotStore::kMaxHierarchyDepth)
		{
			return MemberQueryFailure(
				Runtime::TypeMemberQueryError::HierarchyDepthExceeded, input.Path);
		}
		const Runtime::ReflectedType* parent =
			snapshot->FindByObjectIndex(current->Super->Index);
		if (!parent
			|| !SameHandle(*current->Super, parent->Handle)
			|| parent->Kind != Runtime::ReflectedTypeKind::Class)
		{
			return MemberQueryFailure(
				Runtime::TypeMemberQueryError::HierarchyReferenceInvalid, input.Path);
		}
		json serialized = SerializeTypeReference(*parent);
		serialized["inheritance_depth"] = depth;
		if (!AppendBounded(parents, std::move(serialized), serializedBytes))
		{
			return Failure(
				"TYPE_RESPONSE_LIMIT_EXCEEDED",
				"Serialized class parent chain exceeds the 4 MiB command limit",
				{{"path", input.Path}});
		}
		current = parent;
	}

	json children = json::array();
	children.get_ref<json::array_t&>().reserve(pageSize);
	for (std::size_t offset = 0; offset < pageSize; ++offset)
	{
		const std::size_t typeIndex = (*childIndices)[start + offset];
		if (typeIndex >= snapshot->Types().size())
		{
			return MemberQueryFailure(
				Runtime::TypeMemberQueryError::HierarchyReferenceInvalid, input.Path);
		}
		if (!AppendBounded(
				children,
				SerializeTypeReference(snapshot->Types()[typeIndex]),
				serializedBytes))
		{
			return Failure(
				"TYPE_RESPONSE_LIMIT_EXCEEDED",
				"Serialized class hierarchy page exceeds the 4 MiB command limit",
				{{"path", input.Path}});
		}
	}
	const bool hasMore = start + pageSize < total;
	return SuccessBounded({
		{"type_snapshot_generation", snapshot->Generation()},
		{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
		{"context_generation", snapshot->ContextGeneration()},
		{"path", input.Path},
		{"parents", std::move(parents)},
		{"children", std::move(children)},
		{"total", total},
		{"matched", total},
		{"limit", input.Limit},
		{"has_more", hasMore},
		{"next_cursor", NextCursor(*snapshot, fingerprint, start, pageSize, hasMore)}
	});
}

TypeCommandResult ExecuteDefaultObject(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data)
{
	TypePathInput input;
	if (!TryParsePathInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Class default-object data must contain exactly one non-empty full path field named path");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(
		snapshot, input.Path, Runtime::ReflectedTypeKind::Class, type);
	if (!found.Ok())
		return found;
	return SuccessBounded({
		{"type_snapshot_generation", snapshot->Generation()},
		{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
		{"context_generation", snapshot->ContextGeneration()},
		{"class_path", input.Path},
		{"state", Runtime::ToString(type->DefaultObjectState)},
		{"handle", type->DefaultObject
			? SerializeObjectHandle(*type->DefaultObject)
			: json(nullptr)},
		{"reason_code", NullableText(type->DefaultObjectReasonCode)},
		{"reason", NullableText(type->DefaultObjectReason)}
	});
}

TypeCommandResult ExecuteEnumValues(
	const std::shared_ptr<const Runtime::TypeSnapshot>& snapshot,
	const json& data)
{
	TypePathPageInput input;
	if (!TryParsePathPageInput(data, input))
	{
		return Failure(
			"INVALID_ARGUMENT",
			"Enum value data must contain exactly path, cursor, and limit; limit is 1..128");
	}
	const Runtime::ReflectedType* type = nullptr;
	TypeCommandResult found = FindExpectedType(
		snapshot, input.Path, Runtime::ReflectedTypeKind::Enum, type);
	if (!found.Ok())
		return found;
	const std::string fingerprint = QueryFingerprint(*snapshot, kEnumValues, input.Path);
	const std::size_t total = type->EnumEntries.size();
	std::size_t start = 0;
	TypeCommandResult cursorResult = ValidateCursor(
		input.Cursor, *snapshot, fingerprint, total, start);
	if (!cursorResult.Ok())
		return cursorResult;
	const std::size_t pageSize = (std::min)(input.Limit, total - start);
	json items = json::array();
	items.get_ref<json::array_t&>().reserve(pageSize);
	std::size_t serializedBytes = 2;
	for (std::size_t offset = 0; offset < pageSize; ++offset)
	{
		const Runtime::ReflectedEnumEntry& entry = type->EnumEntries[start + offset];
		if (!AppendBounded(
				items,
				{{"name", entry.Name}, {"value", std::to_string(entry.Value)}},
				serializedBytes))
		{
			return Failure(
				"TYPE_RESPONSE_LIMIT_EXCEEDED",
				"Serialized enum value page exceeds the 4 MiB command limit",
				{{"path", input.Path}});
		}
	}
	const bool hasMore = start + pageSize < total;
	return SuccessBounded({
		{"type_snapshot_generation", snapshot->Generation()},
		{"object_snapshot_generation", snapshot->ObjectSnapshotGeneration()},
		{"context_generation", snapshot->ContextGeneration()},
		{"path", input.Path},
		{"state", Runtime::ToString(type->EnumState)},
		{"underlying_kind", type->EnumUnderlyingKind == Runtime::PropertyKind::Unknown
			? json(nullptr)
			: json(Runtime::ToString(type->EnumUnderlyingKind))},
		{"reason_code", NullableText(type->EnumReasonCode)},
		{"reason", NullableText(type->EnumReason)},
		{"items", std::move(items)},
		{"total", total},
		{"matched", total},
		{"limit", input.Limit},
		{"has_more", hasMore},
		{"next_cursor", NextCursor(*snapshot, fingerprint, start, pageSize, hasMore)}
	});
}

} // namespace

bool TypeCommandService::Handles(const std::string_view operation) noexcept
{
	return operation == kClassGet
		|| operation == kClassFields
		|| operation == kClassFunctions
		|| operation == kClassHierarchy
		|| operation == kClassDefaultObject
		|| operation == kFunctionGet
		|| operation == kStructGet
		|| operation == kStructFields
		|| operation == kEnumGet
		|| operation == kEnumValues;
}

TypeCommandResult TypeCommandService::Execute(
	std::shared_ptr<const Runtime::TypeSnapshot> snapshot,
	const std::string_view operation,
	const json& data,
	const std::string_view expectedSessionId,
	const std::uint64_t expectedContextGeneration,
	const std::uint64_t expectedObjectSnapshotGeneration) noexcept
{
	try
	{
		if (!Handles(operation))
		{
			return Failure(
				"OPERATION_NOT_SUPPORTED",
				"No immutable type command is registered for the requested operation",
				{{"operation", operation}});
		}
		if (!snapshot)
		{
			return Failure(
				"TYPE_SNAPSHOT_UNAVAILABLE",
				"No complete immutable type snapshot is published");
		}
		if (expectedSessionId.empty()
			|| expectedContextGeneration == 0
			|| expectedObjectSnapshotGeneration == 0
			|| snapshot->SessionId() != expectedSessionId
			|| snapshot->ContextGeneration() != expectedContextGeneration
			|| snapshot->ObjectSnapshotGeneration() != expectedObjectSnapshotGeneration
			|| !snapshot->IsConfigured(expectedContextGeneration))
		{
			return Failure(
				"TYPE_SNAPSHOT_CONTEXT_MISMATCH",
				"Published type snapshot does not match the active session and dependency generations",
				{
					{"snapshot_session_id", snapshot->SessionId()},
					{"snapshot_context_generation", snapshot->ContextGeneration()},
					{"snapshot_object_generation", snapshot->ObjectSnapshotGeneration()},
					{"active_session_id", expectedSessionId},
					{"active_context_generation", expectedContextGeneration},
					{"active_object_generation", expectedObjectSnapshotGeneration}
				});
		}

		if (operation == kClassGet)
			return ExecuteTypeDetail(snapshot, data, Runtime::ReflectedTypeKind::Class);
		if (operation == kClassFields)
		{
			return ExecuteFields(
				snapshot, operation, data, Runtime::ReflectedTypeKind::Class);
		}
		if (operation == kClassFunctions)
			return ExecuteFunctions(snapshot, data);
		if (operation == kClassHierarchy)
			return ExecuteHierarchy(snapshot, data);
		if (operation == kClassDefaultObject)
			return ExecuteDefaultObject(snapshot, data);
		if (operation == kFunctionGet)
			return ExecuteFunctionDetail(snapshot, data);
		if (operation == kStructGet)
			return ExecuteTypeDetail(snapshot, data, Runtime::ReflectedTypeKind::Struct);
		if (operation == kStructFields)
		{
			return ExecuteFields(
				snapshot, operation, data, Runtime::ReflectedTypeKind::Struct);
		}
		if (operation == kEnumGet)
			return ExecuteTypeDetail(snapshot, data, Runtime::ReflectedTypeKind::Enum);
		return ExecuteEnumValues(snapshot, data);
	}
	catch (const std::bad_alloc&)
	{
		return Failure(
			"TYPE_QUERY_ALLOCATION_FAILED",
			"Immutable type query could not allocate its bounded response");
	}
	catch (...)
	{
		return Failure(
			"TYPE_QUERY_INTERNAL_ERROR",
			"Immutable type query raised an unexpected error");
	}
}

} // namespace UExplorer::Services
