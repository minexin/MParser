#pragma once

#include "mparser/module_execution.h"

#include <string>
#include <string_view>

namespace mparser {

inline constexpr unsigned kMachineResultProtocolMajor = 1;
inline constexpr unsigned kMachineResultProtocolMinor = 0;

std::string serializeMachineResultJsonV1(
    const ModuleInvocationResult& result,
    std::string_view engineVersion);

int machineResultExitCode(ModuleInvocationStatus status) noexcept;

} // namespace mparser
