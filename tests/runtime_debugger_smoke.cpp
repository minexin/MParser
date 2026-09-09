#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/session/runtime_debugger.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
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
        RuntimeDebugger* active = nullptr;
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
                const auto evaluated = active->evaluate(event.sequence, 1, {"x+2", 1});
                require(evaluated.succeeded && evaluated.outputs.size() == 1 &&
                            evaluated.outputs[0].number == 5,
                        "dynamic frame evaluation lost inherited workspace");
                require(active->evaluate(event.sequence, 1,
                            {"x=10; debug_dynamic=42;", 0}).succeeded,
                        "dynamic frame assignment failed");
                require(active->evaluate(event.sequence, 2, {"in=20;", 0}).succeeded,
                        "dynamic callee assignment failed");
                return RuntimeDebugAction::Continue;
            });
        active = debugger.get();
        debugger->setBreakpoints({{"debug.m", 5}});
        std::vector<RuntimeVariable> variables;
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            require(result.diagnostics.empty(), "HIR eval stack debug failed");
            variables = result.variables;
        } else {
            ModuleInvocationRequest request;
            request.executionControl = control(debugger);
            const auto result = module.createSession(session).execute(request);
            require(result.succeeded(), "VM eval stack debug failed");
            variables = result.variables;
        }
        require(callbacks == 1, "eval callback breakpoint not reached");
        size_t matches = 0;
        for (const auto& variable : variables) {
            if (variable.name == "z" && variable.value.number == 24) { ++matches; }
            if (variable.name == "x" && variable.value.number == 10) { ++matches; }
            if (variable.name == "debug_dynamic" && variable.value.number == 42) { ++matches; }
        }
        if (matches != 3) {
            for (const auto& variable : variables) {
                std::cerr << (hir ? "HIR " : "VM ") << variable.name << '='
                          << variable.value.number << '\n';
            }
        }
        require(matches == 3, "dynamic frame edits were lost after eval returned");
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
    RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    auto session = std::make_shared<RuntimeSessionState>(
        std::make_shared<RuntimeSystemContext>(systemOptions));
    const auto created = producer.createSession(session).execute();
    require(created.succeeded() && created.variables.size() == 1,
            "cross-module handle creation failed");
    const auto consumer = compile("out=h(3);\n");
    size_t stops = 0;
    RuntimeDebugger* active = nullptr;
    auto debugger = std::make_shared<RuntimeDebugger>(
        [&](const RuntimeDebugEvent& event) {
            ++stops;
            require(event.frames.size() == 2 &&
                    event.frames[0].sourceName == "debug.m" &&
                    event.frames[1].sourceName == "producer.m" &&
                    event.frames[1].functionName == "increment" &&
                    event.frames[1].variables.at("x").number == 3,
                    "cross-module owner/current frame was lost");
            const auto value = active->evaluate(event.sequence, 1, {"x+1", 1});
            require(value.succeeded && value.outputs.size() == 1 &&
                        value.outputs[0].number == 4,
                    "cross-module frame expression failed");
            require(active->evaluate(event.sequence, 1, {"x=20;", 0}).succeeded,
                    "cross-module callee assignment failed");
            require(active->evaluate(event.sequence, 0, {"debug_marker=42;", 0}).succeeded,
                    "cross-module caller assignment failed");
            return RuntimeDebugAction::Continue;
        });
    active = debugger.get();
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
    size_t matches = 0;
    for (const auto& variable : result.variables) {
        if (variable.name == "out" && variable.value.number == 24) { ++matches; }
        if (variable.name == "debug_marker" && variable.value.number == 42) { ++matches; }
    }
    require(matches == 2, "cross-module edits were lost on resume");
}

void checkNestedSharedLocals() {
    const auto module = compile(
        "result=outer();\nfunction out=outer()\nshared=1;\nout=inner();\n"
        "function value=inner()\nshared=5;\nvalue=leaf();\n"
        "function result=leaf()\nshared=9;\nresult=shared;\nend\nend\nend\n");
    for (bool hir : {false, true}) {
        size_t calls = 0;
        RuntimeDebugger* active = nullptr;
        RuntimeSystemContextOptions systemOptions;
        systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(systemOptions));
        auto debugger = std::make_shared<RuntimeDebugger>(
            [&](const RuntimeDebugEvent& event) {
                ++calls;
                require(event.frames.size() == 4 &&
                        event.frames[1].variables.at("shared").number == 9 &&
                        event.frames[2].variables.at("shared").number == 9 &&
                        event.frames[3].variables.at("shared").number == 9,
                        "nested shared local snapshot is stale");
                const auto failed = active->evaluate(event.sequence, 1,
                    {"shared=25; missing_debug_name;", 0});
                require(!failed.succeeded && !failed.diagnostics.empty(),
                        "expected partial-effect evaluation error");
                for (size_t frame = 1; frame < 4; ++frame) {
                    const auto value = active->evaluate(event.sequence, frame, {"shared", 1});
                    require(value.succeeded && value.outputs.size() == 1 &&
                                value.outputs[0].number == 25,
                            "multi-level capture lost an executed debug assignment");
                }
                return RuntimeDebugAction::Continue;
            });
        active = debugger.get();
        debugger->setBreakpoints({{"debug.m", 10}});
        std::vector<RuntimeVariable> variables;
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            require(result.diagnostics.empty(),
                    "HIR nested shared local debug failed");
            variables = result.variables;
        } else {
            BytecodeVmOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = module.invoke(options);
            require(result.diagnostics.empty(), "VM nested shared local debug failed");
            variables = result.variables;
        }
        require(calls == 1, "nested shared local breakpoint not reached");
        bool matched = false;
        for (const auto& variable : variables) {
            if (variable.name == "result") { matched = variable.value.number == 25; }
        }
        require(matched, "multi-level capture reverted on return");
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
void checkLiveFrameEvaluation(bool hir) {
    auto module = compile(
        "x=2;\n"
        "y=inner(5);\n"
        "z=x+y;\n"
        "function out=inner(in)\n"
        "local=in*3;\n"
        "out=local+in;\n"
        "end\n"
        "function out=helper(in)\n"
        "out=in+1;\n"
        "end\n");
    RuntimeDebugger* active = nullptr;
    size_t pauses = 0;
    auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
        ++pauses;
        const auto evaluate = [&](size_t frame, std::string source, size_t outputs) {
            auto result = active->evaluate(event.sequence, frame, {std::move(source), outputs});
            if (!result.succeeded) {
                for (const auto& diagnostic : result.diagnostics) {
                    std::cerr << diagnostic.identifier << ": " << diagnostic.message << '\n';
                }
            }
            require(result.succeeded, "live debug evaluation failed");
            return result;
        };
        require(event.frames.size() == 2, "unexpected live frame count");
        const auto local = evaluate(1, "local+in", 1);
        require(local.outputs.size() == 1 && local.outputs[0].number == 20,
                "evaluation did not see function locals");
        evaluate(0, "x=10;", 0);
        evaluate(1, "local=helper(19); in=7;", 0);
        const auto output = evaluate(1, "disp(local);", 0);
        require(output.capturedOutput.find("20") != std::string::npos,
                "debug output was not captured");
        const auto invalid = active->evaluate(event.sequence, 1, {"missing_debug_variable", 1});
        require(!invalid.succeeded && !invalid.diagnostics.empty(),
                "invalid expression was silently accepted");
        require(evaluate(1, "local+in", 1).outputs[0].number == 27,
                "evaluation did not recover after an invalid expression");
        return RuntimeDebugAction::Continue;
    });
    active = debugger.get();
    debugger->setBreakpoints({{"debug.m", 6}});
    RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    const auto session = std::make_shared<RuntimeSessionState>(
        std::make_shared<RuntimeSystemContext>(systemOptions));
    std::vector<RuntimeVariable> variables;
    if (hir) {
        InterpreterOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = Interpreter{}.run(module.semantic(), options);
        require(result.diagnostics.empty(), "HIR debug evaluation polluted execution");
        variables = result.variables;
    } else {
        BytecodeVmOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = module.invoke(options);
        require(result.diagnostics.empty(), "VM debug evaluation polluted execution");
        variables = result.variables;
    }
    bool matched = false;
    for (const auto& variable : variables) {
        if (variable.name == "z") { matched = variable.value.number == 37; }
    }
    require(pauses == 1 && matched, "debug assignment did not affect resumed execution");
}

void checkLiveSharedFrameEvaluation(bool hir) {
    auto module = compile(
        "global g; g=1;\n"
        "answer=outer();\n"
        "final_global=g;\n"
        "function y=outer()\n"
        "global g; persistent p;\n"
        "if isempty(p); p=2; end\n"
        "x=3;\n"
        "inner();\n"
        "y=x+g+p;\n"
        "function inner()\n"
        "x=4;\n"
        "marker=x;\n"
        "end\n"
        "end\n");
    RuntimeDebugger* active = nullptr;
    size_t pauses = 0;
    auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
        ++pauses;
        require(event.frames.size() == 3, "nested evaluation frame count mismatch");
        const auto before = active->evaluate(event.sequence, 1, {"x+g+p", 1});
        require(before.succeeded && before.outputs.size() == 1 && before.outputs[0].number == 7,
                "outer evaluation missed the live captured or shared value");
        const auto changed = active->evaluate(event.sequence, 1, {"x=10; g=7; p=11;", 0});
        require(changed.succeeded, "shared debug assignment failed");
        const auto inner = active->evaluate(event.sequence, 2, {"x", 1});
        require(inner.succeeded && inner.outputs[0].number == 10,
                "nested frame did not observe outer captured assignment");
        return RuntimeDebugAction::Continue;
    });
    active = debugger.get();
    debugger->setBreakpoints({{"debug.m", 12}});
    RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    const auto session = std::make_shared<RuntimeSessionState>(
        std::make_shared<RuntimeSystemContext>(systemOptions));
    std::vector<RuntimeVariable> variables;
    if (hir) {
        InterpreterOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = Interpreter{}.run(module.semantic(), options);
        require(result.diagnostics.empty(), "HIR shared debug evaluation failed");
        variables = result.variables;
    } else {
        BytecodeVmOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = module.invoke(options);
        require(result.diagnostics.empty(), "VM shared debug evaluation failed");
        variables = result.variables;
    }
    size_t matches = 0;
    for (const auto& variable : variables) {
        if (variable.name == "answer" && variable.value.number == 28) { ++matches; }
        if (variable.name == "final_global" && variable.value.number == 7) { ++matches; }
    }
    require(pauses == 1 && matches == 2, "shared debug edits were lost after resume");
}

void checkDebugEvaluationControls(bool hir, bool cancel) {
    auto module = compile("x=1;\nx=2;\n");
    RuntimeCancellationToken token;
    RuntimeDebugger* active = nullptr;
    size_t pauses = 0;
    auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
        ++pauses;
        if (cancel) { token.requestCancellation(); }
        const auto result = active->evaluate(event.sequence, 0, {"x=99;", 0});
        require(!result.succeeded, "debug evaluation bypassed execution controls");
        if (!cancel) {
            require(!result.diagnostics.empty() &&
                        result.diagnostics.front().identifier == "MParser:SystemCapabilityDenied",
                    "debug evaluation did not preserve capability denial");
        }
        return RuntimeDebugAction::Continue;
    });
    active = debugger.get();
    debugger->setBreakpoints({{"debug.m", 2}});
    auto execution = std::make_shared<RuntimeExecutionControl>(
        RuntimeExecutionLimits{}, token, debugger);
    RuntimeSystemContextOptions systemOptions;
    if (cancel) { systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation; }
    const auto session = std::make_shared<RuntimeSessionState>(
        std::make_shared<RuntimeSystemContext>(systemOptions));
    std::vector<RuntimeVariable> variables;
    if (hir) {
        InterpreterOptions options;
        options.executionControl = execution;
        options.sessionState = session;
        variables = Interpreter{}.run(module.semantic(), options).variables;
    } else {
        BytecodeVmOptions options;
        options.executionControl = execution;
        options.sessionState = session;
        variables = module.invoke(options).variables;
    }
    require(pauses == 1, "control test did not reach its breakpoint");
    if (cancel) {
        require(execution->stopReason() == RuntimeExecutionStopReason::Cancelled,
                "debug evaluation lost cancellation");
    }
    for (const auto& variable : variables) {
        if (variable.name == "x") {
            require(variable.value.number == (cancel ? 1 : 2),
                    "rejected debug assignment mutated the workspace");
        }
    }
}

void checkConditionalBreakpoints(bool hir) {
    auto module = compile("total=0;\nfor k=1:4\ntotal=total+k;\nend\n");
    for (const std::string condition : {"k==3", "missing_condition_name", "'text'"}) {
        size_t pauses = 0;
        auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
            ++pauses;
            require(event.reason == RuntimeDebugReason::Breakpoint,
                    "conditional breakpoint reported wrong reason");
            if (condition == "k==3") {
                require(event.conditionDiagnostics.empty() &&
                            event.frames.back().variables.at("k").number == 3,
                        "false breakpoint condition paused execution");
            } else {
                require(!event.conditionDiagnostics.empty(),
                        "invalid breakpoint condition silently continued");
            }
            return RuntimeDebugAction::Continue;
        });
        debugger->setBreakpoints({{"debug.m", 3, condition}});
        RuntimeSystemContextOptions systemOptions;
        systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(systemOptions));
        std::vector<RuntimeVariable> variables;
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = Interpreter{}.run(module.semantic(), options);
            require(result.diagnostics.empty(), "HIR conditional breakpoint polluted program");
            variables = result.variables;
        } else {
            BytecodeVmOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            const auto result = module.invoke(options);
            require(result.diagnostics.empty(), "VM conditional breakpoint polluted program");
            variables = result.variables;
        }
        require(pauses == (condition == "k==3" ? 1u : 4u),
                "conditional breakpoint pause count mismatch");
        bool matched = false;
        for (const auto& variable : variables) {
            if (variable.name == "total") { matched = variable.value.number == 10; }
        }
        require(matched, "conditional breakpoint changed execution result");
    }
}

void checkSelectedSourceFrame() {
    RuntimeDebugger* active = nullptr;
    size_t sequence = 0;
    size_t pauses = 0;
    RuntimeDebugger debugger([&](const RuntimeDebugEvent& event) {
        ++pauses;
        sequence = event.sequence;
        require(active->selectedSourceFrame() == 1, "pause did not reset selection to current frame");
        auto evaluate = [&](const std::string& source, size_t frame = RuntimeDebugger::selectedFrame) {
            return active->evaluate(sequence, frame, {source, 1});
        };
        require(evaluate("read").outputs[0].number == 1, "selected current frame was not evaluated");
        require(evaluate("up").succeeded && active->selectedSourceFrame() == 0,
                "caller navigation failed");
        require(evaluate("read").outputs[0].number == 0, "navigation did not affect subsequent evaluation");
        require(evaluate("read", 1).outputs[0].number == 1 && active->selectedSourceFrame() == 0,
                "explicit frame evaluation changed persistent selection");
        require(!evaluate("up").succeeded && active->selectedSourceFrame() == 0,
                "out-of-range navigation corrupted selection");
        auto foreign = std::async(std::launch::async, [&] {
            return active->selectedSourceFrame();
        });
        require(!foreign.get(), "selection leaked to a foreign thread");
        require(evaluate("down").succeeded && active->selectedSourceFrame() == 1,
                "callee navigation failed");
        require(!evaluate("down").succeeded, "navigation exceeded current frame");
        return RuntimeDebugAction::Continue;
    });
    active = &debugger;
    RuntimeDebugScope scope(&debugger, [&](auto& frames) {
        for (size_t index = 0; index < 2; ++index) {
            RuntimeDebugFrame frame;
            frame.bindEvaluator([&, index](const RuntimeDebugEvaluationRequest& request) {
                bool succeeded = true;
                if (request.source == "up") { succeeded = active->moveSourceFrame(true); }
                if (request.source == "down") { succeeded = active->moveSourceFrame(false); }
                return RuntimeDebugEvaluationResult{succeeded, {makeRuntimeNumberValue(static_cast<double>(index))}, {}, {}};
            });
            frames.push_back(std::move(frame));
        }
    });
    for (size_t iteration = 0; iteration < 2; ++iteration) {
        debugger.requestPause();
        require(debugger.statement("selection.m", {}), "selection callback failed");
        require(!debugger.selectedSourceFrame() && !debugger.moveSourceFrame(true),
                "selection survived pause expiration");
    }
    require(pauses == 2, "selection test lost pause");
}

void checkSourceActionArbitration() {
    for (const auto requested : {RuntimeDebugAction::Continue, RuntimeDebugAction::StepInto,
            RuntimeDebugAction::StepOver, RuntimeDebugAction::StepOut, RuntimeDebugAction::Stop}) {
        for (bool hostStops : {false, true}) {
            RuntimeDebugger* active = nullptr;
            size_t pauses = 0;
            RuntimeDebugger debugger([&](const RuntimeDebugEvent& event) {
                ++pauses;
                if (pauses == 1) {
                    require(!active->sourceStack(), "stack accessible outside evaluation");
                    require(!active->requestSourceAction(requested),
                            "accepted source control outside explicit evaluation");
                    auto wrongThread = std::async(std::launch::async, [&] {
                        return active->requestSourceAction(requested);
                    });
                    require(!wrongThread.get(), "accepted cross-thread source control");
                    require(active->evaluate(event.sequence, 0, {"action", 0}).succeeded,
                            "source action evaluation failed");
                }
                return hostStops ? RuntimeDebugAction::Stop : RuntimeDebugAction::Continue;
            });
            active = &debugger;
            RuntimeDebugScope scope(&debugger, [&](auto& frames) {
                RuntimeDebugFrame frame;
                frame.bindEvaluator([&](const RuntimeDebugEvaluationRequest& request) {
                    if (request.source == "condition") {
                        require(!active->sourceStack(), "condition exposed a host paused stack");
                        require(!active->requestSourceAction(requested),
                                "condition evaluation accepted source control");
                        return RuntimeDebugEvaluationResult{true, {makeRuntimeNumberValue(1)}, {}, {}};
                    }
                    require(active->requestSourceAction(requested), "source action rejected");
                    const auto stack = active->sourceStack();
                    require(stack && stack->size() == 1,
                            "explicit evaluation did not preserve paused stack identity");
                    if (requested == RuntimeDebugAction::Stop) {
                        require(active->requestSourceAction(RuntimeDebugAction::Continue),
                                "subsequent source control rejected");
                    }
                    return RuntimeDebugEvaluationResult{true, {}, {}, {}};
                });
                frames.push_back(std::move(frame));
            });
            debugger.setBreakpoints({{"actions.m", 1, "condition"}});
            const bool resumed = debugger.statement("actions.m", {});
            const bool stopped = hostStops || requested == RuntimeDebugAction::Stop;
            require(resumed == !stopped && pauses == 1, "host/source stop arbitration failed");
            require(!debugger.requestSourceAction(requested), "accepted expired source control");
            require(!debugger.sourceStack(), "expired paused stack remained accessible");
            debugger.setBreakpoints({});
            if (!stopped) {
                require(debugger.statement("actions.m", {}), "subsequent statement stopped");
                const size_t expected = requested == RuntimeDebugAction::StepInto ||
                    requested == RuntimeDebugAction::StepOver ? 2 : 1;
                require(pauses == expected, "queued stepping action was not applied");
            }
        }
    }
}

void checkSourceBreakpointCommands(bool hir) {
    auto module = compile(
        "dbstop in debug.m at 7\n"
        "dbstop('in','debug.m','at',7,'if','k==2');\n"
        "s=dbstatus(); assert(numel(s)==1); assert(s.line==7); assert(strcmp(s.expression,'k==2'));\n"
        "total=0;\n"
        "for k=1:3\n"
        "marker=k;\n"
        "total=total+k;\n"
        "end\n"
        "dbclear in debug.m at 7\n"
        "s=dbstatus(); assert(isempty(s));\n"
        "dbstop in debug.m at 99\n"
        "dbstop in other.m at 1\n"
        "dbclear in debug.m\n"
        "s=dbstatus(); assert(numel(s)==1); assert(strcmp(s.file,'other.m'));\n"
        "dbstatus\n"
        "dbclear all\n"
        "assert(isempty(dbstatus())); assert(total==6);\n");
    RuntimeDebugger* active = nullptr;
    size_t pauses = 0;
    auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
        ++pauses;
        require(event.frames.back().variables.at("k").number == 2,
                "source condition did not replace the unconditional breakpoint");
        for (const std::string source : {
                "dbstop('in','debug.m','at',0)",
                "dbstop('in','debug.m','at','99999999999999999')",
                "dbstop('in','debug.m','at',7,'if','')",
                "dbclear('in','debug.m','bad',7)"}) {
            const auto result = active->evaluate(event.sequence, 0, {source, 0});
            require(!result.succeeded, "invalid source debugger command succeeded");
            const auto points = active->breakpoints();
            require(points.size() == 1 && points[0].line == 7 && points[0].condition == "k==2",
                    "invalid source debugger command changed the configured breakpoint");
        }
        const auto status = active->evaluate(event.sequence, 0, {"numel(dbstatus())", 1});
        require(status.succeeded && status.outputs.front().number == 1,
                "source breakpoint query in paused evaluation failed");
        const auto clear = active->evaluate(event.sequence, 0, {"dbclear('in','debug.m','at',7)", 0});
        require(clear.succeeded && active->breakpoints().empty(),
                "source breakpoint removal in paused evaluation failed");
        return RuntimeDebugAction::Continue;
    });
    active = debugger.get();
    RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    auto session = std::make_shared<RuntimeSessionState>(
        std::make_shared<RuntimeSystemContext>(systemOptions));
    if (hir) {
        InterpreterOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = Interpreter{}.run(module.semantic(), options);
        require(result.diagnostics.empty(), "HIR source debugger commands failed");
    } else {
        BytecodeVmOptions options;
        options.executionControl = control(debugger);
        options.sessionState = session;
        const auto result = module.invoke(options);
        require(result.diagnostics.empty(), "VM source debugger commands failed");
    }
    require(pauses == 1 && debugger->breakpoints().empty(),
            "source debugger command configuration did not survive execution");
    const auto unavailable = compile("dbstop in debug.m at 2\nx=1;\n").invoke();
    require(!unavailable.diagnostics.empty() && unavailable.diagnostics.front().identifier ==
                "MParser:Debugger:Unavailable", "unattached source debugger did not report its boundary");
}

void checkSourceResumeCommands(bool hir) {
    for (const std::string command : {"dbcont", "dbstep", "dbstep in", "dbstep out", "dbquit"}) {
        auto module = compile(callSource);
        RuntimeDebugger* active = nullptr;
        std::vector<int> lines;
        auto debugger = std::make_shared<RuntimeDebugger>([&](const RuntimeDebugEvent& event) {
            lines.push_back(event.frames.back().location.begin.line);
            if (lines.size() == 1) {
                const auto stack = active->evaluate(event.sequence, event.frames.size() - 1,
                    {"s=dbstack(); assert(numel(s)==2); assert(strcmp(s(1).name,'inner')); assert(s(1).line==5); assert(s(2).line==2);", 0});
                require(stack.succeeded, "source stack included temporary evaluation frames or wrong order");
                const auto invalid = active->evaluate(event.sequence, event.frames.size() - 1,
                    {"dbstep('invalid')", 0});
                require(!invalid.succeeded, "invalid source step mode succeeded");
            }
            const auto result = active->evaluate(event.sequence, event.frames.size() - 1,
                {lines.size() == 1 ? command : "dbcont", 0});
            require(result.succeeded, "source resume command failed");
            return RuntimeDebugAction::Continue;
        });
        active = debugger.get();
        debugger->setBreakpoints({{"debug.m", 5}});
        RuntimeSystemContextOptions systemOptions;
        systemOptions.capabilities = RuntimeSystemCapability::DynamicEvaluation;
        auto session = std::make_shared<RuntimeSessionState>(
            std::make_shared<RuntimeSystemContext>(systemOptions));
        bool succeeded;
        if (hir) {
            InterpreterOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            succeeded = Interpreter{}.run(module.semantic(), options).diagnostics.empty();
        } else {
            BytecodeVmOptions options;
            options.executionControl = control(debugger);
            options.sessionState = session;
            succeeded = module.invoke(options).diagnostics.empty();
        }
        require(succeeded == (command != "dbquit"), "source stop did not control execution");
        const auto expected = command == "dbstep out" ? std::vector<int>{5, 3}
            : command == "dbstep" || command == "dbstep in" ? std::vector<int>{5, 6}
            : std::vector<int>{5};
        require(lines == expected, "source stepping paused at the wrong statement");
    }
    auto debugger = std::make_shared<RuntimeDebugger>([](const RuntimeDebugEvent&) {
        return RuntimeDebugAction::Continue;
    });
    BytecodeVmOptions options;
    options.executionControl = control(debugger);
    const auto outside = compile("dbcont;\nx=1;\n").invoke(options);
    require(!outside.diagnostics.empty() && outside.diagnostics.front().identifier ==
        "MParser:Debugger:NotPausedEvaluation", "ordinary execution accepted source resume");
}

void checkEvaluationExceptionWriteback() {
    RuntimeSessionState session;
    RuntimeWorkspace borrowed{{"x", makeRuntimeNumberValue(1)},
                              {"removed", makeRuntimeNumberValue(2)}};
    std::deque<RuntimeCallFrame> frames(2);
    frames[0].debugWorkspaceAlias = &borrowed;
    frames[1].workspace = {{"x", makeRuntimeNumberValue(3)},
                           {"removed", makeRuntimeNumberValue(4)}};
    const std::map<std::string, size_t> owners{{"x", 0}, {"removed", 0}};
    frames[1].debugCaptureOwners = &owners;
    bool threw = false;
    try {
        evaluateRuntimeDebugFrame(frames[0], session, frames,
            [&]() -> RuntimeDebugEvaluationResult {
                require(frames[0].workspace.at("x").number == 3,
                        "evaluation did not import the live capture");
                frames[0].workspace["x"] = makeRuntimeNumberValue(42);
                frames[0].workspace["created"] = makeRuntimeNumberValue(7);
                frames[0].workspace.erase("removed");
                throw std::runtime_error("host evaluation exception");
            });
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()) == "host evaluation exception";
    }
    require(threw, "evaluation did not preserve the host exception");
    require(borrowed.at("x").number == 42 && borrowed.contains("created") &&
                !borrowed.contains("removed"),
            "exception lost edits in the borrowed evaluation workspace");
    require(frames[1].workspace.at("x").number == 42 &&
                !frames[1].workspace.contains("removed"),
            "exception lost edits in the live nested capture");
    const auto recovered = evaluateRuntimeDebugFrame(frames[0], session, frames, [&] {
        return RuntimeDebugEvaluationResult{true, {frames[0].workspace.at("x")}, {}, {}};
    });
    require(recovered.succeeded && recovered.outputs.front().number == 42,
            "subsequent evaluation restored values from before the exception");
}

void checkPausedEvaluationLifetime() {
    RuntimeDebugger* active = nullptr;
    size_t sequence = 0;
    size_t pauses = 0;
    size_t evaluations = 0;
    const auto hasError = [](const RuntimeDebugEvaluationResult& result,
                             const std::string& identifier) {
        return !result.succeeded && result.diagnostics.size() == 1 &&
               result.diagnostics.front().identifier == identifier;
    };
    RuntimeDebugger debugger([&](const RuntimeDebugEvent& event) {
        ++pauses;
        sequence = event.sequence;
        require(hasError(active->evaluate(sequence + 1, 0, {}),
                         "MParser:Debugger:StaleEvent"), "accepted stale event");
        require(hasError(active->evaluate(sequence, 2, {}),
                         "MParser:Debugger:InvalidFrame"), "accepted invalid frame");
        require(hasError(active->evaluate(sequence, 1, {}),
                         "MParser:Debugger:EvaluationUnavailable"),
                "accepted frame without evaluator");
        auto foreign = std::async(std::launch::async, [&] {
            return active->evaluate(sequence, 0, {});
        });
        require(hasError(foreign.get(), "MParser:Debugger:WrongThread"),
                "evaluated on the wrong thread");
        bool threw = false;
        try {
            active->evaluate(sequence, 0, {"throw", 0});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "evaluation exception was swallowed");
        const auto recovered = active->evaluate(sequence, 0, {"recover", 1});
        require(recovered.succeeded && recovered.outputs.size() == 1 &&
                    recovered.outputs.front().number == 42,
                "evaluation did not recover after exception");
        return RuntimeDebugAction::Continue;
    });
    active = &debugger;
    RuntimeDebugScope outer(&debugger, [&](std::vector<RuntimeDebugFrame>& frames) {
        RuntimeDebugFrame frame;
        frame.bindEvaluator([&](const RuntimeDebugEvaluationRequest& request) {
            ++evaluations;
            require(hasError(active->evaluate(sequence, 0, {}),
                             "MParser:Debugger:ReentrantEvaluation"),
                    "recursive evaluation was accepted");
            RuntimeDebugScope nested(active, [](auto&) {});
            require(active->statement("debug.m", {}),
                    "evaluation statement was stopped");
            if (request.source == "throw") {
                throw std::runtime_error("evaluation failed");
            }
            RuntimeDebugEvaluationResult result;
            result.succeeded = true;
            result.outputs.push_back(makeRuntimeNumberValue(42));
            return result;
        });
        frames.push_back(std::move(frame));
        frames.emplace_back();
    });
    require(hasError(debugger.evaluate(0, 0, {}), "MParser:Debugger:StaleEvent"),
            "evaluation accepted before pause");
    for (size_t index = 0; index < 2; ++index) {
        debugger.requestPause();
        require(debugger.statement("debug.m", {}), "pause did not resume");
        require(hasError(debugger.evaluate(sequence, 0, {}),
                         "MParser:Debugger:StaleEvent"),
                "evaluation accepted after resume");
    }
    require(pauses == 2 && evaluations == 4,
            "evaluation triggered extra debugger callbacks");
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
        checkPausedEvaluationLifetime();
        checkSourceActionArbitration();
        checkSelectedSourceFrame();
        checkEvaluationExceptionWriteback();
        checkLiveFrameEvaluation(false);
        checkLiveFrameEvaluation(true);
        checkLiveSharedFrameEvaluation(false);
        checkLiveSharedFrameEvaluation(true);
        for (bool hir : {false, true}) {
            checkSourceBreakpointCommands(hir);
            checkSourceResumeCommands(hir);
            checkConditionalBreakpoints(hir);
            checkDebugEvaluationControls(hir, false);
            checkDebugEvaluationControls(hir, true);
        }
        std::cout << "runtime debugger = stepping,frames,locals,loops,stop,threaded,guards\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
