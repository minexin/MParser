#include "mparser/cpp_api.hpp"

#include <array>
#include <iostream>

int main() {
    using namespace mparser::sdk;
    const auto module = Module::compile(
        "x=3;\ny=square(x);\nz=y+1;\n"
        "function out=square(value)\nout=value*value;\nend\n",
        "debug_demo.m");
    if (!module.isValid()) {
        return 1;
    }
    std::size_t pauses = 0;
    SystemContextOptions system;
    system.rootDirectory = ".";
    system.capabilities = SystemCapability::DynamicEvaluation;
    const auto context = SystemContext::rootedNative(system);
    Debugger debugger([&](const DebugEvent& event) {
        ++pauses;
        for (const auto& frame : event.frames) {
            std::cout << frame.functionName << " at " << frame.source.sourceName
                      << ':' << frame.source.begin.line << '\n';
        }
        if (pauses == 2) {
            const auto evaluation = event.evaluate(1, "value=4;", 0);
            if (!evaluation.succeeded()) { return DebugAction::Stop; }
            const auto configuration = event.evaluate(1,
                "s=dbstatus(); assert(numel(s)==1); dbclear all; dbstep out;", 0);
            if (!configuration.succeeded()) { return DebugAction::Stop; }
            return DebugAction::Continue; // The evaluated source queued step-out.
        }
        // A UI host may hand a copied event to its UI, wait for a command here,
        // then return the selected action. This example resumes immediately.
        return pauses == 1 ? DebugAction::StepInto : DebugAction::StepOut;
    });
    debugger.setBreakpoints(std::array{Breakpoint{"debug_demo.m", 2, "x==3"}});
    Invocation invocation;
    invocation.debugger = debugger;
    const auto result = module.execute(invocation, context);
    if (!result.succeeded() || pauses != 3) {
        return 1;
    }
    bool changed = false;
    for (const auto& variable : result.variables()) {
        if (variable.name == "z") { changed = variable.value.numericData()[0] == 17; }
    }
    if (!changed) { return 1; }
    std::cout << "debugger sdk demo = breakpoint,step-into,step-out,3\n";
}
