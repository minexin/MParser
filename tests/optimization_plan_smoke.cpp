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
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);

    mparser::BytecodeVm vm;
    const auto runtime = vm.run(bytecode, semantic);
    assert(runtime.diagnostics.empty());

    mparser::BytecodeOptimizationPlanner planner;
    return planner.plan(runtime.profile, bytecode);
}

const mparser::BytecodeOptimizationCandidate* findCandidate(
    const mparser::BytecodeOptimizationPlan& plan, std::string_view kind,
    std::string_view target) {
    for (const auto& candidate : plan.candidates) {
        if (candidate.kind == kind && candidate.target == target) {
            return &candidate;
        }
    }
    return nullptr;
}

const mparser::BytecodeOptimizationGuard* findGuard(
    const mparser::BytecodeOptimizationCandidate& candidate,
    std::string_view role) {
    for (const auto& guard : candidate.guards) {
        if (guard.role == role) {
            return &guard;
        }
    }
    return nullptr;
}

void assertScalarNumberGuard(
    const mparser::BytecodeOptimizationCandidate& candidate,
    std::string_view role, size_t observations) {
    const auto* guard = findGuard(candidate, role);
    assert(guard != nullptr);
    assert(guard->kind == "number");
    assert(guard->numericClass == "double");
    assert(guard->rows == 1);
    assert(guard->columns == 1);
    assert(guard->observationCount == observations);
}

void runHotLoopPlanSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + kernel(i);
end

for j = 1:3
    y = y + j;
end
end

function z = kernel(x)
z = x * x + 1;
end
)";

    const auto result = plan(source);
    assert(result.hotLoopThreshold == 10);

    const auto* hotLoop = findCandidate(result, "hot-loop", "i");
    assert(hotLoop != nullptr);
    assert(hotLoop->executionCount == 12);
    assertScalarNumberGuard(*hotLoop, "variable", 12);
    assert(hotLoop->region.available);
    assert(hotLoop->region.closed);
    assert(hotLoop->region.beginPc == 7);
    assert(hotLoop->region.endPc == 15);
    assert(hotLoop->region.bodyBeginPc == 8);
    assert(hotLoop->region.bodyEndPc == 14);
    assert(hotLoop->region.hasCalls);
    assert(!hotLoop->region.eligibleForTypedExecution);

    assert(findCandidate(result, "hot-loop", "j") == nullptr);

    const auto* kernelCall =
        findCandidate(result, "function-site", "kernel");
    assert(kernelCall != nullptr);
    assert(kernelCall->executionCount == 12);
    assertScalarNumberGuard(*kernelCall, "arg0", 12);
    assertScalarNumberGuard(*kernelCall, "result0", 12);

    const auto* loopAssignment =
        findCandidate(result, "name-assignment", "y");
    assert(loopAssignment != nullptr);
    assert(loopAssignment->executionCount == 12);
    assertScalarNumberGuard(*loopAssignment, "value", 12);
    assert(loopAssignment->region.available);
    assert(!loopAssignment->region.closed);
    assert(!loopAssignment->region.eligibleForTypedExecution);
}

void runStaticLoopPlanSmoke() {
    mparser::Lexer lexer(R"(y = 0;
for i = 1:12
    y = y + i;
end
for j = 1:12
    y = kernel(j);
end

function value = kernel(input)
value = input + 1;
end
)");
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());
    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());
    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);

    mparser::BytecodeOptimizationPlanner planner;
    const auto result = planner.planStaticLoops(bytecode);
    const auto* loop = findCandidate(result, "hot-loop", "i");
    assert(loop != nullptr);
    assert(loop->executionCount == 0);
    assert(loop->region.eligibleForTypedExecution);
    assertScalarNumberGuard(*loop, "variable", 0);
    assert(findCandidate(result, "hot-loop", "j") == nullptr);
}

} // namespace

int main() {
    runHotLoopPlanSmoke();
    runStaticLoopPlanSmoke();
    std::cout << "optimization plan smoke tests passed\n";
    return 0;
}
