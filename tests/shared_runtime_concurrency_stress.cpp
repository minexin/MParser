#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::Invocation;
using mparser::sdk::Module;
using mparser::sdk::Runtime;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

constexpr std::size_t kThreadCount = 8;
constexpr std::size_t kIterations = 50;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double scalar(const Value& value) {
    require(value.kind() == ValueKind::Numeric,
            "concurrent output is not numeric");
    const auto data = value.numericData();
    require(data.size() == 1,
            "concurrent output is not scalar");
    return data.front();
}

mparser::sdk::Result execute(
    Runtime& runtime, const Module& module, const char* entry,
    std::vector<Value> arguments = {}) {
    Invocation invocation;
    invocation.entryFunction = entry;
    invocation.arguments = std::move(arguments);
    invocation.requestedOutputCount = 1;
    invocation.backend = mparser::sdk::Backend::Bytecode;
    return runtime.execute(module, invocation);
}

void waitForStart(const std::atomic<bool>& start) {
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void runConcurrencyStress() {
    const Module owner = Module::compile(
        R"(function out = makeTicketHandle()
out = @ticket;
end

function out = ticket()
persistent count
if isempty(count)
    count = 0;
end
count = count + 1;
out = count;
end
)",
        "shared_runtime_concurrency_owner.m");
    const Module caller = Module::compile(
        R"(function out = invokeTicket(callback)
out = callback();
end
)",
        "shared_runtime_concurrency_caller.m");
    require(owner.isValid() && caller.isValid(),
            "concurrency modules did not compile");

    Runtime runtime = Runtime::create();
    auto created = execute(runtime, owner, "makeTicketHandle");
    require(created.succeeded() && created.outputCount() == 1,
            "ticket handle factory failed");
    Value callback = created.output(0);
    created = mparser::sdk::Result{};

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<double> values(kThreadCount * kIterations, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t threadIndex = 0;
         threadIndex < kThreadCount; ++threadIndex) {
        Runtime localRuntime = runtime;
        Module localCaller = caller;
        Value localCallback = callback;
        threads.emplace_back(
            [&, threadIndex,
             localRuntime = std::move(localRuntime),
             localCaller = std::move(localCaller),
             localCallback = std::move(localCallback)]() mutable {
                try {
                    waitForStart(start);
                    for (std::size_t iteration = 0;
                         iteration < kIterations; ++iteration) {
                        const auto result = execute(
                            localRuntime, localCaller, "invokeTicket",
                            {localCallback});
                        if (!result.succeeded() ||
                            result.outputCount() != 1) {
                            failed.store(true,
                                         std::memory_order_relaxed);
                            return;
                        }
                        values[threadIndex * kIterations + iteration] =
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
    require(!failed.load(std::memory_order_relaxed),
            "concurrent shared-runtime execution failed");

    std::sort(values.begin(), values.end());
    for (std::size_t index = 0; index < values.size(); ++index) {
        require(std::abs(values[index] -
                         static_cast<double>(index + 1)) < 1e-9,
                "shared runtime did not serialize persistent updates");
    }
}

} // namespace

int main() {
    try {
        runConcurrencyStress();
        std::cout << "shared runtime concurrency = "
                  << kThreadCount * kIterations
                  << ",serialized,reentrant\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shared runtime concurrency failed: "
                  << error.what() << '\n';
        return 1;
    }
}
