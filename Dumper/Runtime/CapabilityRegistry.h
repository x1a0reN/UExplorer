#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

struct CapabilityStatus
{
	std::string Name;
	bool Available = false;
	std::string ReasonCode;
	std::string Reason;
	std::vector<std::string> Dependencies;
};

class CapabilitySnapshot final
{
public:
	CapabilitySnapshot(
		const std::uint64_t contextGeneration,
		std::map<std::string, CapabilityStatus> capabilities)
		: m_ContextGeneration(contextGeneration),
		  m_Capabilities(std::move(capabilities))
	{
	}

	std::uint64_t ContextGeneration() const noexcept { return m_ContextGeneration; }
	const std::map<std::string, CapabilityStatus>& All() const noexcept { return m_Capabilities; }

	const CapabilityStatus* Find(const std::string& name) const
	{
		const auto it = m_Capabilities.find(name);
		return it == m_Capabilities.end() ? nullptr : &it->second;
	}

	bool IsAvailable(const std::string& name) const
	{
		const CapabilityStatus* capability = Find(name);
		return capability && capability->Available;
	}

private:
	std::uint64_t m_ContextGeneration;
	std::map<std::string, CapabilityStatus> m_Capabilities;
};

class CapabilityRegistryBuilder final
{
public:
	CapabilityRegistryBuilder& Define(
		std::string name,
		const bool probeAvailable,
		std::string reasonCode = {},
		std::string reason = {},
		std::vector<std::string> dependencies = {})
	{
		if (name.empty())
			throw std::invalid_argument("Capability name must not be empty");

		Definition definition{
			.Name = name,
			.ProbeAvailable = probeAvailable,
			.ReasonCode = std::move(reasonCode),
			.Reason = std::move(reason),
			.Dependencies = std::move(dependencies)
		};
		if (!m_Definitions.emplace(std::move(name), std::move(definition)).second)
			throw std::invalid_argument("Duplicate capability definition");
		return *this;
	}

	std::shared_ptr<const CapabilitySnapshot> Build(const std::uint64_t contextGeneration) const
	{
		if (contextGeneration == 0)
			throw std::invalid_argument("Capability context generation must be non-zero");

		std::map<std::string, CapabilityStatus> statuses;
		std::set<std::string> visiting;
		for (const auto& [name, definition] : m_Definitions)
		{
			(void)definition;
			Resolve(name, visiting, statuses);
		}
		return std::make_shared<const CapabilitySnapshot>(contextGeneration, std::move(statuses));
	}

private:
	struct Definition
	{
		std::string Name;
		bool ProbeAvailable = false;
		std::string ReasonCode;
		std::string Reason;
		std::vector<std::string> Dependencies;
	};

	const CapabilityStatus& Resolve(
		const std::string& name,
		std::set<std::string>& visiting,
		std::map<std::string, CapabilityStatus>& statuses) const
	{
		if (const auto existing = statuses.find(name); existing != statuses.end())
			return existing->second;

		const auto definitionIt = m_Definitions.find(name);
		if (definitionIt == m_Definitions.end())
			throw std::invalid_argument("Capability dependency is not defined: " + name);
		if (!visiting.insert(name).second)
			throw std::invalid_argument("Capability dependency cycle at: " + name);

		const Definition& definition = definitionIt->second;
		CapabilityStatus status{
			.Name = definition.Name,
			.Available = definition.ProbeAvailable,
			.ReasonCode = definition.ReasonCode,
			.Reason = definition.Reason,
			.Dependencies = definition.Dependencies
		};

		if (status.Available)
		{
			for (const std::string& dependencyName : definition.Dependencies)
			{
				const CapabilityStatus& dependency = Resolve(dependencyName, visiting, statuses);
				if (!dependency.Available)
				{
					status.Available = false;
					status.ReasonCode = "DEPENDENCY_UNAVAILABLE";
					status.Reason = "Dependency unavailable: " + dependencyName;
					if (!dependency.ReasonCode.empty())
						status.Reason += " (" + dependency.ReasonCode + ")";
					break;
				}
			}
		}

		if (!status.Available && status.ReasonCode.empty())
		{
			status.ReasonCode = "CAPABILITY_PROBE_FAILED";
			status.Reason = "Capability validation did not pass";
		}
		if (status.Available)
		{
			status.ReasonCode.clear();
			status.Reason.clear();
		}

		visiting.erase(name);
		return statuses.emplace(name, std::move(status)).first->second;
	}

	std::map<std::string, Definition> m_Definitions;
};

} // namespace UExplorer::Runtime
