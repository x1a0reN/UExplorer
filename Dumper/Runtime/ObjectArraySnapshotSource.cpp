#include "ObjectArraySnapshotSource.h"

#include "SafeMemory.h"

#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaxOuterDepth = 128;
constexpr std::uint64_t kCastEnum = 0x0000000000000004ULL;
constexpr std::uint64_t kCastStruct = 0x0000000000000008ULL;
constexpr std::uint64_t kCastClass = 0x0000000000000020ULL;
constexpr std::uint64_t kCastFunction = 0x0000000000080000ULL;
constexpr std::uint64_t kCastPackage = 0x0000000400000000ULL;

bool TryAddFieldAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	const std::size_t size,
	std::uintptr_t& address) noexcept
{
	address = 0;
	if (base == 0 || offset <= 0)
		return false;
	const auto unsignedOffset = static_cast<std::uintptr_t>(offset);
	if (unsignedOffset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	address = base + unsignedOffset;
	std::uintptr_t ignored = 0;
	return CheckedAddressRange(address, size, ignored);
}

bool SameIdentity(
	const ObjectIdentity& identity,
	const ObjectHandle& handle) noexcept
{
	return identity.Index == handle.Index
		&& identity.SerialNumber == handle.SerialNumber
		&& identity.Address == handle.Address
		&& identity.ClassFingerprint == handle.ClassFingerprint;
}

bool SameHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameRecord(
	const EngineSnapshotObject& left,
	const EngineSnapshotObject& right) noexcept
{
	return SameHandle(left.Handle, right.Handle)
		&& left.Name == right.Name
		&& left.FullPath == right.FullPath
		&& left.ClassPath == right.ClassPath
		&& left.PackagePath == right.PackagePath
		&& left.Kind == right.Kind;
}

std::string LeafName(const std::string& rawName)
{
	const std::size_t separator = rawName.rfind('/');
	return separator == std::string::npos ? rawName : rawName.substr(separator + 1);
}

} // namespace

ObjectArraySnapshotSource::ObjectArraySnapshotSource(
	std::shared_ptr<const EngineContext> context,
	EngineFacade& engine,
	IObjectSnapshotIdentitySource& identitySource)
	: m_Context(std::move(context)),
	  m_Engine(engine),
	  m_IdentitySource(identitySource),
	  m_Offsets(m_Context ? CaptureObjectIdentityContext(*m_Context) : ObjectIdentityContext{})
{
}

bool ObjectArraySnapshotSource::IsConfigured() const noexcept
{
	return m_Context
		&& m_Context.get() == m_Engine.Context().get()
		&& m_Context->Generation() != 0
		&& m_IdentitySource.ContextGeneration() == m_Context->Generation()
		&& m_IdentitySource.CanReadObjectSlots()
		&& m_Engine.IsConfigured()
		&& m_Engine.Names().IsConfigured()
		&& m_Offsets.ContextGeneration == m_Context->Generation()
		&& m_Offsets.ObjectIndex > 0
		&& m_Offsets.ObjectClass > 0
		&& m_Offsets.ObjectName > 0
		&& m_Offsets.ObjectOuter > 0
		&& m_Offsets.ClassCastFlags > 0;
}

std::uint64_t ObjectArraySnapshotSource::ContextGeneration() const noexcept
{
	return m_Context ? m_Context->Generation() : 0;
}

bool ObjectArraySnapshotSource::IsCurrentExecutionThreadValid() const noexcept
{
	return IsConfigured()
		&& m_Engine.IsCurrentExecutionThreadValid()
		&& m_IdentitySource.IsCurrentExecutionThreadValid();
}

bool ObjectArraySnapshotSource::TryGetObjectCount(std::int32_t& objectCount)
{
	objectCount = -1;
	try
	{
		return IsCurrentExecutionThreadValid()
			&& m_IdentitySource.TryGetObjectCount(objectCount)
			&& objectCount >= 0
			&& objectCount <= EngineSnapshotStore::kMaxSourceObjectCount;
	}
	catch (...)
	{
		objectCount = -1;
		return false;
	}
}

bool ObjectArraySnapshotSource::TryValidateLiveAddress(const std::uintptr_t address) const
{
	std::uintptr_t indexAddress = 0;
	if (!TryAddFieldAddress(address, m_Offsets.ObjectIndex, sizeof(std::int32_t), indexAddress))
		return false;
	std::int32_t index = -1;
	if (!ReadValue(indexAddress, index).Ok() || index < 0)
		return false;
	ObjectIdentity identity;
	return m_IdentitySource.TryReadObjectSlot(index, identity)
		== ObjectSnapshotSlotReadResult::Captured
		&& identity.Index == index
		&& identity.Address == address;
}

bool ObjectArraySnapshotSource::TryReadNode(
	const std::uintptr_t address,
	ObjectNode& node) const
{
	node = {};
	if (!TryValidateLiveAddress(address))
		return false;

	std::uintptr_t classField = 0;
	std::uintptr_t nameField = 0;
	std::uintptr_t outerField = 0;
	if (!TryAddFieldAddress(address, m_Offsets.ObjectClass, sizeof(void*), classField)
		|| !TryAddFieldAddress(
			address,
			m_Offsets.ObjectName,
			static_cast<std::size_t>(m_Context->NameProfile().FNameSize),
			nameField)
		|| !TryAddFieldAddress(address, m_Offsets.ObjectOuter, sizeof(void*), outerField))
	{
		return false;
	}

	std::uintptr_t classAddress = 0;
	std::uintptr_t outerAddress = 0;
	if (!ReadValue(classField, classAddress).Ok() || classAddress == 0
		|| !ReadValue(outerField, outerAddress).Ok())
	{
		return false;
	}
	const EngineNameResult name = m_Engine.Names().DecodeFName(nameField);
	if (!name.Ok()
		|| name.Value.empty()
		|| name.Value.size() > EngineSnapshotStore::kMaxNameBytes)
	{
		return false;
	}

	std::uintptr_t finalClassAddress = 0;
	std::uintptr_t finalOuterAddress = 0;
	if (!ReadValue(classField, finalClassAddress).Ok()
		|| !ReadValue(outerField, finalOuterAddress).Ok()
		|| finalClassAddress != classAddress
		|| finalOuterAddress != outerAddress
		|| !TryValidateLiveAddress(address))
	{
		return false;
	}

	node = {
		.Address = address,
		.ClassAddress = classAddress,
		.OuterAddress = outerAddress,
		.RawName = name.Value
	};
	return true;
}

bool ObjectArraySnapshotSource::TryReadKind(
	const std::uintptr_t classAddress,
	EngineObjectKind& kind) const
{
	std::uintptr_t castFlagsAddress = 0;
	if (!TryValidateLiveAddress(classAddress)
		|| !TryAddFieldAddress(
			classAddress,
			m_Offsets.ClassCastFlags,
			sizeof(std::uint64_t),
			castFlagsAddress))
	{
		return false;
	}
	std::uint64_t castFlags = 0;
	if (!ReadValue(castFlagsAddress, castFlags).Ok())
		return false;

	const auto has = [castFlags](const std::uint64_t flag) {
		return (castFlags & flag) != 0;
	};
	if (has(kCastPackage))
		kind = EngineObjectKind::Package;
	else if (has(kCastClass))
		kind = EngineObjectKind::Class;
	else if (has(kCastFunction))
		kind = EngineObjectKind::Function;
	else if (has(kCastEnum))
		kind = EngineObjectKind::Enum;
	else if (has(kCastStruct))
		kind = EngineObjectKind::Struct;
	else
		kind = EngineObjectKind::Object;
	return TryValidateLiveAddress(classAddress);
}

bool ObjectArraySnapshotSource::TryBuildPath(
	const std::uintptr_t address,
	ObjectPath& path) const
{
	path = {};
	std::vector<ObjectNode> nodes;
	nodes.reserve(8);
	std::uintptr_t current = address;
	while (current != 0)
	{
		if (nodes.size() == kMaxOuterDepth)
			return false;
		for (const ObjectNode& existing : nodes)
		{
			if (existing.Address == current)
				return false;
		}
		ObjectNode node;
		if (!TryReadNode(current, node))
			return false;
		nodes.push_back(std::move(node));
		current = nodes.back().OuterAddress;
	}
	if (nodes.empty())
		return false;

	EngineObjectKind outermostKind = EngineObjectKind::Object;
	if (!TryReadKind(nodes.back().ClassAddress, outermostKind)
		|| outermostKind != EngineObjectKind::Package)
	{
		return false;
	}

	std::string fullPath;
	for (std::size_t reverseIndex = nodes.size(); reverseIndex > 0; --reverseIndex)
	{
		const std::string& component = nodes[reverseIndex - 1].RawName;
		const std::size_t separatorBytes = fullPath.empty() ? 0 : 1;
		if (component.empty()
			|| fullPath.size() > EngineSnapshotStore::kMaxPathBytes - separatorBytes
			|| component.size() > EngineSnapshotStore::kMaxPathBytes - separatorBytes - fullPath.size())
		{
			return false;
		}
		if (!fullPath.empty())
			fullPath.push_back('.');
		fullPath += component;
	}

	std::string leafName = LeafName(nodes.front().RawName);
	if (leafName.empty() || leafName.size() > EngineSnapshotStore::kMaxNameBytes)
		return false;
	path = {
		.LeafName = std::move(leafName),
		.FullPath = std::move(fullPath),
		.PackagePath = nodes.back().RawName,
		.ClassAddress = nodes.front().ClassAddress
	};
	return !path.PackagePath.empty()
		&& path.PackagePath.size() <= EngineSnapshotStore::kMaxPathBytes;
}

bool ObjectArraySnapshotSource::TryBuildRecord(
	const ObjectHandle& handle,
	EngineSnapshotObject& object) const
{
	object = {};
	ObjectPath objectPath;
	if (!TryBuildPath(handle.Address, objectPath))
		return false;
	EngineObjectKind kind = EngineObjectKind::Object;
	if (!TryReadKind(objectPath.ClassAddress, kind))
		return false;
	ObjectPath classPath;
	if (!TryBuildPath(objectPath.ClassAddress, classPath))
		return false;

	object = {
		.Handle = handle,
		.Name = std::move(objectPath.LeafName),
		.FullPath = std::move(objectPath.FullPath),
		.ClassPath = std::move(classPath.FullPath),
		.PackagePath = std::move(objectPath.PackagePath),
		.Kind = kind
	};
	return true;
}

SnapshotSlotReadResult ObjectArraySnapshotSource::TryCaptureObject(
	const std::int32_t index,
	EngineSnapshotObject& object)
{
	object = {};
	try
	{
		if (!IsCurrentExecutionThreadValid() || index < 0)
			return SnapshotSlotReadResult::Failed;

		ObjectIdentity slotIdentity;
		const ObjectSnapshotSlotReadResult slot =
			m_IdentitySource.TryReadObjectSlot(index, slotIdentity);
		if (slot == ObjectSnapshotSlotReadResult::Empty)
			return SnapshotSlotReadResult::Empty;
		if (slot != ObjectSnapshotSlotReadResult::Captured)
			return SnapshotSlotReadResult::Failed;

		const ObjectHandleResult issued = m_Engine.IssueObjectHandle(index);
		if (!issued.Ok() || !SameIdentity(slotIdentity, issued.Value))
			return SnapshotSlotReadResult::Failed;

		EngineSnapshotObject first;
		EngineSnapshotObject second;
		if (!TryBuildRecord(issued.Value, first)
			|| !TryBuildRecord(issued.Value, second)
			|| !SameRecord(first, second)
			|| !m_Engine.ValidateObjectHandle(issued.Value).Ok())
		{
			return SnapshotSlotReadResult::Failed;
		}
		object = std::move(first);
		return SnapshotSlotReadResult::Captured;
	}
	catch (...)
	{
		object = {};
		return SnapshotSlotReadResult::Failed;
	}
}

bool ObjectArraySnapshotSource::ValidateSlot(
	const std::int32_t index,
	const EngineSnapshotObject* expectedObject)
{
	try
	{
		EngineSnapshotObject current;
		const SnapshotSlotReadResult result = TryCaptureObject(index, current);
		if (!expectedObject)
			return result == SnapshotSlotReadResult::Empty;
		return result == SnapshotSlotReadResult::Captured
			&& SameRecord(current, *expectedObject);
	}
	catch (...)
	{
		return false;
	}
}

} // namespace UExplorer::Runtime
