#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"
#include "mparser/typed_ir.h"
#include "mparser/typed_region_executor.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef assert
#undef assert
#endif

#define assert(condition)                                                   \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error("requirement failed: " #condition);   \
        }                                                                   \
    } while (false)

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
           left.rows == right.rows && left.columns == right.columns &&
           left.numericClass == right.numericClass;
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
    assert(execution->executedKernelInstructionCount == 24);
    assert(execution->lastReason ==
           "predecoded scalar kernel executed");
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

void runTypedLogicalResultSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
flag = false;
for i = 1:12
    flag = i > 6;
    y = flag + i;
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

    const auto* flag = findVariable(optimized.variables, "flag");
    assert(flag != nullptr);
    assert(flag->kind == mparser::RuntimeValueKind::Number);
    assert(flag->number == 1.0);
    assert(flag->numericClass ==
           mparser::RuntimeNumericClass::Logical);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(output->number == 13.0);
    assert(output->numericClass ==
           mparser::RuntimeNumericClass::Double);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
}

void runTypedPureMathBuiltinSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    y = abs(i) + sin(i);
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);
    assert(region->region.callTargets ==
           std::vector<std::string>({"abs", "sin"}));

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(output->number - (12.0 + std::sin(12.0))) < 1e-12);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->iterationCount == 12);
    assert(execution->executedInstructionCount == 96);
    assert(execution->executedKernelInstructionCount == 36);
    assert(execution->lastReason ==
           "predecoded scalar kernel executed");
}

void runPredecodedOperationCoverageSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    a = +i;
    b = -i;
    c = ~i;
    d = i + 2;
    e = i - 2;
    f = i * 2;
    g = i / 2;
    h = i ^ 2;
    p = i > 2;
    q = i < 2;
    r = i >= 2;
    s = i <= 2;
    t = i == 2;
    u = i ~= 2;
    v = (i > 1) & (i < 4);
    w = (i > 1) | (i < 0);
    x = abs(b) + acos(1) + asin(0) + atan(0) + cos(i) + exp(0) + ...
        log(1) + sin(i) + sqrt(i) + tan(0);
    y = a + b + c + d + e + f + g + h + p + q + r + s + ...
        t + u + v + w + x;
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
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->lastReason ==
           "predecoded scalar kernel executed");
}

void runEmptyLoopPreservesWorkspaceSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 5;
for i = 1:12
    y = y + i;
end
end
)");
    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);

    mparser::RuntimeValue emptyRange;
    emptyRange.kind = mparser::RuntimeValueKind::Vector;
    emptyRange.rows = 1;
    emptyRange.columns = 0;

    mparser::RuntimeValue y;
    y.kind = mparser::RuntimeValueKind::Number;
    y.number = 5.0;
    y.rows = 1;
    y.columns = 1;

    mparser::RuntimeValue i = y;
    i.number = 42.0;
    const std::map<std::string, mparser::RuntimeValue> variables{
        {"i", i}, {"y", y}};

    mparser::ScalarTypedRegionExecutor executor;
    const auto result = executor.execute(
        pipeline.bytecode, region->region, emptyRange, variables);
    assert(result.status ==
           mparser::TypedRegionExecutionStatus::Executed);
    assert(result.iterationCount == 0);
    assert(result.executedInstructionCount == 0);
    assert(result.executedKernelInstructionCount == 0);
    assert(result.variables.at("i").number == 42.0);
    assert(result.variables.at("y").number == 5.0);
    assert(result.reason == "predecoded scalar kernel executed");
}

void runNestedTypedLoopExecutionSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for j = 1:12
    for i = 1:2:5
        y = y + j * i;
    end
end
end
)");

    const auto* outer = findLoopRegion(pipeline.module, "j");
    const auto* inner = findLoopRegion(pipeline.module, "i");
    assert(outer != nullptr);
    assert(inner != nullptr);
    assert(outer->region.nestedLoopCount == 1);
    assert(outer->region.maxLoopDepth == 2);
    assert(outer->region.eligibleForTypedExecution);

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);
    assert(findVariable(optimized.variables, "y")->number == 702.0);

    const auto* outerExecution =
        findExecution(optimized, "scalar-loop", "j");
    const auto* innerExecution =
        findExecution(optimized, "scalar-loop", "i");
    assert(outerExecution != nullptr);
    assert(innerExecution != nullptr);
    assert(outerExecution->attemptCount == 1);
    assert(outerExecution->executionCount == 1);
    assert(outerExecution->fallbackCount == 0);
    assert(outerExecution->iterationCount == 12);
    assert(outerExecution->nestedIterationCount == 36);
    assert(outerExecution->executedInstructionCount == 312);
    assert(outerExecution->executedKernelInstructionCount == 84);
    assert(outerExecution->lastReason ==
           "predecoded nested scalar kernel executed");
    assert(innerExecution->attemptCount == 0);
}

void runThreeLevelTypedLoopSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for a = 1:12
    for b = 1:2
        for c = 1:3
            y = y + a + b + c;
        end
    end
end
end
)");

    const auto* outer = findLoopRegion(pipeline.module, "a");
    assert(outer != nullptr);
    assert(outer->region.nestedLoopCount == 2);
    assert(outer->region.maxLoopDepth == 3);

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);
    assert(findVariable(optimized.variables, "y")->number == 720.0);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "a");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->iterationCount == 12);
    assert(execution->nestedIterationCount == 96);
}

void runEmptyNestedLoopPreservesValuesSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 7;
i = 42;
for j = 1:12
    for i = 5:1
        y = 999;
    end
end
end
)");

    const auto* outer = findLoopRegion(pipeline.module, "j");
    assert(outer != nullptr);
    assert(outer->region.nestedLoopCount == 1);

    mparser::BytecodeVm vm;
    const auto optimized =
        vm.run(pipeline.bytecode, pipeline.semantic, pipeline.module);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);
    assert(findVariable(optimized.variables, "i")->number == 42.0);
    assert(findVariable(optimized.variables, "y")->number == 7.0);
    assert(findVariable(optimized.variables, "j")->number == 12.0);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "j");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->nestedIterationCount == 0);
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
    try {
        runTypedLoopExecutionSmoke();
        runTypedLoopFallbackSmoke();
        runTypedLogicalResultSmoke();
        runTypedPureMathBuiltinSmoke();
        runPredecodedOperationCoverageSmoke();
        runEmptyLoopPreservesWorkspaceSmoke();
        runNestedTypedLoopExecutionSmoke();
        runThreeLevelTypedLoopSmoke();
        runEmptyNestedLoopPreservesValuesSmoke();
        runDirectTransactionalFallbackSmoke();
        std::cout << "typed region executor smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
