#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/native_scalar_jit.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/runtime_numeric.h"
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

Pipeline prepare(
    std::string_view source,
    const mparser::BytecodeVmOptions& options = {}) {
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
    auto baseline = vm.run(bytecode, semantic, options);
    assert(baseline.diagnostics.empty());

    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    auto module = builder.build(planner.plan(baseline.profile, bytecode));
    return Pipeline{std::move(semantic), std::move(bytecode),
                    std::move(baseline), std::move(module)};
}

mparser::BytecodeVmResult runTyped(
    const Pipeline& pipeline,
    mparser::TypedRegionBackend backend =
        mparser::TypedRegionBackend::Portable) {
    mparser::BytecodeVmOptions options;
    options.typedRegionBackend = backend;
    mparser::BytecodeVm vm;
    return vm.run(pipeline.bytecode, pipeline.semantic,
                  pipeline.module, options);
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
    if (mparser::isRuntimeNumericValue(left) ||
        mparser::isRuntimeNumericValue(right)) {
        return mparser::runtimeNumericValuesIdentical(left, right);
    }
    return left.kind == right.kind && left.number == right.number &&
           left.text == right.text && left.elements == right.elements &&
           left.rows == right.rows && left.columns == right.columns &&
           left.dimensions == right.dimensions &&
           left.numericClass == right.numericClass;
}

void assertVariablesEqual(
    const std::vector<mparser::RuntimeVariable>& expected,
    const std::vector<mparser::RuntimeVariable>& actual);

void runComplexResultFallbackSmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    y = y + sqrt(-1);
end
end
)");
    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    const auto element =
        mparser::runtimeNumericElementValue(*output, 0);
    assert(element.has_value());
    assert(element->complex);
    assert(std::fabs(element->real) < 1e-12);
    assert(std::fabs(element->imaginary - 12.0) < 1e-12);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastFallbackKind ==
           mparser::RuntimeFallbackKind::RuntimeFailed);
    assert(execution->lastReason.find("complex result") !=
           std::string::npos);
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

mparser::RuntimeValue rowVector(std::vector<double> elements) {
    mparser::RuntimeValue value;
    value.kind = mparser::RuntimeValueKind::Vector;
    value.elements = std::move(elements);
    value.rows = 1;
    value.columns = value.elements.size();
    value.dimensions = {value.rows, value.columns};
    return value;
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

    const auto optimized = runTyped(pipeline);
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
    assert(execution->lastFallbackKind ==
           mparser::RuntimeFallbackKind::None);
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

    const auto optimized = runTyped(pipeline);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastFallbackKind ==
           mparser::RuntimeFallbackKind::KernelRejected);
    assert(execution->lastReason.find("y") != std::string::npos);

    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Vector);
    assert(output->elements == std::vector<double>({650.0, 650.0}));
}

void runMatrixForLoopTypedFallbackSmoke() {
    mparser::BytecodeVmOptions profileOptions;
    profileOptions.initialWorkspace.push_back(
        mparser::RuntimeVariable{
            "range", rowVector({1.0, 2.0, 3.0, 4.0, 5.0, 6.0,
                                7.0, 8.0, 9.0, 10.0, 11.0, 12.0})});
    auto pipeline = prepare(R"(sumFirst = 0;
for col = range
    sumFirst = sumFirst + col(1);
end
)", profileOptions);

    const auto* region = findLoopRegion(pipeline.module, "col");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);

    std::vector<double> elements;
    for (double value = 1.0; value <= 12.0; value += 1.0) {
        elements.push_back(value);
    }
    for (double value = 101.0; value <= 112.0; value += 1.0) {
        elements.push_back(value);
    }

    mparser::BytecodeVmOptions matrixOptions;
    matrixOptions.typedRegionBackend =
        mparser::TypedRegionBackend::Portable;
    matrixOptions.initialWorkspace.push_back(
        mparser::RuntimeVariable{
            "range", mparser::makeRuntimeMatrixValue(
                         2, 12, std::move(elements))});
    mparser::BytecodeVm vm;
    const auto result = vm.run(
        pipeline.bytecode, pipeline.semantic, pipeline.module,
        matrixOptions);

    assert(result.diagnostics.empty());
    const auto* execution =
        findExecution(result, "scalar-loop", "col");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastFallbackKind ==
           mparser::RuntimeFallbackKind::UnsupportedRuntimeValue);
    assert(execution->lastReason.find("scalar column") !=
           std::string::npos);

    const auto* sumFirst = findVariable(result.variables, "sumFirst");
    assert(sumFirst != nullptr);
    assert(sumFirst->kind == mparser::RuntimeValueKind::Number);
    assert(sumFirst->number == 78.0);
    const auto* column = findVariable(result.variables, "col");
    assert(column != nullptr);
    assert(column->kind == mparser::RuntimeValueKind::Matrix);
    assert(column->rows == 2);
    assert(column->columns == 1);
    assert(column->elements == std::vector<double>({12.0, 112.0}));
}

void runLinearArrayTypedLoopSmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function total = main()
x = 1:12;
y = zeros(1, 12);
total = 0;
for i = 1:12
    y(i) = sin(x(i)) + i;
    total = total + y(i);
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);
    assert(region->region.linearIndexReadCount == 2);
    assert(region->region.linearIndexWriteCount == 1);

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);

    const auto* output = findVariable(optimized.variables, "y");
    const auto* total = findVariable(optimized.variables, "total");
    assert(output != nullptr);
    assert(total != nullptr);
    assert(output->elements.size() == 12);
    double expectedTotal = 0.0;
    for (size_t index = 0; index < output->elements.size(); ++index) {
        const double expected =
            std::sin(static_cast<double>(index + 1)) +
            static_cast<double>(index + 1);
        assert(output->elements[index] == expected);
        expectedTotal += expected;
    }
    assert(total->number == expectedTotal);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->iterationCount == 12);
    assert(execution->backend ==
           mparser::typedRegionBackendName(backend));
    assert(execution->lastReason.find("linear-array") !=
           std::string::npos);
    if (backend == mparser::TypedRegionBackend::Native) {
        assert(execution->nativeCompilationCount +
                   execution->nativeCacheHitCount ==
               1);
        assert(execution->nativeCodeSize != 0);
    }
}

void runColumnVectorLinearArraySmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function total = main()
x = (1:12)';
y = zeros(12, 1);
total = 0;
for i = 1:12
    y(i) = x(i) * 3;
    total = total + y(i);
end
end
)");

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);
    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->rows == 12);
    assert(output->columns == 1);
    assert(output->elements.front() == 3.0);
    assert(output->elements.back() == 36.0);
    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
}

void runNestedBranchedLinearArraySmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function y = main()
x = 1:5;
y = zeros(1, 5);
for pass = 1:12
    for i = 1:5
        if i > 2
            y(i) = y(i) + x(i);
        else
            y(i) = y(i) + 2 * x(i);
        end
    end
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "pass");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);
    assert(region->region.nestedLoopCount == 1);
    assert(region->region.conditionalBranchCount == 1);

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);
    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->elements ==
           std::vector<double>({24.0, 48.0, 36.0, 48.0, 60.0}));
    const auto* execution =
        findExecution(optimized, "scalar-loop", "pass");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(execution->iterationCount == 12);
    assert(execution->nestedIterationCount == 60);
    assert(execution->lastReason.find("linear-array") !=
           std::string::npos);
}

void runLinearArrayGrowthFallbackSmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function y = main()
y = zeros(1, 2);
for i = 1:12
    y(i) = i * 2;
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.eligibleForTypedExecution);

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);
    const auto* output = findVariable(optimized.variables, "y");
    assert(output != nullptr);
    assert(output->elements.size() == 12);
    assert(output->elements.front() == 2.0);
    assert(output->elements.back() == 24.0);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastReason.find("preallocated array bounds") !=
           std::string::npos);
}

void runMatrixLinearIndexFallbackSmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function total = main()
A = [1 2 3; 4 5 6];
total = 0;
for i = 1:12
    if i <= 6
        total = total + A(i);
    else
        total = total + A(i - 6);
    end
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.linearIndexReadCount == 2);
    assert(region->region.eligibleForTypedExecution);

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables,
                         optimized.variables);
    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastReason.find("A") != std::string::npos);
}

void runDirectLinearArrayTransactionalFallbackSmoke(
    mparser::TypedRegionBackend backend) {
    auto pipeline = prepare(R"(function y = main()
x = 1:12;
y = zeros(1, 12);
for i = 1:12
    y(i) = x(i) + 1;
end
end
)");
    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);

    const std::map<std::string, mparser::RuntimeValue> variables{
        {"x", rowVector({1.0, 2.0, 3.0})},
        {"y", rowVector({10.0, 20.0, 30.0})}};
    mparser::ScalarTypedRegionExecutor executor;

    const auto boundsFailure = executor.execute(
        pipeline.bytecode, region->region,
        rowVector({1.0, 2.0, 4.0}), variables, backend);
    assert(boundsFailure.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(boundsFailure.fallbackKind ==
           mparser::RuntimeFallbackKind::RuntimeFailed);
    assert(boundsFailure.variables.empty());
    assert(boundsFailure.reason.find("preallocated array bounds") !=
           std::string::npos);
    assert(variables.at("y").elements ==
           std::vector<double>({10.0, 20.0, 30.0}));

    const auto integerFailure = executor.execute(
        pipeline.bytecode, region->region, rowVector({1.5}),
        variables, backend);
    assert(integerFailure.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(integerFailure.fallbackKind ==
           mparser::RuntimeFallbackKind::RuntimeFailed);
    assert(integerFailure.variables.empty());
    assert(integerFailure.reason.find("finite positive integer") !=
           std::string::npos);
    assert(variables.at("y").elements ==
           std::vector<double>({10.0, 20.0, 30.0}));
}

void runNativeLinearArrayDynamicLengthCacheSmoke() {
    if (!mparser::nativeScalarJitAvailable()) {
        return;
    }

    auto pipeline = prepare(R"(function y = main()
x = 1:12;
y = zeros(1, 12);
for i = 1:12
    y(i) = x(i) + 1;
end
end
)");
    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);

    mparser::clearNativeScalarJitCache();
    mparser::resetNativeScalarJitCacheStatistics();
    mparser::ScalarTypedRegionExecutor executor;
    const std::map<std::string, mparser::RuntimeValue> shortVariables{
        {"x", rowVector({1.0, 2.0, 3.0})},
        {"y", rowVector({0.0, 0.0, 0.0})}};
    const auto first = executor.execute(
        pipeline.bytecode, region->region,
        rowVector({1.0, 2.0, 3.0}), shortVariables,
        mparser::TypedRegionBackend::Native);
    assert(first.status ==
           mparser::TypedRegionExecutionStatus::Executed);
    assert(first.nativeCompiled);
    assert(!first.nativeCacheHit);
    assert(first.variables.at("y").elements ==
           std::vector<double>({2.0, 3.0, 4.0}));

    const std::map<std::string, mparser::RuntimeValue> longVariables{
        {"x", rowVector({10.0, 20.0, 30.0, 40.0, 50.0})},
        {"y", rowVector({0.0, 0.0, 0.0, 0.0, 0.0})}};
    const auto second = executor.execute(
        pipeline.bytecode, region->region,
        rowVector({1.0, 2.0, 3.0, 4.0, 5.0}), longVariables,
        mparser::TypedRegionBackend::Native);
    assert(second.status ==
           mparser::TypedRegionExecutionStatus::Executed);
    assert(!second.nativeCompiled);
    assert(second.nativeCacheHit);
    assert(second.variables.at("y").elements ==
           std::vector<double>({11.0, 21.0, 31.0, 41.0, 51.0}));
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

    const auto optimized = runTyped(pipeline);
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

    const auto optimized = runTyped(pipeline);
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

void runPredecodedOperationCoverageSmoke(
    mparser::TypedRegionBackend backend) {
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

    const auto optimized = runTyped(pipeline, backend);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);

    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    if (backend == mparser::TypedRegionBackend::Native) {
        assert(execution->backend == "native");
        assert(execution->nativeCodeSize != 0);
        assert(execution->lastReason.find("SLJIT native") !=
               std::string::npos);
    } else {
        assert(execution->backend == "portable");
        assert(execution->lastReason ==
               "predecoded scalar kernel executed");
    }
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
        pipeline.bytecode, region->region, emptyRange, variables,
        mparser::TypedRegionBackend::Portable);
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

    const auto optimized = runTyped(pipeline);
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

    const auto optimized = runTyped(pipeline);
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

    const auto optimized = runTyped(pipeline);
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

void runStructuredBranchTypedLoopSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
positive = 0;
for j = 1:12
    for i = -4:4
        product = i * j;
        if product > 20
            branchValue = product;
            positive = positive + 1;
        elseif product < -20
            branchValue = -product;
        else
            branchValue = abs(product);
        end
        y = y + branchValue;
    end
end
end
)");

    const auto* outer = findLoopRegion(pipeline.module, "j");
    assert(outer != nullptr);
    assert(outer->region.nestedLoopCount == 1);
    assert(outer->region.conditionalBranchCount == 2);
    assert(outer->region.eligibleForTypedExecution);

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    assert(portable.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, portable.variables);
    assert(findVariable(portable.variables, "y")->number == 1560.0);
    assert(findVariable(portable.variables, "positive")->number == 15.0);

    const auto* portableExecution =
        findExecution(portable, "scalar-loop", "j");
    assert(portableExecution != nullptr);
    assert(portableExecution->backend == "portable");
    assert(portableExecution->executionCount == 1);
    assert(portableExecution->fallbackCount == 0);
    assert(portableExecution->iterationCount == 12);
    assert(portableExecution->nestedIterationCount == 108);

    if (!mparser::nativeScalarJitAvailable()) {
        return;
    }

    const auto native = runTyped(
        pipeline, mparser::TypedRegionBackend::Native);
    const auto cached = runTyped(
        pipeline, mparser::TypedRegionBackend::Native);
    assert(native.diagnostics.empty());
    assert(cached.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, native.variables);
    assertVariablesEqual(pipeline.baseline.variables, cached.variables);

    const auto* nativeExecution =
        findExecution(native, "scalar-loop", "j");
    const auto* cachedExecution =
        findExecution(cached, "scalar-loop", "j");
    assert(nativeExecution != nullptr);
    assert(cachedExecution != nullptr);
    assert(nativeExecution->backend == "native");
    assert(nativeExecution->executionCount == 1);
    assert(nativeExecution->fallbackCount == 0);
    assert(nativeExecution->iterationCount == 12);
    assert(nativeExecution->nestedIterationCount == 108);
    assert(nativeExecution->executedInstructionCount ==
           portableExecution->executedInstructionCount);
    assert(nativeExecution->executedKernelInstructionCount ==
           portableExecution->executedKernelInstructionCount);
    assert(nativeExecution->nativeCompilationCount +
               nativeExecution->nativeCacheHitCount ==
           1);
    assert(nativeExecution->nativeCodeSize != 0);
    assert(cachedExecution->nativeCompilationCount == 0);
    assert(cachedExecution->nativeCacheHitCount == 1);
    assert(cachedExecution->nativeCodeSize ==
           nativeExecution->nativeCodeSize);
}

void runTailBranchTypedLoopSmoke() {
    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    if i > 6
        y = y + i;
    end
end
end
)");

    const auto* region = findLoopRegion(pipeline.module, "i");
    assert(region != nullptr);
    assert(region->region.conditionalBranchCount == 1);
    assert(region->region.eligibleForTypedExecution);

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    assert(portable.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, portable.variables);
    assert(findVariable(portable.variables, "y")->number == 57.0);

    if (!mparser::nativeScalarJitAvailable()) {
        return;
    }

    const auto native = runTyped(
        pipeline, mparser::TypedRegionBackend::Native);
    assert(native.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, native.variables);
    const auto* portableExecution =
        findExecution(portable, "scalar-loop", "i");
    const auto* nativeExecution =
        findExecution(native, "scalar-loop", "i");
    assert(portableExecution != nullptr);
    assert(nativeExecution != nullptr);
    assert(nativeExecution->executionCount == 1);
    assert(nativeExecution->fallbackCount == 0);
    assert(nativeExecution->executedInstructionCount ==
           portableExecution->executedInstructionCount);
    assert(nativeExecution->executedKernelInstructionCount ==
           portableExecution->executedKernelInstructionCount);
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
        pipeline.bytecode, region->region, range, variables,
        mparser::TypedRegionBackend::Portable);
    assert(result.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(result.variables.empty());
    assert(variables.at("y").elements ==
           std::vector<double>({10.0, 20.0}));
}

void runNativeNestedCacheSmoke() {
    if (!mparser::nativeScalarJitAvailable()) {
        return;
    }

    auto pipeline = prepare(R"(function y = main()
y = 1;
for a = 1:12
    for b = 2:-1:0
        y = y + a * b;
    end
end
end
)");

    const auto first = runTyped(
        pipeline, mparser::TypedRegionBackend::Native);
    const auto second = runTyped(
        pipeline, mparser::TypedRegionBackend::Native);
    assert(first.diagnostics.empty());
    assert(second.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, first.variables);
    assertVariablesEqual(pipeline.baseline.variables, second.variables);

    const auto* firstExecution =
        findExecution(first, "scalar-loop", "a");
    const auto* secondExecution =
        findExecution(second, "scalar-loop", "a");
    assert(firstExecution != nullptr);
    assert(secondExecution != nullptr);
    assert(firstExecution->backend == "native");
    assert(firstExecution->nativeCompilationCount +
               firstExecution->nativeCacheHitCount ==
           1);
    assert(firstExecution->nativeCodeSize != 0);
    assert(firstExecution->nativePlatform ==
           mparser::nativeScalarJitPlatform());
    assert(firstExecution->nestedIterationCount == 36);
    assert(secondExecution->backend == "native");
    assert(secondExecution->nativeCompilationCount == 0);
    assert(secondExecution->nativeCacheHitCount == 1);
    assert(secondExecution->nativeCodeSize ==
           firstExecution->nativeCodeSize);
    assert(secondExecution->lastReason.find("cached SLJIT native") !=
           std::string::npos);
}

void runNativeTransactionalRuntimeFallbackSmoke() {
    if (!mparser::nativeScalarJitAvailable()) {
        return;
    }

    auto pipeline = prepare(R"(function y = main()
step = 1;
y = 0;
for j = 1:12
    for i = 1:step:3
        y = y + j * i;
    end
end
end
)");
    const auto* outer = findLoopRegion(pipeline.module, "j");
    assert(outer != nullptr);

    mparser::RuntimeValue range;
    range.kind = mparser::RuntimeValueKind::Vector;
    range.elements = {1.0, 2.0, 3.0};
    range.rows = 1;
    range.columns = 3;

    mparser::RuntimeValue scalar;
    scalar.kind = mparser::RuntimeValueKind::Number;
    scalar.rows = 1;
    scalar.columns = 1;
    const std::map<std::string, mparser::RuntimeValue> variables{
        {"j", mparser::RuntimeValue(scalar)},
        {"step", mparser::RuntimeValue(scalar)},
        {"y", mparser::RuntimeValue(scalar)}};
    auto workingVariables = variables;
    workingVariables.at("j").number = 77.0;
    workingVariables.at("step").number = 0.0;
    workingVariables.at("y").number = 5.0;

    mparser::ScalarTypedRegionExecutor executor;
    const auto result = executor.execute(
        pipeline.bytecode, outer->region, range, workingVariables,
        mparser::TypedRegionBackend::Native);
    assert(result.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(result.variables.empty());
    assert(result.reason.find("step cannot be zero") !=
           std::string::npos);
    assert(workingVariables.at("j").number == 77.0);
    assert(workingVariables.at("step").number == 0.0);
    assert(workingVariables.at("y").number == 5.0);
}

void runDisabledNativeAutoFallbackSmoke() {
    if (mparser::nativeScalarJitAvailable()) {
        return;
    }

    auto pipeline = prepare(R"(function y = main()
y = 0;
for i = 1:12
    y = y + i;
end
end
)");
    const auto optimized = runTyped(
        pipeline, mparser::TypedRegionBackend::Auto);
    assert(optimized.diagnostics.empty());
    assertVariablesEqual(pipeline.baseline.variables, optimized.variables);
    const auto* execution =
        findExecution(optimized, "scalar-loop", "i");
    assert(execution != nullptr);
    assert(execution->backend == "portable");
    assert(execution->lastFallbackKind ==
           mparser::RuntimeFallbackKind::None);
    assert(execution->nativeFallbackKind ==
           mparser::RuntimeFallbackKind::BackendUnavailable);
    assert(execution->nativeFallbackReason.find("disabled") !=
           std::string::npos);
}

} // namespace

int main() {
    try {
        runTypedLoopExecutionSmoke();
        runTypedLoopFallbackSmoke();
        runMatrixForLoopTypedFallbackSmoke();
        runLinearArrayTypedLoopSmoke(
            mparser::TypedRegionBackend::Portable);
        runColumnVectorLinearArraySmoke(
            mparser::TypedRegionBackend::Portable);
        runNestedBranchedLinearArraySmoke(
            mparser::TypedRegionBackend::Portable);
        runLinearArrayGrowthFallbackSmoke(
            mparser::TypedRegionBackend::Portable);
        runMatrixLinearIndexFallbackSmoke(
            mparser::TypedRegionBackend::Portable);
        runDirectLinearArrayTransactionalFallbackSmoke(
            mparser::TypedRegionBackend::Portable);
        if (mparser::nativeScalarJitAvailable()) {
            runLinearArrayTypedLoopSmoke(
                mparser::TypedRegionBackend::Native);
            runColumnVectorLinearArraySmoke(
                mparser::TypedRegionBackend::Native);
            runNestedBranchedLinearArraySmoke(
                mparser::TypedRegionBackend::Native);
            runLinearArrayGrowthFallbackSmoke(
                mparser::TypedRegionBackend::Native);
            runMatrixLinearIndexFallbackSmoke(
                mparser::TypedRegionBackend::Native);
            runDirectLinearArrayTransactionalFallbackSmoke(
                mparser::TypedRegionBackend::Native);
            runNativeLinearArrayDynamicLengthCacheSmoke();
        }
        runTypedLogicalResultSmoke();
        runTypedPureMathBuiltinSmoke();
        runComplexResultFallbackSmoke(
            mparser::TypedRegionBackend::Portable);
        runPredecodedOperationCoverageSmoke(
            mparser::TypedRegionBackend::Portable);
        if (mparser::nativeScalarJitAvailable()) {
            runComplexResultFallbackSmoke(
                mparser::TypedRegionBackend::Native);
            runPredecodedOperationCoverageSmoke(
                mparser::TypedRegionBackend::Native);
        }
        runEmptyLoopPreservesWorkspaceSmoke();
        runNestedTypedLoopExecutionSmoke();
        runThreeLevelTypedLoopSmoke();
        runEmptyNestedLoopPreservesValuesSmoke();
        runStructuredBranchTypedLoopSmoke();
        runTailBranchTypedLoopSmoke();
        runDirectTransactionalFallbackSmoke();
        runNativeNestedCacheSmoke();
        runNativeTransactionalRuntimeFallbackSmoke();
        runDisabledNativeAutoFallbackSmoke();
        std::cout << "typed region executor smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
