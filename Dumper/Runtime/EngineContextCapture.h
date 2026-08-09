#pragma once

#include "EngineContext.h"

#include <cstdint>
#include <memory>

namespace UExplorer::Runtime
{

std::shared_ptr<const EngineContext> CaptureEngineContext(std::uint64_t generation);

} // namespace UExplorer::Runtime
