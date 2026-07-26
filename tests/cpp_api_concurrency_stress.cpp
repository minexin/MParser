#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::Backend;
using mparser::sdk::CancellationToken;
using mparser::sdk::Invocation;
using mparser::sdk::Module;
using mparser::sdk::Result;
using mparser::sdk::Session;
using mparser::sdk::SourceUnit;
using mparser::sdk::StopReason;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

constexpr std::size_t kThreadCount = 8;
constexpr std::size_t kIterations = 100;
constexpr double kTolerance = 1e-9;

const std::vector<SourceUnit> kSources{
    SourceUnit{
        "concurrency_api.m",
        R"(function out = makeCounter()
out = ConcurrentCounter();
end

function out = nextCounter(counter)
out = counter.next();
end

function out = bindSessionCounter(counter)
global sharedCounter
sharedCounter = counter;
out = sharedCounter.next();
end

function out = nextSessionCounter()
global sharedCounter
out = sharedCounter.next();
end

function out = ticket()
persistent count
if isempty(count)
    count = 0;
end
count = count + 1;
out = count;
end

function out = sumTo(limit)
out = 0;
for i = 1:limit
    out = out + i;
end
end

function out = spin()
out = 0;
while 1
    out = out + 1;
end
end
)"},
    SourceUnit{
        "ConcurrentCounter.m",
        R"(classdef ConcurrentCounter < handle
    properties
        Value = 0
    end
    methods
        function obj = ConcurrentCounter()
            obj.Value = 0;
        end
        function out = next(obj)
            obj.Value = obj.Value + 1;
            out = obj.Value;
        end
    end
end
)"}};

double scalar(const Value& value) {
    assert(value.kind() == ValueKind::Numeric);
    const auto data = value.numericData();
    assert(data.size() == 1);
    return data.front();
}

Result execute(
    const Module& module,
    const char* entry,
    std::vector<Value> arguments = {},
    Backend backend = Backend::Automatic) {
    Invocation request;
    request.entryFunction = entry;
    request.arguments = std::move(arguments);
    request.requestedOutputCount = 1;
    request.backend = backend;
    return module.execute(request);
}

Result execute(
    Session& session,
    const char* entry,
    std::vector<Value> arguments = {}) {
    Invocation request;
    request.entryFunction = entry;
    request.arguments = std::move(arguments);
    request.requestedOutputCount = 1;
    return session.execute(request);
}

void waitForStart(const std::atomic<bool>& start) {
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void requireSequence(
    std::vector<double> values,
    std::size_t first = 1) {
    std::sort(values.begin(), values.end());
    for (std::size_t index = 0; index < values.size(); ++index) {
        assert(std::abs(
                   values[index] -
                   static_cast<double>(index + first)) <
               kTolerance);
    }
}

void runStatelessNumericStress(const Module& module) {
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t threadIndex = 0;
         threadIndex < kThreadCount; ++threadIndex) {
        auto localModule = module;
        threads.emplace_back(
            [&, threadIndex,
             localModule = std::move(localModule)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kIterations; ++iteration) {
                        const double limit = static_cast<double>(
                            10 + threadIndex + iteration % 7);
                        const auto result = execute(
                            localModule, "sumTo",
                            {Value::scalar(limit)});
                        const double expected =
                            limit * (limit + 1.0) / 2.0;
                        if (!result.succeeded() ||
                            std::abs(
                                scalar(result.output(0)) -
                                expected) >= kTolerance) {
                            failed.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
}

void runSharedHandleStress(const Module& module) {
    auto created = execute(module, "makeCounter");
    assert(created.succeeded());
    auto counter = created.output(0);
    assert(counter.kind() == ValueKind::Object);
    assert(counter.isModuleBound());
    created = Result{};

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<double> values(
        kThreadCount * kIterations, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t threadIndex = 0;
         threadIndex < kThreadCount; ++threadIndex) {
        auto localModule = module;
        auto localCounter = counter;
        threads.emplace_back(
            [&, threadIndex,
             localModule = std::move(localModule),
             localCounter = std::move(localCounter)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kIterations; ++iteration) {
                        const auto result = execute(
                            localModule, "nextCounter",
                            {localCounter}, Backend::Bytecode);
                        if (!result.succeeded()) {
                            failed.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                        values[
                            threadIndex * kIterations + iteration] =
                            scalar(result.output(0));
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
    requireSequence(std::move(values));
}

void runSharedSessionStress(const Module& module) {
    auto session = module.createSession();
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<double> values(
        kThreadCount * kIterations, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t threadIndex = 0;
         threadIndex < kThreadCount; ++threadIndex) {
        auto localSession = session;
        threads.emplace_back(
            [&, threadIndex,
             localSession = std::move(localSession)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kIterations; ++iteration) {
                        const auto result =
                            execute(localSession, "ticket");
                        if (!result.succeeded()) {
                            failed.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                        values[
                            threadIndex * kIterations + iteration] =
                            scalar(result.output(0));
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
    requireSequence(std::move(values));
}

void runSharedSessionGraphStress(const Module& module) {
    constexpr std::size_t kSessionCount = 2;
    auto created = execute(module, "makeCounter");
    assert(created.succeeded());
    auto counter = created.output(0);
    created = Result{};

    std::vector<Session> sessions;
    sessions.reserve(kSessionCount);
    for (std::size_t index = 0; index < kSessionCount; ++index) {
        auto session = module.createSession();
        const auto initialized = execute(
            session, "bindSessionCounter", {counter});
        assert(initialized.succeeded());
        assert(std::abs(
                   scalar(initialized.output(0)) -
                   static_cast<double>(index + 1)) <
               kTolerance);
        sessions.push_back(std::move(session));
    }
    counter = Value{};

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<double> values(
        kSessionCount * kIterations, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(kSessionCount);
    for (std::size_t index = 0; index < kSessionCount; ++index) {
        auto localSession = sessions[index];
        threads.emplace_back(
            [&, index,
             localSession = std::move(localSession)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kIterations; ++iteration) {
                        const auto result = execute(
                            localSession, "nextSessionCounter");
                        if (!result.succeeded()) {
                            failed.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                        values[index * kIterations + iteration] =
                            scalar(result.output(0));
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
    requireSequence(std::move(values), kSessionCount + 1);
}

void runIndependentSessionStress(const Module& module) {
    constexpr std::size_t kSessionCount = 4;
    constexpr std::size_t kSessionIterations = 50;
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<double> finalValues(kSessionCount, 0.0);
    std::vector<Session> sessions;
    sessions.reserve(kSessionCount);
    for (std::size_t index = 0; index < kSessionCount; ++index) {
        sessions.push_back(module.createSession());
    }

    std::vector<std::thread> threads;
    threads.reserve(kSessionCount);
    for (std::size_t index = 0; index < kSessionCount; ++index) {
        auto localSession = sessions[index];
        threads.emplace_back(
            [&, index,
             localSession = std::move(localSession)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kSessionIterations; ++iteration) {
                        const auto result =
                            execute(localSession, "ticket");
                        if (!result.succeeded()) {
                            failed.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                        finalValues[index] =
                            scalar(result.output(0));
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
    for (const double value : finalValues) {
        assert(std::abs(
                   value -
                   static_cast<double>(kSessionIterations)) <
               kTolerance);
    }
}

void runCancellationStress(const Module& module) {
    CancellationToken token;
    std::atomic<bool> start{false};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        (void)index;
        auto localModule = module;
        auto localToken = token;
        threads.emplace_back(
            [&, localModule = std::move(localModule),
             localToken = std::move(localToken)]() mutable {
                try {
                    ready.fetch_add(1, std::memory_order_release);
                    waitForStart(start);
                    Invocation request;
                    request.entryFunction = "spin";
                    request.requestedOutputCount = 1;
                    request.cancellationToken = localToken;
                    const auto result = localModule.execute(request);
                    if (result.succeeded() ||
                        result.executionSummary().stopReason !=
                            StopReason::Cancelled) {
                        failed.store(
                            true, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    while (ready.load(std::memory_order_acquire) != kThreadCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    token.request();
    for (auto& thread : threads) {
        thread.join();
    }
    assert(token.requested());
    assert(!failed.load(std::memory_order_relaxed));
}

void runResourceIsolationStress(const Module& module) {
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        auto localModule = module;
        threads.emplace_back(
            [&, index,
             localModule = std::move(localModule)]() mutable {
                try {
                    waitForStart(start);
                    Invocation request;
                    request.entryFunction = "spin";
                    request.requestedOutputCount = 1;
                    request.limits.maximumInstructionCount =
                        32 + index;
                    const auto result = localModule.execute(request);
                    const auto summary = result.executionSummary();
                    if (result.succeeded() ||
                        summary.stopReason !=
                            StopReason::InstructionLimit ||
                        summary.executedInstructionCount !=
                            32 + index) {
                        failed.store(
                            true, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    assert(!failed.load(std::memory_order_relaxed));
}

} // namespace

int main() {
    const auto module = Module::compile(kSources);
    assert(module.isValid());

    runStatelessNumericStress(module);
    runSharedHandleStress(module);
    runSharedSessionStress(module);
    runSharedSessionGraphStress(module);
    runIndependentSessionStress(module);
    runCancellationStress(module);
    runResourceIsolationStress(module);

    std::cout << "cpp api concurrency stress = "
              << kThreadCount * kIterations << ','
              << kThreadCount * kIterations
              << ",200,50,cancelled-8,limited-8\n";
    return 0;
}
