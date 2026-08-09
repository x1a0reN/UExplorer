#pragma once

#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

struct ShutdownStageResult
{
	std::string Name;
	bool Succeeded = false;
	std::string Error;
};

struct ShutdownReport
{
	bool SafeToUnload = false;
	std::vector<ShutdownStageResult> Stages;
};

class ShutdownCoordinator final
{
public:
	using Stage = std::function<bool()>;

	void AddStage(std::string name, Stage stage)
	{
		if (name.empty() || !stage)
			throw std::invalid_argument("Shutdown stage must have a name and callback");
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_HasRun)
			throw std::logic_error("Shutdown stages cannot be added after execution");
		m_Stages.push_back({std::move(name), std::move(stage)});
	}

	ShutdownReport Run()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_HasRun)
			return m_Report;

		m_HasRun = true;
		m_Report.SafeToUnload = true;
		for (const RegisteredStage& registered : m_Stages)
		{
			ShutdownStageResult result{.Name = registered.Name};
			try
			{
				result.Succeeded = registered.Callback();
				if (!result.Succeeded)
					result.Error = "Stage could not prove a safe stop";
			}
			catch (const std::exception& error)
			{
				result.Error = error.what();
			}
			catch (...)
			{
				result.Error = "Unknown shutdown exception";
			}
			m_Report.SafeToUnload = m_Report.SafeToUnload && result.Succeeded;
			m_Report.Stages.push_back(std::move(result));
		}
		return m_Report;
	}

private:
	struct RegisteredStage
	{
		std::string Name;
		Stage Callback;
	};

	std::mutex m_Mutex;
	std::vector<RegisteredStage> m_Stages;
	bool m_HasRun = false;
	ShutdownReport m_Report;
};

} // namespace UExplorer::Runtime
