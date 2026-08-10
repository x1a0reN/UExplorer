#pragma once

#include "EngineContext.h"
#include "EngineNameCodec.h"
#include "EngineSnapshot.h"
#include "EngineSnapshotCapture.h"
#include "ObjectHandle.h"
#include "PropertyCodec.h"
#include "ReflectionLayout.h"
#include "ReflectionLayoutCapture.h"
#include "TypeSnapshot.h"
#include "TypeSnapshotCapture.h"

#include <atomic>
#include <memory>
#include <chrono>
#include <mutex>
#include <string>

namespace UExplorer::Runtime
{

// Owns the immutable engine generation boundary used by every live domain command.
class EngineFacade final
{
public:
	EngineFacade(
		std::shared_ptr<const EngineContext> context,
		std::string sessionId,
		IHandleIdentitySource& identitySource);
	EngineFacade(const EngineFacade&) = delete;
	EngineFacade& operator=(const EngineFacade&) = delete;

	bool IsConfigured() const noexcept;
	const std::string& SessionId() const noexcept { return m_SessionId; }
	std::uint64_t ContextGeneration() const noexcept;
	const std::shared_ptr<const EngineContext>& Context() const noexcept { return m_Context; }
	bool IsCurrentExecutionThreadValid() const noexcept;

	ObjectHandleResult IssueObjectHandle(std::int32_t index);
	ObjectValidationResult ValidateObjectHandle(const ObjectHandle& handle);
	FunctionHandleResult IssueFunctionHandle(std::int32_t index);
	FunctionValidationResult ValidateFunctionHandle(const FunctionHandle& handle);
	const EngineNameCodec& Names() const noexcept { return m_Names; }
	bool ConfigureReflectionLayout(
		std::shared_ptr<const ReflectionLayout> layout,
		bool includeFlatPropertyCodec = false) noexcept;
	bool ConfigurePropertyCodec(PropertyCodecProfile profile) noexcept;
	bool ConfigureReflectionCapture(
		IReflectionCandidateSource& source,
		bool includeFlatPropertyCodec = false) noexcept;
	std::shared_ptr<const ReflectionRuntimeSnapshot> Reflection() const noexcept
	{
		return m_Reflection.load(std::memory_order_acquire);
	}
	std::shared_ptr<const PropertyCodec> Properties() const noexcept
	{
		const auto reflection = Reflection();
		return reflection ? reflection->Properties : nullptr;
	}

	EngineSnapshotStore& Snapshots() noexcept { return m_Snapshots; }
	const EngineSnapshotStore& Snapshots() const noexcept { return m_Snapshots; }
	const TypeSnapshotStore& Types() const noexcept { return m_Types; }
	bool ConfigureTypeSnapshotCapture(ITypeSnapshotSource& source) noexcept;
	bool ConfigureSnapshotCapture(IEngineSnapshotSource& source) noexcept;
	EngineSnapshotCapture* SnapshotCapture() noexcept { return m_SnapshotCapture.get(); }
	const EngineSnapshotCapture* SnapshotCapture() const noexcept { return m_SnapshotCapture.get(); }
	ReflectionLayoutCapture* ReflectionCapture() noexcept { return m_ReflectionCapture.get(); }
	const ReflectionLayoutCapture* ReflectionCapture() const noexcept
	{
		return m_ReflectionCapture.get();
	}
	TypeSnapshotCapture* TypeCapture() noexcept { return m_TypeCapture.get(); }
	const TypeSnapshotCapture* TypeCapture() const noexcept { return m_TypeCapture.get(); }
	bool Stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
	friend class TypeSnapshotCapture;
	TypeSnapshotPublishResult PublishTypeSnapshot(TypeSnapshotCandidate candidate) noexcept;

	std::shared_ptr<const EngineContext> m_Context;
	std::string m_SessionId;
	IHandleIdentitySource& m_IdentitySource;
	EngineNameCodec m_Names;
	std::atomic<std::shared_ptr<const ReflectionRuntimeSnapshot>> m_Reflection;
	mutable std::mutex m_ReflectionMutex;
	ObjectHandleService m_Handles;
	EngineSnapshotStore m_Snapshots;
	TypeSnapshotStore m_Types;
	std::unique_ptr<EngineSnapshotCapture> m_SnapshotCapture;
	std::unique_ptr<ReflectionLayoutCapture> m_ReflectionCapture;
	std::unique_ptr<TypeSnapshotCapture> m_TypeCapture;
};

} // namespace UExplorer::Runtime
