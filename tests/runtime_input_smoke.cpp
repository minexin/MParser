#include "mparser/runtime/core/session/runtime_input.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/embedding/adaptive_module_runtime.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/io/runtime_system.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>

#include <cassert>
#include <chrono>
#include <iostream>
#include <new>
#include <stdexcept>

using namespace mparser;

void runInteractiveBuiltinSmoke() {
    const auto module = CompiledModule::compile(
        "x=4; a=input('number: '); t=input('text: ','s'); "
        "e=input('empty: '); keyboard; b=x; "
        "reader=@readLocal; localResult=reader(10); outerResult=x; "
        "function out=readLocal(x); a=input('local: '); keyboard; out=a+x; end");
    if (!module.valid()) {
        for (const auto& diagnostic : module.diagnostics()) {
            std::cerr << diagnostic.message << '\n';
        }
        throw std::runtime_error("interactive script compilation failed");
    }
    for (bool reference : {false, true}) {
        const std::vector<std::string> lines = {
            "unknown_input_variable", "x+2", "  hi  ", "", "x=9;", "dbcont",
            "x+2", "x=20;", "dbcont"};
        size_t next = 0;
        auto execution = std::make_shared<RuntimeExecutionControl>(
            RuntimeExecutionLimits{}, std::nullopt, nullptr,
            [&](const auto&) {
                if (next == lines.size()) {
                    return RuntimeInputResult{};
                }
                return RuntimeInputResult{RuntimeInputStatus::Ready, lines[next++], {}};
            });
        std::vector<RuntimeVariable> variables;
        std::vector<Diagnostic> diagnostics;
        std::vector<RuntimeOutputEvent> events;
        RuntimeSystemContextOptions systemOptions;
        systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        const auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(systemOptions));
        if (reference) {
            InterpreterOptions options;
            options.executionControl = execution;
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            variables = result.variables;
            diagnostics = result.diagnostics;
            events = result.outputEvents;
        } else {
            BytecodeVmOptions options;
            options.executionControl = execution;
            options.sessionState = session;
            const auto result = module.invoke(options);
            variables = result.variables;
            diagnostics = result.diagnostics;
            events = result.outputEvents;
        }
        for (const auto& diagnostic : diagnostics) {
            std::cerr << diagnostic.message << '\n';
        }
        if (!diagnostics.empty() || next != lines.size()) {
            for (const auto& event : events) {
                std::cerr << event.text;
            }
            throw std::runtime_error("interactive script execution failed");
        }
        const auto value = [&](std::string_view name) -> const RuntimeValue& {
            const auto found = std::find_if(variables.begin(), variables.end(),
                [&](const auto& variable) { return variable.name == name; });
            if (found == variables.end()) {
                throw std::runtime_error("interactive result variable missing");
            }
            return found->value;
        };
        if (value("a").number != 6 || value("b").number != 9 ||
            value("localResult").number != 32 || value("outerResult").number != 9 ||
            value("e").rows != 0 || value("e").columns != 0 ||
            runtimeTextScalarUtf8(value("t")) != std::optional<std::string>("  hi  ")) {
            throw std::runtime_error("interactive result mismatch");
        }
    }
    size_t deniedReads = 0;
    BytecodeVmOptions deniedOptions;
    deniedOptions.executionControl = std::make_shared<RuntimeExecutionControl>(
        RuntimeExecutionLimits{}, std::nullopt, nullptr, [&](const auto&) {
            ++deniedReads;
            return RuntimeInputResult{RuntimeInputStatus::Ready, "1+2", {}};
        });
    const auto denied = CompiledModule::compile("x=input('');").invoke(deniedOptions);
    if (deniedReads != 1 || std::none_of(denied.diagnostics.begin(), denied.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.identifier == "MParser:SystemCapabilityDenied";
            })) {
        throw std::runtime_error("input permission denial was not terminal");
    }
    BytecodeVmOptions quitOptions;
    quitOptions.executionControl = std::make_shared<RuntimeExecutionControl>(
        RuntimeExecutionLimits{}, std::nullopt, nullptr, [](const auto&) {
            return RuntimeInputResult{RuntimeInputStatus::Ready, "dbquit", {}};
        });
    const auto quit = CompiledModule::compile("x=0; keyboard; x=1;").invoke(quitOptions);
    const auto x = std::find_if(quit.variables.begin(), quit.variables.end(),
        [](const auto& variable) { return variable.name == "x"; });
    if (quitOptions.executionControl->stopReason() != RuntimeExecutionStopReason::Cancelled ||
        x == quit.variables.end() || x->value.number != 0) {
        throw std::runtime_error("keyboard quit executed subsequent statements");
    }
}

void runConsoleOrderingSmoke() {
    const auto module = CompiledModule::compile(R"(
disp('before-marker');
7
text=input('first','s');
eval('disp(''nested-marker''); nested=input(''second'',''s'');');
captured=evalc('disp(''hidden-marker''); 9');
13
keyboard;
after=1;
)");
    if (!module.valid()) {
        throw std::runtime_error("console ordering script compilation failed");
    }
    for (bool reference : {false, true}) {
        std::string console;
        size_t reads = 0;
        const std::vector<std::string> expectedBeforeRead = {
            "before-marker\n\nans = 7\n\n",
            "before-marker\n\nans = 7\n\nnested-marker\n\n",
            "before-marker\n\nans = 7\n\nnested-marker\n\nans = 13\n\n",
            "before-marker\n\nans = 7\n\nnested-marker\n\nans = 13\n\nans = 21\n\n"};
        const std::vector<std::string> replies = {"a", "b", "21", "dbcont"};
        auto execution = std::make_shared<RuntimeExecutionControl>(
            RuntimeExecutionLimits{}, std::nullopt, nullptr,
            [&](const RuntimeInputRequest&) {
                if (reads >= replies.size() || console != expectedBeforeRead[reads]) {
                    throw std::runtime_error("console output did not precede input: " + console);
                }
                return RuntimeInputResult{RuntimeInputStatus::Ready, replies[reads++], {}};
            }, [&](std::string_view text) {
                console.append(text);
                return true;
            });
        RuntimeSystemContextOptions system;
        system.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        const auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(system));
        std::vector<Diagnostic> diagnostics;
        std::string recorded;
        if (reference) {
            InterpreterOptions options;
            options.executionControl = execution;
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            diagnostics = result.diagnostics;
            recorded = runtimeRenderConsole(result.outputEvents, result.expressionResults);
        } else {
            BytecodeVmOptions options;
            options.executionControl = execution;
            options.sessionState = session;
            const auto result = module.invoke(options);
            diagnostics = result.diagnostics;
            recorded = runtimeRenderConsole(result.outputEvents, result.expressionResults);
        }
        if (!diagnostics.empty() || reads != replies.size() || console != recorded ||
            console != expectedBeforeRead.back()) {
            for (const auto& diagnostic : diagnostics) {
                std::cerr << diagnostic.message << '\n';
            }
            throw std::runtime_error("console stream/captured transcript mismatch");
        }
    }
}

void runAdaptiveInputSmoke() {
    const auto module = CompiledModule::compile(
        "function y=readValue(); t=input('value: ','s'); "
        "y=str2double(t); for k=1:20; y=y+1; end; end");
    if (!module.valid()) {
        throw std::runtime_error("adaptive input compilation failed");
    }
    size_t reads = 0;
    auto control = std::make_shared<RuntimeExecutionControl>(
        RuntimeExecutionLimits{}, std::nullopt, nullptr,
        [&](const RuntimeInputRequest& request) {
            if (request.mode != RuntimeInputMode::Text ||
                request.prompt != "value: ") {
                throw std::runtime_error("adaptive input request mismatch");
            }
            return RuntimeInputResult{RuntimeInputStatus::Ready,
                                      std::to_string(++reads), {}};
        });
    AdaptiveModuleRuntimeOptions options;
    options.executionControl = control;
    AdaptiveModuleRuntime runtime(module, options);
    for (size_t i = 1; i <= 2; ++i) {
        const auto result = runtime.invoke("readValue", {}, 1);
        if (!result.adaptive.runtime.diagnostics.empty() ||
            result.adaptive.runtime.outputs.size() != 1 ||
            result.adaptive.runtime.outputs.front().number != i + 20 ||
            (i == 2 && result.adaptive.tier != AdaptiveBytecodeTier::Typed)) {
            throw std::runtime_error("adaptive input control was not preserved");
        }
    }
    if (reads != 2) {
        throw std::runtime_error("adaptive input callback count mismatch");
    }
}

int main() {
    try {
        runInteractiveBuiltinSmoke();
        runConsoleOrderingSmoke();
        runAdaptiveInputSmoke();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    RuntimeInputRequest request{"value: ", RuntimeInputMode::Text, {}};
    RuntimeExecutionControl control;
    size_t polls = 0;
    const auto line = readRuntimeInput([&](const auto& received) {
        assert(received.prompt == "value: ");
        assert(received.mode == RuntimeInputMode::Text);
        return ++polls < 3
            ? RuntimeInputResult{RuntimeInputStatus::Pending, {}, {}}
            : RuntimeInputResult{RuntimeInputStatus::Ready, "  a b  ", {}};
    }, request, control);
    assert(line.status == RuntimeInputStatus::Ready && line.text == "  a b  ");
    assert(polls == 3);
    const auto empty = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Ready, {}, {}};
    }, request, control);
    assert(empty.status == RuntimeInputStatus::Ready && empty.text.empty());
    const auto eof = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{};
    }, request, control);
    assert(eof.status == RuntimeInputStatus::EndOfInput);
    assert(readRuntimeInput({}, request, control).status == RuntimeInputStatus::Error);
    const auto failed = readRuntimeInput([](const auto&) -> RuntimeInputResult {
        throw std::runtime_error("host failure");
    }, request, control);
    assert(failed.status == RuntimeInputStatus::Error && failed.error == "host failure");
    const auto invalidUtf8 = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Ready, std::string(1, '\xff'), {}};
    }, request, control);
    assert(invalidUtf8.status == RuntimeInputStatus::Error && invalidUtf8.text.empty());
    const auto unicode = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Ready, "\xe4\xb8\xad\xf0\x9f\x98\x80", {}};
    }, request, control);
    assert(unicode.status == RuntimeInputStatus::Ready && unicode.text.size() == 7);
    const auto invalidError = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Error, {}, std::string(1, '\xff')};
    }, request, control);
    assert(invalidError.error == "interactive input error is not valid UTF-8");

    RuntimeCancellationToken token;
    RuntimeExecutionControl cancelled({}, token);
    const auto stopped = readRuntimeInput([&](const auto&) {
        token.requestCancellation();
        return RuntimeInputResult{RuntimeInputStatus::Ready, "discard", {}};
    }, request, cancelled);
    assert(stopped.status == RuntimeInputStatus::Stopped && stopped.text.empty());
    assert(cancelled.stopReason() == RuntimeExecutionStopReason::Cancelled);
    bool called = false;
    readRuntimeInput([&](const auto&) {
        called = true;
        return RuntimeInputResult{};
    }, request, cancelled);
    assert(!called);

    RuntimeExecutionLimits limits;
    limits.maxWallTime = std::chrono::milliseconds(15);
    RuntimeExecutionControl timed(limits);
    const auto timeout = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Pending, {}, {}};
    }, request, timed);
    assert(timeout.status == RuntimeInputStatus::Stopped);
    assert(timed.stopReason() == RuntimeExecutionStopReason::WallTimeLimit);
    limits = {};
    limits.maxArrayBytes = 2;
    RuntimeExecutionControl bounded(limits);
    const auto oversized = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{RuntimeInputStatus::Ready, "abc", {}};
    }, request, bounded);
    assert(oversized.status == RuntimeInputStatus::Stopped);
    assert(bounded.stopReason() == RuntimeExecutionStopReason::ArrayByteLimit);
    RuntimeExecutionControl* invocation = nullptr;
    RuntimeExecutionControl inputControl({}, {}, {}, [&](const auto& nestedRequest) {
        const auto reentrant = invocation->readInput(nestedRequest);
        assert(reentrant.status == RuntimeInputStatus::Error);
        return RuntimeInputResult{RuntimeInputStatus::Ready, "shared", {}};
    });
    invocation = &inputControl;
    assert(inputControl.readInput(request).text == "shared");
    assert(inputControl.readInput(request).text == "shared");
    bool firstRead = true;
    RuntimeExecutionControl allocationControl({}, {}, {}, [&](const auto&) {
        if (firstRead) {
            firstRead = false;
            throw std::bad_alloc();
        }
        return RuntimeInputResult{RuntimeInputStatus::Ready, "recovered", {}};
    });
    bool allocationFailed = false;
    try {
        allocationControl.readInput(request);
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    assert(allocationFailed);
    assert(allocationControl.readInput(request).text == "recovered");
    const auto invalidStatus = readRuntimeInput([](const auto&) {
        return RuntimeInputResult{static_cast<RuntimeInputStatus>(999), {}, {}};
    }, request, control);
    assert(invalidStatus.status == RuntimeInputStatus::Error);
    std::cout << "runtime input smoke passed\n";
}
