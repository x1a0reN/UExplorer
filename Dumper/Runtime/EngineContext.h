#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

enum class ValidationState : std::uint8_t
{
	Missing,
	Discovered,
	Validated
};

inline const char* ToString(const ValidationState state)
{
	switch (state)
	{
	case ValidationState::Missing:
		return "missing";
	case ValidationState::Discovered:
		return "discovered";
	case ValidationState::Validated:
		return "validated";
	}
	return "unknown";
}

struct OffsetReport
{
	std::string Name;
	std::int64_t Value = -1;
	bool Required = false;
	ValidationState State = ValidationState::Missing;
	std::string Source;
	std::vector<std::string> Checks;
	std::string ReasonCode;
	std::string Reason;

	bool IsValidated() const noexcept
	{
		return State == ValidationState::Validated;
	}
};

struct EngineProfile
{
	bool UsesFProperty = false;
	bool UsesNamePool = false;
	bool UsesLargeWorldCoordinates = false;
	bool UsesCasePreservingName = false;
	bool EnumNameOnly = false;
	bool SmallEnumValue = false;
};

enum class EngineNameStorageKind : std::uint8_t
{
	Unavailable,
	ChunkedArray,
	NamePool
};

inline const char* ToString(const EngineNameStorageKind kind) noexcept
{
	switch (kind)
	{
	case EngineNameStorageKind::Unavailable: return "unavailable";
	case EngineNameStorageKind::ChunkedArray: return "chunked_array";
	case EngineNameStorageKind::NamePool: return "name_pool";
	}
	return "unknown";
}

struct EngineNameProfile
{
	EngineNameStorageKind Storage = EngineNameStorageKind::Unavailable;
	std::uintptr_t StorageAddress = 0;
	std::int32_t FNameSize = -1;
	std::int32_t ComparisonIndexOffset = -1;
	std::int32_t NumberOffset = -1;
	std::int32_t BlockOffsetBits = -1;
	std::int32_t EntryStride = -1;
	std::int32_t ChunksStart = -1;
	std::int32_t MaxChunkIndexOffset = -1;
	std::int32_t NumElementsOffset = -1;
	std::int32_t ByteCursorOffset = -1;
	std::int32_t EntryStringOffset = -1;
	std::int32_t EntryHeaderOffset = -1;
	std::int32_t EntryIndexOffset = -1;
	std::int32_t EntryLengthShift = -1;
	bool UsesOutlineNumber = false;
	bool Validated = false;
	std::string Source;
	std::vector<std::string> Checks;
	std::string ReasonCode;
	std::string Reason;
};

class EngineContext final
{
public:
	std::uint64_t Generation() const noexcept { return m_Generation; }
	std::uintptr_t ModuleBase() const noexcept { return m_ModuleBase; }
	std::uintptr_t ObjectArrayAddress() const noexcept { return m_ObjectArrayAddress; }
	std::uint32_t ProcessId() const noexcept { return m_ProcessId; }
	std::int32_t ObjectCount() const noexcept { return m_ObjectCount; }
	const std::string& GameName() const noexcept { return m_GameName; }
	const std::string& GameVersion() const noexcept { return m_GameVersion; }
	const EngineProfile& Profile() const noexcept { return m_Profile; }
	const EngineNameProfile& NameProfile() const noexcept { return m_NameProfile; }
	const std::map<std::string, OffsetReport>& Offsets() const noexcept { return m_Offsets; }

	const OffsetReport* FindOffset(const std::string& name) const
	{
		const auto it = m_Offsets.find(name);
		return it == m_Offsets.end() ? nullptr : &it->second;
	}

	bool HasValidatedOffset(const std::string& name) const
	{
		const OffsetReport* report = FindOffset(name);
		return report && report->IsValidated();
	}

private:
	friend class EngineContextBuilder;

	std::uint64_t m_Generation = 0;
	std::uintptr_t m_ModuleBase = 0;
	std::uintptr_t m_ObjectArrayAddress = 0;
	std::uint32_t m_ProcessId = 0;
	std::int32_t m_ObjectCount = 0;
	std::string m_GameName;
	std::string m_GameVersion;
	EngineProfile m_Profile;
	EngineNameProfile m_NameProfile;
	std::map<std::string, OffsetReport> m_Offsets;
};

class EngineContextBuilder final
{
public:
	explicit EngineContextBuilder(const std::uint64_t generation)
		: m_Generation(generation)
	{
	}

	EngineContextBuilder& SetIdentity(
		const std::uintptr_t moduleBase,
		const std::uintptr_t objectArrayAddress,
		const std::uint32_t processId,
		const std::int32_t objectCount,
		std::string gameName,
		std::string gameVersion)
	{
		m_ModuleBase = moduleBase;
		m_ObjectArrayAddress = objectArrayAddress;
		m_ProcessId = processId;
		m_ObjectCount = objectCount;
		m_GameName = std::move(gameName);
		m_GameVersion = std::move(gameVersion);
		return *this;
	}

	EngineContextBuilder& SetProfile(const EngineProfile& profile)
	{
		m_Profile = profile;
		return *this;
	}

	EngineContextBuilder& SetNameProfile(EngineNameProfile profile)
	{
		m_NameProfile = std::move(profile);
		return *this;
	}

	EngineContextBuilder& AddOffset(OffsetReport report)
	{
		if (report.Name.empty())
			throw std::invalid_argument("Offset report name must not be empty");
		if (!m_Offsets.emplace(report.Name, std::move(report)).second)
			throw std::invalid_argument("Duplicate offset report");
		return *this;
	}

	std::shared_ptr<const EngineContext> Build() const
	{
		if (m_Generation == 0)
			throw std::invalid_argument("Engine context generation must be non-zero");
		if (m_ModuleBase == 0 || m_ObjectArrayAddress == 0 || m_ProcessId == 0)
			throw std::runtime_error("Engine context identity is incomplete");
		if (m_ObjectCount < 0)
			throw std::runtime_error("Engine context object count is invalid");

		std::vector<std::string> invalidRequired;
		for (const auto& [name, report] : m_Offsets)
		{
			if (report.Required && !report.IsValidated())
				invalidRequired.push_back(name);
		}
		if (!invalidRequired.empty())
		{
			std::string message = "Required engine offsets are not validated:";
			for (const std::string& name : invalidRequired)
				message += " " + name;
			throw std::runtime_error(message);
		}

		auto context = std::shared_ptr<EngineContext>(new EngineContext());
		context->m_Generation = m_Generation;
		context->m_ModuleBase = m_ModuleBase;
		context->m_ObjectArrayAddress = m_ObjectArrayAddress;
		context->m_ProcessId = m_ProcessId;
		context->m_ObjectCount = m_ObjectCount;
		context->m_GameName = m_GameName;
		context->m_GameVersion = m_GameVersion;
		context->m_Profile = m_Profile;
		context->m_NameProfile = m_NameProfile;
		context->m_Offsets = m_Offsets;
		return std::shared_ptr<const EngineContext>(std::move(context));
	}

private:
	std::uint64_t m_Generation;
	std::uintptr_t m_ModuleBase = 0;
	std::uintptr_t m_ObjectArrayAddress = 0;
	std::uint32_t m_ProcessId = 0;
	std::int32_t m_ObjectCount = 0;
	std::string m_GameName;
	std::string m_GameVersion;
	EngineProfile m_Profile;
	EngineNameProfile m_NameProfile;
	std::map<std::string, OffsetReport> m_Offsets;
};

} // namespace UExplorer::Runtime
