#include "EngineFacade.h"

#include <Windows.h>

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
		m_Context ? m_Context->Generation() : 0),
	  m_Types(
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
		&& !m_Snapshots.IsStopped()
		&& m_Types.IsConfigured()
		&& !m_Types.IsStopped();
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

bool EngineFacade::ConfigureReflectionLayout(
	std::shared_ptr<const ReflectionLayout> layout,
	const bool includeFlatPropertyCodec) noexcept
{
	if (!layout
		|| !IsCurrentExecutionThreadValid()
		|| layout->ValidatedOnThreadId() != GetCurrentThreadId()
		|| !IsReflectionLayoutValid(*layout, ContextGeneration()))
	{
		return false;
	}
	try
	{
		std::shared_ptr<const PropertyCodec> properties;
		if (includeFlatPropertyCodec)
		{
			PropertyCodecProfile profile{
				.Validated = true,
				.ReflectionLayoutFingerprint = layout->Fingerprint(),
				.Source = "witnessed_flat_property_descriptors_v1"
			};
			properties = std::make_shared<const PropertyCodec>(m_Names, std::move(profile));
			if (!properties->IsConfigured())
				return false;
		}
		auto reflection = std::make_shared<const ReflectionRuntimeSnapshot>(
			ReflectionRuntimeSnapshot{
				.Layout = std::move(layout),
				.Properties = std::move(properties)
			});
		if (!reflection->IsLayoutConfigured(ContextGeneration())
			|| reflection->IsPropertyCodecConfigured(ContextGeneration())
				!= includeFlatPropertyCodec)
			return false;
		std::lock_guard lock(m_ReflectionMutex);
		if (!IsCurrentExecutionThreadValid()
			|| reflection->Layout->ValidatedOnThreadId() != GetCurrentThreadId()
			|| Reflection())
		{
			return false;
		}
		m_Reflection.store(std::move(reflection), std::memory_order_release);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool EngineFacade::ConfigurePropertyCodec(PropertyCodecProfile profile) noexcept
{
	const std::shared_ptr<const ReflectionRuntimeSnapshot> current = Reflection();
	if (!current
		|| !current->IsLayoutConfigured(ContextGeneration())
		|| current->Properties
		|| !IsCurrentExecutionThreadValid()
		|| current->Layout->ValidatedOnThreadId() != GetCurrentThreadId()
		|| profile.ReflectionLayoutFingerprint != current->Layout->Fingerprint())
	{
		return false;
	}
	try
	{
		auto codec = std::make_shared<const PropertyCodec>(m_Names, std::move(profile));
		if (!codec->IsConfigured())
			return false;
		auto upgraded = std::make_shared<const ReflectionRuntimeSnapshot>(
			ReflectionRuntimeSnapshot{
				.Layout = current->Layout,
				.Properties = std::move(codec)
			});
		if (!upgraded->IsPropertyCodecConfigured(ContextGeneration()))
			return false;
		std::lock_guard lock(m_ReflectionMutex);
		if (!IsCurrentExecutionThreadValid()
			|| upgraded->Layout->ValidatedOnThreadId() != GetCurrentThreadId()
			|| Reflection() != current)
		{
			return false;
		}
		m_Reflection.store(std::move(upgraded), std::memory_order_release);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool EngineFacade::ConfigureReflectionCapture(
	IReflectionCandidateSource& source,
	const bool includeFlatPropertyCodec) noexcept
{
	if (!IsConfigured()
		|| m_ReflectionCapture
		|| Reflection()
		|| source.ContextGeneration() != ContextGeneration())
	{
		return false;
	}
	try
	{
		auto capture = std::make_unique<ReflectionLayoutCapture>(
			ContextGeneration(),
			source,
			*this,
			includeFlatPropertyCodec);
		if (!capture->IsConfigured())
			return false;
		m_ReflectionCapture = std::move(capture);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

TypeSnapshotPublishResult EngineFacade::PublishTypeSnapshot(
	TypeSnapshotCandidate candidate) noexcept
{
	if (!IsConfigured())
		return {.Error = TypeSnapshotPublishError::StoreInvalid};
	const std::shared_ptr<const EngineSnapshot> objects = m_Snapshots.Current();
	const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection = Reflection();
	return m_Types.Publish(
		std::move(candidate),
		objects,
		reflection);
}

bool EngineFacade::ConfigureTypeSnapshotCapture(ITypeSnapshotSource& source) noexcept
{
	if (!IsConfigured()
		|| m_TypeCapture
		|| source.ContextGeneration() != ContextGeneration())
	{
		return false;
	}
	try
	{
		auto capture = std::make_unique<TypeSnapshotCapture>(
			ContextGeneration(),
			source,
			*this);
		if (!capture->IsConfigured())
			return false;
		m_TypeCapture = std::move(capture);
		return true;
	}
	catch (...)
	{
		return false;
	}
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
	if (m_TypeCapture && !m_TypeCapture->StopAndDrain(timeout))
		return false;
	if (m_ReflectionCapture && !m_ReflectionCapture->StopAndDrain(timeout))
		return false;
	if (m_SnapshotCapture && !m_SnapshotCapture->StopAndDrain(timeout))
		return false;
	std::lock_guard lock(m_ReflectionMutex);
	m_TypeCapture.reset();
	m_ReflectionCapture.reset();
	m_SnapshotCapture.reset();
	m_Types.Stop();
	m_Reflection.store({}, std::memory_order_release);
	m_Snapshots.Stop();
	return true;
}

} // namespace UExplorer::Runtime
