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
    SystemContextOptions system;
    system.rootDirectory = ".";
    system.capabilities = SystemCapability::DynamicEvaluation;
    auto runtime = Runtime::create(SystemContext::rootedNative(system));
    const auto producer = Module::compile(
        "factor=4;\nh=@(x) factor*x;\nz=0;\n", "producer.m");
    Value handle;
    size_t captureCount = 0;
    Debugger capture([&](const DebugEvent& event) {
        const auto evaluated = event.evaluate(event.frames.size() - 1, "h");
        require(evaluated.succeeded(), "runtime debug handle evaluation failed");
        handle = evaluated.output(0);
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
        const auto evaluated = event.evaluate(1, "factor+2");
        const auto output = evaluated.output(0);
        require(evaluated.succeeded() && output.numericData()[0] == 6,
                "SDK cross-module evaluation lost captured values");
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

void checkEvaluationLifetime() {
    SystemContextOptions system;
    system.rootDirectory = ".";
    system.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(system);
    const auto module = Module::compile("x=3;\ny=x+1;\n", "evaluate.m");
    for (int scenario = 0; scenario < 6; ++scenario) {
        const int mode = scenario / 2;
        const bool fail = scenario % 2 != 0;
        DebugEvent retained;
        Result evaluated;
        Result stackResult;
        Debugger debugger([&](const DebugEvent& event) {
            retained = event;
            stackResult = event.evaluate(0, "dbstack()");
            require(stackResult.succeeded(), "SDK stack query failed");
            auto foreign = std::async(std::launch::async, [snapshot = event] {
                try { (void)snapshot.evaluate(0, "x=99;", 0); }
                catch (const ApiError& error) {
                    return error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
                }
                return false;
            });
            require(foreign.get(), "foreign-thread evaluation was accepted");
            require(!event.evaluate(0, "unknown_debug_variable").succeeded(),
                    "debug evaluation error was lost");
            evaluated = event.evaluate(0, "x+2");
            const auto output = evaluated.output(0);
            require(evaluated.succeeded() && output.numericData()[0] == 5,
                    "C++ debug expression result incorrect");
            require(event.evaluate(0, "x=10;", 0).succeeded(), "C++ debug assignment failed");
            require(local(event.frames[0], "x").numericData()[0] == 3,
                    "evaluation changed the owned pause snapshot");
            if (fail) { throw std::runtime_error("expire on unwind"); }
            return DebugAction::Continue;
        });
        debugger.setBreakpoints(std::array{Breakpoint{"evaluate.m", 2}});
        Invocation invocation;
        invocation.debugger = debugger;
        auto session = module.createSession(context);
        auto runtime = Runtime::create(context);
        const auto result = mode == 0 ? module.execute(invocation, context)
            : mode == 1 ? session.execute(invocation)
                        : runtime.execute(module, invocation);
        require(result.succeeded() != fail, "evaluation callback outcome incorrect");
        const auto retainedOutput = evaluated.output(0);
        require(evaluated.succeeded() && retainedOutput.numericData()[0] == 5,
                "owned evaluation result did not survive callback");
        const auto stackValue = stackResult.output(0);
        require(stackValue.structFieldNames() == std::vector<std::string>{"file", "name", "line"},
                "owned stack query lost its field schema");
        const auto stackLine = stackValue.structField(0, 2);
        require(stackLine.numericData()[0] == 2,
                "owned stack query lost paused line after callback");
        bool expired = false;
        try { (void)retained.evaluate(0, "x=99;", 0); }
        catch (const ApiError& error) {
            expired = error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        require(expired, "retained event evaluated after resume or unwind");
        if (!fail) {
            bool found = false;
            for (const auto& value : result.variables()) {
                if (value.name == "y") { found = value.value.numericData()[0] == 11; }
            }
            require(found, "resumed execution lost C++ debug assignment");
        }
    }
}

void checkSourceNavigation() {
    const auto module = Module::compile(
        "x=3;\ny=f(4);\nz=x+y;\nfunction out=f(value)\nout=value*2;\nend\n", "navigate.m");
    SystemContextOptions system;
    system.rootDirectory = ".";
    system.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(system);
    size_t pauses = 0;
    Debugger debugger([&](const DebugEvent& event) {
        ++pauses;
        auto command = [&](std::string_view source) {
            return event.evaluate(DebugEvent::selectedFrame, source, 0);
        };
        require(command("assert(value==4); dbup;").succeeded(), "navigation to caller failed");
        require(command("assert(x==3); x=10;").succeeded(), "selected caller write failed");
        require(!command("dbup").succeeded(), "navigation exceeded outermost frame");
        require(command("assert(x==10); dbdown;").succeeded(), "navigation corrupted caller selection");
        require(command("value=5;").succeeded(), "selected callee write failed");
        require(!command("dbdown").succeeded(), "navigation exceeded current frame");
        return DebugAction::Continue;
    });
    debugger.setBreakpoints(std::array{Breakpoint{"navigate.m", 5}});
    Invocation invocation;
    invocation.debugger = debugger;
    const auto result = module.execute(invocation, context);
    require(result.succeeded() && pauses == 1, "navigation interrupted execution");
    bool found = false;
    for (const auto& variable : result.variables()) {
        if (variable.name == "z") { found = variable.value.numericData()[0] == 20; }
    }
    require(found, "navigation writes did not survive resume");
}

void checkConditionalBreakpoints() {
    const auto module = Module::compile(
        "total=0;\nfor k=1:4\ntotal=total+k;\nend\n", "condition.m");
    SystemContextOptions system;
    system.rootDirectory = ".";
    system.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(system);
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool denied = scenario == 3;
        std::vector<DebugEvent> events;
        Debugger debugger([&](const DebugEvent& event) {
            events.push_back(event);
            require(event.reason == DebugReason::Breakpoint, "condition pause reason incorrect");
            return DebugAction::Continue;
        });
        debugger.setBreakpoints(std::array{
            Breakpoint{"condition.m", 3, scenario == 1 ? "unknown_condition" :
                scenario == 2 ? "'text'" : "k==3"}});
        Invocation invocation;
        invocation.debugger = debugger;
        const auto result = denied ? module.execute(invocation) : module.execute(invocation, context);
        require(result.succeeded(), "condition error aborted execution");
        require(events.size() == (scenario == 0 ? 1u : 4u), "conditional pause count incorrect");
        for (const auto& event : events) {
            require(event.conditionDiagnostics.empty() == (scenario == 0),
                    "condition diagnostics were lost or invented");
            if (scenario == 2) {
                require(event.conditionDiagnostics[0].source &&
                        event.conditionDiagnostics[0].source->sourceName == "condition.m",
                        "invalid condition diagnostic lost its source");
            }
            if (denied) {
                require(event.conditionDiagnostics[0].identifier == "MParser:SystemCapabilityDenied",
                        "condition bypassed the capability requirement");
            }
        }
        if (scenario == 0) {
            require(local(events[0].frames.back(), "k").numericData()[0] == 3,
                    "condition did not use current loop variable");
        }
    }
    size_t pauses = 0;
    Debugger ordered([&](const DebugEvent& event) {
        ++pauses;
        require(event.conditionDiagnostics.empty(), "condition ordering evaluated an unreachable error");
        return DebugAction::Continue;
    });
    ordered.setBreakpoints(std::array{Breakpoint{"condition.m", 3, "0"},
        Breakpoint{"condition.m", 3, "1"}, Breakpoint{"condition.m", 3, "unknown_condition"}});
    Invocation invocation;
    invocation.debugger = ordered;
    require(module.execute(invocation, context).succeeded() && pauses == 4,
            "matching breakpoint conditions did not run in configuration order");

    const auto stepping = Module::compile("x=1;\ny=x+1;\n", "step-condition.m");
    std::vector<DebugReason> reasons;
    Debugger stepper([&](const DebugEvent& event) {
        reasons.push_back(event.reason);
        return reasons.size() == 1 ? DebugAction::StepOver : DebugAction::Continue;
    });
    stepper.setBreakpoints(std::array{Breakpoint{"step-condition.m", 2, "0"}});
    stepper.requestPause();
    invocation.debugger = stepper;
    require(stepping.execute(invocation, context).succeeded() &&
            reasons == std::vector<DebugReason>{DebugReason::PauseRequest, DebugReason::Step},
            "false condition swallowed a pause or step request");
}
} // namespace

int main() {
    try {
        checkEntryPoints();
        checkSharedRuntimeOwnership();
        checkPauseAndReentry();
        checkStopAndFailure();
        checkEvaluationLifetime();
        checkConditionalBreakpoints();
        checkSourceNavigation();
        std::cout << "debugger SDK = steps,locals,evaluation,runtime,retained,threads,reentry,cancel\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
