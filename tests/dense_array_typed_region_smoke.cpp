#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/execution/jit/typed_ir.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct Pipeline {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
    mparser::BytecodeVmResult baseline;
    mparser::BytecodeTypedIrModule typedModule;
    std::vector<mparser::RuntimeVariable> initialWorkspace;
};

Pipeline prepare(std::string_view source,
                 std::vector<mparser::RuntimeVariable> initialWorkspace = {},
                 bool staticPlanning = false) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(!mparser::hasErrorDiagnostics(parsed.diagnostics),
            "dense typed smoke source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    require(!mparser::hasErrorDiagnostics(semantic.diagnostics),
            "dense typed smoke source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    mparser::BytecodeVmOptions baselineOptions;
    baselineOptions.initialWorkspace = initialWorkspace;
    mparser::BytecodeVm vm;
    auto baseline = vm.run(bytecode, semantic, baselineOptions);
    require(!mparser::hasErrorDiagnostics(baseline.diagnostics),
            "dense typed smoke baseline failed");

    mparser::BytecodeOptimizationPlanner planner;
    const auto plan = staticPlanning
                          ? planner.planStaticRegions(
                                bytecode, semantic.builtinRegistry)
                          : planner.plan(
                                baseline.profile, bytecode,
                                semantic.builtinRegistry);
    mparser::BytecodeTypedIrBuilder builder;
    return Pipeline{std::move(semantic), std::move(bytecode),
                    std::move(baseline), builder.build(plan),
                    std::move(initialWorkspace)};
}

mparser::BytecodeVmResult runTyped(
    const Pipeline& pipeline, mparser::TypedRegionBackend backend,
    std::shared_ptr<mparser::RuntimeExecutionControl> executionControl = {}) {
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.initialWorkspace = pipeline.initialWorkspace;
    options.typedRegionBackend = backend;
    options.executionControl = std::move(executionControl);
    mparser::BytecodeVm vm;
    return vm.run(pipeline.bytecode, pipeline.semantic,
                  pipeline.typedModule, options);
}

const mparser::RuntimeValue* findVariable(
    const std::vector<mparser::RuntimeVariable>& variables,
    std::string_view name) {
    const auto found = std::find_if(
        variables.begin(), variables.end(),
        [name](const mparser::RuntimeVariable& variable) {
            return variable.name == name;
        });
    return found == variables.end() ? nullptr : &found->value;
}

const mparser::BytecodeTypedIrRegion* findRegion(
    const mparser::BytecodeTypedIrModule& module,
    std::string_view target) {
    const auto found = std::find_if(
        module.regions.begin(), module.regions.end(),
        [target](const mparser::BytecodeTypedIrRegion& region) {
            return region.target == target;
        });
    return found == module.regions.end() ? nullptr : &*found;
}

mparser::BytecodeTypedIrRegion* findRegion(
    mparser::BytecodeTypedIrModule& module, std::string_view target) {
    const auto found = std::find_if(
        module.regions.begin(), module.regions.end(),
        [target](const mparser::BytecodeTypedIrRegion& region) {
            return region.target == target;
        });
    return found == module.regions.end() ? nullptr : &*found;
}

const mparser::BytecodeTypedRegionExecutionProfile* findExecution(
    const mparser::BytecodeVmResult& result, std::string_view target) {
    const auto found = std::find_if(
        result.typedRegionExecutions.begin(),
        result.typedRegionExecutions.end(),
        [target](const mparser::BytecodeTypedRegionExecutionProfile& region) {
            return region.target == target;
        });
    return found == result.typedRegionExecutions.end() ? nullptr : &*found;
}

bool closeDouble(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) {
        return std::isnan(left) && std::isnan(right);
    }
    if (std::isinf(left) || std::isinf(right)) {
        return left == right;
    }
    return std::fabs(left - right) <=
           1e-11 * (1.0 + std::max(std::fabs(left), std::fabs(right)));
}

void requireNumericMatch(const mparser::RuntimeValue& expected,
                         const mparser::RuntimeValue& actual,
                         std::string_view message) {
    require(mparser::isRuntimeNumericValue(expected) &&
                mparser::isRuntimeNumericValue(actual),
            message);
    require(mparser::runtimeDimensions(expected) ==
                mparser::runtimeDimensions(actual) &&
                expected.numericClass == actual.numericClass &&
                expected.numericComplex == actual.numericComplex,
            message);
    const size_t count = mparser::runtimeShapeElementCount(expected);
    require(count == mparser::runtimeShapeElementCount(actual), message);
    for (size_t index = 0; index < count; ++index) {
        const auto left =
            mparser::runtimeNumericElementValue(expected, index);
        const auto right =
            mparser::runtimeNumericElementValue(actual, index);
        require(left.has_value() && right.has_value() &&
                    left->complex == right->complex &&
                    closeDouble(left->real, right->real) &&
                    closeDouble(left->imaginary, right->imaginary),
                message);
    }
}

void requireVariableMatch(const Pipeline& pipeline,
                          const mparser::BytecodeVmResult& actual,
                          std::string_view name) {
    const auto* expected = findVariable(pipeline.baseline.variables, name);
    const auto* candidate = findVariable(actual.variables, name);
    require(expected != nullptr && candidate != nullptr,
            "dense typed result variable is missing");
    requireNumericMatch(*expected, *candidate, name);
}

mparser::RuntimeValue denseValue(std::vector<size_t> dimensions,
                                 std::vector<double> logicalValues) {
    auto value = mparser::runtimeNumericValueFromLogicalOrder(
        std::move(dimensions), std::move(logicalValues),
        mparser::RuntimeNumericClass::Double);
    require(value.has_value(), "failed to construct dense test value");
    return std::move(*value);
}

void runProfiledFusionAndReductionSmoke() {
    const auto pipeline = prepare(R"(
x = [1 2 3; 4 5 6];
row = [10 20 30];
column = [100; 200];
y = sin(x) .* 2 + row + column;
scaled = 2 * y / 4;
sum_default = sum(scaled);
sum_second = sum(scaled, 2);
sum_all = sum(scaled, "all");
)");

    const auto* yRegion = findRegion(pipeline.typedModule, "y");
    require(yRegion != nullptr && yRegion->kind == "dense-elementwise" &&
                yRegion->region.denseElementwiseOperationCount >= 4 &&
                yRegion->region.hasCalls &&
                yRegion->guards.size() == 3,
            "profiled dense fusion region is incomplete");
    const auto* scaledRegion = findRegion(pipeline.typedModule, "scaled");
    const auto* sumRegion = findRegion(pipeline.typedModule, "sum_all");
    require(scaledRegion != nullptr && !scaledRegion->region.hasCalls &&
                sumRegion != nullptr && sumRegion->region.hasCalls,
            "dense region call metadata is inconsistent");
    require(std::all_of(
                yRegion->guards.begin(), yRegion->guards.end(),
                [](const mparser::BytecodeTypedIrGuard& guard) {
                    return guard.source == "region-input" &&
                           guard.value.shapeKnown;
                }),
            "profiled dense guards did not retain exact shapes");

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(portable.diagnostics),
            "portable dense fusion failed");
    for (std::string_view name : {"y", "scaled", "sum_default",
                                  "sum_second", "sum_all"}) {
        requireVariableMatch(pipeline, portable, name);
        const auto* execution = findExecution(portable, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0 &&
                    execution->backend == "portable",
                "portable dense region did not execute");
    }

    const auto automatic = runTyped(
        pipeline, mparser::TypedRegionBackend::Auto);
    requireVariableMatch(pipeline, automatic, "y");
    const auto* autoY = findExecution(automatic, "y");
    require(autoY != nullptr && autoY->executionCount == 1 &&
                autoY->fallbackCount == 0 &&
                autoY->backend == "portable" &&
                autoY->nativeFallbackKind ==
                    mparser::RuntimeFallbackKind::BackendUnsupported &&
                (!mparser::nativeScalarJitAvailable() ||
                 !autoY->nativePlatform.empty()),
            "auto dense broadcast did not fall back to portable execution");
    for (std::string_view name : {"sum_default", "sum_second"}) {
        requireVariableMatch(pipeline, automatic, name);
        const auto* execution = findExecution(automatic, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0 &&
                    execution->backend == "portable" &&
                    execution->nativeFallbackKind ==
                        mparser::RuntimeFallbackKind::BackendUnsupported,
                "auto multi-output reduction did not use portable lowering");
    }

    if (mparser::nativeScalarJitAvailable()) {
        const auto native = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        requireVariableMatch(pipeline, native, "y");
        const auto* nativeY = findExecution(native, "y");
        require(nativeY != nullptr && nativeY->executionCount == 0 &&
                    nativeY->fallbackCount == 1 &&
                    nativeY->lastFallbackKind ==
                        mparser::RuntimeFallbackKind::BackendUnsupported,
                "explicit native broadcast did not return to the VM");
    }
}

void runProductAndMeanReductionSmoke() {
    const auto pipeline = prepare(R"(
x = [1 2 3; 4 5 6];
product_default = prod(x);
product_second = prod(x, 2);
product_all = prod(x, "all");
mean_default = mean(x);
mean_second = mean(x, 2);
mean_all = mean(x, "all");
nested_mean = mean(sin(x), "all");
empty = zeros(0, 2);
empty_product = prod(empty, "all");
empty_mean = mean(empty, "all");
)");

    for (std::string_view name : {"product_default", "product_second",
                                  "product_all", "mean_default",
                                  "mean_second", "mean_all", "nested_mean",
                                  "empty_product", "empty_mean"}) {
        const auto* region = findRegion(pipeline.typedModule, name);
        require(region != nullptr &&
                    region->region.reductionOperationCount == 1,
                "product/mean reduction region was not planned");
    }
    const auto* productRegion =
        findRegion(pipeline.typedModule, "product_all");
    const auto* meanRegion = findRegion(pipeline.typedModule, "mean_all");
    const auto* nestedMeanRegion =
        findRegion(pipeline.typedModule, "nested_mean");
    require(productRegion != nullptr && meanRegion != nullptr &&
                nestedMeanRegion != nullptr,
            "product/mean typed regions are missing");
    require(std::any_of(
                productRegion->operations.begin(),
                productRegion->operations.end(),
                [](const mparser::BytecodeTypedIrOperation& operation) {
                    return operation.opcode == "lower-reduction" &&
                           operation.operand.find("kind=prod") !=
                               std::string::npos;
                }),
            "Typed IR did not retain the product reduction identity");
    require(std::any_of(
                meanRegion->operations.begin(), meanRegion->operations.end(),
                [](const mparser::BytecodeTypedIrOperation& operation) {
                    return operation.opcode == "lower-reduction" &&
                           operation.operand.find("kind=mean") !=
                               std::string::npos;
                }),
            "Typed IR did not retain the mean reduction identity");
    require(std::any_of(
                nestedMeanRegion->operations.begin(),
                nestedMeanRegion->operations.end(),
                [](const mparser::BytecodeTypedIrOperation& operation) {
                    return operation.opcode == "lower-reduction" &&
                           operation.operand.find("kind=mean") !=
                               std::string::npos;
                }),
            "Typed IR lost the reduction identity with a nested builtin");

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
        require(!mparser::hasErrorDiagnostics(portable.diagnostics),
            "portable product/mean reduction failed");
    for (std::string_view name : {"product_default", "product_second",
                                  "product_all", "mean_default",
                                  "mean_second", "mean_all", "nested_mean",
                                  "empty_product", "empty_mean"}) {
        requireVariableMatch(pipeline, portable, name);
        const auto* execution = findExecution(portable, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0 &&
                    execution->backend == "portable",
                "portable product/mean reduction did not execute");
    }

    const auto* productAll =
        findVariable(portable.variables, "product_all");
    const auto* meanAll = findVariable(portable.variables, "mean_all");
    require(productAll && meanAll,
            "portable product/mean scalar outputs are missing");
    const auto productValue =
        mparser::runtimeNumericElementValue(*productAll, 0);
    const auto meanValue = mparser::runtimeNumericElementValue(*meanAll, 0);
    require(productValue && closeDouble(productValue->real, 720.0) &&
                meanValue && closeDouble(meanValue->real, 3.5),
            "portable product/mean scalar values are incorrect");

    if (mparser::nativeScalarJitAvailable()) {
        const auto native = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        require(!mparser::hasErrorDiagnostics(native.diagnostics),
                "native product/mean reduction failed");
        for (std::string_view name : {"product_all", "mean_all", "nested_mean",
                                      "empty_product", "empty_mean"}) {
            requireVariableMatch(pipeline, native, name);
            const auto* execution = findExecution(native, name);
            require(execution != nullptr &&
                        execution->executionCount == 1 &&
                        execution->fallbackCount == 0 &&
                        execution->backend == "native",
                    "native scalar product/mean reduction did not execute");
        }
        for (std::string_view name : {"product_default", "product_second",
                                      "mean_default", "mean_second"}) {
            const auto* execution = findExecution(native, name);
            require(execution != nullptr &&
                        execution->executionCount == 0 &&
                        execution->fallbackCount == 1 &&
                        execution->lastFallbackKind ==
                            mparser::RuntimeFallbackKind::BackendUnsupported,
                    "native array-valued product/mean did not fall back");
        }
    }
}

void runStaticNativeAndTamperSmoke() {
    const auto pipeline = prepare(R"(
x = linspace(-1, 1, 64);
y = linspace(1, 2, 64);
b = sqrt(abs(x) + 1);
z = sin(x) .* cos(y) + b;
total = sum(z .* z, "all");
)", {}, true);

    for (std::string_view name : {"b", "z", "total"}) {
        const auto* region = findRegion(pipeline.typedModule, name);
        require(region != nullptr &&
                    region->region.eligibleForTypedExecution,
                "static dense planning missed an array expression");
        require(std::all_of(
                    region->guards.begin(), region->guards.end(),
                    [](const mparser::BytecodeTypedIrGuard& guard) {
                        return !guard.value.shapeKnown;
                    }),
                "static dense planning unexpectedly froze a shape");
    }

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    for (std::string_view name : {"b", "z", "total"}) {
        requireVariableMatch(pipeline, portable, name);
    }

    if (mparser::nativeScalarJitAvailable()) {
        const auto native = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        for (std::string_view name : {"b", "z", "total"}) {
            requireVariableMatch(pipeline, native, name);
            const auto* execution = findExecution(native, name);
            require(execution != nullptr && execution->executionCount == 1 &&
                        execution->fallbackCount == 0 &&
                        execution->backend == "native" &&
                        execution->nativeCodeSize != 0,
                    "exact-shape native dense region did not execute");
        }
    }

    auto tamperedModule = pipeline.typedModule;
    auto* tampered = findRegion(tamperedModule, "z");
    require(tampered != nullptr, "missing dense region to tamper");
    ++tampered->region.beginPc;
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.typedRegionBackend = mparser::TypedRegionBackend::Portable;
    mparser::BytecodeVm vm;
    const auto rejected = vm.run(
        pipeline.bytecode, pipeline.semantic, tamperedModule, options);
    requireVariableMatch(pipeline, rejected, "z");
    const auto* execution = findExecution(rejected, "z");
    require(execution != nullptr && !execution->eligible &&
                execution->attemptCount == 0 &&
                execution->lastFallbackKind ==
                    mparser::RuntimeFallbackKind::InvalidContract,
            "tampered dense contract was not rejected");

    auto guardTamperedModule = pipeline.typedModule;
    auto* guardTampered = findRegion(guardTamperedModule, "z");
    require(guardTampered != nullptr,
            "missing dense region guard to tamper");
    guardTampered->guards.clear();
    const auto guardRejected = vm.run(
        pipeline.bytecode, pipeline.semantic, guardTamperedModule,
        options);
    requireVariableMatch(pipeline, guardRejected, "z");
    execution = findExecution(guardRejected, "z");
    require(execution != nullptr && !execution->eligible &&
                execution->attemptCount == 0 &&
                execution->lastFallbackKind ==
                    mparser::RuntimeFallbackKind::InvalidContract,
            "tampered dense guards were not rejected");
}

void runNdImplicitExpansionSmoke() {
    std::vector<mparser::RuntimeVariable> workspace;
    workspace.push_back(mparser::RuntimeVariable{
        "left_seed", denseValue({2, 1, 3}, {1, 2, 3, 4, 5, 6})});
    workspace.push_back(mparser::RuntimeVariable{
        "right_seed", denseValue({1, 4}, {10, 20, 30, 40})});
    const auto pipeline = prepare(R"(
left = left_seed;
right = right_seed;
out = left .* right + 1;
sum_first = sum(out, 1);
sum_second = sum(out, 2);
sum_third = sum(out, 3);
sum_all = sum(out, "all");
)", std::move(workspace));

    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(portable.diagnostics),
            "N-D portable dense execution failed");
    for (std::string_view name : {"out", "sum_first", "sum_second",
                                  "sum_third", "sum_all"}) {
        requireVariableMatch(pipeline, portable, name);
        const auto* execution = findExecution(portable, name);
        require(execution != nullptr && execution->executionCount == 1,
                "N-D portable dense region did not execute");
    }
    const auto* output = findVariable(portable.variables, "out");
    require(output != nullptr &&
                mparser::runtimeDimensions(*output) ==
                    std::vector<size_t>({2, 4, 3}),
            "N-D implicit expansion shape mismatch");

    const auto automatic = runTyped(
        pipeline, mparser::TypedRegionBackend::Auto);
    for (std::string_view name : {"out", "sum_first", "sum_second",
                                  "sum_third", "sum_all"}) {
        requireVariableMatch(pipeline, automatic, name);
    }

    if (mparser::nativeScalarJitAvailable()) {
        const auto native = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        for (std::string_view name : {"out", "sum_first", "sum_second",
                                      "sum_third", "sum_all"}) {
            requireVariableMatch(pipeline, native, name);
        }
        const auto* allExecution = findExecution(native, "sum_all");
        require(allExecution != nullptr &&
                    allExecution->executionCount == 1 &&
                    allExecution->backend == "native",
                "N-D scalar reduction did not use native lowering");
    }
}

void runComplexPortableSmoke() {
    const auto pipeline = prepare(R"(
x = [-1 4];
y = [99 99];
y = sqrt(x);
checksum = sum(abs(y), "all");
)");
    const auto typed = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(typed.diagnostics),
            "complex fallback did not return to the VM");
    requireVariableMatch(pipeline, typed, "y");
    requireVariableMatch(pipeline, typed, "checksum");
    const auto* y = findVariable(typed.variables, "y");
    require(y != nullptr && y->numericComplex,
            "portable complex result was not committed");
    const auto* execution = findExecution(typed, "y");
    require(execution != nullptr && execution->executionCount == 1 &&
                execution->fallbackCount == 0 &&
                execution->backend == "portable",
            "complex dense domain did not execute in the portable kernel");

    if (mparser::nativeScalarJitAvailable()) {
        const auto automatic = runTyped(
            pipeline, mparser::TypedRegionBackend::Auto);
        requireVariableMatch(pipeline, automatic, "y");
        const auto* autoExecution = findExecution(automatic, "y");
        require(autoExecution != nullptr &&
                    autoExecution->executionCount == 1 &&
                    autoExecution->fallbackCount == 0 &&
                    autoExecution->backend == "portable" &&
                    autoExecution->nativeFallbackKind ==
                        mparser::RuntimeFallbackKind::BackendUnsupported &&
                    autoExecution->nativeCodeSize == 0 &&
                    autoExecution->nativeCompilationCount == 0 &&
                    autoExecution->nativeCacheHitCount == 0 &&
                    autoExecution->nativeFallbackReason.find("complex") !=
                        std::string::npos &&
                    !autoExecution->nativePlatform.empty(),
                std::string("auto complex input did not fall back to portable Typed: ") +
                    (autoExecution == nullptr
                         ? "missing"
                         : "backend=" + autoExecution->backend +
                               " executions=" +
                               std::to_string(autoExecution->executionCount) +
                               " fallbacks=" +
                               std::to_string(autoExecution->fallbackCount) +
                               " nativeKind=" +
                               std::string(mparser::runtimeFallbackKindName(
                                   autoExecution->nativeFallbackKind)) +
                               " nativeReason=" +
                               autoExecution->nativeFallbackReason));

        const auto explicitNative = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        requireVariableMatch(pipeline, explicitNative, "y");
        const auto* nativeExecution = findExecution(explicitNative, "y");
        require(nativeExecution != nullptr &&
                    nativeExecution->executionCount == 0 &&
                    nativeExecution->fallbackCount == 1 &&
                    nativeExecution->lastFallbackKind ==
                        mparser::RuntimeFallbackKind::BackendUnsupported &&
                    nativeExecution->nativeCodeSize == 0,
                "explicit native complex input did not return to the VM");
    }
}

void runShadowedBuiltinFallbackSmoke() {
    std::vector<mparser::RuntimeVariable> workspace;
    workspace.push_back(mparser::RuntimeVariable{
        "sum", denseValue({1, 3}, {10, 20, 30})});
    const auto pipeline = prepare(R"(
x = [1 2];
out = sum(x);
)", std::move(workspace));
    const auto typed = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(typed.diagnostics),
            "shadowed builtin fallback failed");
    requireVariableMatch(pipeline, typed, "out");
    const auto* execution = findExecution(typed, "out");
    require(execution != nullptr && execution->executionCount == 0 &&
                execution->fallbackCount == 1 &&
                execution->lastFallbackKind ==
                    mparser::RuntimeFallbackKind::UnsupportedInput &&
                execution->lastReason.find("shadowed") !=
                    std::string::npos,
            "shadowed sum was incorrectly lowered as a reduction");
}

void runProfileShapeGuardFallbackSmoke() {
    std::vector<mparser::RuntimeVariable> workspace;
    workspace.push_back(mparser::RuntimeVariable{
        "seed_data", denseValue({1, 3}, {1, 2, 3})});
    const auto pipeline = prepare(R"(
source_data = seed_data;
out = source_data .* 2;
)", std::move(workspace));

    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.typedRegionBackend = mparser::TypedRegionBackend::Portable;
    options.initialWorkspace.push_back(mparser::RuntimeVariable{
        "seed_data", denseValue({1, 4}, {1, 2, 3, 4})});
    mparser::BytecodeVm vm;
    const auto guarded = vm.run(
        pipeline.bytecode, pipeline.semantic, pipeline.typedModule,
        options);
    require(!mparser::hasErrorDiagnostics(guarded.diagnostics),
            "profile shape guard fallback failed");
    const auto* output = findVariable(guarded.variables, "out");
    const auto expected = denseValue({1, 4}, {2, 4, 6, 8});
    require(output != nullptr, "shape guard fallback output is missing");
    requireNumericMatch(expected, *output,
                        "shape guard fallback output mismatch");
    const auto* execution = findExecution(guarded, "out");
    require(execution != nullptr,
            "profile shape guard region is missing");
    require(
        execution->executionCount == 0 && execution->fallbackCount == 1 &&
            execution->lastFallbackKind ==
                mparser::RuntimeFallbackKind::UnsupportedInput,
        std::string("profile shape change did not return to the VM: ") +
            "executions=" + std::to_string(execution->executionCount) +
            ", fallbacks=" + std::to_string(execution->fallbackCount) +
            ", kind=" + std::string(mparser::runtimeFallbackKindName(
                              execution->lastFallbackKind)));
}

void runNonDoubleFallbackSmoke() {
    const auto pipeline = prepare(R"(
input = single([1 2 3]);
out = input .* 2;
total = sum(out, "all");
)", {}, true);
    const auto typed = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(typed.diagnostics),
            "single dense portable execution failed");
    for (std::string_view name : {"out", "total"}) {
        requireVariableMatch(pipeline, typed, name);
        const auto* execution = findExecution(typed, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0 &&
                    execution->backend == "portable",
                std::string("single dense region did not execute in the portable kernel: ") +
                    std::string(name) +
                    (execution == nullptr
                         ? " missing"
                         : " backend=" + execution->backend +
                               " executions=" +
                               std::to_string(execution->executionCount) +
                               " fallbacks=" +
                               std::to_string(execution->fallbackCount) +
                               " reason=" + execution->lastReason));
    }
    const auto* output = findVariable(typed.variables, "out");
    require(output != nullptr &&
                output->numericClass == mparser::RuntimeNumericClass::Single,
            "single dense portable execution changed numeric class");
    const auto* total = findVariable(typed.variables, "total");
    require(total != nullptr &&
                total->numericClass == mparser::RuntimeNumericClass::Single,
            "single dense reduction changed numeric class");
}

void runProfiledNumericTypeGuardSmoke() {
    const auto singlePipeline = prepare(R"(
input = single([1 2 3]);
out = input .* 2;
)");
    const auto* singleRegion = findRegion(
        singlePipeline.typedModule, "out");
    require(singleRegion != nullptr && singleRegion->guards.size() == 1 &&
                singleRegion->guards.front().value.numericClass == "single" &&
                singleRegion->guards.front().value.numericClassKnown &&
                singleRegion->guards.front().value.complexKnown &&
                !singleRegion->guards.front().value.numericComplex,
            "profiled single dense guard lost its type contract");
    const auto singleTyped = runTyped(
        singlePipeline, mparser::TypedRegionBackend::Portable);
    requireVariableMatch(singlePipeline, singleTyped, "out");
    const auto* singleExecution = findExecution(singleTyped, "out");
    require(singleExecution != nullptr &&
                singleExecution->executionCount == 1 &&
                singleExecution->fallbackCount == 0 &&
                singleExecution->backend == "portable",
            "profiled single dense region did not execute portably");

    const auto complexPipeline = prepare(R"(
input = [1 + 2i, 3 - 4i];
out = input .* 2;
)");
    const auto* complexRegion = findRegion(
        complexPipeline.typedModule, "out");
    require(complexRegion != nullptr && complexRegion->guards.size() == 1 &&
                complexRegion->guards.front().value.numericClass == "double" &&
                complexRegion->guards.front().value.numericClassKnown &&
                complexRegion->guards.front().value.complexKnown &&
                complexRegion->guards.front().value.numericComplex,
            "profiled complex dense guard lost its complexity contract");
    const auto complexTyped = runTyped(
        complexPipeline, mparser::TypedRegionBackend::Portable);
    requireVariableMatch(complexPipeline, complexTyped, "out");
    const auto* complexExecution = findExecution(complexTyped, "out");
    require(complexExecution != nullptr &&
                complexExecution->executionCount == 1 &&
                complexExecution->fallbackCount == 0 &&
                complexExecution->backend == "portable",
            "profiled complex dense region did not execute portably");
}

void runComplexInputPortableSmoke() {
    const auto pipeline = prepare(R"(
input = [1 + 2i, 3 - 4i];
out = input .* (2 - i) + 1i;
total = sum(out, "all");
)", {}, true);
    const auto typed = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(typed.diagnostics),
            "complex input portable execution failed");
    for (std::string_view name : {"out", "total"}) {
        requireVariableMatch(pipeline, typed, name);
        const auto* execution = findExecution(typed, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0 &&
                    execution->backend == "portable",
                std::string("complex input region did not execute portably: ") +
                    std::string(name) +
                    (execution == nullptr
                         ? " missing"
                         : " backend=" + execution->backend +
                               " executions=" +
                               std::to_string(execution->executionCount) +
                               " fallbacks=" +
                               std::to_string(execution->fallbackCount) +
                               " reason=" + execution->lastReason));
    }
    const auto* output = findVariable(typed.variables, "out");
    require(output != nullptr && output->numericComplex,
            "complex input result lost its imaginary channel");
}

void runMatrixOperatorFallbackSmoke() {
    const auto pipeline = prepare(R"(
left = [1 2; 3 4];
right = [5 6; 7 8];
out = left * right;
)");
    const auto typed = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(typed.diagnostics),
            "matrix multiply fallback failed");
    requireVariableMatch(pipeline, typed, "out");
    const auto* execution = findExecution(typed, "out");
    require(execution != nullptr && execution->executionCount == 0 &&
                execution->fallbackCount == 1 &&
                execution->lastFallbackKind ==
                    mparser::RuntimeFallbackKind::UnsupportedInput,
            "matrix multiply was incorrectly lowered element-wise");
}

void runEmptyAndAliasedInputSmoke() {
    const auto pipeline = prepare(R"(
empty = zeros(0, 3);
empty_sum = sum(empty, 1);
zero_by_zero = zeros(0, 0);
default_empty = sum(zero_by_zero);
explicit_empty = sum(zero_by_zero, 1);
all_empty = sum(zero_by_zero, "all");
x = [1 2 3];
x = x .* 2 + 1;
checksum = sum(x, "all");
)");
    const auto portable = runTyped(
        pipeline, mparser::TypedRegionBackend::Portable);
    require(!mparser::hasErrorDiagnostics(portable.diagnostics),
            "empty or aliased portable dense execution failed");
    for (std::string_view name : {
             "empty_sum", "default_empty", "explicit_empty",
             "all_empty", "x", "checksum"}) {
        requireVariableMatch(pipeline, portable, name);
        const auto* execution = findExecution(portable, name);
        require(execution != nullptr && execution->executionCount == 1 &&
                    execution->fallbackCount == 0,
                "empty or aliased portable region did not execute");
    }

    if (mparser::nativeScalarJitAvailable()) {
        const auto native = runTyped(
            pipeline, mparser::TypedRegionBackend::Native);
        for (std::string_view name : {"default_empty", "all_empty", "x",
                                      "checksum"}) {
            requireVariableMatch(pipeline, native, name);
            const auto* execution = findExecution(native, name);
            require(execution != nullptr && execution->executionCount == 1 &&
                        execution->fallbackCount == 0,
                    "empty or aliased native region did not execute");
        }
        for (std::string_view name : {"empty_sum", "explicit_empty"}) {
            requireVariableMatch(pipeline, native, name);
            const auto* execution = findExecution(native, name);
            require(execution != nullptr && execution->executionCount == 0 &&
                        execution->fallbackCount == 1 &&
                        execution->lastFallbackKind ==
                            mparser::RuntimeFallbackKind::BackendUnsupported,
                    "multi-output native reduction did not fall back to VM");
        }
    }
}

void runDiagnosticAndResourceBoundarySmoke() {
    constexpr std::string_view source = R"(
left = [1 2];
right = [1 2 3];
out = left .* right;
)";
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(!mparser::hasErrorDiagnostics(parsed.diagnostics),
            "shape diagnostic source did not parse");
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    require(!mparser::hasErrorDiagnostics(semantic.diagnostics),
            "shape diagnostic source failed semantics");
    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    mparser::BytecodeVm vm;
    const auto baseline = vm.run(bytecode, semantic);
    require(mparser::hasErrorDiagnostics(baseline.diagnostics),
            "incompatible baseline shapes unexpectedly succeeded");

    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    const auto module = builder.build(
        planner.planStaticRegions(bytecode, semantic.builtinRegistry));
    mparser::BytecodeVmOptions options;
    options.typedRegionBackend = mparser::TypedRegionBackend::Portable;
    const auto typed = vm.run(bytecode, semantic, module, options);
    require(mparser::hasErrorDiagnostics(typed.diagnostics) &&
                typed.diagnostics.front().message ==
                    baseline.diagnostics.front().message,
            "dense fallback changed the VM shape diagnostic");
    const auto* shapeExecution = findExecution(typed, "out");
    require(shapeExecution != nullptr &&
                shapeExecution->fallbackCount == 1,
            "incompatible shape did not record typed fallback");

    const auto limitedPipeline = prepare(R"(
x = linspace(0, 1, 64);
out = sin(x) .* 2;
checksum = sum(out, "all");
)", {}, true);
    mparser::RuntimeExecutionLimits limits;
    limits.maxInstructionCount = 100000;
    const auto limited = runTyped(
        limitedPipeline, mparser::TypedRegionBackend::Portable,
        std::make_shared<mparser::RuntimeExecutionControl>(limits));
    require(!mparser::hasErrorDiagnostics(limited.diagnostics) &&
                limited.execution.optimizedExecutionSuppressed,
            "instruction checkpoints did not suppress dense typed execution");
    for (const auto& execution : limited.typedRegionExecutions) {
        require(execution.attemptCount == 0,
                "suppressed dense region was still attempted");
    }
    requireVariableMatch(limitedPipeline, limited, "checksum");
}

} // namespace

int main() {
    try {
        runProfiledFusionAndReductionSmoke();
        runProductAndMeanReductionSmoke();
        runStaticNativeAndTamperSmoke();
        runNdImplicitExpansionSmoke();
        runComplexPortableSmoke();
        runShadowedBuiltinFallbackSmoke();
        runProfileShapeGuardFallbackSmoke();
        runNonDoubleFallbackSmoke();
        runProfiledNumericTypeGuardSmoke();
        runComplexInputPortableSmoke();
        runMatrixOperatorFallbackSmoke();
        runEmptyAndAliasedInputSmoke();
        runDiagnosticAndResourceBoundarySmoke();
        std::cout << "dense array typed region smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dense array typed region smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
