#pragma once

#include "ObjectIdentityContext.h"

#include <cstdint>

namespace UExplorer::Runtime
{

// Frozen legacy-discovery results that remain necessary for type metadata but
// are not part of the witnessed ReflectionLayout field set.
struct TypeMetadataContext final
{
	std::uint64_t ContextGeneration = 0;
	std::int32_t ObjectClass = -1;
	std::int32_t ClassCastFlags = -1;
	std::int32_t ClassDefaultObject = -1;
	std::int32_t FunctionFlags = -1;
	std::int32_t FunctionExec = -1;
	std::int32_t FunctionScript = -1;

	bool IsConfigured() const noexcept
	{
		return ContextGeneration != 0
			&& ObjectClass > 0
			&& ClassCastFlags > 0
			&& ClassDefaultObject > 0
			&& FunctionFlags > 0;
	}
};

inline TypeMetadataContext CaptureTypeMetadataContext(
	const EngineContext& context)
{
	return {
		.ContextGeneration = context.Generation(),
		.ObjectClass = ValidatedContextOffset(context, "uobject.class"),
		.ClassCastFlags = ValidatedContextOffset(context, "uclass.cast_flags"),
		.ClassDefaultObject = ValidatedContextOffset(context, "uclass.default_object"),
		.FunctionFlags = ValidatedContextOffset(context, "ufunction.function_flags"),
		.FunctionExec = ValidatedContextOffset(context, "ufunction.exec_function"),
		.FunctionScript = ValidatedContextOffset(context, "ufunction.script")
	};
}

} // namespace UExplorer::Runtime
