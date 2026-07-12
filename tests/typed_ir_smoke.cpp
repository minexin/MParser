#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"
#include "mparser/typed_ir.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TypedIrRun {
    mparser::BytecodeTypedIrModule module;
    std::vector<mparser::RuntimeVariable> variables;
};

mparser::RuntimeValue numberValue(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

TypedIrRun typedIr(std::string_view source) {
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
    assert(runtime.typedRegionExecutions.empty());

    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    return TypedIrRun{
        builder.build(planner.plan(runtime.profile, bytecode)),
        runtime.variables};
}

const mparser::BytecodeTypedIrRegion* findRegion(
    const mparser::BytecodeTypedIrModule& module, std::string_view kind,
    std::string_view target) {
    for (const auto& region : module.regions) {
        if (region.kind == kind && region.target == target) {
            return &region;
        }
    }
    return nullptr;
}

bool hasOperation(const mparser::BytecodeTypedIrRegion& region,
                  std::string_view opcode) {
    for (const auto& operation : region.operations) {
        if (operation.opcode == opcode) {
            return true;
        }
    }
    return false;
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

const mparser::BytecodeTypedIrRegionEvaluation* findEvaluation(
    const mparser::BytecodeTypedIrEvaluation& evaluation,
    std::string_view kind, std::string_view target) {
    for (const auto& region : evaluation.regions) {
        if (region.kind == kind && region.target == target) {
            return &region;
        }
    }
    return nullptr;
}

void assertScalarGuard(const mparser::BytecodeTypedIrRegion& region,
                       std::string_view role, size_t observations) {
    for (const auto& guard : region.guards) {
        if (guard.role != role) {
            continue;
        }
        assert(guard.value.kind == "number");
        assert(guard.value.rows == 1);
        assert(guard.value.columns == 1);
        assert(guard.observationCount == observations);
        return;
    }
    assert(false && "missing expected guard");
}

void runTypedIrSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * x + 1;
end
)";

    const auto run = typedIr(source);
    const auto& module = run.module;
    assert(!module.regions.empty());

    const auto* loop = findRegion(module, "scalar-loop", "i");
    assert(loop != nullptr);
    assertScalarGuard(*loop, "variable", 12);
    assert(loop->region.available);
    assert(loop->region.closed);
    assert(loop->region.hasCalls);
    assert(!loop->region.eligibleForTypedExecution);
    assert(!hasOperation(*loop, "specialize-loop"));
    assert(hasOperation(*loop, "reject-region"));
    assert(hasOperation(*loop, "deopt-on-guard-failure"));

    const auto* call = findRegion(module, "scalar-call-site", "kernel");
    assert(call != nullptr);
    assertScalarGuard(*call, "arg0", 12);
    assertScalarGuard(*call, "result0", 12);
    assert(!hasOperation(*call, "specialize-call"));
    assert(hasOperation(*call, "reject-region"));

    const auto* store = findRegion(module, "scalar-assignment", "y");
    assert(store != nullptr);
    assertScalarGuard(*store, "value", 12);
    assert(!hasOperation(*store, "specialize-store"));
    assert(hasOperation(*store, "reject-region"));

    mparser::BytecodeTypedIrGuardEvaluator evaluator;
    const auto evaluation = evaluator.evaluate(module, run.variables);

    const auto* loopCheck = findEvaluation(evaluation, "scalar-loop", "i");
    assert(loopCheck != nullptr);
    assert(loopCheck->checkedCount == 1);
    assert(loopCheck->passedCount == 1);
    assert(loopCheck->failedCount == 0);
    assert(!loopCheck->regionEligible);
    assert(!loopCheck->canEnterTypedPath);

    const auto* callCheck =
        findEvaluation(evaluation, "scalar-call-site", "kernel");
    assert(callCheck != nullptr);
    assert(callCheck->checkedCount == 0);
    assert(callCheck->skippedCount == 2);
    assert(!callCheck->regionEligible);
    assert(!callCheck->canEnterTypedPath);

    const auto* storeCheck =
        findEvaluation(evaluation, "scalar-assignment", "y");
    assert(storeCheck != nullptr);
    assert(storeCheck->checkedCount == 1);
    assert(storeCheck->passedCount == 1);
    assert(!storeCheck->regionEligible);
    assert(!storeCheck->canEnterTypedPath);

    const auto failedEvaluation = evaluator.evaluate(
        module, {mparser::RuntimeVariable{"i", numberValue(12.0)}});
    const auto* missingStoreCheck =
        findEvaluation(failedEvaluation, "scalar-assignment", "y");
    assert(missingStoreCheck != nullptr);
    assert(missingStoreCheck->checkedCount == 0);
    assert(missingStoreCheck->skippedCount == 1);
    assert(!missingStoreCheck->canEnterTypedPath);
}

void runClosedScalarLoopSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + i * i;
end
end
)";

    const auto run = typedIr(source);
    const auto* loop = findRegion(run.module, "scalar-loop", "i");
    assert(loop != nullptr);
    assert(loop->region.available);
    assert(loop->region.closed);
    assert(loop->region.eligibleForTypedExecution);
    assert(loop->region.stackInputCount == 1);
    assert(loop->region.stackOutputCount == 0);
    assert(hasName(loop->region.inputs, "y"));
    assert(!hasName(loop->region.inputs, "i"));
    assert(hasName(loop->region.writes, "i"));
    assert(hasName(loop->region.writes, "y"));
    assert(loop->region.callTargets.empty());
    assert(hasOperation(*loop, "region-contract"));
    assert(hasOperation(*loop, "specialize-loop"));
    assert(!hasOperation(*loop, "reject-region"));

    mparser::BytecodeTypedIrGuardEvaluator evaluator;
    const auto evaluation = evaluator.evaluate(run.module, run.variables);
    const auto* loopCheck =
        findEvaluation(evaluation, "scalar-loop", "i");
    assert(loopCheck != nullptr);
    assert(loopCheck->regionEligible);
    assert(loopCheck->checkedCount == 1);
    assert(loopCheck->passedCount == 1);
    assert(loopCheck->canEnterTypedPath);
}

} // namespace

int main() {
    runTypedIrSmoke();
    runClosedScalarLoopSmoke();
    std::cout << "typed IR smoke tests passed\n";
    return 0;
}
