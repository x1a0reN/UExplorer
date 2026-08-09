#pragma once

#include "EngineSnapshot.h"
#include "PropertyCodec.h"
#include "ReflectionLayout.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

enum class ReflectedTypeKind : std::uint8_t
{
	Class,
	Struct,
	Enum
};

const char* ToString(ReflectedTypeKind kind) noexcept;

enum class ReflectedMemberState : std::uint8_t
{
	Supported,
	Unsupported,
	Unavailable
};

const char* ToString(ReflectedMemberState state) noexcept;

enum class ReflectedParameterDirection : std::uint8_t
{
	Input,
	Output,
	InOut,
	Return
};

const char* ToString(ReflectedParameterDirection direction) noexcept;

enum class ReflectedFunctionImplementation : std::uint8_t
{
	Unavailable,
	Native,
	Bytecode,
	NativeAndBytecode
};

const char* ToString(ReflectedFunctionImplementation implementation) noexcept;

enum class ClassDefaultObjectState : std::uint8_t
{
	NotApplicable,
	Present,
	NotConstructed,
	Unavailable
};

const char* ToString(ClassDefaultObjectState state) noexcept;

struct ReflectedProperty
{
	std::string Name;
	std::string TypeName;
	PropertyKind Kind = PropertyKind::Unknown;
	std::uint32_t Offset = 0;
	std::uint32_t Size = 0;
	std::uint32_t ArrayDim = 1;
	std::uint64_t Flags = 0;
	ReflectedMemberState State = ReflectedMemberState::Unavailable;
	std::string ReasonCode;
	std::string Reason;
	std::shared_ptr<const PropertyDescriptor> Descriptor;
};

struct ReflectedParameter
{
	ReflectedParameterDirection Direction = ReflectedParameterDirection::Input;
	ReflectedProperty Property;
};

struct ReflectedFunction
{
	FunctionHandle Handle;
	std::string Name;
	std::string FullPath;
	std::uint64_t Flags = 0;
	std::uint32_t ParameterSize = 0;
	std::uintptr_t NativeAddress = 0;
	ReflectedFunctionImplementation Implementation =
		ReflectedFunctionImplementation::Unavailable;
	std::string ReasonCode;
	std::string Reason;
	std::vector<ReflectedParameter> Parameters;
};

struct ReflectedEnumEntry
{
	std::string Name;
	std::int64_t Value = 0;
};

struct ReflectedType
{
	ObjectHandle Handle;
	ReflectedTypeKind Kind = ReflectedTypeKind::Struct;
	std::string Name;
	std::string FullPath;
	std::string PackagePath;
	std::uint32_t PropertiesSize = 0;
	std::uint32_t MinAlignment = 0;
	std::optional<ObjectHandle> Super;
	ClassDefaultObjectState DefaultObjectState =
		ClassDefaultObjectState::NotApplicable;
	std::optional<ObjectHandle> DefaultObject;
	std::string DefaultObjectReasonCode;
	std::string DefaultObjectReason;
	std::vector<ReflectedProperty> DirectProperties;
	std::vector<ReflectedFunction> DirectFunctions;
	ReflectedMemberState EnumState = ReflectedMemberState::Unavailable;
	PropertyKind EnumUnderlyingKind = PropertyKind::Unknown;
	std::string EnumReasonCode;
	std::string EnumReason;
	std::vector<ReflectedEnumEntry> EnumEntries;
};

struct ReflectedFunctionLookup
{
	const ReflectedFunction* Function = nullptr;
	const ReflectedType* DeclaringType = nullptr;

	bool Found() const noexcept { return Function && DeclaringType; }
};

struct TypeSnapshotCandidate
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t Generation = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t ReflectionLayoutFingerprint = 0;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::uint64_t CaptureDurationUs = 0;
	std::string Source;
	std::vector<ReflectedType> Types;
};

enum class TypeSnapshotPublishError : std::uint8_t
{
	None,
	StoreInvalid,
	StoreStopped,
	DependencyInvalid,
	EnvelopeInvalid,
	CountLimitExceeded,
	TypeCoverageMismatch,
	TypeRecordInvalid,
	RelationshipInvalid,
	HierarchyCycle,
	HierarchyDepthExceeded,
	MemberLimitExceeded,
	PropertyInvalid,
	DescriptorInvalid,
	DescriptorCycle,
	FunctionCoverageMismatch,
	FunctionInvalid,
	EnumInvalid,
	GenerationNotMonotonic,
	WorkerThreadRequired,
	AllocationFailed
};

const char* ToString(TypeSnapshotPublishError error) noexcept;

class TypeSnapshot;

struct TypeSnapshotPublishResult
{
	TypeSnapshotPublishError Error = TypeSnapshotPublishError::None;
	std::int32_t TypeIndex = -1;
	std::int32_t MemberIndex = -1;
	std::shared_ptr<const TypeSnapshot> Snapshot;

	bool Ok() const noexcept
	{
		return Error == TypeSnapshotPublishError::None
			&& static_cast<bool>(Snapshot);
	}
};

class TypeSnapshot final
{
public:
	const std::string& SessionId() const noexcept { return m_SessionId; }
	std::uint64_t ContextGeneration() const noexcept { return m_ContextGeneration; }
	std::uint64_t Generation() const noexcept { return m_Generation; }
	std::uint64_t ObjectSnapshotGeneration() const noexcept
	{
		return m_ObjectSnapshotGeneration;
	}
	std::uint64_t ReflectionLayoutFingerprint() const noexcept
	{
		return m_ReflectionLayoutFingerprint;
	}
	std::uint64_t CapturedAtMonotonicUs() const noexcept
	{
		return m_CapturedAtMonotonicUs;
	}
	std::uint64_t CaptureDurationUs() const noexcept { return m_CaptureDurationUs; }
	const std::string& Source() const noexcept { return m_Source; }
	const std::vector<ReflectedType>& Types() const noexcept { return m_Types; }
	const ReflectedType* FindByFullPath(std::string_view fullPath) const noexcept;
	const ReflectedType* FindByObjectIndex(std::int32_t objectIndex) const noexcept;
	ReflectedFunctionLookup FindFunctionByFullPath(
		std::string_view fullPath) const noexcept;
	const std::vector<std::size_t>* FindDirectChildIndices(
		std::int32_t superObjectIndex) const noexcept;
	bool IsConfigured(std::uint64_t expectedContextGeneration) const noexcept;

private:
	friend class TypeSnapshotStore;
	TypeSnapshot() = default;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	std::uint64_t m_Generation = 0;
	std::uint64_t m_ObjectSnapshotGeneration = 0;
	std::uint64_t m_ReflectionLayoutFingerprint = 0;
	std::uint64_t m_CapturedAtMonotonicUs = 0;
	std::uint64_t m_CaptureDurationUs = 0;
	std::uint64_t m_ValidationFingerprint = 0;
	std::string m_Source;
	std::shared_ptr<const EngineSnapshot> m_ObjectSnapshot;
	std::shared_ptr<const ReflectionLayout> m_ReflectionLayout;
	std::vector<ReflectedType> m_Types;
	std::map<std::string, std::size_t, std::less<>> m_TypeByFullPath;
	std::map<std::int32_t, std::size_t> m_TypeByObjectIndex;
	std::map<
		std::string,
		std::pair<std::size_t, std::size_t>,
		std::less<>> m_FunctionByFullPath;
	std::map<std::int32_t, std::vector<std::size_t>> m_DirectChildrenBySuperIndex;
	std::size_t m_DirectChildCount = 0;
	std::size_t m_FunctionCount = 0;
};

enum class TypeMemberScope : std::uint8_t
{
	Direct,
	IncludeInherited
};

const char* ToString(TypeMemberScope scope) noexcept;

enum class TypeMemberQueryError : std::uint8_t
{
	None,
	SnapshotInvalid,
	TypeNotFound,
	TypeKindInvalid,
	ScopeInvalid,
	HierarchyReferenceInvalid,
	HierarchyCycle,
	HierarchyDepthExceeded,
	AllocationFailed
};

const char* ToString(TypeMemberQueryError error) noexcept;

template<typename T>
struct TypeMemberView
{
	const T* Member = nullptr;
	const ReflectedType* DeclaringType = nullptr;
	std::uint32_t InheritanceDepth = 0;
};

struct TypePropertyQueryResult
{
	TypeMemberQueryError Error = TypeMemberQueryError::None;
	std::shared_ptr<const TypeSnapshot> Snapshot;
	std::vector<TypeMemberView<ReflectedProperty>> Members;

	bool Ok() const noexcept { return Error == TypeMemberQueryError::None; }
};

struct TypeFunctionQueryResult
{
	TypeMemberQueryError Error = TypeMemberQueryError::None;
	std::shared_ptr<const TypeSnapshot> Snapshot;
	std::vector<TypeMemberView<ReflectedFunction>> Members;

	bool Ok() const noexcept { return Error == TypeMemberQueryError::None; }
};

TypePropertyQueryResult QueryTypeProperties(
	std::shared_ptr<const TypeSnapshot> snapshot,
	std::string_view typeFullPath,
	TypeMemberScope scope) noexcept;

TypeFunctionQueryResult QueryTypeFunctions(
	std::shared_ptr<const TypeSnapshot> snapshot,
	std::string_view typeFullPath,
	TypeMemberScope scope) noexcept;

class TypeSnapshotStore final
{
public:
	static constexpr std::size_t kMaxSourceBytes = 1024;
	static constexpr std::size_t kMaxTypeRecords = 1'000'000;
	static constexpr std::size_t kMaxTotalMembers = 8'000'000;
	static constexpr std::size_t kMaxDescriptorNodes = 1'000'000;
	static constexpr std::size_t kMaxDescriptorDepth = 32;
	static constexpr std::size_t kMaxDescriptorFields = 1024;
	static constexpr std::size_t kMaxEnumEntries = 1'000'000;
	static constexpr std::size_t kMaxHierarchyDepth = 512;
	static constexpr std::size_t kMaxTextBytes = 4096;
	static constexpr std::uint32_t kMaxValueSize = 1024u * 1024u * 1024u;
	static constexpr std::uint32_t kMaxParameterSize = 16u * 1024u * 1024u;

	TypeSnapshotStore(std::string sessionId, std::uint64_t contextGeneration);
	TypeSnapshotStore(const TypeSnapshotStore&) = delete;
	TypeSnapshotStore& operator=(const TypeSnapshotStore&) = delete;

	bool IsConfigured() const noexcept;
	bool IsStopped() const noexcept { return m_Stopped.load(std::memory_order_acquire); }
	TypeSnapshotPublishResult Publish(
		TypeSnapshotCandidate candidate,
		std::shared_ptr<const EngineSnapshot> objectSnapshot,
		std::shared_ptr<const ReflectionRuntimeSnapshot> reflection) noexcept;
	std::shared_ptr<const TypeSnapshot> Current() const noexcept;
	std::uint64_t CurrentGeneration() const noexcept;
	void Stop() noexcept;

private:
	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	mutable std::mutex m_PublishMutex;
	std::atomic<std::shared_ptr<const TypeSnapshot>> m_Current;
	std::atomic<bool> m_Stopped{false};
};

} // namespace UExplorer::Runtime
