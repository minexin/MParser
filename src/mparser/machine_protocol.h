#pragma once

#include "mparser/module_execution.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace mparser {

inline constexpr unsigned kMachineResultProtocolMajor = 1;
inline constexpr unsigned kMachineResultProtocolMinor = 1;

std::string serializeMachineResultJsonV1(
    const ModuleInvocationResult& result,
    std::string_view engineVersion);

std::string_view machineProtocolEmergencyJsonV1() noexcept;

int writeMachineProtocolEmergencyJsonV1(
    std::FILE* output) noexcept;

int writeMachineResultJsonV1(
    std::FILE* output,
    const ModuleInvocationResult& result,
    std::string_view engineVersion) noexcept;

int machineResultExitCode(ModuleInvocationStatus status) noexcept;

} // namespace mparser
