#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"
#include "mparser/typed_ir.h"
#include "mparser/typed_region_executor.h"

#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Pipeline {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
    mparser::BytecodeVmResult baseline;
    mparser::BytecodeTypedIrModule module;
};

Pipeline prepare(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    mparser::BytecodeVm vm;
    auto baseline = vm.run(bytecode, semantic);
    assert(baseline.diagnostics.empty());

    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    auto module = builder.build(planner.plan(baseline.profile, bytecode));
    return Pipeline{std::move(semantic), std::move(bytecode),
                    std::move(baseline), std::move(module)};
}

const mparser::RuntimeValue* findVariable(
    const std::vector<mparser::RuntimeVariable>& variables,
    std::string_view name) {
    for (const auto& variable : variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const mparser::BytecodeTypedIrRegion* findLoopRegion(
    const mparser::BytecodeTypedIrModule& module,
    std::string_view target) {
    for (const auto& region : module.regions) {
        if (region.kind == "scalar-loop" && region.target == target) {
            return &region;
        }
    }
    return nullptr;
}

const mparser::BytecodeTypedRegionExecutionProfile* findExecution(
    const mparser::BytecodeVmResult& result, std::string_view kind,
    std::string_view target) {
    for (const auto& execution : result.typedRegionExecutions) {
        if (execution.kind == kind && execution.target == target) {
            return &execution;
        }
    }
    return nullptr;
}

bool runtimeValueEqual(const mparser::RuntimeValue& left,
                       const mparser::RuntimeValue& right) {
    return left.kind == right.kind && left.number == right.number &&
           left.text == right.text && left.elements == right.elements &&
           left.rows == right.rows && left.columns == right.columns;
}

void assertVariablesEqual(
    const std::vector<mparser::RuntimeVariable>& expected,
    const std::vector<mparser::RuntimeVariable>& actual) {
    assert(expected.size() == actual.size());
    for (const auto& variable : expected) {
        const auto* candidate = findVariable(actual, variable.name);
        assert(candidate != nullptr);
        assert(runtimeValueEqual(variable.value, *candidate));
    }
}

void runTypedLoopExecutionSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    y = y + i * i;
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);
    assert(optimized.executedInstructionCount <
           pipeline.baseline.executedInstructionCount);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(output->number == 650.0);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->eligible);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->iterationCount == 12);
    assert(execution->executedInstructionCount == 72);
}

void runTypedLoopFallbackSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = [0 0];
for i = 1:12
    y = y + i * i;
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastReason.find("y") != std::string::npos);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Vector);
    assert(output->elements == std::vector<double>({650.0, 650.0}));
}

void runDirectTransactionalFallbackSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    y = y + i;
end
end
)");
    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);

    mparser::RuntimeValue range;
    range.kind = mparser::RuntimeValueKind::Vector;
    range.elements = {1.0, 2.0, 3.0};
    range.rows = 1;
    range.columns = 3;

    mparser::RuntimeValue vectorInput;
    vectorInput.kind = mparser::RuntimeValueKind::Vector;
    vectorInput.elements = {10.0, 20.0};
    vectorInput.rows = 1;
    vectorInput.columns = 2;
    const std::map<std::string, mparser::RuntimeValue> variables{
        {"y", vectorInput}};

    mparser::ScalarTypedRegionExecutor executor;
    const auto result = executor.execute(
        pipeline.bytecode, region->region, range, variables);
    assert(result.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(result.variables.empty());
    assert(variables.at("y").elements ==
           std::vector<double>({10.0, 20.0}));
}

} // namespace

int main() {
    runTypedLoopExecutionSmoke();
    runTypedLoopFallbackSmoke();
    runDirectTransactionalFallbackSmoke();
    std::cout << "typed region executor smoke tests passed\n";
    return 0;
}
