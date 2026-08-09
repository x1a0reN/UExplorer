#pragma once

#include "EngineContext.h"

#include <cstdint>
#include <limits>

namespace UExplorer::Runtime
{

struct ObjectIdentityContext final
{
	std::uint64_t ContextGeneration = 0;
	std::int32_t FUObjectItemSerial = -1;
	std::int32_t ObjectIndex = -1;
	std::int32_t ObjectClass = -1;
	std::int32_t ObjectName = -1;
	std::int32_t ObjectOuter = -1;
	std::int32_t FNameSize = -1;
	std::int32_t FNameComparisonIndex = -1;
	std::int32_t FNameNumber = -1;
	std::int32_t ClassCastFlags = -1;
	std::int32_t FunctionFlags = -1;
	std::int32_t FunctionExec = -1;
	std::int32_t StructSize = -1;

	bool CanIssueObjectHandles() const noexcept
	{
		return ContextGeneration != 0
			&& FUObjectItemSerial >= 0
			&& ObjectIndex > 0
			&& ObjectClass > 0;
	}

	bool CanIssueFunctionHandles() const noexcept
	{
		if (!CanIssueObjectHandles()
			|| ObjectName <= 0
			|| ObjectOuter <= 0
			|| FNameSize < static_cast<std::int32_t>(sizeof(std::uint32_t))
			|| FNameSize > 64
			|| FNameComparisonIndex < 0
			|| FNameNumber < 0
			|| ClassCastFlags <= 0
			|| FunctionFlags <= 0
			|| FunctionExec <= 0
			|| StructSize <= 0)
		{
			return false;
		}
		const auto nameSize = static_cast<std::uint32_t>(FNameSize);
		const auto comparisonOffset = static_cast<std::uint32_t>(FNameComparisonIndex);
		const auto numberOffset = static_cast<std::uint32_t>(FNameNumber);
		return comparisonOffset <= nameSize - sizeof(std::uint32_t)
			&& numberOffset <= nameSize - sizeof(std::uint32_t);
	}
};

inline std::int32_t ValidatedContextOffset(
	const EngineContext& context,
	const char* name)
{
	const OffsetReport* report = context.FindOffset(name);
	if (!report || !report->IsValidated()
		|| report->Value < (std::numeric_limits<std::int32_t>::min)()
		|| report->Value > (std::numeric_limits<std::int32_t>::max)())
	{
		return -1;
	}
	return static_cast<std::int32_t>(report->Value);
}

inline ObjectIdentityContext CaptureObjectIdentityContext(
	const EngineContext& context)
{
	return {
		.ContextGeneration = context.Generation(),
		.FUObjectItemSerial = ValidatedContextOffset(context, "fuobjectitem.serial_number"),
		.ObjectIndex = ValidatedContextOffset(context, "uobject.index"),
		.ObjectClass = ValidatedContextOffset(context, "uobject.class"),
		.ObjectName = ValidatedContextOffset(context, "uobject.name"),
		.ObjectOuter = ValidatedContextOffset(context, "uobject.outer"),
		.FNameSize = ValidatedContextOffset(context, "fname.size"),
		.FNameComparisonIndex = ValidatedContextOffset(context, "fname.comparison_index"),
		.FNameNumber = ValidatedContextOffset(context, "fname.number"),
		.ClassCastFlags = ValidatedContextOffset(context, "uclass.cast_flags"),
		.FunctionFlags = ValidatedContextOffset(context, "ufunction.function_flags"),
		.FunctionExec = ValidatedContextOffset(context, "ufunction.exec_function"),
		.StructSize = ValidatedContextOffset(context, "ustruct.size")
	};
}

} // namespace UExplorer::Runtime
