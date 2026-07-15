#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::BytecodeOptimizationPlan plan(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    mparser::BytecodeVm vm;
    const auto runtime = vm.run(bytecode, semantic);
    assert(runtime.diagnostics.empty());
    assert(runtime.typedRegionExecutions.empty());

    mparser::BytecodeOptimizationPlanner planner;
    return planner.plan(runtime.profile, bytecode);
}

const mparser::BytecodeOptimizationCandidate* findLoop(
    const mparser::BytecodeOptimizationPlan& plan,
    std::string_view target) {
    for (const auto& candidate : plan.candidates) {
        if (candidate.kind == "hot-loop" && candidate.target == target) {
            return &candidate;
        }
    }
    return nullptr;
}

bool hasName(const std::vector<std::string>& names,
             std::string_view name) {
    for (const auto& candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

void runClosedLoopSmoke() {
    const auto result = plan(R"(function y = main()
y = 0;
for i = 1:12
    y = y + i * i;
end
end
)");

    const auto* loop = findLoop(result, "i");
    assert(loop != nullptr);
    const auto& region = loop->region;
    assert(region.available);
    assert(region.closed);
    assert(region.beginPc < region.bodyBeginPc);
    assert(region.bodyBeginPc < region.bodyEndPc);
    assert(region.bodyEndPc < region.endPc);
    assert(region.stackInputCount == 1);
    assert(region.stackOutputCount == 0);
    assert(hasName(region.reads, "i"));
    assert(hasName(region.reads, "y"));
    assert(hasName(region.inputs, "y"));
    assert(!hasName(region.inputs, "i"));
    assert(hasName(region.outputs, "i"));
    assert(hasName(region.outputs, "y"));
    assert(region.callTargets.empty());
    assert(!region.hasCalls);
    assert(!region.hasMutation);
    assert(!region.hasUnsupportedControlFlow);
    assert(!region.hasUnsupportedOperations);
    assert(region.eligibleForTypedExecution);
}

void runControlFlowRejectionSmoke() {
    const auto result = plan(R"(function y = main()
y = 0;
for i = 1:12
    if i > 3
        y = y + i;
    end
end
end
)");

    const auto* loop = findLoop(result, "i");
    assert(loop != nullptr);
    assert(loop->region.available);
    assert(loop->region.closed);
    assert(loop->region.hasUnsupportedControlFlow);
    assert(!loop->region.eligibleForTypedExecution);
}

void runPureMathCallRegionSmoke() {
    const auto result = plan(R"(function y = main()
y = 0;
for i = 1:12
    y = abs(i) + sin(i);
end
end
)");

    const auto* loop = findLoop(result, "i");
    assert(loop != nullptr);
    assert(loop->region.available);
    assert(loop->region.closed);
    assert(hasName(loop->region.callTargets, "abs"));
    assert(hasName(loop->region.callTargets, "sin"));
    assert(!loop->region.hasCalls);
    assert(!loop->region.hasUnsupportedOperations);
    assert(loop->region.eligibleForTypedExecution);
}

void runGeneralBuiltinCallRejectionSmoke() {
    const auto result = plan(R"(function y = main()
y = 0;
for i = 1:12
    y = sum(i);
end
end
)");

    const auto* loop = findLoop(result, "i");
    assert(loop != nullptr);
    assert(hasName(loop->region.callTargets, "sum"));
    assert(loop->region.hasCalls);
    assert(!loop->region.eligibleForTypedExecution);
}

} // namespace

int main() {
    runClosedLoopSmoke();
    runControlFlowRejectionSmoke();
    runPureMathCallRegionSmoke();
    runGeneralBuiltinCallRejectionSmoke();
    std::cout << "bytecode region smoke tests passed\n";
    return 0;
}
