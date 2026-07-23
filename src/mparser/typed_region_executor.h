#pragma once

#include "mparser/bytecode_region.h"
#include "mparser/runtime_value.h"

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
    RuntimeFallbackKind fallbackKind = RuntimeFallbackKind::None;
    RuntimeFallbackKind nativeFallbackKind =
        RuntimeFallbackKind::None;
    RuntimeWorkspace variables;
    size_t iterationCount = 0;
    size_t nestedIterationCount = 0;
    size_t executedInstructionCount = 0;
    size_t executedKernelInstructionCount = 0;
    TypedRegionBackend backend = TypedRegionBackend::Portable;
    bool nativeCompiled = false;
    bool nativeCacheHit = false;
    bool nativeCacheStored = false;
    bool nativeCacheBypassed = false;
    size_t nativeCacheEvictionCount = 0;
    size_t nativeCacheEvictedCodeBytes = 0;
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
        const RuntimeWorkspace& variables,
        TypedRegionBackend backend = TypedRegionBackend::Auto) const;
};

} // namespace mparser
