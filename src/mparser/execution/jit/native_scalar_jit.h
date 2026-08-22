#pragma once

#include "mparser/execution/jit/typed_scalar_kernel.h"

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

struct NativeScalarJitCacheLimits {
    size_t maxEntries = 256;
    size_t maxCodeBytes = 16 * 1024 * 1024;
};

struct NativeScalarJitCacheStatistics {
    NativeScalarJitCacheLimits limits;
    size_t entryCount = 0;
    size_t codeBytes = 0;
    size_t lookupCount = 0;
    size_t hitCount = 0;
    size_t missCount = 0;
    size_t compilationCount = 0;
    size_t compilationFailureCount = 0;
    size_t insertionCount = 0;
    size_t duplicateCompilationCount = 0;
    size_t bypassCount = 0;
    size_t evictionCount = 0;
    size_t evictedCodeBytes = 0;
    size_t clearCount = 0;
    size_t clearedEntryCount = 0;
    size_t clearedCodeBytes = 0;
};

struct NativeScalarJitResult {
    NativeScalarJitStatus status = NativeScalarJitStatus::Unavailable;
    std::vector<uint8_t> writtenSlots;
    std::vector<uint8_t> writtenArrays;
    ScalarKernelExecutionCounters counters;
    bool compiled = false;
    bool cacheHit = false;
    bool cacheStored = false;
    bool cacheBypassed = false;
    size_t cacheEvictionCount = 0;
    size_t cacheEvictedCodeBytes = 0;
    size_t codeSize = 0;
    std::string reason;
};

bool nativeScalarJitAvailable();
std::string_view nativeScalarJitPlatform();
void configureNativeScalarJitCache(
    const NativeScalarJitCacheLimits& limits);
void clearNativeScalarJitCache();
void resetNativeScalarJitCacheStatistics();
NativeScalarJitCacheStatistics nativeScalarJitCacheStatistics();
NativeScalarJitResult executeNativeScalarKernel(
    ScalarKernel& kernel, const double* outerValues,
    size_t outerValueCount);

} // namespace mparser
