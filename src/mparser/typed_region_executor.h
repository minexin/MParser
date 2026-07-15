#pragma once

#include "mparser/bytecode_region.h"
#include "mparser/interpreter.h"

#include <cstddef>
#include <map>
#include <string>

namespace mparser {

enum class TypedRegionExecutionStatus {
    Executed,
    Fallback,
};

struct TypedRegionExecutionResult {
    TypedRegionExecutionStatus status =
        TypedRegionExecutionStatus::Fallback;
    std::map<std::string, RuntimeValue> variables;
    size_t iterationCount = 0;
    size_t executedInstructionCount = 0;
    size_t executedKernelInstructionCount = 0;
    std::string reason;
};

class ScalarTypedRegionExecutor {
public:
    TypedRegionExecutionResult execute(
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeValue& loopRange,
        const std::map<std::string, RuntimeValue>& variables) const;
};

} // namespace mparser
