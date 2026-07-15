#pragma once

#include "mparser/typed_scalar_kernel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class NativeScalarJitStatus {
    Executed,
    Unavailable,
    Unsupported,
    CompilationFailed,
    RuntimeFailed,
};

struct NativeScalarJitResult {
    NativeScalarJitStatus status = NativeScalarJitStatus::Unavailable;
    std::vector<uint8_t> writtenSlots;
    ScalarKernelExecutionCounters counters;
    bool compiled = false;
    bool cacheHit = false;
    size_t codeSize = 0;
    std::string reason;
};

bool nativeScalarJitAvailable();
std::string_view nativeScalarJitPlatform();
NativeScalarJitResult executeNativeScalarKernel(
    ScalarKernel& kernel, const double* outerValues,
    size_t outerValueCount);

} // namespace mparser
