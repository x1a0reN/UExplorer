#include "EngineFacade.h"

#include <utility>

namespace UExplorer::Runtime
{

EngineFacade::EngineFacade(
	std::shared_ptr<const EngineContext> context,
	std::string sessionId,
	IHandleIdentitySource& identitySource)
	: m_Context(std::move(context)),
	  m_SessionId(std::move(sessionId)),
	  m_IdentitySource(identitySource),
	  m_Names(m_Context ? m_Context->NameProfile() : EngineNameProfile{}),
	  m_Handles(
		m_SessionId,
		m_Context ? m_Context->Generation() : 0,
		m_IdentitySource),
	  m_Snapshots(
		m_SessionId,
		m_Context ? m_Context->Generation() : 0)
{
}

bool EngineFacade::IsConfigured() const noexcept
{
	return m_Context
		&& m_Context->Generation() != 0
		&& m_IdentitySource.ContextGeneration() == m_Context->Generation()
		&& m_Handles.IsConfigured()
		&& m_Snapshots.IsConfigured()
		&& !m_Snapshots.IsStopped();
}

std::uint64_t EngineFacade::ContextGeneration() const noexcept
{
	return m_Context ? m_Context->Generation() : 0;
}

bool EngineFacade::IsCurrentExecutionThreadValid() const noexcept
{
	return IsConfigured() && m_IdentitySource.IsCurrentExecutionThreadValid();
}

ObjectHandleResult EngineFacade::IssueObjectHandle(const std::int32_t index)
{
	return m_Handles.IssueObject(index);
}

ObjectValidationResult EngineFacade::ValidateObjectHandle(const ObjectHandle& handle)
{
	return m_Handles.ValidateObject(handle);
}

FunctionHandleResult EngineFacade::IssueFunctionHandle(const std::int32_t index)
{
	return m_Handles.IssueFunction(index);
}

FunctionValidationResult EngineFacade::ValidateFunctionHandle(const FunctionHandle& handle)
{
	return m_Handles.ValidateFunction(handle);
}

bool EngineFacade::ConfigureSnapshotCapture(IEngineSnapshotSource& source) noexcept
{
	if (!IsConfigured()
		|| m_SnapshotCapture
		|| source.ContextGeneration() != ContextGeneration())
	{
		return false;
	}
	try
	{
		auto capture = std::make_unique<EngineSnapshotCapture>(
			m_SessionId,
			ContextGeneration(),
			source,
			m_Snapshots);
		if (!capture->IsConfigured())
			return false;
		m_SnapshotCapture = std::move(capture);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool EngineFacade::Stop(const std::chrono::milliseconds timeout)
{
	if (m_SnapshotCapture && !m_SnapshotCapture->StopAndDrain(timeout))
		return false;
	m_SnapshotCapture.reset();
	m_Snapshots.Stop();
	return true;
}

} // namespace UExplorer::Runtime
