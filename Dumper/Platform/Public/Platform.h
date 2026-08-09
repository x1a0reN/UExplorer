#pragma once

#if defined(_WIN32) || defined(_WIN64)

#if !defined(_WIN64)
#error "UExplorer Core supports Windows x64 only."
#endif

#include "Platform/Private/PlatformWindows.h"

namespace Platform = PlatformWindows;

#define PLATFORM_WINDOWS

// A macro to append the correct postfix for this platform to a function name and call the function with the supplied parameters.
#define CALL_PLATFORM_SPECIFIC_FUNCTION(FunctionName, ...) FunctionName##_Windows(__VA_ARGS__)

#define PLATFORM_WINDOWS64

#elif defined (__ANDROID__)
#error "The dumper does not support android."
#elif defined (__linux__)
#error "The dumper does not support linux."
#else
#error "Unknown and unsupported platform."
#endif // _WIN32 || _WIN64

