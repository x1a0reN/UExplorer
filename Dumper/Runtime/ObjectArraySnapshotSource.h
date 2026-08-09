#pragma once

#include "EngineFacade.h"
#include "EngineSnapshotCapture.h"
#include "ObjectIdentityContext.h"
#include "ObjectSnapshotIdentitySource.h"

#include <cstdint>
#include <memory>
#include <string>

namespace UExplorer::Runtime
{

// Builds copied object metadata without using live UE wrappers or mutable offset globals.
class ObjectArraySnapshotSource final : public IEngineSnapshotSource
{
public:
	ObjectArraySnapshotSource(
		std::shared_ptr<const EngineContext> context,
		EngineFacade& engine,
		IObjectSnapshotIdentitySource& identitySource);

	bool IsConfigured() const noexcept;
	std::uint64_t ContextGeneration() const noexcept override;
	bool IsCurrentExecutionThreadValid() const noexcept override;
	bool TryGetObjectCount(std::int32_t& objectCount) override;
	SnapshotSlotReadResult TryCaptureObject(
		std::int32_t index,
		EngineSnapshotObject& object) override;
	bool ValidateSlot(
		std::int32_t index,
		const EngineSnapshotObject* expectedObject) override;

private:
	struct ObjectNode
	{
		std::uintptr_t Address = 0;
		std::uintptr_t ClassAddress = 0;
		std::uintptr_t OuterAddress = 0;
		std::string RawName;
	};

	struct ObjectPath
	{
		std::string LeafName;
		std::string FullPath;
		std::string PackagePath;
		std::uintptr_t ClassAddress = 0;
	};

	bool TryValidateLiveAddress(std::uintptr_t address) const;
	bool TryReadNode(std::uintptr_t address, ObjectNode& node) const;
	bool TryReadKind(std::uintptr_t classAddress, EngineObjectKind& kind) const;
	bool TryBuildPath(std::uintptr_t address, ObjectPath& path) const;
	bool TryBuildRecord(const ObjectHandle& handle, EngineSnapshotObject& object) const;

	std::shared_ptr<const EngineContext> m_Context;
	EngineFacade& m_Engine;
	IObjectSnapshotIdentitySource& m_IdentitySource;
	ObjectIdentityContext m_Offsets;
};

} // namespace UExplorer::Runtime
