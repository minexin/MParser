#include "mparser/native_scalar_jit.h"

#include <barrier>
#include <cstddef>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef assert
#undef assert
#endif

#define assert(condition)                                                   \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error("requirement failed: " #condition);   \
        }                                                                   \
    } while (false)

namespace {

constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();

mparser::ScalarKernel makeKernel(double increment) {
    mparser::ScalarKernel kernel;
    kernel.slotNames = {"i", "sum"};
    kernel.slots.resize(2);
    kernel.initialized = {false, true};
    kernel.loopSlot = 0;

    mparser::ScalarKernelInstruction add;
    add.op = mparser::ScalarKernelOp::Add;
    add.destination = {mparser::ScalarKernelStorage::Slot, 1};
    add.left = {mparser::ScalarKernelStorage::Slot, 1, {}};
    add.right = {mparser::ScalarKernelStorage::Literal, 0,
                 {increment}};
    add.sourceInstructionCount = 1;
    kernel.instructions.push_back(add);
    return kernel;
}

mparser::NativeScalarJitResult executeKernel(double increment) {
    auto kernel = makeKernel(increment);
    const double outerValues[] = {1.0, 2.0, 3.0};
    auto result = mparser::executeNativeScalarKernel(
        kernel, outerValues, 3);
    assert(result.status == mparser::NativeScalarJitStatus::Executed);
    assert(result.writtenSlots.size() == kernel.slots.size());
    assert(result.writtenSlots[1] != 0);
    assert(std::abs(kernel.slots[1].value - increment * 3.0) < 1e-12);
    assert(result.codeSize != 0);
    return result;
}

void resetCache(const mparser::NativeScalarJitCacheLimits& limits) {
    mparser::configureNativeScalarJitCache(limits);
    mparser::clearNativeScalarJitCache();
    mparser::resetNativeScalarJitCacheStatistics();
}

class CacheStateRestorer {
public:
    CacheStateRestorer()
        : limits_(mparser::nativeScalarJitCacheStatistics().limits) {}

    ~CacheStateRestorer() {
        mparser::clearNativeScalarJitCache();
        mparser::configureNativeScalarJitCache(limits_);
        mparser::resetNativeScalarJitCacheStatistics();
    }

private:
    mparser::NativeScalarJitCacheLimits limits_;
};

void runDisabledBackendControlSmoke() {
    resetCache({0, 0});
    auto statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.limits.maxEntries == 0);
    assert(statistics.limits.maxCodeBytes == 0);
    assert(statistics.entryCount == 0);
    assert(statistics.codeBytes == 0);

    mparser::clearNativeScalarJitCache();
    statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.clearCount == 1);
    assert(statistics.clearedEntryCount == 0);
}

void runLeastRecentlyUsedSmoke() {
    resetCache({2, kUnlimited});

    const auto first = executeKernel(1.0);
    const auto second = executeKernel(2.0);
    const auto firstHit = executeKernel(1.0);
    const auto third = executeKernel(3.0);
    const auto firstHitAgain = executeKernel(1.0);
    const auto secondRecompiled = executeKernel(2.0);

    assert(first.compiled && first.cacheStored && !first.cacheHit);
    assert(second.compiled && second.cacheStored && !second.cacheHit);
    assert(firstHit.cacheHit && !firstHit.compiled);
    assert(third.compiled && third.cacheStored);
    assert(third.cacheEvictionCount == 1);
    assert(firstHitAgain.cacheHit && !firstHitAgain.compiled);
    assert(secondRecompiled.compiled && secondRecompiled.cacheStored);
    assert(secondRecompiled.cacheEvictionCount == 1);

    const auto statistics =
        mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 2);
    assert(statistics.lookupCount == 6);
    assert(statistics.hitCount == 2);
    assert(statistics.missCount == 4);
    assert(statistics.compilationCount == 4);
    assert(statistics.compilationFailureCount == 0);
    assert(statistics.insertionCount == 4);
    assert(statistics.evictionCount == 2);
    assert(statistics.evictedCodeBytes > 0);
}

void runCodeBudgetBypassSmoke(size_t codeSize) {
    assert(codeSize > 0);
    resetCache({8, codeSize - 1});

    const auto first = executeKernel(4.0);
    const auto second = executeKernel(4.0);
    assert(first.compiled && first.cacheBypassed && !first.cacheStored);
    assert(second.compiled && second.cacheBypassed && !second.cacheHit);

    const auto statistics =
        mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 0);
    assert(statistics.codeBytes == 0);
    assert(statistics.lookupCount == 2);
    assert(statistics.hitCount == 0);
    assert(statistics.missCount == 2);
    assert(statistics.compilationCount == 2);
    assert(statistics.insertionCount == 0);
    assert(statistics.bypassCount == 2);
}

void runZeroRetentionSmoke() {
    resetCache({0, kUnlimited});

    const auto first = executeKernel(5.0);
    const auto second = executeKernel(5.0);
    assert(first.compiled && first.cacheBypassed && !first.cacheStored);
    assert(second.compiled && second.cacheBypassed && !second.cacheHit);

    const auto statistics =
        mparser::nativeScalarJitCacheStatistics();
    assert(statistics.limits.maxEntries == 0);
    assert(statistics.entryCount == 0);
    assert(statistics.compilationCount == 2);
    assert(statistics.bypassCount == 2);
}

void runDynamicShrinkAndClearSmoke() {
    resetCache({3, kUnlimited});
    (void)executeKernel(1.0);
    (void)executeKernel(2.0);
    (void)executeKernel(3.0);
    auto statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 3);
    assert(statistics.codeBytes > 1);

    mparser::configureNativeScalarJitCache(
        {3, statistics.codeBytes - 1});
    statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 2);
    assert(statistics.evictionCount == 1);
    assert(statistics.codeBytes <= statistics.limits.maxCodeBytes);

    mparser::configureNativeScalarJitCache({1, kUnlimited});
    statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 1);
    assert(statistics.evictionCount == 2);
    assert(executeKernel(3.0).cacheHit);

    mparser::resetNativeScalarJitCacheStatistics();
    statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.limits.maxEntries == 1);
    assert(statistics.limits.maxCodeBytes == kUnlimited);
    assert(statistics.entryCount == 1);
    assert(statistics.codeBytes > 0);
    assert(statistics.lookupCount == 0);
    assert(statistics.evictionCount == 0);
    assert(executeKernel(3.0).cacheHit);
    assert(mparser::nativeScalarJitCacheStatistics().hitCount == 1);

    mparser::clearNativeScalarJitCache();
    statistics = mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 0);
    assert(statistics.codeBytes == 0);
    assert(statistics.clearCount == 1);
    assert(statistics.clearedEntryCount == 1);
    assert(statistics.clearedCodeBytes > 0);
}

void runConcurrentLookupSmoke() {
    resetCache({8, kUnlimited});
    constexpr size_t threadCount = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(threadCount));
    std::vector<mparser::NativeScalarJitResult> results(threadCount);
    std::vector<std::exception_ptr> failures(threadCount);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (size_t index = 0; index < threadCount; ++index) {
        threads.emplace_back([&, index]() {
            try {
                start.arrive_and_wait();
                results[index] = executeKernel(7.0);
            } catch (...) {
                failures[index] = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    for (const auto& result : results) {
        assert(result.status == mparser::NativeScalarJitStatus::Executed);
    }
    const auto statistics =
        mparser::nativeScalarJitCacheStatistics();
    assert(statistics.entryCount == 1);
    assert(statistics.lookupCount == threadCount);
    assert(statistics.hitCount + statistics.missCount == threadCount);
    assert(statistics.compilationCount == statistics.missCount);
    assert(statistics.insertionCount == 1);
    assert(statistics.insertionCount +
               statistics.duplicateCompilationCount ==
           statistics.compilationCount);
    assert(statistics.evictionCount == 0);
}

} // namespace

int main() {
    CacheStateRestorer restore;
    try {
        if (!mparser::nativeScalarJitAvailable()) {
            runDisabledBackendControlSmoke();
            std::cout << "native scalar JIT cache smoke tests passed "
                         "(backend disabled)\n";
            return 0;
        }

        runLeastRecentlyUsedSmoke();
        const size_t codeSize = executeKernel(9.0).codeSize;
        runCodeBudgetBypassSmoke(codeSize);
        runZeroRetentionSmoke();
        runDynamicShrinkAndClearSmoke();
        runConcurrentLookupSmoke();
        std::cout << "native scalar JIT cache smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
