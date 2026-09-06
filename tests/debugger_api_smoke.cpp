#include "mparser/cpp_api.hpp"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace mparser::sdk;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const Value& local(const DebugFrame& frame, const std::string& name) {
    for (const auto& variable : frame.variables) {
        if (variable.name == name) {
            return variable.value;
        }
    }
    throw std::runtime_error("missing debug variable: " + name);
}

void checkEntryPoints() {
    const auto module = Module::compile(
        "x=2;\ny=inner(x);\nz=y+1;\n"
        "function out=inner(in)\nlocal=in*3;\nout=local+4;\nend\n", "debug.m");
    require(module.isValid(), "SDK debug source did not compile");
    for (const auto backend : {Backend::Bytecode, Backend::Automatic,
                              Backend::Portable, Backend::Native}) {
        for (int mode = 0; mode < 3; ++mode) {
            std::vector<int> lines;
            std::vector<DebugEvent> snapshots;
            Debugger debugger([&](const DebugEvent& event) {
                snapshots.push_back(event);
                lines.push_back(event.frames.back().source.begin.line);
                switch (lines.size()) {
                case 1: return DebugAction::StepOver;
                case 2: return DebugAction::StepInto;
                case 3: return DebugAction::StepOut;
                default: return DebugAction::Continue;
                }
            });
            debugger.requestPause();
            Invocation invocation;
            invocation.backend = backend;
            invocation.debugger = debugger;
            auto session = module.createSession();
            auto runtime = Runtime::create();
            const auto result = mode == 0 ? module.execute(invocation)
                : mode == 1 ? session.execute(invocation)
                            : runtime.execute(module, invocation);
            require(result.succeeded(), "SDK debug execution failed");
            require(lines == std::vector<int>{1, 2, 5, 3}, "SDK step sequence incorrect");
            require(snapshots[2].frames.size() == 2 &&
                    snapshots[2].frames.back().functionName == "inner" &&
                    local(snapshots[2].frames.back(), "in").numericData()[0] == 2 &&
                    local(snapshots.back().frames.back(), "y").numericData()[0] == 10,
                    "retained debug snapshot lost its variables");
            const auto execution = result.executionSummary();
            require(execution.resourceControlsActive &&
                    execution.nativeCompilationCount == 0 &&
                    execution.typedRegionExecutionCount == 0,
                    "debugger did not suppress optimized execution");
        }
    }
}

void checkSharedRuntimeOwnership() {
    auto runtime = Runtime::create();
    const auto producer = Module::compile(
        "factor=4;\nh=@(x) factor*x;\nz=0;\n", "producer.m");
    Value handle;
    size_t captureCount = 0;
    Debugger capture([&](const DebugEvent& event) {
        handle = local(event.frames.back(), "h");
        ++captureCount;
        return DebugAction::Continue;
    });
    capture.setBreakpoints(std::array{Breakpoint{"producer.m", 3}});
    Invocation invocation;
    invocation.debugger = capture;
    require(runtime.execute(producer, invocation).succeeded() && captureCount == 1,
            "debugger did not export runtime-owned handle");
    const auto consumer = Module::compile("out=h(3);\n", "consumer.m");
    size_t calls = 0;
    Debugger inspect([&](const DebugEvent& event) {
        ++calls;
        require(event.frames.size() == 2 &&
                event.frames[0].source.sourceName == "consumer.m" &&
                event.frames[1].source.sourceName == "producer.m" &&
                local(event.frames[1], "factor").numericData()[0] == 4,
                "SDK cross-module stack or locals lost");
        return DebugAction::Continue;
    });
    inspect.setBreakpoints(std::array{Breakpoint{"producer.m", 2}});
    invocation.debugger = inspect;
    invocation.initialWorkspace = {{"h", handle}};
    const auto result = runtime.execute(consumer, invocation);
    require(result.succeeded() && calls == 1, "SDK cross-module debug call failed");
}

void checkPauseAndReentry() {
    using namespace std::chrono_literals;
    const auto module = Module::compile("x=1;\ny=x+2;\n", "debug.m");
    auto session = module.createSession();
    auto runtime = Runtime::create();
    Invocation invocation;
    std::promise<void> paused;
    auto pausedFuture = paused.get_future();
    std::promise<void> resume;
    const auto resumed = resume.get_future().share();
    auto rejects = [](auto operation) {
        try { operation(); }
        catch (const ApiError& error) {
            return error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        return false;
    };
    Debugger debugger([&](const DebugEvent& event) {
        require(event.frames.back().source.begin.line == 2, "pause line incorrect");
        require(rejects([&] { (void)session.execute(); }) &&
                rejects([&] { (void)module.execute(); }) &&
                rejects([&] { (void)runtime.execute(module); }) &&
                rejects([&] { session.reset(); }) &&
                rejects([&] { runtime.reset(); }),
                "callback execution/state mutation was not rejected");
        paused.set_value();
        require(resumed.wait_for(5s) == std::future_status::ready, "resume timed out");
        return DebugAction::Continue;
    });
    debugger.setBreakpoints(std::array{Breakpoint{"debug.m", 2}});
    invocation.debugger = debugger;
    auto future = std::async(std::launch::async, [&] { return session.execute(invocation); });
    const bool didPause = pausedFuture.wait_for(5s) == std::future_status::ready;
    const bool rejected = didPause && rejects([&] { (void)module.execute(invocation); });
    debugger.setBreakpoints({});
    resume.set_value();
    const auto result = future.get();
    require(didPause && rejected && result.succeeded(), "threaded debug guard failed");
    require(module.execute(invocation).succeeded(), "debugger was not reusable after resume");
}

void checkStopAndFailure() {
    const auto module = Module::compile("x=1;\ntry\nx=2;\ncatch\nx=3;\nend\n", "debug.m");
    for (bool fail : {false, true}) {
        Debugger debugger([fail](const DebugEvent&) {
            if (fail) { throw std::runtime_error("host callback failure"); }
            return DebugAction::Stop;
        });
        debugger.setBreakpoints(std::array{Breakpoint{"debug.m", 3}});
        Invocation invocation;
        invocation.debugger = debugger;
        const auto result = module.execute(invocation);
        require(!result.succeeded() &&
                result.executionSummary().stopReason == StopReason::Cancelled,
                "SDK debug stop was swallowed");
        bool callbackDiagnostic = false;
        for (const auto& diagnostic : result.diagnostics()) {
            callbackDiagnostic = callbackDiagnostic ||
                diagnostic.identifier == "MParser:DebugCallbackFailed";
        }
        require(callbackDiagnostic == fail, "callback failure diagnostic incorrect");
    }
    CancellationToken cancellation;
    Debugger debugger([&](const DebugEvent&) {
        cancellation.request();
        return DebugAction::Continue;
    });
    debugger.requestPause();
    Invocation invocation;
    invocation.debugger = debugger;
    invocation.cancellationToken = cancellation;
    const auto result = module.execute(invocation);
    require(!result.succeeded() &&
            result.executionSummary().stopReason == StopReason::Cancelled,
            "cancellation during pause was not observed before mutation");
    require(result.variables().empty(), "cancelled paused statement mutated workspace");
}
} // namespace

int main() {
    try {
        checkEntryPoints();
        checkSharedRuntimeOwnership();
        checkPauseAndReentry();
        checkStopAndFailure();
        std::cout << "debugger SDK = steps,locals,runtime,retained,threads,reentry,cancel\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
