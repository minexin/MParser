#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/session/runtime_debugger.h"
#include "mparser/runtime/io/runtime_system.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mparser;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

CompiledModule compile(const std::string& source) {
    auto module = CompiledModule::compile(
        std::vector<SourceUnit>{{"debug.m", source}});
    require(module.valid(), "debug source did not compile");
    return module;
}

std::shared_ptr<RuntimeExecutionControl> control(
    const std::shared_ptr<RuntimeDebugger>& debugger) {
    return std::make_shared<RuntimeExecutionControl>(
        RuntimeExecutionLimits{}, std::nullopt, debugger);
}

constexpr auto callSource =
    "x = 2;\n"
    "y = inner(x);\n"
    "z = y + 1;\n"
    "function out = inner(in)\n"
    "local = in * 3;\n"
    "out = leaf(local);\n"
    "local = 99;\n"
    "end\n"
    "function out = leaf(in)\n"
    "out = in + 4;\n"
    "end\n";

void checkStepping(bool hir) {
    auto module = compile(callSource);
    std::vector<int> lines;
    std::vector<size_t> depths;
    const std::vector<RuntimeDebugAction> actions{
        RuntimeDebugAction::StepOver, RuntimeDebugAction::StepInto,
        RuntimeDebugAction::StepOver, RuntimeDebugAction::StepInto,
        RuntimeDebugAction::StepOut, RuntimeDebugAction::StepOut,
        RuntimeDebugAction::Continue};
    auto debugger = std::make_shared<RuntimeDebugger>(
        [&](const RuntimeDebugEvent& event) {
            require(!event.frames.empty(), "empty paused stack");
            const auto& current = event.frames.back();
            const auto line = current.location.begin.line;
            lines.push_back(line);
            depths.push_back(event.frames.size());
            require(current.sourceName == "debug.m", "missing source name");
            if (line == 2) {
                require(current.variables.at("x").number == 2,
                        "caller assignment was not visible");
                require(!current.variables.contains("y"),
                        "pause must happen before statement mutation");
            }
            if (line == 10) {
                require(current.variables.at("in").number == 6 &&
                        current.suppliedArgumentCount == 1 &&
                        current.requestedOutputCount == 1,
                        "callee inputs or arity were lost");
                require(event.frames.at(1).variables.at("local").number == 6,
                        "suspended caller locals were lost");
                require(event.frames.front().location.begin.line == 2,
                        "caller call-site location was lost");
            }
            if (line == 3) {
                require(current.variables.at("y").number == 10,
                        "step out did not complete the call");
            }
            require(lines.size() <= actions.size(), "unexpected extra stop");
            return actions.at(lines.size() - 1);
        });
    debugger->requestPause();
    if (hir) {
        InterpreterOptions options;
        options.executionControl = control(debugger);
        const auto result = Interpreter{}.run(module.semantic(), options);
        require(result.diagnostics.empty(), "HIR debug execution failed");
    } else {
        ModuleInvocationRequest request;
        request.executionControl = control(debugger);
        const auto result = module.execute(request);
        require(result.succeeded(), "VM debug execution failed");
    }
    require(lines == std::vector<int>({1, 2, 5, 6, 10, 7, 3}),
            "step sequence differs from language statements");
    require(depths == std::vector<size_t>({1, 1, 2, 2, 3, 2, 1}),
            "step depth does not follow calls");
}

void checkLoopParity() {
    auto module = compile(
        "total=0;\nfor i=1:3\nif i==2\ncontinue;\nend\n"
        "total=total+i;\nend\nwhile total<6\ntotal=total+1;\nend\n"
        "a=1; b=2;\n");
    std::vector<std::pair<int, int>> reference;
    for (bool hir : {false, true}) {
        std::vector<std::pair<int, int>> locations;
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                const auto& point = event.frames.back().location.begin;
                locations.emplace_back(point.line, point.column);
                return RuntimeDebugAction::StepInto;
            });
        debugger->requestPause();
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            require(Interpreter{}.run(module.semantic(), options)
                        .diagnostics.empty(), "HIR loop debug failed");
            require(locations == reference, "HIR and VM stop locations differ");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            require(module.execute(request).succeeded(), "VM loop debug failed");
            reference = locations;
        }
        require(locations.size() >= 15, "loop backedges were not observed");
        require(locations[locations.size()-2].first == 11 &&
                locations.back().first == 11 &&
                locations[locations.size()-2].second != locations.back().second,
                "multiple statements on one line were collapsed");
    }
}

void checkBreakpointsAndSuppression() {
    const auto module = compile("x=0;\nfor i=1:4\nx=x+i;\nend\ny=x;\n");
    for (const auto backend : {ModuleExecutionBackend::Bytecode,
                              ModuleExecutionBackend::Automatic,
                              ModuleExecutionBackend::Portable,
                              ModuleExecutionBackend::Native}) {
        size_t stops = 0;
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                require(event.reason == RuntimeDebugReason::Breakpoint,
                        "unexpected breakpoint reason");
                require(event.frames.back().location.begin.line == 3,
                        "wrong breakpoint line");
                ++stops;
                return RuntimeDebugAction::Continue;
            });
        debugger->setBreakpoints({{"other.m", 1}, {"debug.m", 3}});
        ModuleInvocationRequest request;
        request.backend = backend;
        request.executionControl = control(debugger);
        const auto result = module.execute(request);
        require(result.succeeded() && stops == 4,
                "loop breakpoints lost under an execution backend");
        require(result.execution.nativeCompilationCount == 0 &&
                result.execution.typedRegionExecutionCount == 0,
                "debug execution entered an optimized region");
    }
}

void checkStopAndCallbackFailure() {
    const auto module = compile("a=1;\ntry\na=2;\ncatch\na=3;\nend\na=4;\n");
    for (bool hir : {false, true}) {
        auto debugger = std::make_shared<RuntimeDebugger>(
            [](const RuntimeDebugEvent&) { return RuntimeDebugAction::Stop; });
        debugger->setBreakpoints({{"debug.m", 3}});
        const auto execution = control(debugger);
        if (hir) {
            InterpreterOptions options;
            options.executionControl = execution;
            const auto result = Interpreter{}.run(module.semantic(), options);
            require(!result.diagnostics.empty(), "HIR stop lost its diagnostic");
            require(result.variables.front().value.number == 1,
                    "HIR continued after debugger stop");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = execution;
            const auto result = module.execute(request);
            require(!result.succeeded() &&
                    result.execution.stopReason == RuntimeExecutionStopReason::Cancelled,
                    "debug stop was swallowed by try/catch");
            require(result.variables.front().value.number == 1,
                    "VM continued after debugger stop");
        }
    }
    auto debugger = std::make_shared<RuntimeDebugger>(
        [](const RuntimeDebugEvent&) -> RuntimeDebugAction {
            throw std::runtime_error("host failure");
        });
    debugger->requestPause();
    ModuleInvocationRequest request;
    request.executionControl = control(debugger);
    const auto result = module.execute(request);
    require(!result.succeeded(), "host exception was swallowed");
    bool diagnostic = false;
    for (const auto& item : result.diagnostics) {
        diagnostic = diagnostic || item.identifier == "MParser:DebugCallbackFailed";
    }
    require(diagnostic, "host exception lost debug diagnostic");
}

void checkPauseFromAnotherThread() {
    using namespace std::chrono_literals;
    const auto module = compile("x=1;\ny=x+2;\n");
    std::promise<void> paused;
    auto pauseFuture = paused.get_future();
    std::promise<void> resume;
    const auto resumeFuture = resume.get_future().share();
    auto debugger = std::make_shared<RuntimeDebugger>(
        [&](const RuntimeDebugEvent& event) {
            require(event.frames.back().variables.at("x").number == 1,
                    "paused workspace is incorrect");
            paused.set_value();
            require(resumeFuture.wait_for(5s) == std::future_status::ready,
                    "host did not resume execution");
            return RuntimeDebugAction::Continue;
        });
    debugger->setBreakpoints({{"debug.m", 2}});
    auto execution = std::async(std::launch::async, [&] {
        ModuleInvocationRequest request;
        request.executionControl = control(debugger);
        return module.execute(request);
    });
    const bool didPause = pauseFuture.wait_for(5s) == std::future_status::ready;
    debugger->setBreakpoints({});
    resume.set_value();
    const auto result = execution.get();
    require(didPause && result.succeeded(), "threaded pause/resume failed");
}

void checkStopInsideExpression() {
    for (const std::string expression : {
             "y=inner()+effect();", "[y,z]=inner();",
             "y=[inner() effect()];", "y=sum(inner());",
             "y(inner())=4;", "y=effect(inner());"}) {
        const auto module = compile(
            "global touched; touched=0; y=99; z=88;\ntry\n" +
            expression + "\ncatch\ntouched=100;\nend\n"
            "function [out,other]=inner()\nout=1;\nother=2;\nend\n"
            "function out=effect(varargin)\nglobal touched;\n"
            "touched=touched+1;\nout=2;\nend\n");
        for (bool hir : {false, true}) {
            auto debugger = std::make_shared<RuntimeDebugger>(
                [](const RuntimeDebugEvent&) { return RuntimeDebugAction::Stop; });
            debugger->setBreakpoints({{"debug.m", 8}});
            const auto execution = control(debugger);
            auto verify = [&](const auto& result) {
                require(!result.diagnostics.empty(), "callee stop lost diagnostic");
                size_t preserved = 0;
                for (const auto& variable : result.variables) {
                    if (variable.name == "y" || variable.name == "z" ||
                        variable.name == "touched") {
                        const auto expected = variable.name == "y" ? 99 :
                            variable.name == "z" ? 88 : 0;
                        require(variable.value.kind == RuntimeValueKind::Number &&
                                variable.value.number == expected,
                                std::string(hir ? "HIR" : "VM") +
                                    " mutated caller after callee stop: " + expression);
                        ++preserved;
                    }
                }
                require(preserved == 3, "callee stop discarded caller bindings");
            };
            if (hir) {
                InterpreterOptions options;
                options.executionControl = execution;
                verify(Interpreter{}.run(module.semantic(), options));
            } else {
                ModuleInvocationRequest request;
                request.executionControl = execution;
                verify(module.execute(request));
            }
        }
    }
}

void checkMalformedDebugMetadata() {
    const auto module = compile("a=1;\n");
    auto bytecode = module.bytecode();
    bool changed = false;
    for (auto& instruction : bytecode.instructions) {
        if (instruction.debugStatement) {
            instruction.debugStatement->begin.line = 0;
            changed = true;
            break;
        }
    }
    require(changed && !validateBytecodeProgram(bytecode, &module.semantic()).succeeded,
            "malformed debug source metadata was accepted");
}

void checkDynamicStackOrder() {
    const auto module = compile(
        "x=3;\ny=eval('inner(x)');\nz=y;\n"
        "function out=inner(in)\nout=in+4;\nend\n");
    for (bool hir : {false, true}) {
        RuntimeSystemContextOptions systemOptions;
        systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(std::move(systemOptions)));
        size_t callbacks = 0;
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                ++callbacks;
                require(event.frames.size() == 3, "eval stack lost a frame");
                require(event.frames[0].sourceName == "debug.m" &&
                        event.frames[0].location.begin.line == 2,
                        "eval stack lost the outer call site");
                require(event.frames[1].sourceName != "debug.m" &&
                        event.frames[1].kind == RuntimeCallFrameKind::Script,
                        "eval stack is not in dynamic call order");
                require(event.frames[2].functionName == "inner" &&
                        event.frames[2].variables.at("in").number == 3,
                        "eval callback frame is not current");
                return RuntimeDebugAction::Continue;
            });
        debugger->setBreakpoints({{"debug.m", 5}});
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            require(result.diagnostics.empty(), "HIR eval stack debug failed");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            const auto result = module.createSession(session).execute(request);
            require(result.succeeded(), "VM eval stack debug failed");
        }
        require(callbacks == 1, "eval callback breakpoint not reached");
    }
}

void checkAnonymousCaptureAndSessionBindings() {
    for (bool hir : {false, true}) {
        auto module = compile("factor=4;\nh=@(x) x*factor;\nfactor=99;\ny=h(3);\n");
        size_t calls = 0;
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                const auto& frame = event.frames.back();
                if (frame.kind == RuntimeCallFrameKind::AnonymousFunction) {
                    ++calls;
                    require(event.frames.size() == 2 &&
                            frame.location.begin.line == 2 &&
                            frame.variables.at("x").number == 3 &&
                            frame.variables.at("factor").number == 4 &&
                            event.frames[0].variables.at("factor").number == 99,
                            "anonymous capture or stack was not preserved");
                }
                return RuntimeDebugAction::StepInto;
            });
        debugger->requestPause();
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            require(Interpreter{}.run(module.semantic(), options).diagnostics.empty(),
                    "HIR anonymous debug failed");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            require(module.execute(request).succeeded(), "VM anonymous debug failed");
        }
        require(calls == 1, "anonymous body had no checkpoint");

        module = compile(
            "global g;\ng=1;\ny=outer();\n"
            "function out=outer()\nglobal g;\npersistent p;\n"
            "if isempty(p)\np=7;\nend\ng=2;\nout=inner();\np=p+1;\nend\n"
            "function out=inner()\nglobal g;\ng=9;\nout=g;\nend\n");
        calls = 0;
        debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                ++calls;
                require(event.frames.size() == 3, "session binding stack incomplete");
                for (const auto& frame : event.frames) {
                    require(frame.variables.at("g").number == 9,
                            "suspended frame contains a stale global value");
                }
                require(event.frames[1].variables.at("p").number == 7,
                        "persistent local was not visible");
                return RuntimeDebugAction::Continue;
            });
        debugger->setBreakpoints({{"debug.m", 17}});
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            require(Interpreter{}.run(module.semantic(), options).diagnostics.empty(),
                    "HIR session binding debug failed");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            require(module.execute(request).succeeded(), "VM session binding debug failed");
        }
        require(calls == 1, "global/persistent breakpoint not reached");
    }
}

void checkCrossModuleFrames() {
    const auto producer = CompiledModule::compile(std::vector<SourceUnit>{
        {"producer.m", "h=@increment;\nfunction out=increment(x)\nout=x+4;\nend\n"}});
    require(producer.valid(), "cross-module producer failed to compile");
    auto session = std::make_shared<RuntimeSessionState>();
    const auto created = producer.createSession(session).execute();
    require(created.succeeded() && created.variables.size() == 1,
            "cross-module handle creation failed");
    const auto consumer = compile("out=h(3);\n");
    size_t stops = 0;
    auto debugger = std::make_shared<RuntimeDebugger>(
        [&](const RuntimeDebugEvent& event) {
            ++stops;
            require(event.frames.size() == 2 &&
                    event.frames[0].sourceName == "debug.m" &&
                    event.frames[1].sourceName == "producer.m" &&
                    event.frames[1].functionName == "increment" &&
                    event.frames[1].variables.at("x").number == 3,
                    "cross-module owner/current frame was lost");
            return RuntimeDebugAction::Continue;
        });
    debugger->setBreakpoints({{"producer.m", 3}});
    ModuleInvocationRequest request;
    request.initialWorkspace = created.variables;
    request.executionControl = control(debugger);
    request.externalCallableInvoker = [&](const RuntimeValue& callable,
        const std::vector<RuntimeValue>& arguments, size_t count, SourceSpan,
        RuntimeWorkspace*) {
        return producer.invokeCallable(callable, arguments, count, session,
            request.executionControl, ModuleExecutionBackend::Native, {});
    };
    const auto result = consumer.createSession(session).execute(request);
    require(result.succeeded() && stops == 1, "cross-module debugger execution failed");
}

void checkNestedSharedLocals() {
    const auto module = compile(
        "result=outer();\nfunction out=outer()\nshared=1;\nout=inner();\n"
        "function value=inner()\nshared=5;\nvalue=leaf();\n"
        "function result=leaf()\nshared=9;\nresult=shared;\nend\nend\nend\n");
    for (bool hir : {false, true}) {
        size_t calls = 0;
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                ++calls;
                require(event.frames.size() == 4 &&
                        event.frames[1].variables.at("shared").number == 9 &&
                        event.frames[2].variables.at("shared").number == 9 &&
                        event.frames[3].variables.at("shared").number == 9,
                        "nested shared local snapshot is stale");
                return RuntimeDebugAction::Continue;
            });
        debugger->setBreakpoints({{"debug.m", 10}});
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            require(Interpreter{}.run(module.semantic(), options).diagnostics.empty(),
                    "HIR nested shared local debug failed");
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            require(module.execute(request).succeeded(), "VM nested shared local debug failed");
        }
        require(calls == 1, "nested shared local breakpoint not reached");
    }
}

void checkClassAndDefaultFrames() {
    const auto module = CompiledModule::compile(std::vector<SourceUnit>{
        {"debug.m", "obj=DebugValue();\ny=obj.scale(3);\n"},
        {"DebugValue.m", "classdef DebugValue\nproperties\nValue=4;\nend\n"
            "methods\nfunction out=scale(obj,x)\nout=obj.Value*x;\nend\nend\nend\n"}});
    require(module.valid(), "debug class did not compile");
    std::vector<RuntimeCallFrameKind> kinds;
    auto debugger = std::make_shared<RuntimeDebugger>(
        [&](const RuntimeDebugEvent& event) {
            require(event.frames.size() == 2, "class initialization/method frame missing");
            kinds.push_back(event.frames.back().kind);
            return RuntimeDebugAction::Continue;
        });
    debugger->setBreakpoints({{"DebugValue.m", 3}, {"DebugValue.m", 7}});
    ModuleInvocationRequest request;
    request.executionControl = control(debugger);
    require(module.execute(request).succeeded(), "class debug execution failed");
    require(kinds == std::vector<RuntimeCallFrameKind>{
                RuntimeCallFrameKind::Initializer, RuntimeCallFrameKind::Function},
            "class initializer or method checkpoint not reached");

    const auto defaults = compile(
        "y=withDefault();\nfunction out=withDefault(x)\narguments\n"
        "x=defaultValue()\nend\nout=x;\nend\n"
        "function out=defaultValue()\nout=7;\nend\n");
    for (bool hir : {false, true}) {
        size_t calls = 0;
        debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                ++calls;
                require(event.frames.size() == 3 &&
                        event.frames[1].functionName == "withDefault" &&
                        event.frames[1].location.begin.line == 4,
                        "default argument caller frame was hidden");
                return RuntimeDebugAction::Continue;
            });
        debugger->setBreakpoints({{"debug.m", 9}});
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            require(Interpreter{}.run(defaults.semantic(), options).diagnostics.empty(),
                    "HIR default argument debug failed");
        } else {
            request.executionControl = control(debugger);
            require(defaults.execute(request).succeeded(), "VM default argument debug failed");
        }
        require(calls == 1, "argument default callback breakpoint not reached");
    }
}
} // namespace

int main() {
    try {
        checkStepping(false);
        checkStepping(true);
        checkLoopParity();
        checkBreakpointsAndSuppression();
        checkStopAndCallbackFailure();
        checkStopInsideExpression();
        checkPauseFromAnotherThread();
        checkMalformedDebugMetadata();
        checkDynamicStackOrder();
        checkAnonymousCaptureAndSessionBindings();
        checkCrossModuleFrames();
        checkNestedSharedLocals();
        checkClassAndDefaultFrames();
        std::cout << "runtime debugger = stepping,frames,locals,loops,stop,threaded,guards\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
