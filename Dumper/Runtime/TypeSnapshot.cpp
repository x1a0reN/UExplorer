#include "TypeSnapshot.h"

#include <algorithm>
#include <new>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaxSessionIdBytes = 128;
constexpr std::uint64_t kPropertyFlagParm = 0x0000000000000080ull;
constexpr std::uint64_t kPropertyFlagOutParm = 0x0000000000000100ull;
constexpr std::uint64_t kPropertyFlagReturnParm = 0x0000000000000400ull;
constexpr std::uint64_t kPropertyFlagReferenceParm = 0x0000000008000000ull;

bool SameHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool IsValidText(const std::string& value) noexcept
{
	return !value.empty() && value.size() <= TypeSnapshotStore::kMaxTextBytes;
}

bool IsPowerOfTwo(const std::uint32_t value) noexcept
{
	return value != 0 && (value & (value - 1)) == 0;
}

bool TryMultiply(
	const std::uint32_t left,
	const std::uint32_t right,
	std::uint64_t& result) noexcept
{
	result = static_cast<std::uint64_t>(left) * right;
	return right == 0 || result / right == left;
}

bool IsIntegerKind(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Int8:
	case PropertyKind::Int16:
	case PropertyKind::Int32:
	case PropertyKind::Int64:
	case PropertyKind::UInt8:
	case PropertyKind::UInt16:
	case PropertyKind::UInt32:
	case PropertyKind::UInt64:
		return true;
	default:
		return false;
	}
}

bool IsKnownPropertyKind(const PropertyKind kind) noexcept
{
	switch (kind)
	{
	case PropertyKind::Unknown:
	case PropertyKind::Bool:
	case PropertyKind::Int8:
	case PropertyKind::Int16:
	case PropertyKind::Int32:
	case PropertyKind::Int64:
	case PropertyKind::UInt8:
	case PropertyKind::UInt16:
	case PropertyKind::UInt32:
	case PropertyKind::UInt64:
	case PropertyKind::Float:
	case PropertyKind::Double:
	case PropertyKind::Name:
	case PropertyKind::String:
	case PropertyKind::Text:
	case PropertyKind::Object:
	case PropertyKind::WeakObject:
	case PropertyKind::SoftObject:
	case PropertyKind::Enum:
	case PropertyKind::Struct:
	case PropertyKind::Array:
	case PropertyKind::Map:
	case PropertyKind::Set:
	case PropertyKind::Delegate:
		return true;
	}
	return false;
}

bool IsKnownMemberState(const ReflectedMemberState state) noexcept
{
	switch (state)
	{
	case ReflectedMemberState::Supported:
	case ReflectedMemberState::Unsupported:
	case ReflectedMemberState::Unavailable:
		return true;
	}
	return false;
}

bool IsKnownParameterDirection(const ReflectedParameterDirection direction) noexcept
{
	switch (direction)
	{
	case ReflectedParameterDirection::Input:
	case ReflectedParameterDirection::Output:
	case ReflectedParameterDirection::InOut:
	case ReflectedParameterDirection::Return:
		return true;
	}
	return false;
}

bool TryDeriveParameterDirection(
	const std::uint64_t flags,
	ReflectedParameterDirection& direction) noexcept
{
	if ((flags & kPropertyFlagParm) == 0)
		return false;
	if ((flags & kPropertyFlagReturnParm) != 0)
	{
		direction = ReflectedParameterDirection::Return;
		return true;
	}
	if ((flags & kPropertyFlagOutParm) == 0)
	{
		direction = ReflectedParameterDirection::Input;
		return true;
	}
	direction = (flags & kPropertyFlagReferenceParm) != 0
		? ReflectedParameterDirection::InOut
		: ReflectedParameterDirection::Output;
	return true;
}

std::uint32_t ExactScalarSize(const PropertyKind kind) noexcept
{
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
}

bool HasNestedMetadata(const PropertyDescriptor& descriptor) noexcept
{
	return descriptor.Element
		|| descriptor.Key
		|| descriptor.Mapped
		|| !descriptor.Fields.empty()
		|| !descriptor.EnumEntries.empty();
}

struct DescriptorCloneContext
{
	std::size_t Nodes = 0;
	std::size_t Members = 0;
	TypeSnapshotPublishError Error = TypeSnapshotPublishError::None;
	std::unordered_map<
		const PropertyDescriptor*,
		std::shared_ptr<const PropertyDescriptor>> Completed;
	std::unordered_set<const PropertyDescriptor*> Visiting;
};

bool CloneDescriptor(
	const std::shared_ptr<const PropertyDescriptor>& source,
	const std::size_t depth,
	DescriptorCloneContext& context,
	std::shared_ptr<const PropertyDescriptor>& frozen)
{
	frozen.reset();
	if (!source || depth > TypeSnapshotStore::kMaxDescriptorDepth)
	{
		context.Error = TypeSnapshotPublishError::DescriptorInvalid;
		return false;
	}
	if (const auto completed = context.Completed.find(source.get());
		completed != context.Completed.end())
	{
		frozen = completed->second;
		return true;
	}
	if (!context.Visiting.emplace(source.get()).second)
	{
		context.Error = TypeSnapshotPublishError::DescriptorCycle;
		return false;
	}
	struct VisitGuard final
	{
		DescriptorCloneContext& Context;
		const PropertyDescriptor* Descriptor;
		~VisitGuard() { Context.Visiting.erase(Descriptor); }
	} visitGuard{context, source.get()};
	if (++context.Nodes > TypeSnapshotStore::kMaxDescriptorNodes
		|| !IsValidText(source->TypeName)
		|| !IsKnownPropertyKind(source->Kind)
		|| source->Size == 0
		|| source->Size > TypeSnapshotStore::kMaxValueSize
		|| source->Fields.size() > TypeSnapshotStore::kMaxDescriptorFields
		|| source->EnumEntries.size() > TypeSnapshotStore::kMaxEnumEntries)
	{
		context.Error = TypeSnapshotPublishError::DescriptorInvalid;
		return false;
	}
	const std::size_t nestedMembers = source->Fields.size() + source->EnumEntries.size();
	if (nestedMembers > TypeSnapshotStore::kMaxTotalMembers - context.Members)
	{
		context.Error = TypeSnapshotPublishError::DescriptorInvalid;
		return false;
	}
	context.Members += nestedMembers;

	auto clone = std::make_shared<PropertyDescriptor>();
	clone->Kind = source->Kind;
	clone->TypeName = source->TypeName;
	clone->Size = source->Size;
	clone->BoolByteOffset = source->BoolByteOffset;
	clone->BoolMask = source->BoolMask;
	clone->ElementStride = source->ElementStride;
	clone->ElementValueOffset = source->ElementValueOffset;
	clone->MapKeyOffset = source->MapKeyOffset;
	clone->MapValueOffset = source->MapValueOffset;
	if (source->Kind != PropertyKind::Bool
		&& (source->BoolByteOffset != 0 || source->BoolMask != 0))
	{
		context.Error = TypeSnapshotPublishError::DescriptorInvalid;
		return false;
	}

	const auto noNested = [&]() {
		return !HasNestedMetadata(*source)
			&& source->ElementStride == 0
			&& source->ElementValueOffset == 0
			&& source->MapKeyOffset == 0
			&& source->MapValueOffset == 0;
	};
	const std::uint32_t exactSize = ExactScalarSize(source->Kind);
	if (exactSize != 0)
	{
		if (source->Size != exactSize || !noNested())
		{
			context.Error = TypeSnapshotPublishError::DescriptorInvalid;
			return false;
		}
	}
	else
	{
		switch (source->Kind)
		{
		case PropertyKind::Bool:
			if (source->BoolMask == 0
				|| source->BoolByteOffset >= source->Size
				|| !noNested())
			{
				context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			break;
		case PropertyKind::Name:
		case PropertyKind::String:
		case PropertyKind::Text:
		case PropertyKind::Object:
		case PropertyKind::WeakObject:
		case PropertyKind::SoftObject:
			if (!noNested())
			{
				context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			break;
		case PropertyKind::Enum:
		{
			if (!source->Element
				|| source->Key
				|| source->Mapped
				|| !source->Fields.empty()
				|| source->ElementStride != 0
				|| source->ElementValueOffset != 0
				|| source->MapKeyOffset != 0
				|| source->MapValueOffset != 0)
			{
				context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			if (!CloneDescriptor(source->Element, depth + 1, context, clone->Element)
				|| !clone->Element
				|| !IsIntegerKind(clone->Element->Kind)
				|| clone->Element->Size != source->Size)
			{
				if (context.Error == TypeSnapshotPublishError::None)
					context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			std::set<std::string> names;
			clone->EnumEntries.reserve(source->EnumEntries.size());
			for (const PropertyEnumEntry& entry : source->EnumEntries)
			{
				if (!IsValidText(entry.Name) || !names.emplace(entry.Name).second)
				{
					context.Error = TypeSnapshotPublishError::DescriptorInvalid;
					return false;
				}
				clone->EnumEntries.push_back(entry);
			}
			break;
		}
		case PropertyKind::Struct:
		{
			if (source->Element
				|| source->Key
				|| source->Mapped
				|| !source->EnumEntries.empty()
				|| source->ElementStride != 0
				|| source->ElementValueOffset != 0
				|| source->MapKeyOffset != 0
				|| source->MapValueOffset != 0)
			{
				context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			std::set<std::string> names;
			clone->Fields.reserve(source->Fields.size());
			for (const PropertyFieldDescriptor& field : source->Fields)
			{
				std::shared_ptr<const PropertyDescriptor> child;
				if (!IsValidText(field.Name)
					|| !names.emplace(field.Name).second
					|| !CloneDescriptor(field.Descriptor, depth + 1, context, child)
					|| field.Offset > source->Size
					|| child->Size > source->Size - field.Offset)
				{
					if (context.Error == TypeSnapshotPublishError::None)
						context.Error = TypeSnapshotPublishError::DescriptorInvalid;
					return false;
				}
				clone->Fields.push_back({field.Name, field.Offset, std::move(child)});
			}
			break;
		}
		case PropertyKind::Array:
			if (!source->Element
				|| source->Key
				|| source->Mapped
				|| !source->Fields.empty()
				|| !source->EnumEntries.empty()
				|| source->ElementStride == 0
				|| source->ElementValueOffset != 0
				|| source->MapKeyOffset != 0
				|| source->MapValueOffset != 0
				|| !CloneDescriptor(source->Element, depth + 1, context, clone->Element)
				|| clone->Element->Size > source->ElementStride)
			{
				if (context.Error == TypeSnapshotPublishError::None)
					context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			break;
		case PropertyKind::Map:
			if (source->Element
				|| !source->Key
				|| !source->Mapped
				|| !source->Fields.empty()
				|| !source->EnumEntries.empty()
				|| source->ElementStride == 0
				|| source->ElementValueOffset != 0
				|| !CloneDescriptor(source->Key, depth + 1, context, clone->Key)
				|| !CloneDescriptor(source->Mapped, depth + 1, context, clone->Mapped)
				|| source->MapKeyOffset > source->ElementStride
				|| clone->Key->Size > source->ElementStride - source->MapKeyOffset
				|| source->MapValueOffset > source->ElementStride
				|| clone->Mapped->Size > source->ElementStride - source->MapValueOffset
				|| (static_cast<std::uint64_t>(source->MapKeyOffset)
					< static_cast<std::uint64_t>(source->MapValueOffset) + clone->Mapped->Size
					&& static_cast<std::uint64_t>(source->MapValueOffset)
					< static_cast<std::uint64_t>(source->MapKeyOffset) + clone->Key->Size))
			{
				if (context.Error == TypeSnapshotPublishError::None)
					context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			break;
		case PropertyKind::Set:
			if (!source->Element
				|| source->Key
				|| source->Mapped
				|| !source->Fields.empty()
				|| !source->EnumEntries.empty()
				|| source->ElementStride == 0
				|| source->MapKeyOffset != 0
				|| source->MapValueOffset != 0
				|| !CloneDescriptor(source->Element, depth + 1, context, clone->Element)
				|| source->ElementValueOffset > source->ElementStride
				|| clone->Element->Size
					> source->ElementStride - source->ElementValueOffset)
			{
				if (context.Error == TypeSnapshotPublishError::None)
					context.Error = TypeSnapshotPublishError::DescriptorInvalid;
				return false;
			}
			break;
		case PropertyKind::Delegate:
		case PropertyKind::Unknown:
			context.Error = TypeSnapshotPublishError::DescriptorInvalid;
			return false;
		case PropertyKind::Int8:
		case PropertyKind::Int16:
		case PropertyKind::Int32:
		case PropertyKind::Int64:
		case PropertyKind::UInt8:
		case PropertyKind::UInt16:
		case PropertyKind::UInt32:
		case PropertyKind::UInt64:
		case PropertyKind::Float:
		case PropertyKind::Double:
			break;
		}
	}

	frozen = std::shared_ptr<const PropertyDescriptor>(std::move(clone));
	context.Completed.emplace(source.get(), frozen);
	return true;
}

bool FreezeProperty(
	ReflectedProperty& property,
	const std::uint32_t ownerSize,
	DescriptorCloneContext& descriptors,
	TypeSnapshotPublishError& error)
{
	std::uint64_t totalSize = 0;
	if (!IsValidText(property.Name)
		|| !IsValidText(property.TypeName)
		|| !IsKnownPropertyKind(property.Kind)
		|| !IsKnownMemberState(property.State)
		|| property.Size == 0
		|| property.Size > TypeSnapshotStore::kMaxValueSize
		|| property.ArrayDim == 0
		|| property.ArrayDim > 1024
		|| !TryMultiply(property.Size, property.ArrayDim, totalSize)
		|| property.Offset > ownerSize
		|| totalSize > static_cast<std::uint64_t>(ownerSize - property.Offset))
	{
		error = TypeSnapshotPublishError::PropertyInvalid;
		return false;
	}

	if (property.State == ReflectedMemberState::Supported)
	{
		if (property.Kind == PropertyKind::Unknown
			|| property.Kind == PropertyKind::Delegate
			|| !property.ReasonCode.empty()
			|| !property.Reason.empty())
		{
			error = TypeSnapshotPublishError::PropertyInvalid;
			return false;
		}
		std::shared_ptr<const PropertyDescriptor> frozen;
		if (!CloneDescriptor(property.Descriptor, 0, descriptors, frozen)
			|| !frozen
			|| frozen->Kind != property.Kind
			|| frozen->TypeName != property.TypeName
			|| frozen->Size != property.Size)
		{
			error = descriptors.Error == TypeSnapshotPublishError::None
				? TypeSnapshotPublishError::DescriptorInvalid
				: descriptors.Error;
			return false;
		}
		property.Descriptor = std::move(frozen);
		return true;
	}

	if (property.Descriptor
		|| !IsValidText(property.ReasonCode)
		|| !IsValidText(property.Reason))
	{
		error = TypeSnapshotPublishError::PropertyInvalid;
		return false;
	}
	return true;
}

const EngineSnapshotObject* FindObject(
	const EngineSnapshot& snapshot,
	const std::int32_t objectIndex) noexcept
{
	const auto it = std::lower_bound(
		snapshot.Objects.begin(),
		snapshot.Objects.end(),
		objectIndex,
		[](const EngineSnapshotObject& record, const std::int32_t index) {
			return record.Handle.Index < index;
		});
	return it != snapshot.Objects.end() && it->Handle.Index == objectIndex
		? &*it
		: nullptr;
}

bool IsExpectedTypeKind(
	const ReflectedTypeKind typeKind,
	const EngineObjectKind objectKind) noexcept
{
	switch (typeKind)
	{
	case ReflectedTypeKind::Class: return objectKind == EngineObjectKind::Class;
	case ReflectedTypeKind::Struct: return objectKind == EngineObjectKind::Struct;
	case ReflectedTypeKind::Enum: return objectKind == EngineObjectKind::Enum;
	}
	return false;
}

std::uint64_t HashSnapshot(const TypeSnapshot& snapshot) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	const auto addBytes = [&hash](const void* data, const std::size_t size) {
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for (std::size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	};
	const auto addString = [&addBytes](const std::string& value) {
		addBytes(value.data(), value.size());
	};
	addString(snapshot.SessionId());
	const std::uint64_t contextGeneration = snapshot.ContextGeneration();
	const std::uint64_t generation = snapshot.Generation();
	const std::uint64_t objectGeneration = snapshot.ObjectSnapshotGeneration();
	const std::uint64_t reflectionFingerprint = snapshot.ReflectionLayoutFingerprint();
	addBytes(&contextGeneration, sizeof(contextGeneration));
	addBytes(&generation, sizeof(generation));
	addBytes(&objectGeneration, sizeof(objectGeneration));
	addBytes(&reflectionFingerprint, sizeof(reflectionFingerprint));
	for (const ReflectedType& type : snapshot.Types())
	{
		addBytes(&type.Kind, sizeof(type.Kind));
		addBytes(&type.Handle.Index, sizeof(type.Handle.Index));
		addBytes(&type.Handle.SerialNumber, sizeof(type.Handle.SerialNumber));
		addString(type.FullPath);
		addBytes(&type.PropertiesSize, sizeof(type.PropertiesSize));
		addBytes(&type.MinAlignment, sizeof(type.MinAlignment));
		const std::int32_t superIndex = type.Super ? type.Super->Index : -1;
		addBytes(&superIndex, sizeof(superIndex));
		for (const ReflectedProperty& property : type.DirectProperties)
		{
			addString(property.Name);
			addString(property.TypeName);
			addBytes(&property.Kind, sizeof(property.Kind));
			addBytes(&property.Offset, sizeof(property.Offset));
			addBytes(&property.Size, sizeof(property.Size));
			addBytes(&property.ArrayDim, sizeof(property.ArrayDim));
			addBytes(&property.Flags, sizeof(property.Flags));
			addBytes(&property.State, sizeof(property.State));
		}
		for (const ReflectedFunction& function : type.DirectFunctions)
		{
			addString(function.FullPath);
			addBytes(&function.Handle.SignatureFingerprint,
				sizeof(function.Handle.SignatureFingerprint));
		}
		for (const ReflectedEnumEntry& entry : type.EnumEntries)
		{
			addString(entry.Name);
			addBytes(&entry.Value, sizeof(entry.Value));
		}
	}
	return hash == 0 ? 1 : hash;
}

TypeSnapshotPublishResult Failure(
	const TypeSnapshotPublishError error,
	const std::int32_t typeIndex = -1,
	const std::int32_t memberIndex = -1) noexcept
{
	return {
		.Error = error,
		.TypeIndex = typeIndex,
		.MemberIndex = memberIndex
	};
}

template<typename TMember, typename TQueryResult, typename TAccessor>
TQueryResult QueryMembers(
	std::shared_ptr<const TypeSnapshot> snapshot,
	const std::string_view typeFullPath,
	const TypeMemberScope scope,
	TAccessor accessor) noexcept
{
	TQueryResult result;
	result.Snapshot = std::move(snapshot);
	if (!result.Snapshot
		|| !result.Snapshot->IsConfigured(result.Snapshot->ContextGeneration()))
	{
		result.Error = TypeMemberQueryError::SnapshotInvalid;
		return result;
	}
	if (scope != TypeMemberScope::Direct
		&& scope != TypeMemberScope::IncludeInherited)
	{
		result.Error = TypeMemberQueryError::ScopeInvalid;
		return result;
	}
	const ReflectedType* current = result.Snapshot->FindByFullPath(typeFullPath);
	if (!current)
	{
		result.Error = TypeMemberQueryError::TypeNotFound;
		return result;
	}
	if (current->Kind == ReflectedTypeKind::Enum)
	{
		result.Error = TypeMemberQueryError::TypeKindInvalid;
		return result;
	}
	try
	{
		std::set<std::int32_t> visited;
		std::uint32_t depth = 0;
		while (current)
		{
			if (!visited.emplace(current->Handle.Index).second)
			{
				result.Error = TypeMemberQueryError::HierarchyCycle;
				result.Members.clear();
				return result;
			}
			for (const TMember& member : accessor(*current))
			{
				result.Members.push_back({
					.Member = &member,
					.DeclaringType = current,
					.InheritanceDepth = depth
				});
			}
			if (scope == TypeMemberScope::Direct || !current->Super)
				break;
			if (++depth > TypeSnapshotStore::kMaxHierarchyDepth)
			{
				result.Error = TypeMemberQueryError::HierarchyDepthExceeded;
				result.Members.clear();
				return result;
			}
			const ReflectedType* parent =
				result.Snapshot->FindByObjectIndex(current->Super->Index);
			if (!parent
				|| !SameHandle(*current->Super, parent->Handle)
				|| parent->Kind != current->Kind)
			{
				result.Error = TypeMemberQueryError::HierarchyReferenceInvalid;
				result.Members.clear();
				return result;
			}
			current = parent;
		}
		return result;
	}
	catch (...)
	{
		result.Error = TypeMemberQueryError::AllocationFailed;
		result.Members.clear();
		return result;
	}
}

} // namespace

const char* ToString(const ReflectedTypeKind kind) noexcept
{
	switch (kind)
	{
	case ReflectedTypeKind::Class: return "class";
	case ReflectedTypeKind::Struct: return "struct";
	case ReflectedTypeKind::Enum: return "enum";
	}
	return "unknown";
}

const char* ToString(const ReflectedMemberState state) noexcept
{
	switch (state)
	{
	case ReflectedMemberState::Supported: return "supported";
	case ReflectedMemberState::Unsupported: return "unsupported";
	case ReflectedMemberState::Unavailable: return "unavailable";
	}
	return "unavailable";
}

const char* ToString(const ReflectedParameterDirection direction) noexcept
{
	switch (direction)
	{
	case ReflectedParameterDirection::Input: return "input";
	case ReflectedParameterDirection::Output: return "output";
	case ReflectedParameterDirection::InOut: return "inout";
	case ReflectedParameterDirection::Return: return "return";
	}
	return "input";
}

const char* ToString(const ReflectedFunctionImplementation implementation) noexcept
{
	switch (implementation)
	{
	case ReflectedFunctionImplementation::Unavailable: return "unavailable";
	case ReflectedFunctionImplementation::Native: return "native";
	case ReflectedFunctionImplementation::Bytecode: return "bytecode";
	case ReflectedFunctionImplementation::NativeAndBytecode: return "native_and_bytecode";
	}
	return "unavailable";
}

const char* ToString(const ClassDefaultObjectState state) noexcept
{
	switch (state)
	{
	case ClassDefaultObjectState::NotApplicable: return "not_applicable";
	case ClassDefaultObjectState::Present: return "present";
	case ClassDefaultObjectState::NotConstructed: return "not_constructed";
	case ClassDefaultObjectState::Unavailable: return "unavailable";
	}
	return "unavailable";
}

const char* ToString(const TypeSnapshotPublishError error) noexcept
{
	switch (error)
	{
	case TypeSnapshotPublishError::None: return "NONE";
	case TypeSnapshotPublishError::StoreInvalid: return "TYPE_SNAPSHOT_STORE_INVALID";
	case TypeSnapshotPublishError::StoreStopped: return "TYPE_SNAPSHOT_STORE_STOPPED";
	case TypeSnapshotPublishError::DependencyInvalid: return "TYPE_SNAPSHOT_DEPENDENCY_INVALID";
	case TypeSnapshotPublishError::EnvelopeInvalid: return "TYPE_SNAPSHOT_ENVELOPE_INVALID";
	case TypeSnapshotPublishError::CountLimitExceeded: return "TYPE_SNAPSHOT_COUNT_LIMIT_EXCEEDED";
	case TypeSnapshotPublishError::TypeCoverageMismatch: return "TYPE_SNAPSHOT_TYPE_COVERAGE_MISMATCH";
	case TypeSnapshotPublishError::TypeRecordInvalid: return "TYPE_SNAPSHOT_TYPE_RECORD_INVALID";
	case TypeSnapshotPublishError::RelationshipInvalid: return "TYPE_SNAPSHOT_RELATIONSHIP_INVALID";
	case TypeSnapshotPublishError::HierarchyCycle: return "TYPE_SNAPSHOT_HIERARCHY_CYCLE";
	case TypeSnapshotPublishError::HierarchyDepthExceeded: return "TYPE_SNAPSHOT_HIERARCHY_DEPTH_EXCEEDED";
	case TypeSnapshotPublishError::MemberLimitExceeded: return "TYPE_SNAPSHOT_MEMBER_LIMIT_EXCEEDED";
	case TypeSnapshotPublishError::PropertyInvalid: return "TYPE_SNAPSHOT_PROPERTY_INVALID";
	case TypeSnapshotPublishError::DescriptorInvalid: return "TYPE_SNAPSHOT_DESCRIPTOR_INVALID";
	case TypeSnapshotPublishError::DescriptorCycle: return "TYPE_SNAPSHOT_DESCRIPTOR_CYCLE";
	case TypeSnapshotPublishError::FunctionCoverageMismatch: return "TYPE_SNAPSHOT_FUNCTION_COVERAGE_MISMATCH";
	case TypeSnapshotPublishError::FunctionInvalid: return "TYPE_SNAPSHOT_FUNCTION_INVALID";
	case TypeSnapshotPublishError::EnumInvalid: return "TYPE_SNAPSHOT_ENUM_INVALID";
	case TypeSnapshotPublishError::GenerationNotMonotonic: return "TYPE_SNAPSHOT_GENERATION_NOT_MONOTONIC";
	case TypeSnapshotPublishError::WorkerThreadRequired: return "TYPE_SNAPSHOT_WORKER_THREAD_REQUIRED";
	case TypeSnapshotPublishError::AllocationFailed: return "TYPE_SNAPSHOT_ALLOCATION_FAILED";
	}
	return "TYPE_SNAPSHOT_UNKNOWN_ERROR";
}

const ReflectedType* TypeSnapshot::FindByFullPath(
	const std::string_view fullPath) const noexcept
{
	const auto it = m_TypeByFullPath.find(fullPath);
	return it == m_TypeByFullPath.end() ? nullptr : &m_Types[it->second];
}

const ReflectedType* TypeSnapshot::FindByObjectIndex(
	const std::int32_t objectIndex) const noexcept
{
	const auto it = m_TypeByObjectIndex.find(objectIndex);
	return it == m_TypeByObjectIndex.end() ? nullptr : &m_Types[it->second];
}

ReflectedFunctionLookup TypeSnapshot::FindFunctionByFullPath(
	const std::string_view fullPath) const noexcept
{
	const auto it = m_FunctionByFullPath.find(fullPath);
	if (it == m_FunctionByFullPath.end())
		return {};
	const auto [typeIndex, functionIndex] = it->second;
	if (typeIndex >= m_Types.size()
		|| functionIndex >= m_Types[typeIndex].DirectFunctions.size())
	{
		return {};
	}
	return {
		.Function = &m_Types[typeIndex].DirectFunctions[functionIndex],
		.DeclaringType = &m_Types[typeIndex]
	};
}

const std::vector<std::size_t>* TypeSnapshot::FindDirectChildIndices(
	const std::int32_t superObjectIndex) const noexcept
{
	const auto it = m_DirectChildrenBySuperIndex.find(superObjectIndex);
	return it == m_DirectChildrenBySuperIndex.end() ? nullptr : &it->second;
}

bool TypeSnapshot::IsConfigured(
	const std::uint64_t expectedContextGeneration) const noexcept
{
	return expectedContextGeneration != 0
		&& m_ContextGeneration == expectedContextGeneration
		&& m_Generation != 0
		&& m_ObjectSnapshotGeneration != 0
		&& m_ReflectionLayoutFingerprint != 0
		&& m_CapturedAtMonotonicUs != 0
		&& m_ValidationFingerprint != 0
		&& !m_SessionId.empty()
		&& !m_Source.empty()
		&& m_ObjectSnapshot
		&& m_ObjectSnapshot->SessionId == m_SessionId
		&& m_ObjectSnapshot->ContextGeneration == m_ContextGeneration
		&& m_ObjectSnapshot->Generation == m_ObjectSnapshotGeneration
		&& m_ReflectionLayout
		&& IsReflectionLayoutValid(*m_ReflectionLayout, m_ContextGeneration)
		&& m_ReflectionLayout->Fingerprint() == m_ReflectionLayoutFingerprint
		&& m_TypeByFullPath.size() == m_Types.size()
		&& m_TypeByObjectIndex.size() == m_Types.size()
		&& m_DirectChildCount <= m_Types.size()
		&& m_FunctionByFullPath.size() == m_FunctionCount;
}

const char* ToString(const TypeMemberScope scope) noexcept
{
	switch (scope)
	{
	case TypeMemberScope::Direct: return "direct";
	case TypeMemberScope::IncludeInherited: return "include_inherited";
	}
	return "direct";
}

const char* ToString(const TypeMemberQueryError error) noexcept
{
	switch (error)
	{
	case TypeMemberQueryError::None: return "NONE";
	case TypeMemberQueryError::SnapshotInvalid: return "TYPE_SNAPSHOT_INVALID";
	case TypeMemberQueryError::TypeNotFound: return "TYPE_NOT_FOUND";
	case TypeMemberQueryError::TypeKindInvalid: return "TYPE_KIND_INVALID";
	case TypeMemberQueryError::ScopeInvalid: return "TYPE_MEMBER_SCOPE_INVALID";
	case TypeMemberQueryError::HierarchyReferenceInvalid: return "TYPE_HIERARCHY_REFERENCE_INVALID";
	case TypeMemberQueryError::HierarchyCycle: return "TYPE_HIERARCHY_CYCLE";
	case TypeMemberQueryError::HierarchyDepthExceeded: return "TYPE_HIERARCHY_DEPTH_EXCEEDED";
	case TypeMemberQueryError::AllocationFailed: return "TYPE_QUERY_ALLOCATION_FAILED";
	}
	return "TYPE_QUERY_UNKNOWN_ERROR";
}

TypePropertyQueryResult QueryTypeProperties(
	std::shared_ptr<const TypeSnapshot> snapshot,
	const std::string_view typeFullPath,
	const TypeMemberScope scope) noexcept
{
	return QueryMembers<ReflectedProperty, TypePropertyQueryResult>(
		std::move(snapshot),
		typeFullPath,
		scope,
		[](const ReflectedType& type) -> const std::vector<ReflectedProperty>& {
			return type.DirectProperties;
		});
}

TypeFunctionQueryResult QueryTypeFunctions(
	std::shared_ptr<const TypeSnapshot> snapshot,
	const std::string_view typeFullPath,
	const TypeMemberScope scope) noexcept
{
	return QueryMembers<ReflectedFunction, TypeFunctionQueryResult>(
		std::move(snapshot),
		typeFullPath,
		scope,
		[](const ReflectedType& type) -> const std::vector<ReflectedFunction>& {
			return type.DirectFunctions;
		});
}

TypeSnapshotStore::TypeSnapshotStore(
	std::string sessionId,
	const std::uint64_t contextGeneration)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration)
{
}

bool TypeSnapshotStore::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= kMaxSessionIdBytes
		&& m_ContextGeneration != 0
		&& m_ContextGeneration <= EngineSnapshotStore::kMaxProtocolGeneration;
}

TypeSnapshotPublishResult TypeSnapshotStore::Publish(
	TypeSnapshotCandidate candidate,
	std::shared_ptr<const EngineSnapshot> objectSnapshot,
	std::shared_ptr<const ReflectionRuntimeSnapshot> reflection) noexcept
{
	if (!IsConfigured())
		return Failure(TypeSnapshotPublishError::StoreInvalid);
	if (IsStopped())
		return Failure(TypeSnapshotPublishError::StoreStopped);
	if (!objectSnapshot
		|| !reflection
		|| !reflection->IsLayoutConfigured(m_ContextGeneration)
		|| objectSnapshot->SessionId != m_SessionId
		|| objectSnapshot->ContextGeneration != m_ContextGeneration
		|| objectSnapshot->Generation == 0
		|| reflection->Layout->Fingerprint() == 0)
	{
		return Failure(TypeSnapshotPublishError::DependencyInvalid);
	}
	if (candidate.SessionId != m_SessionId
		|| candidate.ContextGeneration != m_ContextGeneration
		|| candidate.Generation == 0
		|| candidate.Generation > EngineSnapshotStore::kMaxProtocolGeneration
		|| candidate.ObjectSnapshotGeneration != objectSnapshot->Generation
		|| candidate.ReflectionLayoutFingerprint != reflection->Layout->Fingerprint()
		|| candidate.CapturedAtMonotonicUs == 0
		|| candidate.Source.empty()
		|| candidate.Source.size() > TypeSnapshotStore::kMaxSourceBytes)
	{
		return Failure(TypeSnapshotPublishError::EnvelopeInvalid);
	}
	if (candidate.Types.size() > kMaxTypeRecords)
		return Failure(TypeSnapshotPublishError::CountLimitExceeded);

	try
	{
		auto published = std::shared_ptr<TypeSnapshot>(new TypeSnapshot());
		published->m_SessionId = std::move(candidate.SessionId);
		published->m_ContextGeneration = candidate.ContextGeneration;
		published->m_Generation = candidate.Generation;
		published->m_ObjectSnapshotGeneration = candidate.ObjectSnapshotGeneration;
		published->m_ReflectionLayoutFingerprint = candidate.ReflectionLayoutFingerprint;
		published->m_CapturedAtMonotonicUs = candidate.CapturedAtMonotonicUs;
		published->m_CaptureDurationUs = candidate.CaptureDurationUs;
		published->m_Source = std::move(candidate.Source);
		published->m_ObjectSnapshot = std::move(objectSnapshot);
		published->m_ReflectionLayout = reflection->Layout;
		published->m_Types = std::move(candidate.Types);

		std::size_t expectedTypeCount = 0;
		std::size_t expectedFunctionCount = 0;
		for (const EngineSnapshotObject& object : published->m_ObjectSnapshot->Objects)
		{
			if (object.Kind == EngineObjectKind::Class
				|| object.Kind == EngineObjectKind::Struct
				|| object.Kind == EngineObjectKind::Enum)
			{
				++expectedTypeCount;
			}
			else if (object.Kind == EngineObjectKind::Function)
			{
				++expectedFunctionCount;
			}
		}
		if (expectedTypeCount != published->m_Types.size())
			return Failure(TypeSnapshotPublishError::TypeCoverageMismatch);

		DescriptorCloneContext descriptors;
		descriptors.Completed.reserve(1024);
		descriptors.Visiting.reserve(kMaxDescriptorDepth);
		std::set<std::int32_t> seenFunctions;
		std::size_t totalMembers = 0;
		const auto consumeMembers = [&totalMembers](const std::size_t count) {
			if (count > TypeSnapshotStore::kMaxTotalMembers - totalMembers)
				return false;
			totalMembers += count;
			return true;
		};
		std::size_t typePosition = 0;
		for (const EngineSnapshotObject& object : published->m_ObjectSnapshot->Objects)
		{
			if (object.Kind != EngineObjectKind::Class
				&& object.Kind != EngineObjectKind::Struct
				&& object.Kind != EngineObjectKind::Enum)
			{
				continue;
			}
			ReflectedType& type = published->m_Types[typePosition];
			const std::int32_t typeErrorIndex = static_cast<std::int32_t>(typePosition);
			if (!SameHandle(type.Handle, object.Handle)
				|| !IsExpectedTypeKind(type.Kind, object.Kind)
				|| type.Name != object.Name
				|| type.FullPath != object.FullPath
				|| type.PackagePath != object.PackagePath
				|| !IsValidText(type.Name)
				|| !IsValidText(type.FullPath)
				|| !IsValidText(type.PackagePath)
				|| !published->m_TypeByFullPath.emplace(type.FullPath, typePosition).second
				|| !published->m_TypeByObjectIndex.emplace(type.Handle.Index, typePosition).second)
			{
				return Failure(TypeSnapshotPublishError::TypeRecordInvalid, typeErrorIndex);
			}
			if (type.Kind == ReflectedTypeKind::Enum)
			{
				if (type.PropertiesSize != 0
					|| type.MinAlignment != 0
					|| type.Super
					|| type.DefaultObjectState != ClassDefaultObjectState::NotApplicable
					|| type.DefaultObject
					|| !type.DefaultObjectReasonCode.empty()
					|| !type.DefaultObjectReason.empty()
					|| !type.DirectProperties.empty()
					|| !type.DirectFunctions.empty()
					|| !IsKnownMemberState(type.EnumState))
				{
					return Failure(TypeSnapshotPublishError::EnumInvalid, typeErrorIndex);
				}
				if (type.EnumState == ReflectedMemberState::Supported)
				{
					if (!IsIntegerKind(type.EnumUnderlyingKind)
						|| !type.EnumReasonCode.empty()
						|| !type.EnumReason.empty()
						|| type.EnumEntries.size() > kMaxEnumEntries)
					{
						return Failure(TypeSnapshotPublishError::EnumInvalid, typeErrorIndex);
					}
					std::set<std::string> enumNames;
					for (std::size_t entryIndex = 0;
						entryIndex < type.EnumEntries.size();
						++entryIndex)
					{
						if (!IsValidText(type.EnumEntries[entryIndex].Name)
							|| !enumNames.emplace(type.EnumEntries[entryIndex].Name).second)
						{
							return Failure(
								TypeSnapshotPublishError::EnumInvalid,
								typeErrorIndex,
								static_cast<std::int32_t>(entryIndex));
						}
					}
				}
				else if (type.EnumUnderlyingKind != PropertyKind::Unknown
					|| !type.EnumEntries.empty()
					|| !IsValidText(type.EnumReasonCode)
					|| !IsValidText(type.EnumReason))
				{
					return Failure(TypeSnapshotPublishError::EnumInvalid, typeErrorIndex);
				}
				if (!consumeMembers(type.EnumEntries.size()))
					return Failure(TypeSnapshotPublishError::MemberLimitExceeded, typeErrorIndex);
				++typePosition;
				continue;
			}

			if (type.PropertiesSize > kMaxValueSize
				|| !IsPowerOfTwo(type.MinAlignment)
				|| type.MinAlignment > 4096
				|| type.EnumState != ReflectedMemberState::Unavailable
				|| type.EnumUnderlyingKind != PropertyKind::Unknown
				|| !type.EnumReasonCode.empty()
				|| !type.EnumReason.empty()
				|| !type.EnumEntries.empty())
			{
				return Failure(TypeSnapshotPublishError::TypeRecordInvalid, typeErrorIndex);
			}
			if (type.Kind == ReflectedTypeKind::Class)
			{
				switch (type.DefaultObjectState)
				{
				case ClassDefaultObjectState::Present:
				{
					if (!type.DefaultObject
						|| !type.DefaultObjectReasonCode.empty()
						|| !type.DefaultObjectReason.empty())
					{
						return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
					}
					const EngineSnapshotObject* defaultObject = FindObject(
						*published->m_ObjectSnapshot,
						type.DefaultObject->Index);
					if (!defaultObject
						|| !SameHandle(*type.DefaultObject, defaultObject->Handle)
						|| defaultObject->ClassPath != type.FullPath)
					{
						return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
					}
					break;
				}
				case ClassDefaultObjectState::NotConstructed:
					if (type.DefaultObject
						|| !type.DefaultObjectReasonCode.empty()
						|| !type.DefaultObjectReason.empty())
					{
						return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
					}
					break;
				case ClassDefaultObjectState::Unavailable:
					if (type.DefaultObject
						|| !IsValidText(type.DefaultObjectReasonCode)
						|| !IsValidText(type.DefaultObjectReason))
					{
						return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
					}
					break;
				case ClassDefaultObjectState::NotApplicable:
					return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
				default:
					return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
				}
			}
			else if (type.DefaultObjectState != ClassDefaultObjectState::NotApplicable
				|| type.DefaultObject
				|| !type.DefaultObjectReasonCode.empty()
				|| !type.DefaultObjectReason.empty())
			{
				return Failure(TypeSnapshotPublishError::RelationshipInvalid, typeErrorIndex);
			}

			if (!consumeMembers(type.DirectProperties.size())
				|| !consumeMembers(type.DirectFunctions.size()))
				return Failure(TypeSnapshotPublishError::MemberLimitExceeded, typeErrorIndex);
			std::set<std::string> propertyNames;
			for (std::size_t propertyIndex = 0;
				propertyIndex < type.DirectProperties.size();
				++propertyIndex)
			{
				ReflectedProperty& property = type.DirectProperties[propertyIndex];
				TypeSnapshotPublishError propertyError = TypeSnapshotPublishError::None;
				if (!propertyNames.emplace(property.Name).second
					|| !FreezeProperty(
						property,
						type.PropertiesSize,
						descriptors,
						propertyError))
				{
					return Failure(
						propertyError == TypeSnapshotPublishError::None
							? TypeSnapshotPublishError::PropertyInvalid
							: propertyError,
						typeErrorIndex,
						static_cast<std::int32_t>(propertyIndex));
				}
				if ((property.Flags & kPropertyFlagParm) != 0)
				{
					return Failure(
						TypeSnapshotPublishError::PropertyInvalid,
						typeErrorIndex,
						static_cast<std::int32_t>(propertyIndex));
				}
			}

			for (std::size_t functionIndex = 0;
				functionIndex < type.DirectFunctions.size();
				++functionIndex)
			{
				ReflectedFunction& function = type.DirectFunctions[functionIndex];
				const std::int32_t memberIndex = static_cast<std::int32_t>(functionIndex);
				const EngineSnapshotObject* functionObject = FindObject(
					*published->m_ObjectSnapshot,
					function.Handle.Function.Index);
				if (!functionObject
					|| functionObject->Kind != EngineObjectKind::Function
					|| !SameHandle(function.Handle.Function, functionObject->Handle)
					|| !SameHandle(function.Handle.Owner, type.Handle)
					|| !IsValidText(function.Handle.FullPath)
					|| function.FullPath != functionObject->FullPath
					|| function.Name != functionObject->Name
					|| function.Handle.SignatureFingerprint == 0
					|| !IsValidText(function.Name)
					|| !IsValidText(function.FullPath)
					|| function.ParameterSize > kMaxParameterSize
					|| !seenFunctions.emplace(function.Handle.Function.Index).second
					|| !published->m_FunctionByFullPath.emplace(
						function.FullPath,
						std::pair{typePosition, functionIndex}).second)
				{
					return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
				}
				++published->m_FunctionCount;
				switch (function.Implementation)
				{
				case ReflectedFunctionImplementation::Unavailable:
					if (function.NativeAddress != 0
						|| !IsValidText(function.ReasonCode)
						|| !IsValidText(function.Reason))
					{
						return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
					}
					break;
				case ReflectedFunctionImplementation::Native:
				case ReflectedFunctionImplementation::NativeAndBytecode:
					if (function.NativeAddress == 0
						|| !function.ReasonCode.empty()
						|| !function.Reason.empty())
					{
						return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
					}
					break;
				case ReflectedFunctionImplementation::Bytecode:
					if (function.NativeAddress != 0
						|| !function.ReasonCode.empty()
						|| !function.Reason.empty())
					{
						return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
					}
					break;
				default:
					return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
				}
				if (!consumeMembers(function.Parameters.size()))
					return Failure(TypeSnapshotPublishError::MemberLimitExceeded, typeErrorIndex, memberIndex);
				std::set<std::string> parameterNames;
				std::uint32_t returnCount = 0;
				for (std::size_t parameterIndex = 0;
					parameterIndex < function.Parameters.size();
					++parameterIndex)
				{
					ReflectedParameter& parameter = function.Parameters[parameterIndex];
					ReflectedParameterDirection derivedDirection{};
					if (!IsKnownParameterDirection(parameter.Direction)
						|| !TryDeriveParameterDirection(
							parameter.Property.Flags,
							derivedDirection)
						|| derivedDirection != parameter.Direction)
					{
						return Failure(TypeSnapshotPublishError::FunctionInvalid, typeErrorIndex, memberIndex);
					}
					if (parameter.Direction == ReflectedParameterDirection::Return)
						++returnCount;
					TypeSnapshotPublishError propertyError = TypeSnapshotPublishError::None;
					if (returnCount > 1
						|| !parameterNames.emplace(parameter.Property.Name).second
						|| !FreezeProperty(
							parameter.Property,
							function.ParameterSize,
							descriptors,
							propertyError))
					{
						return Failure(
							propertyError == TypeSnapshotPublishError::None
								? TypeSnapshotPublishError::FunctionInvalid
								: propertyError,
							typeErrorIndex,
							memberIndex);
					}
				}
			}
			++typePosition;
		}
		if (seenFunctions.size() != expectedFunctionCount)
			return Failure(TypeSnapshotPublishError::FunctionCoverageMismatch);

		for (std::size_t index = 0; index < published->m_Types.size(); ++index)
		{
			const ReflectedType& type = published->m_Types[index];
			if (type.Kind == ReflectedTypeKind::Enum || !type.Super)
				continue;
			const ReflectedType* parent = published->FindByObjectIndex(type.Super->Index);
			if (!parent
				|| !SameHandle(*type.Super, parent->Handle)
				|| parent->Kind != type.Kind
				|| type.PropertiesSize < parent->PropertiesSize)
			{
				return Failure(
					TypeSnapshotPublishError::RelationshipInvalid,
					static_cast<std::int32_t>(index));
			}
			published->m_DirectChildrenBySuperIndex[type.Super->Index].push_back(index);
			++published->m_DirectChildCount;
		}

		for (std::size_t index = 0; index < published->m_Types.size(); ++index)
		{
			const ReflectedType* current = &published->m_Types[index];
			std::set<std::int32_t> visited;
			std::size_t depth = 0;
			while (current && current->Super)
			{
				if (!visited.emplace(current->Handle.Index).second)
				{
					return Failure(
						TypeSnapshotPublishError::HierarchyCycle,
						static_cast<std::int32_t>(index));
				}
				if (depth++ >= kMaxHierarchyDepth)
				{
					return Failure(
						TypeSnapshotPublishError::HierarchyDepthExceeded,
						static_cast<std::int32_t>(index));
				}
				current = published->FindByObjectIndex(current->Super->Index);
				if (!current)
				{
					return Failure(
						TypeSnapshotPublishError::RelationshipInvalid,
						static_cast<std::int32_t>(index));
				}
			}
		}

		for (std::size_t index = 0; index < published->m_Types.size(); ++index)
		{
			const ReflectedType& type = published->m_Types[index];
			if (type.Kind == ReflectedTypeKind::Enum || !type.Super)
				continue;
			const ReflectedType* parent = published->FindByObjectIndex(type.Super->Index);
			if (!parent)
				return Failure(TypeSnapshotPublishError::RelationshipInvalid,
					static_cast<std::int32_t>(index));
			for (const ReflectedProperty& property : type.DirectProperties)
			{
				if (property.Offset < parent->PropertiesSize)
				{
					return Failure(
						TypeSnapshotPublishError::RelationshipInvalid,
						static_cast<std::int32_t>(index));
				}
			}
		}

		published->m_ValidationFingerprint = HashSnapshot(*published);
		if (!published->IsConfigured(m_ContextGeneration))
			return Failure(TypeSnapshotPublishError::EnvelopeInvalid);

		std::lock_guard<std::mutex> lock(m_PublishMutex);
		if (IsStopped())
			return Failure(TypeSnapshotPublishError::StoreStopped);
		const std::shared_ptr<const TypeSnapshot> current = Current();
		if (current && published->Generation() <= current->Generation())
			return Failure(TypeSnapshotPublishError::GenerationNotMonotonic);
		std::shared_ptr<const TypeSnapshot> immutable = std::move(published);
		m_Current.store(immutable, std::memory_order_release);
		return {.Snapshot = std::move(immutable)};
	}
	catch (const std::bad_alloc&)
	{
		return Failure(TypeSnapshotPublishError::AllocationFailed);
	}
	catch (...)
	{
		return Failure(TypeSnapshotPublishError::AllocationFailed);
	}
}

std::shared_ptr<const TypeSnapshot> TypeSnapshotStore::Current() const noexcept
{
	return m_Current.load(std::memory_order_acquire);
}

std::uint64_t TypeSnapshotStore::CurrentGeneration() const noexcept
{
	const std::shared_ptr<const TypeSnapshot> current = Current();
	return current ? current->Generation() : 0;
}

void TypeSnapshotStore::Stop() noexcept
{
	std::lock_guard<std::mutex> lock(m_PublishMutex);
	m_Stopped.store(true, std::memory_order_release);
}

} // namespace UExplorer::Runtime
