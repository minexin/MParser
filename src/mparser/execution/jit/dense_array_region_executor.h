#pragma once

#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/jit/typed_region_executor.h"
#include "mparser/execution/runtime_fallback.h"
#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mparser {

class BuiltinRegistry;
class BytecodeVmTrustedAccess;

struct DenseArrayRegionAnalysis {
    bool available = false;
    bool closed = false;
    bool eligible = false;
    bool hasUnsupportedCall = false;
    bool hasUnsupportedOperation = false;
    bool explicitArraySyntax = false;
    size_t beginPc = 0;
    size_t endPc = 0;
    size_t storePc = 0;
    size_t elementwiseOperationCount = 0;
    size_t reductionOperationCount = 0;
    std::string target;
    std::vector<std::string> inputs;
    std::vector<std::string> callTargets;
    RuntimeFallbackKind fallbackKind = RuntimeFallbackKind::None;
    std::string reason;
};

DenseArrayRegionAnalysis analyzeDenseArrayAssignmentRegion(
    const BytecodeProgram& program, size_t storePc,
    const BuiltinRegistry& builtinRegistry);

struct DenseArrayRegionExecutionResult {
    TypedRegionExecutionStatus status =
        TypedRegionExecutionStatus::Fallback;
    RuntimeFallbackKind fallbackKind = RuntimeFallbackKind::None;
    RuntimeFallbackKind nativeFallbackKind =
        RuntimeFallbackKind::None;
    RuntimeValue value;
    size_t elementCount = 0;
    size_t executedInstructionCount = 0;
    size_t executedKernelInstructionCount = 0;
    TypedRegionBackend backend = TypedRegionBackend::Portable;
    size_t nativeCompilationCount = 0;
    size_t nativeCacheHitCount = 0;
    size_t nativeCacheInsertionCount = 0;
    size_t nativeCacheBypassCount = 0;
    size_t nativeCacheEvictionCount = 0;
    size_t nativeCacheEvictedCodeBytes = 0;
    size_t nativeCodeSize = 0;
    std::string nativePlatform;
    std::string nativeFallbackReason;
    std::string reason;
};

class DenseArrayTypedRegionExecutor {
public:
    DenseArrayTypedRegionExecutor();
    explicit DenseArrayTypedRegionExecutor(
        std::shared_ptr<const BuiltinRegistry> builtinRegistry);

    DenseArrayRegionExecutionResult execute(
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeWorkspace& variables,
        TypedRegionBackend backend = TypedRegionBackend::Auto) const;

private:
    friend class BytecodeVmTrustedAccess;

    DenseArrayRegionExecutionResult executeValidated(
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeWorkspace& variables,
        TypedRegionBackend backend = TypedRegionBackend::Auto) const;

    std::shared_ptr<const BuiltinRegistry> builtinRegistry_;
};

} // namespace mparser
