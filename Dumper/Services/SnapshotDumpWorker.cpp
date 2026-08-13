#include "SnapshotDumpWorker.h"

#include "Generator/Public/Generators/UsmapContainer.h"
#include "Utils/Json/json.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UExplorer::Services
{
namespace
{

using json = nlohmann::json;
using Runtime::DumpJobDiagnostic;
using Runtime::DumpJobDiagnosticSeverity;
using Runtime::DumpJobProgress;
using Runtime::DumpJobWorkerResult;
using Runtime::DumpJobWorkerStatus;
using Runtime::PropertyDescriptor;
using Runtime::PropertyKind;
using Runtime::ReflectedFunction;
using Runtime::ReflectedMemberState;
using Runtime::ReflectedParameter;
using Runtime::ReflectedParameterDirection;
using Runtime::ReflectedProperty;
using Runtime::ReflectedType;
using Runtime::ReflectedTypeKind;

constexpr std::string_view kOptionsSchema = "uexplorer.dump.start.v1";
constexpr std::string_view kManifestSchema = "uexplorer.dump.manifest.v1";
constexpr std::string_view kSdkSchema = "uexplorer.snapshot-sdk.v2";
constexpr std::string_view kIdaSchema = "uexplorer.ida-script.v1";
constexpr std::size_t kIoChunkBytes = 1024 * 1024;
constexpr std::size_t kMaxWindowsPathChars = 32'767;

enum class ArtifactKind : std::uint8_t
{
	Sdk,
	Usmap,
	Json,
	IdaScript,
	Manifest
};

struct GeneratedArtifact
{
	std::string Name;
	ArtifactKind Kind = ArtifactKind::Json;
	std::vector<std::uint8_t> Bytes;
	std::uint64_t ExpectedRecords = 0;
};

struct WrittenArtifact
{
	std::string Name;
	std::uint64_t Size = 0;
	std::string Sha256;
	std::string Validation;
};

struct GenerationFailure
{
	std::string Code;
	std::string Message;

	explicit operator bool() const noexcept { return !Code.empty(); }
};

DumpJobWorkerResult Failed(std::string code, std::string message)
{
	return {
		.Status = DumpJobWorkerStatus::Failed,
		.ErrorCode = std::move(code),
		.ErrorMessage = std::move(message)};
}

DumpJobWorkerResult Cancelled()
{
	return {
		.Status = DumpJobWorkerStatus::Cancelled,
		.ErrorCode = "DUMP_WORKER_CANCELLED",
		.ErrorMessage = "The snapshot dump worker observed cancellation."};
}

bool ValidLimits(const SnapshotDumpWorkerLimits& limits) noexcept
{
	return limits.MaxTypes > 0
		&& limits.MaxTypes <= SnapshotDumpWorker::kHardMaxTypes
		&& limits.MaxMembers > 0
		&& limits.MaxMembers <= SnapshotDumpWorker::kHardMaxMembers
		&& limits.MaxArtifacts > 0
		&& limits.MaxArtifacts <= SnapshotDumpWorker::kHardMaxArtifacts
		&& limits.MaxArtifactBytes > 0
		&& limits.MaxArtifactBytes <= SnapshotDumpWorker::kHardMaxArtifactBytes
		&& limits.MaxTotalArtifactBytes >= limits.MaxArtifactBytes
		&& limits.MaxTotalArtifactBytes <= SnapshotDumpWorker::kHardMaxTotalArtifactBytes
		&& limits.MaxOutputIdentityBytes > 0
		&& limits.MaxOutputIdentityBytes <= 128;
}

bool IsSafePathToken(const std::string_view value, const std::size_t maximum) noexcept
{
	if (value.empty() || value.size() > maximum || value == "." || value == "..")
		return false;
	const unsigned char first = static_cast<unsigned char>(value.front());
	if (!((first >= 'a' && first <= 'z')
		|| (first >= 'A' && first <= 'Z')
		|| (first >= '0' && first <= '9')))
	{
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
		return (character >= 'a' && character <= 'z')
			|| (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9')
			|| character == '-'
			|| character == '_'
			|| character == '.';
	});
}

bool IsWithinPath(
	const std::filesystem::path& child,
	const std::filesystem::path& parent) noexcept
{
	auto childIt = child.begin();
	for (auto parentIt = parent.begin(); parentIt != parent.end(); ++parentIt, ++childIt)
	{
		if (childIt == child.end()
			|| CompareStringOrdinal(
				childIt->c_str(),
				static_cast<int>(childIt->native().size()),
				parentIt->c_str(),
				static_cast<int>(parentIt->native().size()),
				TRUE) != CSTR_EQUAL)
		{
			return false;
		}
	}
	return true;
}

bool CheckedAdd(std::size_t& value, const std::size_t increment) noexcept
{
	if (increment > (std::numeric_limits<std::size_t>::max)() - value)
		return false;
	value += increment;
	return true;
}

class BoundedText final
{
public:
	explicit BoundedText(const std::size_t limit) noexcept : m_Limit(limit) {}

	bool Append(const std::string_view value)
	{
		if (value.size() > m_Limit - m_Value.size())
			return false;
		m_Value.append(value);
		return true;
	}

	bool AppendJson(const json& value)
	{
		const std::string encoded = value.dump(
			-1,
			' ',
			false,
			nlohmann::detail::error_handler_t::strict);
		return Append(encoded);
	}

	std::size_t Size() const noexcept { return m_Value.size(); }

	std::vector<std::uint8_t> ReleaseBytes()
	{
		return std::vector<std::uint8_t>(m_Value.begin(), m_Value.end());
	}

private:
	std::size_t m_Limit = 0;
	std::string m_Value;
};

class JsonArrayDocument final
{
public:
	JsonArrayDocument(
		const std::size_t limit,
		const std::string_view updatedAt,
		const bool includeCredit = false)
		: m_Text(limit)
	{
		json prefix = {
			{"updated_at", updatedAt},
			{"version", 10202}
		};
		if (includeCredit)
		{
			prefix["credit"] = {
				{"dumper_used", "UExplorer immutable snapshot worker"},
				{"schema", "uexplorer.dumpspace.snapshot.v1"}
			};
		}
		std::string encoded = prefix.dump();
		if (!encoded.empty() && encoded.back() == '}')
			encoded.pop_back();
		m_Valid = m_Text.Append(encoded) && m_Text.Append(",\"data\":[");
	}

	bool Add(const json& value)
	{
		if (!m_Valid || m_Closed)
			return false;
		if (!m_First && !m_Text.Append(","))
			return false;
		m_First = false;
		m_Valid = m_Text.AppendJson(value);
		return m_Valid;
	}

	std::vector<std::uint8_t> Finish()
	{
		if (!m_Valid || m_Closed || !m_Text.Append("]}"))
			return {};
		m_Closed = true;
		return m_Text.ReleaseBytes();
	}

	bool Valid() const noexcept { return m_Valid; }

private:
	BoundedText m_Text;
	bool m_First = true;
	bool m_Closed = false;
	bool m_Valid = false;
};

std::string Hex(const std::uint64_t value)
{
	std::ostringstream output;
	output << "0x" << std::uppercase << std::hex << value;
	return output.str();
}

std::string LastPathToken(const std::string_view value)
{
	const std::size_t separator = value.find_last_of("/.: ");
	return std::string(separator == std::string_view::npos
		? value
		: value.substr(separator + 1));
}

std::string CppIdentifier(const std::string_view value, const std::uint64_t suffix)
{
	std::string result;
	result.reserve((std::min)(value.size(), std::size_t{80}) + 24);
	for (const unsigned char character : value)
	{
		if (result.size() >= 80)
			break;
		result.push_back(
			(character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9')
				? static_cast<char>(character)
				: '_');
	}
	if (result.empty() || (result.front() >= '0' && result.front() <= '9'))
		result.insert(result.begin(), '_');
	result += "_" + std::to_string(suffix);
	return result;
}

const char* IntegerCppType(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8: return "std::int8_t";
	case PropertyKind::Int16: return "std::int16_t";
	case PropertyKind::Int32: return "std::int32_t";
	case PropertyKind::Int64: return "std::int64_t";
	case PropertyKind::UInt8: return "std::uint8_t";
	case PropertyKind::UInt16: return "std::uint16_t";
	case PropertyKind::UInt32: return "std::uint32_t";
	case PropertyKind::UInt64: return "std::uint64_t";
	default: return nullptr;
	}
}

std::string ScalarTypeName(const PropertyKind kind)
{
	switch (kind)
	{
	case PropertyKind::Bool: return "bool";
	case PropertyKind::Int8: return "int8";
	case PropertyKind::Int16: return "int16";
	case PropertyKind::Int32: return "int32";
	case PropertyKind::Int64: return "int64";
	case PropertyKind::UInt8: return "uint8";
	case PropertyKind::UInt16: return "uint16";
	case PropertyKind::UInt32: return "uint32";
	case PropertyKind::UInt64: return "uint64";
	case PropertyKind::Float: return "float";
	case PropertyKind::Double: return "double";
	case PropertyKind::Name: return "FName";
	case PropertyKind::String: return "FString";
	case PropertyKind::Text: return "FText";
	case PropertyKind::Delegate: return "FDelegate";
	default: return "std::byte";
	}
}

struct TypeNameIndex
{
	std::map<std::string, std::string, std::less<>> ByPath;
};

TypeNameIndex BuildTypeNameIndex(const Runtime::TypeSnapshot& types)
{
	std::map<std::string, std::size_t, std::less<>> counts;
	for (const ReflectedType& type : types.Types())
		++counts[type.Name];

	TypeNameIndex result;
	std::set<std::string> issued;
	for (const ReflectedType& type : types.Types())
	{
		std::string display = type.Name;
		if (counts[type.Name] > 1)
		{
			const std::string package = LastPathToken(type.PackagePath);
			display = (package.empty() ? "Package" : package) + "::" + type.Name;
		}
		if (!issued.emplace(display).second)
			display += "::" + std::to_string(type.Handle.Index);
		result.ByPath.emplace(type.FullPath, std::move(display));
	}
	return result;
}

std::string DisplayTypeName(
	const TypeNameIndex& names,
	const std::string_view fullPath)
{
	const auto found = names.ByPath.find(fullPath);
	return found == names.ByPath.end()
		? LastPathToken(fullPath)
		: found->second;
}

json DumpspaceMemberType(
	const PropertyKind kind,
	const std::string_view typeName,
	const std::shared_ptr<const PropertyDescriptor>& descriptor,
	const TypeNameIndex& names,
	const std::size_t depth = 0)
{
	if (depth > Runtime::TypeSnapshotStore::kMaxDescriptorDepth)
		return json::array({"std::byte", "D", "", json::array()});

	std::string name = ScalarTypeName(kind);
	std::string category = "D";
	std::string extension;
	json subtypes = json::array();
	if (kind == PropertyKind::Struct)
	{
		name = DisplayTypeName(names, typeName);
		category = "S";
	}
	else if (kind == PropertyKind::Enum)
	{
		name = DisplayTypeName(names, typeName);
		category = "E";
	}
	else if (kind == PropertyKind::Object)
	{
		name = typeName.empty() || typeName == "object"
			? "UObject"
			: DisplayTypeName(names, typeName);
		category = "C";
		extension = "*";
	}
	else if (kind == PropertyKind::WeakObject || kind == PropertyKind::SoftObject)
	{
		name = kind == PropertyKind::WeakObject ? "TWeakObjectPtr" : "TSoftObjectPtr";
		category = "C";
		if (descriptor && !descriptor->TypeName.empty())
		{
			subtypes.push_back(json::array({
				DisplayTypeName(names, descriptor->TypeName), "C", "*", json::array()}));
		}
	}
	else if (kind == PropertyKind::Array)
	{
		name = "TArray";
		category = "C";
		if (descriptor && descriptor->Element)
		{
			subtypes.push_back(DumpspaceMemberType(
				descriptor->Element->Kind,
				descriptor->Element->TypeName,
				descriptor->Element,
				names,
				depth + 1));
		}
	}
	else if (kind == PropertyKind::Map)
	{
		name = "TMap";
		category = "C";
		if (descriptor && descriptor->Key && descriptor->Mapped)
		{
			subtypes.push_back(DumpspaceMemberType(
				descriptor->Key->Kind,
				descriptor->Key->TypeName,
				descriptor->Key,
				names,
				depth + 1));
			subtypes.push_back(DumpspaceMemberType(
				descriptor->Mapped->Kind,
				descriptor->Mapped->TypeName,
				descriptor->Mapped,
				names,
				depth + 1));
		}
	}
	else if (kind == PropertyKind::Set)
	{
		name = "TSet";
		category = "C";
		if (descriptor && descriptor->Element)
		{
			subtypes.push_back(DumpspaceMemberType(
				descriptor->Element->Kind,
				descriptor->Element->TypeName,
				descriptor->Element,
				names,
				depth + 1));
		}
	}
	return json::array({name, category, extension, std::move(subtypes)});
}

std::vector<std::string> SuperNames(
	const ReflectedType& type,
	const Runtime::TypeSnapshot& types,
	const TypeNameIndex& names)
{
	std::vector<std::string> result;
	std::set<std::int32_t> visited;
	const ReflectedType* current = &type;
	while (current && current->Super)
	{
		if (!visited.emplace(current->Super->Index).second)
			break;
		current = types.FindByObjectIndex(current->Super->Index);
		if (!current)
			break;
		result.push_back(DisplayTypeName(names, current->FullPath));
		if (result.size() >= Runtime::TypeSnapshotStore::kMaxHierarchyDepth)
			break;
	}
	return result;
}

bool ReportTypeProgress(
	Runtime::IDumpJobExecutionContext& context,
	const char* phase,
	const std::size_t completed,
	const std::size_t total)
{
	if (completed != total && completed % 128 != 0)
		return !context.IsCancellationRequested();
	return context.ReportProgress(DumpJobProgress{
		.Phase = phase,
		.Completed = static_cast<std::uint64_t>(completed),
		.Total = static_cast<std::uint64_t>(total),
		.Message = "Processing immutable reflected type records."});
}

GenerationFailure GenerateDumpspace(
	const SnapshotDumpInput& input,
	const SnapshotDumpWorkerLimits& limits,
	Runtime::IDumpJobExecutionContext& context,
	std::vector<GeneratedArtifact>& artifacts)
{
	const std::string updatedAt = std::to_string(input.Types->CapturedAtMonotonicUs());
	JsonArrayDocument offsets(limits.MaxArtifactBytes, updatedAt, true);
	JsonArrayDocument classes(limits.MaxArtifactBytes, updatedAt);
	JsonArrayDocument functions(limits.MaxArtifactBytes, updatedAt);
	JsonArrayDocument structs(limits.MaxArtifactBytes, updatedAt);
	JsonArrayDocument enums(limits.MaxArtifactBytes, updatedAt);
	if (!offsets.Valid() || !classes.Valid() || !functions.Valid()
		|| !structs.Valid() || !enums.Valid())
	{
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "Dumpspace document headers exceed the artifact limit."};
	}

	for (const auto& [name, report] : input.Context->Offsets())
	{
		if (!report.IsValidated())
			continue;
		if (!offsets.Add(json::array({name, report.Value})))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "OffsetsInfo.json exceeds the artifact limit."};
	}

	const TypeNameIndex names = BuildTypeNameIndex(*input.Types);
	std::size_t completed = 0;
	for (const ReflectedType& type : input.Types->Types())
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "Dumpspace generation was cancelled."};
		const std::string displayName = DisplayTypeName(names, type.FullPath);
		if (type.Kind == ReflectedTypeKind::Enum)
		{
			json members = json::array();
			for (const auto& entry : type.EnumEntries)
				members.push_back(json{{entry.Name, entry.Value}});
			const char* underlying = IntegerCppType(type.EnumUnderlyingKind);
			json encoded = {{displayName, json::array({
				std::move(members),
				underlying ? underlying : "unavailable"})}};
			if (!enums.Add(encoded))
				return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "EnumsInfo.json exceeds the artifact limit."};
		}
		else
		{
			json members = json::array();
			members.push_back(json{{"__InheritInfo", SuperNames(type, *input.Types, names)}});
			members.push_back(json{{"__MDKClassSize", type.PropertiesSize}});
			for (const ReflectedProperty& property : type.DirectProperties)
			{
				const std::uint64_t totalSize = static_cast<std::uint64_t>(property.Size)
					* static_cast<std::uint64_t>(property.ArrayDim);
				if (totalSize > (std::numeric_limits<std::uint32_t>::max)())
					return {"DUMP_DUMPSPACE_MEMBER_SIZE_EXCEEDED", "A reflected member size cannot be represented by Dumpspace."};
				json definition = json::array({
					DumpspaceMemberType(
						property.Kind,
						property.TypeName,
						property.Descriptor,
						names),
					property.Offset,
					static_cast<std::uint32_t>(totalSize),
					property.ArrayDim});
				if (property.Kind == PropertyKind::Bool && property.Descriptor)
					definition.push_back(property.Descriptor->BoolMask);
				members.push_back(json{{property.Name, std::move(definition)}});
			}
			json encoded = {{displayName, std::move(members)}};
			JsonArrayDocument& target = type.Kind == ReflectedTypeKind::Class
				? classes
				: structs;
			if (!target.Add(encoded))
				return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "A Dumpspace type document exceeds the artifact limit."};

			if (!type.DirectFunctions.empty())
			{
				json functionList = json::array();
				for (const ReflectedFunction& function : type.DirectFunctions)
				{
					json parameters = json::array();
					json returnType = json::array({"void", "D", "", json::array()});
					for (const ReflectedParameter& parameter : function.Parameters)
					{
						json memberType = DumpspaceMemberType(
							parameter.Property.Kind,
							parameter.Property.TypeName,
							parameter.Property.Descriptor,
							names);
						if (parameter.Direction == ReflectedParameterDirection::Return)
						{
							returnType = std::move(memberType);
							continue;
						}
						parameters.push_back(json::array({
							std::move(memberType),
							parameter.Direction == ReflectedParameterDirection::InOut ? "&" : "",
							parameter.Property.Name}));
					}
					const std::uint64_t rva = function.NativeAddress >= input.Context->ModuleBase()
						? static_cast<std::uint64_t>(function.NativeAddress - input.Context->ModuleBase())
						: 0;
					functionList.push_back(json{{function.Name, json::array({
						std::move(returnType),
						std::move(parameters),
						rva,
						Hex(function.Flags)})}});
				}
				if (!functions.Add(json{{displayName, std::move(functionList)}}))
					return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "FunctionsInfo.json exceeds the artifact limit."};
			}
		}
		++completed;
		if (!ReportTypeProgress(context, "dumpspace", completed, input.Types->Types().size()))
			return {"DUMP_WORKER_CANCELLED", "Dumpspace generation was cancelled."};
	}

	artifacts.push_back({"OffsetsInfo.json", ArtifactKind::Json, offsets.Finish(), 1});
	artifacts.push_back({"ClassesInfo.json", ArtifactKind::Json, classes.Finish(), 1});
	artifacts.push_back({"FunctionsInfo.json", ArtifactKind::Json, functions.Finish(), 1});
	artifacts.push_back({"StructsInfo.json", ArtifactKind::Json, structs.Finish(), 1});
	artifacts.push_back({"EnumsInfo.json", ArtifactKind::Json, enums.Finish(), 1});
	if (std::any_of(artifacts.end() - 5, artifacts.end(), [](const GeneratedArtifact& artifact) {
		return artifact.Bytes.empty();
	}))
	{
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "A Dumpspace document could not be finalized within its limit."};
	}
	return {};
}

GenerationFailure GenerateSdk(
	const SnapshotDumpInput& input,
	const SnapshotDumpWorkerLimits& limits,
	Runtime::IDumpJobExecutionContext& context,
	std::vector<GeneratedArtifact>& artifacts)
{
	struct TypeIndex
	{
		std::map<std::string, std::string, std::less<>> ByPath;
		std::map<std::int32_t, std::string> ByObjectIndex;
		std::map<std::string, std::string, std::less<>> ByLookup;
		std::map<std::string, const ReflectedType*, std::less<>> Records;
	};
	struct ValueType
	{
		std::string Name;
		std::uint32_t Size = 0;
	};
	struct LayoutMember
	{
		const ReflectedProperty* Property = nullptr;
		std::string Direction;
	};
	struct FunctionRecord
	{
		const ReflectedType* Owner = nullptr;
		const ReflectedFunction* Function = nullptr;
		std::string Name;
	};

	const auto isKeyword = [](const std::string_view value) noexcept {
		static constexpr std::array<std::string_view, 95> keywords{
			"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
			"bitor", "bool", "break", "case", "catch", "char", "char8_t",
			"char16_t", "char32_t", "class", "compl", "concept", "const",
			"consteval", "constexpr", "constinit", "const_cast", "continue",
			"co_await", "co_return", "co_yield", "decltype", "default", "delete",
			"do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
			"extern", "false", "float", "for", "friend", "goto", "if", "inline",
			"int", "long", "mutable", "namespace", "new", "noexcept", "not",
			"not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
			"public", "register", "reinterpret_cast", "requires", "return", "short",
			"signed", "sizeof", "static", "static_assert", "static_cast", "struct",
			"switch", "template", "this", "thread_local", "throw", "true", "try",
			"typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
			"void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
		return std::find(keywords.begin(), keywords.end(), value) != keywords.end();
	};
	const auto identifier = [&](const std::string_view value, const std::string_view fallback) {
		std::string result;
		result.reserve((std::min)(value.size(), std::size_t{96}) + 8);
		for (const unsigned char character : value)
		{
			if (result.size() >= 96)
				break;
			const bool valid = (character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9');
			result.push_back(valid ? static_cast<char>(character) : '_');
		}
		if (result.empty())
			result.assign(fallback);
		if (result.front() >= '0' && result.front() <= '9')
			result.insert(result.begin(), '_');
		if (isKeyword(result)
			|| (result.size() >= 2 && result[0] == '_'
				&& (result[1] == '_' || (result[1] >= 'A' && result[1] <= 'Z'))))
		{
			result.insert(0, "Sdk");
		}
		return result;
	};
	const auto uniqueIdentifier = [&](std::string base, std::set<std::string>& used, const std::uint64_t suffix) {
		if (used.emplace(base).second)
			return base;
		const std::string original = base;
		base += "_" + std::to_string(suffix);
		std::uint64_t collision = 2;
		while (!used.emplace(base).second)
			base = original + "_" + std::to_string(suffix) + "_" + std::to_string(collision++);
		return base;
	};

	TypeIndex names;
	std::map<std::string, std::size_t, std::less<>> lookupCounts;
	for (const ReflectedType& type : input.Types->Types())
	{
		std::set<std::string> tokens{type.Name, LastPathToken(type.FullPath)};
		for (const std::string& token : tokens)
			++lookupCounts[token];
	}
	std::set<std::string> issuedTypeNames;
	for (const ReflectedType& type : input.Types->Types())
	{
		const std::uint64_t suffix = static_cast<std::uint64_t>(
			static_cast<std::uint32_t>(type.Handle.Index));
		const std::string symbol = uniqueIdentifier(
			identifier(type.Name, "Type"),
			issuedTypeNames,
			suffix);
		names.ByPath.emplace(type.FullPath, symbol);
		names.ByObjectIndex.emplace(type.Handle.Index, symbol);
		names.Records.emplace(type.FullPath, &type);
		std::set<std::string> tokens{type.Name, LastPathToken(type.FullPath)};
		for (const std::string& token : tokens)
		{
			if (lookupCounts[token] == 1)
				names.ByLookup.emplace(token, symbol);
		}
	}
	const auto findSymbol = [&](const std::string_view typeName) -> const std::string* {
		const auto exact = names.ByPath.find(typeName);
		if (exact != names.ByPath.end())
			return &exact->second;
		const auto lookup = names.ByLookup.find(typeName);
		return lookup == names.ByLookup.end() ? nullptr : &lookup->second;
	};
	const auto findType = [&](const std::string_view typeName) -> const ReflectedType* {
		const auto exact = names.Records.find(typeName);
		if (exact != names.Records.end())
			return exact->second;
		const std::string* symbol = findSymbol(typeName);
		if (!symbol)
			return nullptr;
		for (const ReflectedType& candidate : input.Types->Types())
		{
			const auto byPath = names.ByPath.find(candidate.FullPath);
			if (byPath != names.ByPath.end() && byPath->second == *symbol)
				return &candidate;
		}
		return nullptr;
	};
	const auto scalarSize = [](const PropertyKind kind) noexcept -> std::uint32_t {
		switch (kind)
		{
		case PropertyKind::Int8:
		case PropertyKind::UInt8: return 1;
		case PropertyKind::Int16:
		case PropertyKind::UInt16: return 2;
		case PropertyKind::Int32:
		case PropertyKind::UInt32:
		case PropertyKind::Float: return 4;
		case PropertyKind::Int64:
		case PropertyKind::UInt64:
		case PropertyKind::Double: return 8;
		default: return 0;
		}
	};
	const auto storageType = [](const std::uint32_t size) {
		return ValueType{"::UExplorerSDK::TStorage<" + std::to_string(size) + ">", size};
	};
	const std::int32_t fNameSize = input.Context->NameProfile().Validated
		? input.Context->NameProfile().FNameSize
		: -1;

	auto valueType = [&](auto&& self,
		const PropertyKind kind,
		const std::string_view typeName,
		const std::shared_ptr<const PropertyDescriptor>& descriptor,
		const std::uint32_t size,
		const std::set<std::string>& definedTypes,
		const bool allTypesDefined,
		const std::size_t depth) -> ValueType {
		if (depth > Runtime::TypeSnapshotStore::kMaxDescriptorDepth)
			return storageType(size);
		if (const char* scalar = IntegerCppType(kind))
			return scalarSize(kind) == size ? ValueType{scalar, size} : storageType(size);
		if (kind == PropertyKind::Float)
			return size == sizeof(float) ? ValueType{"float", size} : storageType(size);
		if (kind == PropertyKind::Double)
			return size == sizeof(double) ? ValueType{"double", size} : storageType(size);
		if (kind == PropertyKind::Bool)
			return size == 1 ? ValueType{"std::uint8_t", size} : storageType(size);
		if (kind == PropertyKind::Name)
		{
			return fNameSize > 0 && size == static_cast<std::uint32_t>(fNameSize)
				? ValueType{"::UExplorerSDK::FName", size}
				: ValueType{"::UExplorerSDK::TFName<" + std::to_string(size) + ">", size};
		}
		if (kind == PropertyKind::String)
		{
			return size == 16
				? ValueType{"::UExplorerSDK::FString", size}
				: ValueType{"::UExplorerSDK::TString<" + std::to_string(size) + ">", size};
		}
		if (kind == PropertyKind::Text)
			return ValueType{"::UExplorerSDK::TText<" + std::to_string(size) + ">", size};
		if (kind == PropertyKind::Object)
		{
			if (size != sizeof(std::uintptr_t))
				return storageType(size);
			const std::string* target = findSymbol(typeName);
			return ValueType{target
				? "::UExplorerSDK::Types::" + *target + "*"
				: "void*", size};
		}
		if (kind == PropertyKind::WeakObject || kind == PropertyKind::SoftObject)
		{
			const std::string referenced = descriptor && !descriptor->TypeName.empty()
				? descriptor->TypeName
				: std::string(typeName);
			const std::string* target = findSymbol(referenced);
			const std::string pointee = target
				? "::UExplorerSDK::Types::" + *target
				: "void";
			if (kind == PropertyKind::WeakObject && size == 8)
				return ValueType{"::UExplorerSDK::TWeakObjectPtr<" + pointee + ">", size};
			return ValueType{
				kind == PropertyKind::WeakObject
					? "::UExplorerSDK::TWeakObjectStorage<" + pointee + ", " + std::to_string(size) + ">"
					: "::UExplorerSDK::TSoftObjectPtr<" + pointee + ", " + std::to_string(size) + ">",
				size};
		}
		if (kind == PropertyKind::Enum)
		{
			const ReflectedType* targetType = findType(typeName);
			const std::string* target = findSymbol(typeName);
			if (targetType && target && targetType->Kind == ReflectedTypeKind::Enum
				&& IntegerCppType(targetType->EnumUnderlyingKind)
				&& scalarSize(targetType->EnumUnderlyingKind) == size)
			{
				return ValueType{"::UExplorerSDK::Types::" + *target, size};
			}
			return storageType(size);
		}
		if (kind == PropertyKind::Struct)
		{
			const ReflectedType* targetType = findType(typeName);
			const std::string* target = findSymbol(typeName);
			if (!targetType || !target || targetType->Kind == ReflectedTypeKind::Enum
				|| targetType->PropertiesSize != size)
			{
				return storageType(size);
			}
			const std::string qualified = "::UExplorerSDK::Types::" + *target;
			if (allTypesDefined || definedTypes.contains(targetType->FullPath))
				return ValueType{qualified, size};
			return ValueType{
				"::UExplorerSDK::TInlineObject<" + qualified + ", " + std::to_string(size) + ">",
				size};
		}
		if (kind == PropertyKind::Array && descriptor && descriptor->Element && size == 16)
		{
			const ValueType element = self(
				self,
				descriptor->Element->Kind,
				descriptor->Element->TypeName,
				descriptor->Element,
				descriptor->Element->Size,
				definedTypes,
				true,
				depth + 1);
			return ValueType{"::UExplorerSDK::TArray<" + element.Name + ">", size};
		}
		if (kind == PropertyKind::Map && descriptor && descriptor->Key && descriptor->Mapped)
		{
			const ValueType key = self(
				self, descriptor->Key->Kind, descriptor->Key->TypeName, descriptor->Key,
				descriptor->Key->Size, definedTypes, true, depth + 1);
			const ValueType mapped = self(
				self, descriptor->Mapped->Kind, descriptor->Mapped->TypeName, descriptor->Mapped,
				descriptor->Mapped->Size, definedTypes, true, depth + 1);
			return ValueType{
				"::UExplorerSDK::TMap<" + key.Name + ", " + mapped.Name + ", "
					+ std::to_string(size) + ">",
				size};
		}
		if (kind == PropertyKind::Set && descriptor && descriptor->Element)
		{
			const ValueType element = self(
				self, descriptor->Element->Kind, descriptor->Element->TypeName, descriptor->Element,
				descriptor->Element->Size, definedTypes, true, depth + 1);
			return ValueType{
				"::UExplorerSDK::TSet<" + element.Name + ", " + std::to_string(size) + ">",
				size};
		}
		if (kind == PropertyKind::Delegate)
			return ValueType{"::UExplorerSDK::TDelegate<" + std::to_string(size) + ">", size};
		return storageType(size);
	};

	const auto appendLayout = [&](BoundedText& output,
		const std::string& name,
		const std::uint32_t ownerSize,
		const std::uint32_t alignment,
		const std::vector<LayoutMember>& sourceMembers,
		const ReflectedType* superType,
		const std::set<std::string>& definedTypes,
		const bool allTypesDefined) {
		struct PreparedMember
		{
			const ReflectedProperty* Property = nullptr;
			std::string Field;
			std::string Getter;
			std::string Setter;
			std::string Actual;
			std::string Direction;
			ValueType Type;
			std::size_t Ordinal = 0;
			bool Emitted = false;
		};
		std::vector<PreparedMember> members;
		members.reserve(sourceMembers.size());
		std::set<std::string> memberNames{"_Super"};
		std::size_t ordinal = 0;
		for (const LayoutMember& member : sourceMembers)
		{
			if (!member.Property)
				continue;
			PreparedMember prepared;
			prepared.Property = member.Property;
			prepared.Field = uniqueIdentifier(
				identifier(member.Property->Name, "Field"),
				memberNames,
				ordinal + 1);
			prepared.Direction = member.Direction;
			prepared.Ordinal = ordinal++;
			prepared.Type = valueType(
				valueType,
				member.Property->Kind,
				member.Property->TypeName,
				member.Property->Descriptor,
				member.Property->Size,
				definedTypes,
				allTypesDefined,
				0);
			members.push_back(std::move(prepared));
		}
		for (PreparedMember& member : members)
		{
			member.Getter = uniqueIdentifier(
				"Get_" + member.Field,
				memberNames,
				member.Ordinal + 1);
			if (member.Property->Kind == PropertyKind::Bool)
			{
				member.Setter = uniqueIdentifier(
					"Set_" + member.Field,
					memberNames,
					member.Ordinal + 1);
			}
		}
		std::vector<std::size_t> layoutOrder(members.size());
		for (std::size_t index = 0; index < layoutOrder.size(); ++index)
			layoutOrder[index] = index;
		std::sort(layoutOrder.begin(), layoutOrder.end(), [&](const std::size_t left, const std::size_t right) {
			const ReflectedProperty& a = *members[left].Property;
			const ReflectedProperty& b = *members[right].Property;
			return a.Offset != b.Offset ? a.Offset < b.Offset : members[left].Ordinal < members[right].Ordinal;
		});

		std::ostringstream block;
		block << "struct alignas(" << alignment << ") " << name << "\n{\n";
		std::uint64_t cursor = 0;
		if (superType && superType->Kind != ReflectedTypeKind::Enum
			&& superType->PropertiesSize <= ownerSize
			&& definedTypes.contains(superType->FullPath))
		{
			const std::string* superName = findSymbol(superType->FullPath);
			if (superName)
			{
				block << "\t::UExplorerSDK::Types::" << *superName
					<< " _Super; // 0x0 inherited layout\n";
				cursor = superType->PropertiesSize;
			}
		}
		std::size_t paddingOrdinal = 0;
		for (const std::size_t memberIndex : layoutOrder)
		{
			PreparedMember& member = members[memberIndex];
			const ReflectedProperty& property = *member.Property;
			const std::uint64_t totalSize = static_cast<std::uint64_t>(property.Size)
				* property.ArrayDim;
			if (property.Offset < cursor)
				continue;
			if (property.Offset > cursor)
			{
				block << "\tstd::byte _Pad_" << std::hex << std::uppercase << cursor
					<< "_" << std::dec << paddingOrdinal++ << "[" << (property.Offset - cursor)
					<< "];\n";
				cursor = property.Offset;
			}
			const bool bitBool = property.Kind == PropertyKind::Bool
				&& property.Descriptor
				&& property.Descriptor->Kind == PropertyKind::Bool
				&& property.Descriptor->BoolMask != 0;
			const bool directBool = bitBool
				&& property.Descriptor->BoolByteOffset == 0
				&& property.Descriptor->BoolMask == 0xFF
				&& property.Size == 1;
			if (bitBool && !directBool)
			{
				member.Actual = uniqueIdentifier(
					member.Field + "_Storage",
					memberNames,
					member.Ordinal + 1);
				block << "\tstd::byte " << member.Actual;
				if (totalSize != 1)
					block << "[" << totalSize << "]";
			}
			else
			{
				member.Actual = member.Field;
				block << "\t" << (directBool ? "bool" : member.Type.Name)
					<< " " << member.Actual;
				if (property.ArrayDim != 1)
					block << "[" << property.ArrayDim << "]";
			}
			block << "; // " << Hex(property.Offset) << " (" << Hex(totalSize) << ")";
			if (!member.Direction.empty())
				block << " " << member.Direction;
			block << "\n";
			member.Emitted = true;
			cursor += totalSize;
		}
		if (cursor < ownerSize)
		{
			block << "\tstd::byte _Pad_" << std::hex << std::uppercase << cursor
				<< "_" << std::dec << paddingOrdinal++ << "[" << (ownerSize - cursor)
				<< "];\n";
		}
		block << "\n";
		for (const PreparedMember& member : members)
		{
			const ReflectedProperty& property = *member.Property;
			const bool bitBool = property.Kind == PropertyKind::Bool
				&& property.Descriptor
				&& property.Descriptor->Kind == PropertyKind::Bool
				&& property.Descriptor->BoolMask != 0;
			const std::string indexParameter = property.ArrayDim == 1
				? std::string{}
				: "std::size_t index";
			const std::string indexExpression = property.ArrayDim == 1
				? std::string{}
				: " + index * " + std::to_string(property.Size);
			if (bitBool)
			{
				const std::uint64_t byteOffset = static_cast<std::uint64_t>(property.Offset)
					+ property.Descriptor->BoolByteOffset;
				block << "\tbool " << member.Getter << "(" << indexParameter
					<< ") const noexcept\n\t{\n"
					<< "\t\tconst auto* bytes = reinterpret_cast<const std::uint8_t*>(this);\n"
					<< "\t\treturn (bytes[" << byteOffset << indexExpression << "] & "
					<< static_cast<std::uint32_t>(property.Descriptor->BoolMask) << ") != 0;\n\t}\n"
					<< "\tvoid " << member.Setter << "(";
				if (property.ArrayDim != 1)
					block << "std::size_t index, ";
				block << "bool value) noexcept\n\t{\n"
					<< "\t\tauto* bytes = reinterpret_cast<std::uint8_t*>(this);\n"
					<< "\t\tauto& byte = bytes[" << byteOffset << indexExpression << "];\n"
					<< "\t\tif (value) byte |= "
					<< static_cast<std::uint32_t>(property.Descriptor->BoolMask)
					<< "; else byte &= static_cast<std::uint8_t>(~"
					<< static_cast<std::uint32_t>(property.Descriptor->BoolMask) << ");\n\t}\n";
			}
			else
			{
				block << "\t" << member.Type.Name << "& " << member.Getter << "("
					<< indexParameter << ") noexcept\n\t{\n"
					<< "\t\treturn *reinterpret_cast<" << member.Type.Name
					<< "*>(reinterpret_cast<std::byte*>(this) + " << property.Offset
					<< indexExpression << ");\n\t}\n"
					<< "\tconst " << member.Type.Name << "& " << member.Getter << "("
					<< indexParameter << ") const noexcept\n\t{\n"
					<< "\t\treturn *reinterpret_cast<const " << member.Type.Name
					<< "*>(reinterpret_cast<const std::byte*>(this) + " << property.Offset
					<< indexExpression << ");\n\t}\n";
			}
		}
		block << "};\nstatic_assert(sizeof(" << name << ") == " << ownerSize << ");\n"
			<< "static_assert(alignof(" << name << ") == " << alignment << ");\n";
		if (superType && definedTypes.contains(superType->FullPath)
			&& superType->PropertiesSize <= ownerSize)
		{
			block << "static_assert(offsetof(" << name << ", _Super) == 0);\n";
		}
		for (const PreparedMember& member : members)
		{
			const ReflectedProperty& property = *member.Property;
			if (member.Emitted)
			{
				block << "static_assert(offsetof(" << name << ", " << member.Actual
					<< ") == " << property.Offset << ");\n";
			}
			block << "inline constexpr std::size_t " << name << "_" << member.Field
				<< "_Offset = " << property.Offset << ";\n"
				<< "inline constexpr std::size_t " << name << "_" << member.Field
				<< "_Size = " << property.Size << ";\n"
				<< "inline constexpr std::size_t " << name << "_" << member.Field
				<< "_ArrayDim = " << property.ArrayDim << ";\n";
			if (property.Kind == PropertyKind::Bool && property.Descriptor
				&& property.Descriptor->Kind == PropertyKind::Bool)
			{
				block << "inline constexpr std::uint8_t " << name << "_" << member.Field
					<< "_Mask = " << static_cast<std::uint32_t>(property.Descriptor->BoolMask)
					<< ";\n";
			}
		}
		block << "\n";
		return output.Append(block.str());
	};

	std::vector<const ReflectedType*> layoutTypes;
	std::map<std::int32_t, std::size_t> layoutByIndex;
	std::map<std::string, std::size_t, std::less<>> layoutByPath;
	for (const ReflectedType& type : input.Types->Types())
	{
		if (type.Kind == ReflectedTypeKind::Enum)
			continue;
		layoutByIndex.emplace(type.Handle.Index, layoutTypes.size());
		layoutByPath.emplace(type.FullPath, layoutTypes.size());
		layoutTypes.push_back(&type);
	}
	std::vector<std::uint8_t> visitState(layoutTypes.size(), 0);
	std::vector<const ReflectedType*> definitionOrder;
	definitionOrder.reserve(layoutTypes.size());
	auto visit = [&](auto&& self, const std::size_t index) -> void {
		if (visitState[index] == 2)
			return;
		if (visitState[index] == 1)
			return;
		visitState[index] = 1;
		const ReflectedType& type = *layoutTypes[index];
		if (type.Super)
		{
			const auto dependency = layoutByIndex.find(type.Super->Index);
			if (dependency != layoutByIndex.end())
				self(self, dependency->second);
		}
		for (const ReflectedProperty& property : type.DirectProperties)
		{
			if (property.Kind != PropertyKind::Struct)
				continue;
			const ReflectedType* dependencyType = findType(property.TypeName);
			if (!dependencyType || dependencyType->Kind == ReflectedTypeKind::Enum)
				continue;
			const auto dependency = layoutByPath.find(dependencyType->FullPath);
			if (dependency != layoutByPath.end())
				self(self, dependency->second);
		}
		visitState[index] = 2;
		definitionOrder.push_back(&type);
	};
	for (std::size_t index = 0; index < layoutTypes.size(); ++index)
		visit(visit, index);

	std::vector<FunctionRecord> functions;
	std::set<std::string> issuedFunctionNames;
	for (const ReflectedType& type : input.Types->Types())
	{
		const std::string* ownerName = findSymbol(type.FullPath);
		if (!ownerName)
			continue;
		for (const ReflectedFunction& function : type.DirectFunctions)
		{
			const std::uint64_t suffix = static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(function.Handle.Function.Index));
			functions.push_back({
				.Owner = &type,
				.Function = &function,
				.Name = uniqueIdentifier(
					*ownerName + "_" + identifier(function.Name, "Function"),
					issuedFunctionNames,
					suffix)});
		}
	}

	BoundedText text(limits.MaxArtifactBytes);
	std::ostringstream preamble;
	preamble << "#pragma once\n\n"
		<< "#include <cstddef>\n#include <cstdint>\n#include <cstring>\n#include <string_view>\n\n"
		<< "namespace UExplorerSDK\n{\n"
		<< "inline constexpr char kSchema[] = \"" << kSdkSchema << "\";\n"
		<< "inline constexpr char kTargetGame[] = " << json(input.Context->GameName()).dump() << ";\n"
		<< "inline constexpr char kTargetVersion[] = " << json(input.Context->GameVersion()).dump() << ";\n"
		<< "inline constexpr std::uint64_t kContextGeneration = " << input.Context->Generation() << ";\n"
		<< "inline constexpr std::uint64_t kTypeSnapshotGeneration = " << input.Types->Generation() << ";\n"
		<< "static_assert(sizeof(void*) == 8, \"UExplorer SDK requires a 64-bit target\");\n\n"
		<< "template<std::size_t Size>\nstruct TStorage\n{\n"
		<< "\tstatic_assert(Size > 0);\n\tstd::byte Data[Size];\n"
		<< "\ttemplate<typename T> T& As() noexcept { return *reinterpret_cast<T*>(Data); }\n"
		<< "\ttemplate<typename T> const T& As() const noexcept { return *reinterpret_cast<const T*>(Data); }\n};\n\n"
		<< "template<typename T, std::size_t Size>\nstruct TInlineObject\n{\n"
		<< "\tstd::byte Data[Size];\n\tT& Get() noexcept { return *reinterpret_cast<T*>(Data); }\n"
		<< "\tconst T& Get() const noexcept { return *reinterpret_cast<const T*>(Data); }\n};\n\n"
		<< "template<typename T>\nstruct TArray\n{\n"
		<< "\tT* Data = nullptr;\n\tstd::int32_t Num = 0;\n\tstd::int32_t Max = 0;\n"
		<< "\tbool IsValidIndex(std::int32_t index) const noexcept { return index >= 0 && index < Num; }\n"
		<< "\tT& operator[](std::int32_t index) noexcept { return Data[index]; }\n"
		<< "\tconst T& operator[](std::int32_t index) const noexcept { return Data[index]; }\n};\n"
		<< "static_assert(sizeof(TArray<std::byte>) == 16);\n\n"
		<< "struct FString : TArray<wchar_t>\n{\n"
		<< "\tstd::wstring_view View() const noexcept\n\t{\n"
		<< "\t\tconst std::size_t length = Num > 0 && Data && Data[Num - 1] == L'\\0'\n"
		<< "\t\t\t? static_cast<std::size_t>(Num - 1) : static_cast<std::size_t>(Num > 0 ? Num : 0);\n"
		<< "\t\treturn Data ? std::wstring_view(Data, length) : std::wstring_view{};\n\t}\n};\n"
		<< "static_assert(sizeof(FString) == 16);\n\n"
		<< "template<std::size_t Size> using TFName = TStorage<Size>;\n"
		<< "template<std::size_t Size> using TString = TStorage<Size>;\n"
		<< "template<std::size_t Size> using TText = TStorage<Size>;\n"
		<< "template<std::size_t Size> using TDelegate = TStorage<Size>;\n"
		<< "template<typename T, std::size_t Size>\nstruct TObjectStorage\n{\n"
		<< "\tstd::byte Data[Size];\n\tT* Get() noexcept { return reinterpret_cast<T*>(Data); }\n"
		<< "\tconst T* Get() const noexcept { return reinterpret_cast<const T*>(Data); }\n};\n"
		<< "template<typename T, std::size_t Size> using TWeakObjectStorage = TObjectStorage<T, Size>;\n"
		<< "template<typename T, std::size_t Size> using TSoftObjectPtr = TObjectStorage<T, Size>;\n"
		<< "template<typename K, typename V, std::size_t Size> using TMap = TStorage<Size>;\n"
		<< "template<typename T, std::size_t Size> using TSet = TStorage<Size>;\n\n"
		<< "template<typename T>\nstruct TWeakObjectPtr\n{\n"
		<< "\tstd::int32_t ObjectIndex = -1;\n\tstd::int32_t ObjectSerialNumber = 0;\n};\n"
		<< "static_assert(sizeof(TWeakObjectPtr<void>) == 8);\n\n";
	if (fNameSize > 0 && fNameSize <= 64)
	{
		const std::uint32_t fNameAlignment = static_cast<std::uint32_t>(fNameSize) % 4 == 0 ? 4 : 1;
		preamble << "struct alignas(" << fNameAlignment << ") FName\n{\n"
			<< "\tstd::byte Data[" << fNameSize << "];\n";
		const std::int32_t comparisonOffset = input.Context->NameProfile().ComparisonIndexOffset;
		const std::int32_t numberOffset = input.Context->NameProfile().NumberOffset;
		if (comparisonOffset >= 0 && comparisonOffset <= fNameSize - 4)
		{
			preamble << "\tstd::uint32_t ComparisonIndex() const noexcept\n\t{\n"
				<< "\t\tstd::uint32_t value = 0; std::memcpy(&value, Data + " << comparisonOffset
				<< ", sizeof(value)); return value;\n\t}\n";
		}
		if (numberOffset >= 0 && numberOffset <= fNameSize - 4)
		{
			preamble << "\tstd::uint32_t Number() const noexcept\n\t{\n"
				<< "\t\tstd::uint32_t value = 0; std::memcpy(&value, Data + " << numberOffset
				<< ", sizeof(value)); return value;\n\t}\n";
		}
		preamble << "};\nstatic_assert(sizeof(FName) == " << fNameSize << ");\n\n";
	}
	preamble << "namespace Offsets\n{\n";
	std::set<std::string> offsetNames;
	for (const auto& [name, report] : input.Context->Offsets())
	{
		if (!report.IsValidated())
			continue;
		const std::string offsetName = uniqueIdentifier(
			identifier(name, "Offset"), offsetNames, offsetNames.size() + 1);
		preamble << "inline constexpr std::ptrdiff_t " << offsetName << " = " << report.Value << ";\n";
	}
	preamble << "} // namespace Offsets\n\n";
	const Runtime::OffsetReport* processEventIndex = input.Context->FindOffset("process_event.index");
	const Runtime::OffsetReport* processEventOffset = input.Context->FindOffset("process_event.offset");
	const std::int64_t peIndex = processEventIndex && processEventIndex->IsValidated()
		? processEventIndex->Value : -1;
	const std::int64_t peOffset = processEventOffset && processEventOffset->IsValidated()
		? processEventOffset->Value : -1;
	preamble << "inline constexpr std::ptrdiff_t kProcessEventIndex = " << peIndex << ";\n"
		<< "inline constexpr std::ptrdiff_t kProcessEventRva = " << peOffset << ";\n"
		<< "using ProcessEventFn = void(*)(void*, void*, void*);\n"
		<< "inline ProcessEventFn ResolveProcessEvent(void* object) noexcept\n{\n"
		<< "\tif (!object || kProcessEventIndex < 0) return nullptr;\n"
		<< "\tauto** table = *reinterpret_cast<void***>(object);\n"
		<< "\treturn table ? reinterpret_cast<ProcessEventFn>(table[kProcessEventIndex]) : nullptr;\n}\n"
		<< "inline bool Invoke(void* object, void* function, void* parameters) noexcept\n{\n"
		<< "\tProcessEventFn processEvent = ResolveProcessEvent(object);\n"
		<< "\tif (!processEvent || !function) return false;\n"
		<< "\tprocessEvent(object, function, parameters);\n\treturn true;\n}\n\n"
		<< "struct FunctionMetadata\n{\n"
		<< "\tconst char* Path;\n\tstd::uint64_t Flags;\n\tstd::uint32_t ParameterSize;\n"
		<< "\tstd::uintptr_t NativeRva;\n\tconst char* Implementation;\n};\n\n"
		<< "#pragma pack(push, 1)\nnamespace Types\n{\n";
	if (!text.Append(preamble.str()))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK preamble exceeds the artifact limit."};

	for (const ReflectedType& type : input.Types->Types())
	{
		const std::string* typeName = findSymbol(type.FullPath);
		if (!typeName
			|| !text.Append("inline constexpr char ")
			|| !text.Append(*typeName)
			|| !text.Append("_Path[] = ")
			|| !text.AppendJson(json(type.FullPath))
			|| !text.Append(";\n"))
		{
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK type identity output exceeds the artifact limit."};
		}
	}
	if (!text.Append("\n"))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK header exceeds the artifact limit."};
	for (const ReflectedType* type : layoutTypes)
	{
		const std::string* typeName = findSymbol(type->FullPath);
		if (!typeName || !text.Append("struct ") || !text.Append(*typeName) || !text.Append(";\n"))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK forward declarations exceed the artifact limit."};
	}
	if (!text.Append("\n"))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK header exceeds the artifact limit."};

	for (const ReflectedType& type : input.Types->Types())
	{
		if (type.Kind != ReflectedTypeKind::Enum)
			continue;
		const char* underlying = IntegerCppType(type.EnumUnderlyingKind);
		const std::string* typeName = findSymbol(type.FullPath);
		if (!underlying || !typeName)
			continue;
		std::ostringstream encoded;
		encoded << "enum class " << *typeName << " : " << underlying << "\n{\n";
		std::set<std::string> entries;
		std::size_t entryOrdinal = 0;
		for (const auto& entry : type.EnumEntries)
		{
			const std::string entryName = uniqueIdentifier(
				identifier(LastPathToken(entry.Name), "Value"), entries, ++entryOrdinal);
			const std::uint64_t raw = std::bit_cast<std::uint64_t>(entry.Value);
			encoded << "\t" << entryName << " = static_cast<" << underlying << ">(" << Hex(raw) << "ULL),\n";
		}
		encoded << "};\nstatic_assert(sizeof(" << *typeName << ") == "
			<< scalarSize(type.EnumUnderlyingKind) << ");\n\n";
		if (!text.Append(encoded.str()))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK enum output exceeds the artifact limit."};
	}

	std::set<std::string> definedTypes;
	std::size_t completed = 0;
	for (const ReflectedType* type : definitionOrder)
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "SDK generation was cancelled."};
		const std::uint32_t alignment = type->MinAlignment == 0 ? 1 : type->MinAlignment;
		if (type->PropertiesSize == 0
			|| !std::has_single_bit(alignment)
			|| type->PropertiesSize % alignment != 0)
		{
			return {"DUMP_SDK_LAYOUT_INVALID", "A reflected type cannot be represented by a typed C++ layout."};
		}
		const std::string* typeName = findSymbol(type->FullPath);
		if (!typeName)
			return {"DUMP_SDK_LAYOUT_INVALID", "A reflected type has no stable C++ identifier."};
		const ReflectedType* superType = nullptr;
		if (type->Super)
			superType = input.Types->FindByObjectIndex(type->Super->Index);
		std::vector<LayoutMember> members;
		members.reserve(type->DirectProperties.size());
		for (const ReflectedProperty& property : type->DirectProperties)
			members.push_back({.Property = &property});
		if (!appendLayout(
			text,
			*typeName,
			type->PropertiesSize,
			alignment,
			members,
			superType,
			definedTypes,
			false))
		{
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK typed layout output exceeds the artifact limit."};
		}
		definedTypes.emplace(type->FullPath);
		++completed;
		if (!ReportTypeProgress(context, "sdk-types", completed, definitionOrder.size()))
			return {"DUMP_WORKER_CANCELLED", "SDK generation was cancelled."};
	}
	if (!text.Append("} // namespace Types\n\nnamespace Params\n{\n"))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK type output exceeds the artifact limit."};

	for (const FunctionRecord& record : functions)
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "SDK function generation was cancelled."};
		std::vector<LayoutMember> parameters;
		parameters.reserve(record.Function->Parameters.size());
		for (const ReflectedParameter& parameter : record.Function->Parameters)
		{
			parameters.push_back({
				.Property = &parameter.Property,
				.Direction = ToString(parameter.Direction)});
		}
		if (record.Function->ParameterSize == 0)
		{
			if (!text.Append("struct ") || !text.Append(record.Name) || !text.Append(" {};\n\n"))
				return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK zero-parameter function output exceeds the artifact limit."};
		}
		else if (!appendLayout(
			text,
			record.Name,
			record.Function->ParameterSize,
			1,
			parameters,
			nullptr,
			definedTypes,
			true))
		{
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK function parameter output exceeds the artifact limit."};
		}
	}
	if (!text.Append("} // namespace Params\n#pragma pack(pop)\n\nnamespace Functions\n{\n"))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK function output exceeds the artifact limit."};
	for (const FunctionRecord& record : functions)
	{
		const std::uint64_t rva = record.Function->NativeAddress >= input.Context->ModuleBase()
			? static_cast<std::uint64_t>(record.Function->NativeAddress - input.Context->ModuleBase())
			: 0;
		std::ostringstream encoded;
		encoded << "inline constexpr FunctionMetadata " << record.Name << " = {"
			<< json(record.Function->FullPath).dump() << ", " << record.Function->Flags << ", "
			<< record.Function->ParameterSize << ", " << Hex(rva) << ", "
			<< json(ToString(record.Function->Implementation)).dump() << "};\n"
			<< "inline bool Invoke_" << record.Name
			<< "(void* object, void* function, Params::" << record.Name << "& parameters) noexcept\n{\n"
			<< "\treturn ::UExplorerSDK::Invoke(object, function, "
			<< (record.Function->ParameterSize == 0 ? "nullptr" : "&parameters") << ");\n}\n\n";
		if (!text.Append(encoded.str()))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK function metadata output exceeds the artifact limit."};
	}
	if (!text.Append("} // namespace Functions\n\n} // namespace UExplorerSDK\n"))
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "SDK header exceeds the artifact limit."};
	artifacts.push_back({
		"UExplorerSDK.hpp",
		ArtifactKind::Sdk,
		text.ReleaseBytes(),
		static_cast<std::uint64_t>(input.Types->Types().size())});
	return {};
}

class BinaryBuffer final
{
public:
	explicit BinaryBuffer(const std::size_t limit) noexcept : m_Limit(limit) {}

	bool U8(const std::uint8_t value) { return Bytes(&value, sizeof(value)); }
	bool U16(const std::uint16_t value)
	{
		const std::array<std::uint8_t, 2> bytes{
			static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8)};
		return Bytes(bytes.data(), bytes.size());
	}
	bool U32(const std::uint32_t value)
	{
		const std::array<std::uint8_t, 4> bytes{
			static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16),
			static_cast<std::uint8_t>(value >> 24)};
		return Bytes(bytes.data(), bytes.size());
	}
	bool I32(const std::int32_t value)
	{
		return U32(static_cast<std::uint32_t>(value));
	}
	bool I64(const std::int64_t value)
	{
		const std::uint64_t raw = std::bit_cast<std::uint64_t>(value);
		std::array<std::uint8_t, 8> bytes{};
		for (std::size_t index = 0; index < bytes.size(); ++index)
			bytes[index] = static_cast<std::uint8_t>(raw >> (index * 8));
		return Bytes(bytes.data(), bytes.size());
	}
	bool Bytes(const void* data, const std::size_t size)
	{
		if ((size != 0 && !data) || size > m_Limit - m_Bytes.size())
			return false;
		if (size == 0)
			return true;
		const auto* first = static_cast<const std::uint8_t*>(data);
		m_Bytes.insert(m_Bytes.end(), first, first + size);
		return true;
	}
	bool Append(const BinaryBuffer& other)
	{
		return Bytes(other.m_Bytes.data(), other.m_Bytes.size());
	}
	std::vector<std::uint8_t> Release() { return std::move(m_Bytes); }

private:
	std::size_t m_Limit = 0;
	std::vector<std::uint8_t> m_Bytes;
};

class UsmapNameTable final
{
public:
	explicit UsmapNameTable(const std::size_t limit) noexcept : m_Limit(limit) {}

	std::optional<std::int32_t> Add(const std::string& value)
	{
		if (value.empty()
			|| value.size() > (std::numeric_limits<std::uint16_t>::max)()
			|| value.size() > m_Limit
			|| m_Bytes > m_Limit - value.size()
			|| m_Names.size() >= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
		{
			return std::nullopt;
		}
		const auto found = m_Indices.find(value);
		if (found != m_Indices.end())
			return found->second;
		const auto index = static_cast<std::int32_t>(m_Names.size());
		m_Bytes += value.size();
		m_Names.push_back(value);
		m_Indices.emplace(m_Names.back(), index);
		return index;
	}

	bool Write(BinaryBuffer& output) const
	{
		if (m_Names.size() > (std::numeric_limits<std::uint32_t>::max)()
			|| !output.U32(static_cast<std::uint32_t>(m_Names.size())))
		{
			return false;
		}
		for (const std::string& name : m_Names)
		{
			if (!output.U16(static_cast<std::uint16_t>(name.size()))
				|| !output.Bytes(name.data(), name.size()))
			{
				return false;
			}
		}
		return true;
	}

private:
	std::size_t m_Limit = 0;
	std::size_t m_Bytes = 0;
	std::vector<std::string> m_Names;
	std::map<std::string, std::int32_t, std::less<>> m_Indices;
};

std::optional<std::uint8_t> UsmapScalarType(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::UInt8: return static_cast<std::uint8_t>(0);
	case PropertyKind::Bool: return static_cast<std::uint8_t>(1);
	case PropertyKind::Int32: return static_cast<std::uint8_t>(2);
	case PropertyKind::Float: return static_cast<std::uint8_t>(3);
	case PropertyKind::Object: return static_cast<std::uint8_t>(4);
	case PropertyKind::Name: return static_cast<std::uint8_t>(5);
	case PropertyKind::Delegate: return static_cast<std::uint8_t>(6);
	case PropertyKind::Double: return static_cast<std::uint8_t>(7);
	case PropertyKind::String: return static_cast<std::uint8_t>(10);
	case PropertyKind::Text: return static_cast<std::uint8_t>(11);
	case PropertyKind::WeakObject: return static_cast<std::uint8_t>(14);
	case PropertyKind::SoftObject: return static_cast<std::uint8_t>(17);
	case PropertyKind::UInt64: return static_cast<std::uint8_t>(18);
	case PropertyKind::UInt32: return static_cast<std::uint8_t>(19);
	case PropertyKind::UInt16: return static_cast<std::uint8_t>(20);
	case PropertyKind::Int64: return static_cast<std::uint8_t>(21);
	case PropertyKind::Int16: return static_cast<std::uint8_t>(22);
	case PropertyKind::Int8: return static_cast<std::uint8_t>(23);
	default: return std::nullopt;
	}
}

GenerationFailure WriteUsmapPropertyType(
	const PropertyKind kind,
	const std::string& typeName,
	const std::shared_ptr<const PropertyDescriptor>& descriptor,
	const TypeNameIndex& typeNames,
	UsmapNameTable& names,
	BinaryBuffer& output,
	const std::size_t depth = 0)
{
	if (depth > Runtime::TypeSnapshotStore::kMaxDescriptorDepth)
		return {"DUMP_USMAP_DESCRIPTOR_DEPTH_EXCEEDED", "A property descriptor exceeds the USMAP recursion limit."};
	if (const auto scalar = UsmapScalarType(kind))
	{
		return output.U8(*scalar)
			? GenerationFailure{}
			: GenerationFailure{"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
	}
	if (kind == PropertyKind::Struct)
	{
		const auto name = names.Add(DisplayTypeName(typeNames, typeName));
		if (!name)
			return {"DUMP_USMAP_NAME_INVALID", "A referenced struct name cannot be represented by USMAP."};
		return output.U8(9) && output.I32(*name)
			? GenerationFailure{}
			: GenerationFailure{"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
	}
	if (kind == PropertyKind::Enum)
	{
		if (!descriptor || !descriptor->Element)
			return {"DUMP_USMAP_ENUM_DESCRIPTOR_UNAVAILABLE", "An enum property has no exact integer backing descriptor."};
		const auto enumName = names.Add(DisplayTypeName(typeNames, typeName));
		if (!enumName || !output.U8(26))
			return {"DUMP_USMAP_NAME_INVALID", "An enum identity cannot be represented by USMAP."};
		GenerationFailure nested = WriteUsmapPropertyType(
			descriptor->Element->Kind,
			descriptor->Element->TypeName,
			descriptor->Element,
			typeNames,
			names,
			output,
			depth + 1);
		if (nested)
			return nested;
		return output.I32(*enumName)
			? GenerationFailure{}
			: GenerationFailure{"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
	}
	if (kind == PropertyKind::Array || kind == PropertyKind::Set)
	{
		if (!descriptor || !descriptor->Element)
			return {"DUMP_USMAP_CONTAINER_DESCRIPTOR_UNAVAILABLE", "A container property has no exact element descriptor."};
		if (!output.U8(kind == PropertyKind::Array ? 8 : 25))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
		return WriteUsmapPropertyType(
			descriptor->Element->Kind,
			descriptor->Element->TypeName,
			descriptor->Element,
			typeNames,
			names,
			output,
			depth + 1);
	}
	if (kind == PropertyKind::Map)
	{
		if (!descriptor || !descriptor->Key || !descriptor->Mapped)
			return {"DUMP_USMAP_CONTAINER_DESCRIPTOR_UNAVAILABLE", "A map property has no exact key/value descriptors."};
		if (!output.U8(24))
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
		GenerationFailure key = WriteUsmapPropertyType(
			descriptor->Key->Kind,
			descriptor->Key->TypeName,
			descriptor->Key,
			typeNames,
			names,
			output,
			depth + 1);
		if (key)
			return key;
		return WriteUsmapPropertyType(
			descriptor->Mapped->Kind,
			descriptor->Mapped->TypeName,
			descriptor->Mapped,
			typeNames,
			names,
			output,
			depth + 1);
	}
	return {"DUMP_USMAP_PROPERTY_KIND_UNSUPPORTED", "A reflected property kind has no exact USMAP representation."};
}

GenerationFailure GenerateUsmap(
	const SnapshotDumpInput& input,
	const SnapshotDumpWorkerLimits& limits,
	Runtime::IDumpJobExecutionContext& context,
	std::vector<GeneratedArtifact>& artifacts)
{
	std::set<std::string> rawTypeNames;
	for (const ReflectedType& type : input.Types->Types())
	{
		if (type.Name.empty() || !rawTypeNames.emplace(type.Name).second)
		{
			return {
				"DUMP_USMAP_TYPE_NAME_COLLISION",
				"USMAP cannot represent duplicate or empty raw reflected type names without inventing an identity."};
		}
	}
	const TypeNameIndex typeNames = BuildTypeNameIndex(*input.Types);
	UsmapNameTable names(limits.MaxArtifactBytes);
	BinaryBuffer enumData(limits.MaxArtifactBytes);
	BinaryBuffer structData(limits.MaxArtifactBytes);
	std::uint32_t enumCount = 0;
	std::uint32_t structCount = 0;
	std::size_t completed = 0;

	for (const ReflectedType& type : input.Types->Types())
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "USMAP generation was cancelled."};
		if (type.Kind == ReflectedTypeKind::Enum)
		{
			if (type.EnumState != ReflectedMemberState::Supported
				|| type.EnumEntries.empty()
				|| type.EnumEntries.size() > (std::numeric_limits<std::uint16_t>::max)())
			{
				return {"DUMP_USMAP_ENUM_TABLE_UNAVAILABLE", "Every emitted UEnum requires an exact bounded entry table."};
			}
			const auto typeName = names.Add(DisplayTypeName(typeNames, type.FullPath));
			if (!typeName
				|| !enumData.I32(*typeName)
				|| !enumData.U16(static_cast<std::uint16_t>(type.EnumEntries.size())))
			{
				return {"DUMP_USMAP_NAME_INVALID", "A reflected enum identity cannot be represented by USMAP."};
			}
			for (const auto& entry : type.EnumEntries)
			{
				const auto entryName = names.Add(entry.Name);
				if (!entryName || !enumData.I64(entry.Value) || !enumData.I32(*entryName))
					return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP enum table exceeds the artifact limit."};
			}
			if (enumCount == (std::numeric_limits<std::uint32_t>::max)())
				return {"DUMP_USMAP_COUNT_EXCEEDED", "The USMAP enum count exceeds uint32."};
			++enumCount;
		}
		else
		{
			if (type.DirectProperties.size() > (std::numeric_limits<std::uint16_t>::max)())
				return {"DUMP_USMAP_PROPERTY_COUNT_EXCEEDED", "A reflected type has too many direct USMAP properties."};
			std::uint32_t propertyCount = 0;
			for (const ReflectedProperty& property : type.DirectProperties)
			{
				if (property.ArrayDim == 0
					|| property.ArrayDim > (std::numeric_limits<std::uint8_t>::max)()
					|| propertyCount > (std::numeric_limits<std::uint16_t>::max)() - property.ArrayDim)
				{
					return {"DUMP_USMAP_PROPERTY_COUNT_EXCEEDED", "A reflected array dimension cannot be represented by USMAP."};
				}
				propertyCount += property.ArrayDim;
			}

			const auto typeName = names.Add(DisplayTypeName(typeNames, type.FullPath));
			std::int32_t superName = -1;
			if (type.Super)
			{
				const ReflectedType* super = input.Types->FindByObjectIndex(type.Super->Index);
				if (!super)
					return {"DUMP_USMAP_SUPER_UNAVAILABLE", "A reflected super type is absent from the pinned TypeSnapshot."};
				const auto added = names.Add(DisplayTypeName(typeNames, super->FullPath));
				if (!added)
					return {"DUMP_USMAP_NAME_INVALID", "A reflected super identity cannot be represented by USMAP."};
				superName = *added;
			}
			if (!typeName
				|| !structData.I32(*typeName)
				|| !structData.I32(superName)
				|| !structData.U16(static_cast<std::uint16_t>(propertyCount))
				|| !structData.U16(static_cast<std::uint16_t>(type.DirectProperties.size())))
			{
				return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP type table exceeds the artifact limit."};
			}
			std::uint32_t propertyIndex = 0;
			for (const ReflectedProperty& property : type.DirectProperties)
			{
				if (property.State != ReflectedMemberState::Supported
					|| property.Kind == PropertyKind::Unknown)
				{
					return {"DUMP_USMAP_PROPERTY_UNAVAILABLE", "An unsupported reflected property prevents a complete USMAP artifact."};
				}
				const auto propertyName = names.Add(property.Name);
				if (!propertyName
					|| !structData.U16(static_cast<std::uint16_t>(propertyIndex))
					|| !structData.U8(static_cast<std::uint8_t>(property.ArrayDim))
					|| !structData.I32(*propertyName))
				{
					return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP property table exceeds the artifact limit."};
				}
				GenerationFailure propertyType = WriteUsmapPropertyType(
					property.Kind,
					property.TypeName,
					property.Descriptor,
					typeNames,
					names,
					structData);
				if (propertyType)
					return propertyType;
				propertyIndex += property.ArrayDim;
			}
			if (structCount == (std::numeric_limits<std::uint32_t>::max)())
				return {"DUMP_USMAP_COUNT_EXCEEDED", "The USMAP struct count exceeds uint32."};
			++structCount;
		}
		++completed;
		if (!ReportTypeProgress(context, "usmap", completed, input.Types->Types().size()))
			return {"DUMP_WORKER_CANCELLED", "USMAP generation was cancelled."};
	}

	BinaryBuffer payload(limits.MaxArtifactBytes);
	if (!names.Write(payload)
		|| !payload.U32(enumCount)
		|| !payload.Append(enumData)
		|| !payload.U32(structCount)
		|| !payload.Append(structData))
	{
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP payload exceeds the artifact limit."};
	}

	const std::vector<std::uint8_t> rawPayload = payload.Release();
	std::ostringstream container(std::ios::binary);
	UExplorer::Usmap::WriteUncompressed(container, rawPayload);
	const std::string encoded = container.str();
	if (encoded.size() > limits.MaxArtifactBytes)
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The USMAP container exceeds the artifact limit."};
	artifacts.push_back({
		"Mappings.usmap",
		ArtifactKind::Usmap,
		std::vector<std::uint8_t>(encoded.begin(), encoded.end()),
		static_cast<std::uint64_t>(enumCount) + structCount});
	return {};
}

GenerationFailure GenerateIdaScript(
	const SnapshotDumpInput& input,
	const SnapshotDumpWorkerLimits& limits,
	Runtime::IDumpJobExecutionContext& context,
	std::vector<GeneratedArtifact>& artifacts)
{
	struct Record
	{
		std::uint64_t Rva = 0;
		std::string Name;
	};
	std::vector<Record> records;
	for (const ReflectedType& type : input.Types->Types())
	{
		for (const ReflectedFunction& function : type.DirectFunctions)
		{
			if (function.NativeAddress == 0)
				continue;
			if (function.NativeAddress < input.Context->ModuleBase())
				return {"DUMP_IDA_NATIVE_ADDRESS_INVALID", "A native function address is below the pinned module base."};
			records.push_back({
				static_cast<std::uint64_t>(function.NativeAddress - input.Context->ModuleBase()),
				CppIdentifier(type.Name, static_cast<std::uint64_t>(type.Handle.Index))
					+ "__"
					+ CppIdentifier(
						function.Name,
						static_cast<std::uint64_t>(function.Handle.Function.Index))});
		}
	}
	std::sort(records.begin(), records.end(), [](const Record& left, const Record& right) {
		return left.Rva != right.Rva ? left.Rva < right.Rva : left.Name < right.Name;
	});
	const auto duplicate = std::adjacent_find(records.begin(), records.end(), [](const Record& left, const Record& right) {
		return left.Rva == right.Rva && left.Name == right.Name;
	});
	if (duplicate != records.end())
		return {"DUMP_IDA_DUPLICATE_RECORD", "The pinned TypeSnapshot contains a duplicate native function record."};

	BoundedText text(limits.MaxArtifactBytes);
	if (!text.Append("# Generated from an immutable UExplorer TypeSnapshot.\n")
		|| !text.Append("# schema: ")
		|| !text.Append(kIdaSchema)
		|| !text.Append("\nimport ida_name\nimport idaapi\n\nRECORDS = [\n"))
	{
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The IDA script exceeds the artifact limit."};
	}
	std::size_t completed = 0;
	for (const Record& record : records)
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "IDA script generation was cancelled."};
		if (!text.Append("    (")
			|| !text.Append(Hex(record.Rva))
			|| !text.Append(", ")
			|| !text.AppendJson(json(record.Name))
			|| !text.Append("),\n"))
		{
			return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The IDA script exceeds the artifact limit."};
		}
		++completed;
		if (completed % 512 == 0 && !context.ReportProgress(DumpJobProgress{
			.Phase = "ida-script",
			.Completed = static_cast<std::uint64_t>(completed),
			.Total = static_cast<std::uint64_t>(records.size()),
			.Message = "Serializing witnessed native function RVAs."}))
		{
			return {"DUMP_WORKER_CANCELLED", "IDA script generation was cancelled."};
		}
	}
	if (!text.Append("]\n\ndef apply():\n")
		|| !text.Append("    base = idaapi.get_imagebase()\n")
		|| !text.Append("    failures = 0\n")
		|| !text.Append("    for rva, name in RECORDS:\n")
		|| !text.Append("        if not ida_name.set_name(base + rva, name, ida_name.SN_CHECK):\n")
		|| !text.Append("            failures += 1\n")
		|| !text.Append("    print(f'UExplorer: applied {len(RECORDS) - failures}/{len(RECORDS)} names')\n")
		|| !text.Append("\nif __name__ == '__main__':\n    apply()\n"))
	{
		return {"DUMP_ARTIFACT_LIMIT_EXCEEDED", "The IDA script exceeds the artifact limit."};
	}
	artifacts.push_back({
		"UExplorer_IDA.py",
		ArtifactKind::IdaScript,
		text.ReleaseBytes(),
		static_cast<std::uint64_t>(records.size())});
	return {};
}

class BinaryReader final
{
public:
	explicit BinaryReader(const std::span<const std::uint8_t> bytes) noexcept
		: m_Bytes(bytes)
	{
	}

	bool U8(std::uint8_t& value) noexcept
	{
		return Bytes(&value, sizeof(value));
	}
	bool U16(std::uint16_t& value) noexcept
	{
		std::array<std::uint8_t, 2> bytes{};
		if (!Bytes(bytes.data(), bytes.size()))
			return false;
		value = static_cast<std::uint16_t>(bytes[0])
			| (static_cast<std::uint16_t>(bytes[1]) << 8);
		return true;
	}
	bool U32(std::uint32_t& value) noexcept
	{
		std::array<std::uint8_t, 4> bytes{};
		if (!Bytes(bytes.data(), bytes.size()))
			return false;
		value = static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
		return true;
	}
	bool I32(std::int32_t& value) noexcept
	{
		std::uint32_t raw = 0;
		if (!U32(raw))
			return false;
		value = std::bit_cast<std::int32_t>(raw);
		return true;
	}
	bool I64(std::int64_t& value) noexcept
	{
		std::array<std::uint8_t, 8> bytes{};
		if (!Bytes(bytes.data(), bytes.size()))
			return false;
		std::uint64_t raw = 0;
		for (std::size_t index = 0; index < bytes.size(); ++index)
			raw |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
		value = std::bit_cast<std::int64_t>(raw);
		return true;
	}
	bool Skip(const std::size_t size) noexcept
	{
		if (size > Remaining())
			return false;
		m_Offset += size;
		return true;
	}
	std::size_t Remaining() const noexcept { return m_Bytes.size() - m_Offset; }

private:
	bool Bytes(void* destination, const std::size_t size) noexcept
	{
		if (!destination || size > Remaining())
			return false;
		std::memcpy(destination, m_Bytes.data() + m_Offset, size);
		m_Offset += size;
		return true;
	}

	std::span<const std::uint8_t> m_Bytes;
	std::size_t m_Offset = 0;
};

bool ValidNameIndex(const std::int32_t value, const std::uint32_t nameCount) noexcept
{
	return value >= 0 && static_cast<std::uint32_t>(value) < nameCount;
}

bool ParseUsmapPropertyType(
	BinaryReader& reader,
	const std::uint32_t nameCount,
	const std::size_t depth = 0) noexcept
{
	if (depth > Runtime::TypeSnapshotStore::kMaxDescriptorDepth)
		return false;
	std::uint8_t kind = 0;
	if (!reader.U8(kind) || kind == 0xFF)
		return false;
	if (kind == 9)
	{
		std::int32_t name = -1;
		return reader.I32(name) && ValidNameIndex(name, nameCount);
	}
	if (kind == 26)
	{
		if (!ParseUsmapPropertyType(reader, nameCount, depth + 1))
			return false;
		std::int32_t name = -1;
		return reader.I32(name) && ValidNameIndex(name, nameCount);
	}
	if (kind == 8 || kind == 25 || kind == 28)
		return ParseUsmapPropertyType(reader, nameCount, depth + 1);
	if (kind == 24)
	{
		return ParseUsmapPropertyType(reader, nameCount, depth + 1)
			&& ParseUsmapPropertyType(reader, nameCount, depth + 1);
	}
	return kind <= 37;
}

bool ValidateUsmap(
	const std::vector<std::uint8_t>& bytes,
	const std::uint64_t expectedRecords) noexcept
{
	if (bytes.size() < UExplorer::Usmap::kContainerHeaderSize
		|| bytes[0] != static_cast<std::uint8_t>(UExplorer::Usmap::kMagic)
		|| bytes[1] != static_cast<std::uint8_t>(UExplorer::Usmap::kMagic >> 8)
		|| bytes[2] != UExplorer::Usmap::kExplicitEnumValuesVersion
		|| bytes[7] != UExplorer::Usmap::kCompressionNone)
	{
		return false;
	}
	const auto readU32 = [&bytes](const std::size_t offset) noexcept {
		return static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	};
	const std::uint32_t compressed = readU32(8);
	const std::uint32_t uncompressed = readU32(12);
	if (compressed != uncompressed
		|| static_cast<std::uint64_t>(compressed) + UExplorer::Usmap::kContainerHeaderSize != bytes.size())
	{
		return false;
	}

	BinaryReader reader(std::span<const std::uint8_t>(
		bytes.data() + UExplorer::Usmap::kContainerHeaderSize,
		bytes.size() - UExplorer::Usmap::kContainerHeaderSize));
	std::uint32_t nameCount = 0;
	if (!reader.U32(nameCount))
		return false;
	for (std::uint32_t index = 0; index < nameCount; ++index)
	{
		std::uint16_t length = 0;
		if (!reader.U16(length) || length == 0 || !reader.Skip(length))
			return false;
	}
	std::uint32_t enumCount = 0;
	if (!reader.U32(enumCount))
		return false;
	for (std::uint32_t index = 0; index < enumCount; ++index)
	{
		std::int32_t enumName = -1;
		std::uint16_t entryCount = 0;
		if (!reader.I32(enumName)
			|| !ValidNameIndex(enumName, nameCount)
			|| !reader.U16(entryCount)
			|| entryCount == 0)
		{
			return false;
		}
		for (std::uint16_t entry = 0; entry < entryCount; ++entry)
		{
			std::int64_t value = 0;
			std::int32_t name = -1;
			if (!reader.I64(value) || !reader.I32(name) || !ValidNameIndex(name, nameCount))
				return false;
		}
	}
	std::uint32_t structCount = 0;
	if (!reader.U32(structCount))
		return false;
	for (std::uint32_t index = 0; index < structCount; ++index)
	{
		std::int32_t name = -1;
		std::int32_t super = -1;
		std::uint16_t propertyCount = 0;
		std::uint16_t serializableCount = 0;
		if (!reader.I32(name)
			|| !ValidNameIndex(name, nameCount)
			|| !reader.I32(super)
			|| (super != -1 && !ValidNameIndex(super, nameCount))
			|| !reader.U16(propertyCount)
			|| !reader.U16(serializableCount)
			|| serializableCount > propertyCount)
		{
			return false;
		}
		std::uint32_t previousEnd = 0;
		for (std::uint16_t property = 0; property < serializableCount; ++property)
		{
			std::uint16_t serializedIndex = 0;
			std::uint8_t arrayDim = 0;
			std::int32_t propertyName = -1;
			if (!reader.U16(serializedIndex)
				|| !reader.U8(arrayDim)
				|| arrayDim == 0
				|| serializedIndex != previousEnd
				|| !reader.I32(propertyName)
				|| !ValidNameIndex(propertyName, nameCount)
				|| !ParseUsmapPropertyType(reader, nameCount))
			{
				return false;
			}
			previousEnd += arrayDim;
		}
		if (previousEnd != propertyCount)
			return false;
	}
	return reader.Remaining() == 0
		&& static_cast<std::uint64_t>(enumCount) + structCount == expectedRecords;
}

std::size_t CountSubstring(
	const std::string_view haystack,
	const std::string_view needle) noexcept
{
	if (needle.empty())
		return 0;
	std::size_t count = 0;
	std::size_t offset = 0;
	while ((offset = haystack.find(needle, offset)) != std::string_view::npos)
	{
		++count;
		offset += needle.size();
	}
	return count;
}

bool ValidateGeneratedArtifact(const GeneratedArtifact& artifact) noexcept
{
	try
	{
		if (artifact.Bytes.empty())
			return false;
		if (artifact.Kind == ArtifactKind::Usmap)
			return ValidateUsmap(artifact.Bytes, artifact.ExpectedRecords);
		const std::string_view text(
			reinterpret_cast<const char*>(artifact.Bytes.data()),
			artifact.Bytes.size());
		if (artifact.Kind == ArtifactKind::Sdk)
		{
			return text.find(kSdkSchema) != std::string_view::npos
				&& text.ends_with("} // namespace UExplorerSDK\n")
				&& CountSubstring(text, "_Path[] = ") == artifact.ExpectedRecords;
		}
		if (artifact.Kind == ArtifactKind::IdaScript)
		{
			return text.find(kIdaSchema) != std::string_view::npos
				&& text.find("def apply():") != std::string_view::npos
				&& CountSubstring(text, "    (0x") == artifact.ExpectedRecords;
		}
		const json parsed = json::parse(artifact.Bytes.begin(), artifact.Bytes.end());
		if (!parsed.is_object())
			return false;
		if (artifact.Kind == ArtifactKind::Manifest)
		{
			return parsed.value("schema", std::string{}) == kManifestSchema
				&& parsed.value("complete", false)
				&& parsed.contains("artifacts")
				&& parsed.at("artifacts").is_array()
				&& parsed.at("artifacts").size() == artifact.ExpectedRecords;
		}
		return parsed.contains("data") && parsed.at("data").is_array();
	}
	catch (...)
	{
		return false;
	}
}

class Sha256 final
{
public:
	Sha256() noexcept
	{
		DWORD returned = 0;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
			&m_Algorithm,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0)))
		{
			return;
		}
		DWORD objectBytes = 0;
		DWORD hashBytes = 0;
		if (!BCRYPT_SUCCESS(BCryptGetProperty(
			m_Algorithm,
			BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectBytes),
			sizeof(objectBytes),
			&returned,
			0))
			|| !BCRYPT_SUCCESS(BCryptGetProperty(
				m_Algorithm,
				BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&hashBytes),
				sizeof(hashBytes),
				&returned,
				0))
			|| objectBytes == 0
			|| hashBytes != 32)
		{
			return;
		}
		try
		{
			m_Object.resize(objectBytes);
		}
		catch (...)
		{
			return;
		}
		if (!BCRYPT_SUCCESS(BCryptCreateHash(
			m_Algorithm,
			&m_Hash,
			m_Object.data(),
			static_cast<ULONG>(m_Object.size()),
			nullptr,
			0,
			0)))
		{
			m_Hash = nullptr;
			return;
		}
		m_Ready = true;
	}

	~Sha256()
	{
		if (m_Hash)
			BCryptDestroyHash(m_Hash);
		if (m_Algorithm)
			BCryptCloseAlgorithmProvider(m_Algorithm, 0);
	}

	Sha256(const Sha256&) = delete;
	Sha256& operator=(const Sha256&) = delete;

	bool Update(const std::span<const std::uint8_t> bytes) noexcept
	{
		if (!m_Ready || m_Finished)
			return false;
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			const std::size_t chunk = (std::min)(
				bytes.size() - offset,
				static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
			if (!BCRYPT_SUCCESS(BCryptHashData(
				m_Hash,
				const_cast<PUCHAR>(bytes.data() + offset),
				static_cast<ULONG>(chunk),
				0)))
			{
				return false;
			}
			offset += chunk;
		}
		return true;
	}

	bool Finish(std::string& encoded) noexcept
	{
		encoded.clear();
		if (!m_Ready || m_Finished)
			return false;
		std::array<std::uint8_t, 32> digest{};
		if (!BCRYPT_SUCCESS(BCryptFinishHash(
			m_Hash,
			digest.data(),
			static_cast<ULONG>(digest.size()),
			0)))
		{
			return false;
		}
		m_Finished = true;
		static constexpr char kHex[] = "0123456789abcdef";
		try
		{
			encoded.resize(digest.size() * 2);
			for (std::size_t index = 0; index < digest.size(); ++index)
			{
				encoded[index * 2] = kHex[digest[index] >> 4];
				encoded[index * 2 + 1] = kHex[digest[index] & 0x0F];
			}
		}
		catch (...)
		{
			encoded.clear();
			return false;
		}
		return true;
	}

private:
	BCRYPT_ALG_HANDLE m_Algorithm = nullptr;
	BCRYPT_HASH_HANDLE m_Hash = nullptr;
	std::vector<std::uint8_t> m_Object;
	bool m_Ready = false;
	bool m_Finished = false;
};

bool HashMemory(const std::vector<std::uint8_t>& bytes, std::string& encoded) noexcept
{
	Sha256 hash;
	return hash.Update(bytes) && hash.Finish(encoded);
}

enum class HashFileResult : std::uint8_t
{
	Ok,
	Failed,
	Cancelled
};

HashFileResult HashFile(
	const std::filesystem::path& path,
	Runtime::IDumpJobExecutionContext& context,
	std::string& encoded) noexcept
{
	try
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
			return HashFileResult::Failed;
		Sha256 hash;
		std::vector<std::uint8_t> buffer(kIoChunkBytes);
		for (;;)
		{
			if (context.IsCancellationRequested())
				return HashFileResult::Cancelled;
			input.read(
				reinterpret_cast<char*>(buffer.data()),
				static_cast<std::streamsize>(buffer.size()));
			const std::streamsize count = input.gcount();
			if (count > 0 && !hash.Update(std::span<const std::uint8_t>(
				buffer.data(),
				static_cast<std::size_t>(count))))
			{
				return HashFileResult::Failed;
			}
			if (input.eof())
				break;
			if (!input)
				return HashFileResult::Failed;
		}
		return hash.Finish(encoded) ? HashFileResult::Ok : HashFileResult::Failed;
	}
	catch (...)
	{
		return HashFileResult::Failed;
	}
}

GenerationFailure WriteAndVerifyArtifact(
	const std::filesystem::path& outputDirectory,
	const GeneratedArtifact& artifact,
	Runtime::IDumpJobExecutionContext& context,
	WrittenArtifact& written)
{
	written = {};
	if (!ValidateGeneratedArtifact(artifact))
		return {"DUMP_ARTIFACT_CONSUMER_REJECTED", "The generated artifact failed its bounded structural consumer."};

	std::string memoryHash;
	if (!HashMemory(artifact.Bytes, memoryHash))
		return {"DUMP_ARTIFACT_HASH_FAILED", "SHA-256 initialization or hashing failed."};

	const std::filesystem::path finalPath = outputDirectory / artifact.Name;
	const std::filesystem::path partialPath = outputDirectory / (artifact.Name + ".partial");
	std::error_code error;
	if (std::filesystem::exists(finalPath, error)
		|| error
		|| std::filesystem::exists(partialPath, error)
		|| error)
	{
		return {"DUMP_ARTIFACT_ALREADY_EXISTS", "An artifact or partial artifact already exists in the reserved output directory."};
	}

	std::ofstream output(partialPath, std::ios::binary | std::ios::out);
	if (!output)
		return {"DUMP_ARTIFACT_OPEN_FAILED", "The partial artifact file could not be opened."};
	std::size_t offset = 0;
	while (offset < artifact.Bytes.size())
	{
		if (context.IsCancellationRequested())
			return {"DUMP_WORKER_CANCELLED", "Artifact writing was cancelled."};
		const std::size_t chunk = (std::min)(kIoChunkBytes, artifact.Bytes.size() - offset);
		output.write(
			reinterpret_cast<const char*>(artifact.Bytes.data() + offset),
			static_cast<std::streamsize>(chunk));
		if (!output)
			return {"DUMP_ARTIFACT_WRITE_FAILED", "Writing the partial artifact failed."};
		offset += chunk;
	}
	output.flush();
	if (!output)
		return {"DUMP_ARTIFACT_FLUSH_FAILED", "Flushing the partial artifact failed."};
	output.close();
	if (!output)
		return {"DUMP_ARTIFACT_CLOSE_FAILED", "Closing the partial artifact failed."};

	const std::uintmax_t size = std::filesystem::file_size(partialPath, error);
	if (error || size != artifact.Bytes.size())
		return {"DUMP_ARTIFACT_SIZE_MISMATCH", "The partial artifact size differs from the generated byte count."};

	std::string diskHash;
	const HashFileResult hashResult = HashFile(partialPath, context, diskHash);
	if (hashResult == HashFileResult::Cancelled)
		return {"DUMP_WORKER_CANCELLED", "Artifact verification was cancelled."};
	if (hashResult != HashFileResult::Ok)
		return {"DUMP_ARTIFACT_HASH_FAILED", "The partial artifact could not be hashed."};
	if (diskHash != memoryHash)
		return {"DUMP_ARTIFACT_HASH_MISMATCH", "The partial artifact hash differs from the generated bytes."};
	if (context.IsCancellationRequested())
		return {"DUMP_WORKER_CANCELLED", "Artifact commit was cancelled."};

	// Rename is the only externally visible commit point. All fallible content
	// validation is complete while the file still has its .partial suffix.
	std::filesystem::rename(partialPath, finalPath, error);
	if (error)
		return {"DUMP_ARTIFACT_COMMIT_FAILED", "The verified partial artifact could not be committed without replacement."};

	written.Name = artifact.Name;
	written.Size = static_cast<std::uint64_t>(size);
	written.Sha256 = std::move(diskHash);
	written.Validation = artifact.Kind == ArtifactKind::Usmap
		? "usmap_v4_none_consumer"
		: artifact.Kind == ArtifactKind::Json || artifact.Kind == ArtifactKind::Manifest
			? "strict_json_consumer"
			: artifact.Kind == ArtifactKind::Sdk
				? "snapshot_sdk_structure"
				: "ida_python_structure";
	return {};
}

bool DecodeOptionsFormat(
	const std::vector<std::byte>& bytes,
	SnapshotDumpFormat& format) noexcept
{
	try
	{
		std::string encoded;
		encoded.reserve(bytes.size());
		for (const std::byte value : bytes)
			encoded.push_back(static_cast<char>(value));
		const json value = json::parse(encoded);
		if (!value.is_object()
			|| value.size() != 3
			|| !value.contains("format")
			|| !value.contains("options")
			|| !value.contains("schema")
			|| !value.at("format").is_string()
			|| !value.at("options").is_object()
			|| !value.at("options").empty()
			|| !value.at("schema").is_string()
			|| value.at("schema").get_ref<const std::string&>() != kOptionsSchema)
		{
			return false;
		}
		const auto parsed = ParseSnapshotDumpFormat(
			value.at("format").get_ref<const std::string&>());
		if (!parsed)
			return false;
		format = *parsed;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

GenerationFailure ValidateInput(
	const Runtime::DumpJobRequest& request,
	const SnapshotDumpInput& input,
	const SnapshotDumpWorkerLimits& limits) noexcept
{
	if (!input.Context || !input.Objects || !input.Types)
		return {"DUMP_INPUT_SNAPSHOT_UNAVAILABLE", "The job has no complete pinned context/object/type input."};
	SnapshotDumpFormat requestedFormat{};
	if (!DecodeOptionsFormat(request.Spec.OpaqueOptions, requestedFormat)
		|| requestedFormat != input.Format)
	{
		return {"DUMP_INPUT_FORMAT_MISMATCH", "The owned input format does not match the immutable request envelope."};
	}
	if (request.Spec.SessionId != input.Objects->SessionId
		|| request.Spec.SessionId != input.Types->SessionId()
		|| request.Spec.ContextGeneration != input.Context->Generation()
		|| request.Spec.ContextGeneration != input.Objects->ContextGeneration
		|| request.Spec.ContextGeneration != input.Types->ContextGeneration()
		|| request.Spec.ObjectSnapshotGeneration != input.Objects->Generation
		|| request.Spec.ObjectSnapshotGeneration != input.Types->ObjectSnapshotGeneration()
		|| request.Spec.TypeSnapshotGeneration != input.Types->Generation()
		|| !input.Types->IsConfigured(input.Context->Generation()))
	{
		return {"DUMP_INPUT_SCOPE_MISMATCH", "Pinned input pointers do not match the immutable job scope."};
	}
	if (input.Types->Types().size() > limits.MaxTypes)
		return {"DUMP_INPUT_TYPE_LIMIT_EXCEEDED", "The pinned TypeSnapshot exceeds the worker type limit."};

	std::size_t members = 0;
	for (const ReflectedType& type : input.Types->Types())
	{
		if (!CheckedAdd(members, type.DirectProperties.size())
			|| !CheckedAdd(members, type.DirectFunctions.size())
			|| !CheckedAdd(members, type.EnumEntries.size()))
		{
			return {"DUMP_INPUT_MEMBER_LIMIT_EXCEEDED", "The pinned member count overflowed."};
		}
		for (const ReflectedFunction& function : type.DirectFunctions)
		{
			if (!CheckedAdd(members, function.Parameters.size()))
				return {"DUMP_INPUT_MEMBER_LIMIT_EXCEEDED", "The pinned member count overflowed."};
		}
		if (members > limits.MaxMembers)
			return {"DUMP_INPUT_MEMBER_LIMIT_EXCEEDED", "The pinned TypeSnapshot exceeds the worker member limit."};
	}
	return {};
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept
{
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes == INVALID_FILE_ATTRIBUTES
		|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

GenerationFailure ReserveOutputDirectory(
	const std::filesystem::path& configuredRoot,
	const Runtime::DumpJobRequest& request,
	const SnapshotDumpWorkerLimits& limits,
	std::filesystem::path& outputDirectory)
{
	outputDirectory.clear();
	if (!IsSafePathToken(request.Spec.SessionId, 128)
		|| !IsSafePathToken(request.Spec.OutputPathIdentity, limits.MaxOutputIdentityBytes))
	{
		return {"DUMP_OUTPUT_IDENTITY_INVALID", "Session/output identities must be bounded ASCII path tokens."};
	}
	std::error_code error;
	std::filesystem::create_directories(configuredRoot, error);
	if (error)
		return {"DUMP_OUTPUT_ROOT_CREATE_FAILED", "The configured dump output root could not be created."};
	const std::filesystem::path canonicalRoot = std::filesystem::canonical(configuredRoot, error);
	if (error || !canonicalRoot.is_absolute() || canonicalRoot.native().size() > kMaxWindowsPathChars)
		return {"DUMP_OUTPUT_ROOT_INVALID", "The configured dump output root could not be canonicalized."};

	const std::filesystem::path sessionDirectory = canonicalRoot / request.Spec.SessionId;
	std::filesystem::create_directories(sessionDirectory, error);
	if (error)
		return {"DUMP_OUTPUT_SESSION_CREATE_FAILED", "The session-scoped dump directory could not be created."};
	const std::filesystem::path canonicalSession = std::filesystem::canonical(sessionDirectory, error);
	if (error
		|| !IsWithinPath(canonicalSession, canonicalRoot)
		|| IsReparsePoint(sessionDirectory))
	{
		return {"DUMP_OUTPUT_SESSION_INVALID", "The session-scoped dump directory escapes the configured root or is a reparse point."};
	}

	const std::filesystem::path candidate = canonicalSession / request.Spec.OutputPathIdentity;
	if (candidate.native().size() > kMaxWindowsPathChars
		|| std::filesystem::exists(candidate, error)
		|| error)
	{
		return {"DUMP_OUTPUT_ALREADY_EXISTS", "The logical output identity is already reserved or cannot be queried."};
	}
	if (!std::filesystem::create_directory(candidate, error) || error)
		return {"DUMP_OUTPUT_RESERVATION_FAILED", "The logical output identity could not be reserved atomically."};
	const std::filesystem::path canonicalOutput = std::filesystem::canonical(candidate, error);
	if (error
		|| !IsWithinPath(canonicalOutput, canonicalRoot)
		|| IsReparsePoint(candidate))
	{
		return {"DUMP_OUTPUT_DIRECTORY_INVALID", "The reserved output directory escapes the configured root or is a reparse point."};
	}
	outputDirectory = canonicalOutput;
	return {};
}

GeneratedArtifact BuildManifest(
	const Runtime::DumpJobRequest& request,
	const SnapshotDumpInput& input,
	const std::vector<WrittenArtifact>& artifacts,
	const std::size_t maximumBytes)
{
	json encodedArtifacts = json::array();
	for (const WrittenArtifact& artifact : artifacts)
	{
		encodedArtifacts.push_back({
			{"name", artifact.Name},
			{"size", artifact.Size},
			{"sha256", artifact.Sha256},
			{"validation", artifact.Validation}
		});
	}
	const json manifest = {
		{"schema", kManifestSchema},
		{"complete", true},
		{"job_id", std::to_string(request.Id)},
		{"format", ToString(input.Format)},
		{"output_path_identity", request.Spec.OutputPathIdentity},
		{"scope", {
			{"session_id", request.Spec.SessionId},
			{"context_generation", request.Spec.ContextGeneration},
			{"object_snapshot_generation", request.Spec.ObjectSnapshotGeneration},
			{"type_snapshot_generation", request.Spec.TypeSnapshotGeneration}
		}},
		{"target", {
			{"process_id", input.Context->ProcessId()},
			{"game_name", input.Context->GameName()},
			{"game_version", input.Context->GameVersion()}
		}},
		{"input", {
			{"object_count", input.Objects->Objects.size()},
			{"type_count", input.Types->Types().size()},
			{"reflection_layout_fingerprint", input.Types->ReflectionLayoutFingerprint()}
		}},
		{"artifacts", std::move(encodedArtifacts)}
	};
	const std::string bytes = manifest.dump();
	if (bytes.size() > maximumBytes)
		return {};
	return {
		"manifest.json",
		ArtifactKind::Manifest,
		std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
		static_cast<std::uint64_t>(artifacts.size())};
}

} // namespace

const char* ToString(const SnapshotDumpFormat format) noexcept
{
	switch (format)
	{
	case SnapshotDumpFormat::Sdk: return "sdk";
	case SnapshotDumpFormat::Usmap: return "usmap";
	case SnapshotDumpFormat::Dumpspace: return "dumpspace";
	case SnapshotDumpFormat::IdaScript: return "ida-script";
	}
	return "unknown";
}

std::optional<SnapshotDumpFormat> ParseSnapshotDumpFormat(
	const std::string_view value) noexcept
{
	if (value == "sdk") return SnapshotDumpFormat::Sdk;
	if (value == "usmap") return SnapshotDumpFormat::Usmap;
	if (value == "dumpspace") return SnapshotDumpFormat::Dumpspace;
	if (value == "ida-script") return SnapshotDumpFormat::IdaScript;
	return std::nullopt;
}

SnapshotDumpWorker::SnapshotDumpWorker(
	std::filesystem::path outputRoot,
	SnapshotDumpWorkerLimits limits)
	: m_OutputRoot(std::move(outputRoot)),
	  m_Limits(limits)
{
	try
	{
		m_OutputRoot = m_OutputRoot.lexically_normal();
		m_Configured = ValidLimits(m_Limits)
			&& !m_OutputRoot.empty()
			&& m_OutputRoot.is_absolute()
			&& m_OutputRoot.native().size() <= kMaxWindowsPathChars;
	}
	catch (...)
	{
		m_Configured = false;
	}
}

Runtime::DumpJobWorkerResult SnapshotDumpWorker::Execute(
	const std::shared_ptr<const Runtime::DumpJobRequest>& request,
	Runtime::IDumpJobExecutionContext& context)
{
	if (!m_Configured)
		return Failed("DUMP_WORKER_NOT_CONFIGURED", "The snapshot dump worker has no valid bounded output root/limits.");
	if (!request || !request->Spec.Input)
		return Failed("DUMP_INPUT_SNAPSHOT_UNAVAILABLE", "The owned dump request has no pinned execution input.");
	const auto input = std::dynamic_pointer_cast<const SnapshotDumpInput>(request->Spec.Input);
	if (!input)
		return Failed("DUMP_INPUT_TYPE_INVALID", "The owned dump input is not an immutable SnapshotDumpInput.");

	const GenerationFailure inputFailure = ValidateInput(*request, *input, m_Limits);
	if (inputFailure)
		return Failed(inputFailure.Code, inputFailure.Message);
	if (context.IsCancellationRequested())
		return Cancelled();
	if (!context.ReportProgress({
		.Phase = "prepare",
		.Completed = 0,
		.Total = static_cast<std::uint64_t>(input->Types->Types().size()),
		.Message = "Validated and pinned the immutable dump input."}))
	{
		return Cancelled();
	}

	std::filesystem::path outputDirectory;
	const GenerationFailure reservation = ReserveOutputDirectory(
		m_OutputRoot,
		*request,
		m_Limits,
		outputDirectory);
	if (reservation)
		return Failed(reservation.Code, reservation.Message);

	std::vector<GeneratedArtifact> generated;
	generated.reserve(m_Limits.MaxArtifacts);
	GenerationFailure generation;
	switch (input->Format)
	{
	case SnapshotDumpFormat::Sdk:
		generation = GenerateSdk(*input, m_Limits, context, generated);
		break;
	case SnapshotDumpFormat::Usmap:
		generation = GenerateUsmap(*input, m_Limits, context, generated);
		break;
	case SnapshotDumpFormat::Dumpspace:
		generation = GenerateDumpspace(*input, m_Limits, context, generated);
		break;
	case SnapshotDumpFormat::IdaScript:
		generation = GenerateIdaScript(*input, m_Limits, context, generated);
		break;
	}
	if (generation)
	{
		if (generation.Code == "DUMP_WORKER_CANCELLED")
			return Cancelled();
		return Failed(generation.Code, generation.Message);
	}
	if (generated.empty() || generated.size() + 1 > m_Limits.MaxArtifacts)
		return Failed("DUMP_ARTIFACT_COUNT_INVALID", "The generator produced no artifacts or exceeded the artifact count limit.");

	std::size_t totalBytes = 0;
	for (const GeneratedArtifact& artifact : generated)
	{
		if (artifact.Bytes.empty()
			|| artifact.Bytes.size() > m_Limits.MaxArtifactBytes
			|| !CheckedAdd(totalBytes, artifact.Bytes.size())
			|| totalBytes > m_Limits.MaxTotalArtifactBytes)
		{
			return Failed("DUMP_ARTIFACT_LIMIT_EXCEEDED", "Generated artifacts exceed the configured byte limits.");
		}
	}

	std::vector<WrittenArtifact> written;
	written.reserve(generated.size() + 1);
	std::size_t completed = 0;
	for (const GeneratedArtifact& artifact : generated)
	{
		if (context.IsCancellationRequested())
			return Cancelled();
		WrittenArtifact committed;
		const GenerationFailure write = WriteAndVerifyArtifact(
			outputDirectory,
			artifact,
			context,
			committed);
		if (write)
		{
			if (write.Code == "DUMP_WORKER_CANCELLED")
				return Cancelled();
			return Failed(write.Code, write.Message);
		}
		written.push_back(std::move(committed));
		++completed;
		if (!context.ReportProgress({
			.Phase = "write-verify",
			.Completed = static_cast<std::uint64_t>(completed),
			.Total = static_cast<std::uint64_t>(generated.size() + 1),
			.Message = "Committed and SHA-256 verified an artifact."}))
		{
			return Cancelled();
		}
		if (!context.ReportDiagnostic(DumpJobDiagnostic{
			.Severity = DumpJobDiagnosticSeverity::Info,
			.Code = "DUMP_ARTIFACT_VERIFIED",
			.Message = written.back().Name + " committed with size/hash verification."}))
		{
			return Cancelled();
		}
	}

	GeneratedArtifact manifest = BuildManifest(*request, *input, written, m_Limits.MaxArtifactBytes);
	if (manifest.Bytes.empty()
		|| !CheckedAdd(totalBytes, manifest.Bytes.size())
		|| totalBytes > m_Limits.MaxTotalArtifactBytes)
	{
		return Failed("DUMP_MANIFEST_LIMIT_EXCEEDED", "The completion manifest exceeds the configured artifact limits.");
	}
	if (!context.ReportProgress({
		.Phase = "commit-manifest",
		.Completed = static_cast<std::uint64_t>(generated.size()),
		.Total = static_cast<std::uint64_t>(generated.size() + 1),
		.Message = "Committing manifest.json as the final success marker."}))
	{
		return Cancelled();
	}
	WrittenArtifact committedManifest;
	const GenerationFailure manifestWrite = WriteAndVerifyArtifact(
		outputDirectory,
		manifest,
		context,
		committedManifest);
	if (manifestWrite)
	{
		if (manifestWrite.Code == "DUMP_WORKER_CANCELLED")
			return Cancelled();
		return Failed(manifestWrite.Code, manifestWrite.Message);
	}
	return {
		.Status = DumpJobWorkerStatus::Succeeded,
		.SuccessCommitted = true};
}

std::optional<std::filesystem::path> ResolveDefaultSnapshotDumpRoot() noexcept
{
	PWSTR value = nullptr;
	const HRESULT result = SHGetKnownFolderPath(
		FOLDERID_LocalAppData,
		KF_FLAG_CREATE,
		nullptr,
		&value);
	if (FAILED(result) || !value)
	{
		if (value)
			CoTaskMemFree(value);
		return std::nullopt;
	}
	try
	{
		std::filesystem::path path(value);
		CoTaskMemFree(value);
		value = nullptr;
		path /= L"UExplorer";
		path /= L"Dumps";
		path = path.lexically_normal();
		if (!path.is_absolute() || path.native().size() > kMaxWindowsPathChars)
			return std::nullopt;
		return path;
	}
	catch (...)
	{
		if (value)
			CoTaskMemFree(value);
		return std::nullopt;
	}
}

} // namespace UExplorer::Services
