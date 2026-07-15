#pragma once

#include "mparser/bytecode_region.h"
#include "mparser/interpreter.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace mparser {

enum class TypedRegionExecutionStatus {
    Executed,
    Fallback,
};

enum class TypedRegionBackend {
    Auto,
    Portable,
    Native,
};

std::string_view typedRegionBackendName(TypedRegionBackend backend);

struct TypedRegionExecutionResult {
    TypedRegionExecutionStatus status =
        TypedRegionExecutionStatus::Fallback;
    std::map<std::string, RuntimeValue> variables;
    size_t iterationCount = 0;
    size_t nestedIterationCount = 0;
    size_t executedInstructionCount = 0;
    size_t executedKernelInstructionCount = 0;
    TypedRegionBackend backend = TypedRegionBackend::Portable;
    bool nativeCompiled = false;
    bool nativeCacheHit = false;
    size_t nativeCodeSize = 0;
    std::string nativePlatform;
    std::string nativeFallbackReason;
    std::string reason;
};

class ScalarTypedRegionExecutor {
public:
    TypedRegionExecutionResult execute(
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeValue& loopRange,
        const std::map<std::string, RuntimeValue>& variables,
        TypedRegionBackend backend = TypedRegionBackend::Auto) const;
};

} // namespace mparser
