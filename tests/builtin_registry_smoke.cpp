#include "builtin_conformance.h"

#include "mparser/native_scalar_jit.h"
#include "mparser/optimization_plan.h"
#include "mparser/runtime_math.h"
#include "mparser/typed_ir.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void requireNumber(const mparser::RuntimeValue* value,
                   double expected, std::string message) {
    require(value &&
                value->kind == mparser::RuntimeValueKind::Number &&
                std::fabs(value->number - expected) < 1e-9,
            std::move(message));
}

mparser::BuiltinDescriptor customAbsoluteDescriptor() {
    mparser::BuiltinDescriptor descriptor;
    descriptor.name = "custom_abs";
    descriptor.aliases = {"custom_abs_alias"};
    descriptor.inputs = mparser::BuiltinArity::fixed(1);
    descriptor.outputs = mparser::BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        mparser::BuiltinValueConstraint::Numeric,
        mparser::BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        mparser::BuiltinValueConstraint::Numeric,
        mparser::BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation =
        mparser::BuiltinImplementationKind::Shared;
    descriptor.purity = mparser::BuiltinPurity::Pure;
    descriptor.determinism =
        mparser::BuiltinDeterminism::Deterministic;
    descriptor.threadSafety =
        mparser::BuiltinThreadSafety::Reentrant;
    descriptor.typedLowering =
        mparser::BuiltinTypedLowering::Absolute;
    descriptor.summary =
        "Conformance-only custom absolute value builtin.";
    descriptor.handler = [](const mparser::BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return mparser::BuiltinResult::success();
        }
        auto value = mparser::runtimeApplyPureUnaryMathBuiltin(
            "abs", call.arguments.front());
        if (!value) {
            return mparser::BuiltinResult::failure(
                call.span, "custom_abs expects a numeric value",
                "RegistryTest:InvalidArgument");
        }
        return mparser::BuiltinResult::success(
            {std::move(*value)});
    };
    return descriptor;
}

std::shared_ptr<mparser::BuiltinRegistry> makeCustomRegistry() {
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    auto registration =
        registry->registerBuiltin(customAbsoluteDescriptor());
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor throwing;
    throwing.name = "custom_throw";
    throwing.inputs = mparser::BuiltinArity::fixed(0);
    throwing.outputs = mparser::BuiltinArity::fixed(0);
    throwing.implementation =
        mparser::BuiltinImplementationKind::Shared;
    throwing.handler = [](const mparser::BuiltinCall&) ->
        mparser::BuiltinResult {
        throw std::runtime_error("host failure");
    };
    registration = registry->registerBuiltin(std::move(throwing));
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor malformed;
    malformed.name = "custom_malformed";
    malformed.inputs = mparser::BuiltinArity::fixed(0);
    malformed.outputs = mparser::BuiltinArity::variadic();
    malformed.implementation =
        mparser::BuiltinImplementationKind::Shared;
    malformed.handler = [](const mparser::BuiltinCall&) {
        return mparser::BuiltinResult::success(
            {mparser::makeRuntimeNumberValue(1.0)});
    };
    registration = registry->registerBuiltin(std::move(malformed));
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor needsWorkspace;
    needsWorkspace.name = "custom_workspace";
    needsWorkspace.inputs = mparser::BuiltinArity::fixed(0);
    needsWorkspace.outputs = mparser::BuiltinArity::fixed(0);
    needsWorkspace.implementation =
        mparser::BuiltinImplementationKind::Context;
    needsWorkspace.contextPermissions =
        mparser::BuiltinContextPermission::Workspace;
    needsWorkspace.requiredContext =
        mparser::BuiltinContextPermission::Workspace;
    needsWorkspace.handler = [](const mparser::BuiltinCall&) {
        return mparser::BuiltinResult::success();
    };
    registration =
        registry->registerBuiltin(std::move(needsWorkspace));
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor successWithError;
    successWithError.name = "custom_success_error";
    successWithError.inputs = mparser::BuiltinArity::fixed(0);
    successWithError.outputs = mparser::BuiltinArity::fixed(0);
    successWithError.implementation =
        mparser::BuiltinImplementationKind::Shared;
    successWithError.handler = [](const mparser::BuiltinCall& call) {
        return mparser::BuiltinResult::success(
            {}, {mparser::Diagnostic{
                    call.span, "unexpected error", "RegistryTest:Error"}});
    };
    registration =
        registry->registerBuiltin(std::move(successWithError));
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor failureWithWarning;
    failureWithWarning.name = "custom_failure_warning";
    failureWithWarning.inputs = mparser::BuiltinArity::fixed(0);
    failureWithWarning.outputs = mparser::BuiltinArity::fixed(0);
    failureWithWarning.implementation =
        mparser::BuiltinImplementationKind::Shared;
    failureWithWarning.handler = [](const mparser::BuiltinCall& call) {
        return mparser::BuiltinResult{
            false, {},
            {mparser::Diagnostic{
                call.span, "warning only", "RegistryTest:Warning",
                mparser::DiagnosticSeverity::Warning}}};
    };
    registration =
        registry->registerBuiltin(std::move(failureWithWarning));
    require(registration.succeeded, registration.error);
    return registry;
}

void runDefaultCatalogSmoke() {
    const auto registry = mparser::defaultBuiltinRegistry();
    require(registry->frozen(), "default registry is mutable");
    require(mparser::kBuiltinSourceContractMajor == 1 &&
                mparser::kBuiltinSourceContractMinor == 1,
            "builtin source contract version changed");
    require(registry->names().size() == 129,
            "default builtin name catalog changed unexpectedly");

    const auto* absolute = registry->find("abs");
    require(absolute &&
                absolute->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                absolute->purity == mparser::BuiltinPurity::Pure &&
                absolute->typedLowering ==
                    mparser::BuiltinTypedLowering::Absolute,
            "abs descriptor metadata mismatch");

    const auto* maximum = registry->find("max");
    require(maximum && maximum->outputs.maximum == 2,
            "max multi-output metadata mismatch");
    const auto* find = registry->find("find");
    require(find && find->outputs.maximum == 3,
            "find multi-output metadata mismatch");
    const auto* warning = registry->find("warning");
    require(warning &&
                warning->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                mparser::hasBuiltinContextPermission(
                    warning->requiredContext,
                    mparser::BuiltinContextPermission::WarningState),
            "warning context metadata mismatch");
    const auto* reshape = registry->find("reshape");
    require(reshape &&
                mparser::hasBuiltinContextPermission(
                    reshape->contextPermissions,
                    mparser::BuiltinContextPermission::ObjectArrayPolicy),
            "reshape object-array policy metadata mismatch");
    const auto* unsupported = registry->find("disp");
    require(unsupported &&
                unsupported->implementation ==
                    mparser::BuiltinImplementationKind::Unsupported,
            "unsupported builtin boundary mismatch");
}

void runRegistryBoundarySmoke() {
    auto registry = makeCustomRegistry();
    std::vector<mparser::RuntimeValue> arguments{
        mparser::makeRuntimeNumberValue(-3.0)};
    auto result = registry->invoke(
        "custom_abs_alias",
        mparser::BuiltinCall{arguments, 1, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 1,
            "custom alias invocation failed");
    requireNumber(&result.outputs.front(), 3.0,
                  "custom alias result mismatch");

    std::vector<mparser::RuntimeValue> empty;
    result = registry->invoke(
        "custom_throw",
        mparser::BuiltinCall{empty, 0, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:BuiltinHostException",
            "host exception was not converted");

    result = registry->invoke(
        "custom_malformed",
        mparser::BuiltinCall{empty, 2, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:BuiltinContractViolation",
            "malformed output count was accepted");

    mparser::BuiltinDescriptor wrongOutput;
    wrongOutput.name = "custom_wrong_output";
    wrongOutput.inputs = mparser::BuiltinArity::fixed(0);
    wrongOutput.outputs = mparser::BuiltinArity::fixed(1);
    wrongOutput.outputConstraints = {{
        mparser::BuiltinValueConstraint::Text,
        mparser::BuiltinShapeConstraint::Any,
    }};
    wrongOutput.implementation =
        mparser::BuiltinImplementationKind::Shared;
    wrongOutput.handler = [](const mparser::BuiltinCall&) {
        return mparser::BuiltinResult::success(
            {mparser::makeRuntimeNumberValue(1.0)});
    };
    auto registration =
        registry->registerBuiltin(std::move(wrongOutput));
    require(registration.succeeded, registration.error);
    result = registry->invoke(
        "custom_wrong_output",
        mparser::BuiltinCall{empty, 1, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:BuiltinContractViolation",
            "builtin output constraint was not enforced");

    result = registry->invoke(
        "custom_workspace",
        mparser::BuiltinCall{empty, 0, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:MissingBuiltinContext",
            "missing builtin context was accepted");

    result = registry->invoke(
        "custom_success_error",
        mparser::BuiltinCall{empty, 0, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:BuiltinContractViolation",
            "successful builtin returned an error diagnostic");

    result = registry->invoke(
        "custom_failure_warning",
        mparser::BuiltinCall{empty, 0, {}, nullptr});
    require(!result.succeeded &&
                result.diagnostics.front().identifier ==
                    "MParser:BuiltinContractViolation",
            "failed builtin returned no error diagnostic");

    mparser::BuiltinDescriptor duplicate;
    duplicate.name = "duplicate_alias";
    duplicate.aliases = {"custom_abs_alias"};
    const auto duplicateResult =
        registry->registerBuiltin(std::move(duplicate));
    require(!duplicateResult.succeeded,
            "duplicate alias registration was accepted");

    mparser::BuiltinDescriptor repeatedAlias;
    repeatedAlias.name = "repeated_alias";
    repeatedAlias.aliases = {"same_alias", "same_alias"};
    const auto repeatedAliasResult =
        registry->registerBuiltin(std::move(repeatedAlias));
    require(!repeatedAliasResult.succeeded,
            "duplicate aliases in one descriptor were accepted");

    auto unsafeTyped = customAbsoluteDescriptor();
    unsafeTyped.name = "unsafe_typed";
    unsafeTyped.aliases.clear();
    unsafeTyped.purity = mparser::BuiltinPurity::Impure;
    const auto unsafeTypedResult =
        registry->registerBuiltin(std::move(unsafeTyped));
    require(!unsafeTypedResult.succeeded,
            "unsafe typed metadata was accepted");

    mparser::BuiltinDescriptor undeclaredContext;
    undeclaredContext.name = "undeclared_context";
    undeclaredContext.inputs = mparser::BuiltinArity::fixed(0);
    undeclaredContext.outputs = mparser::BuiltinArity::fixed(0);
    undeclaredContext.implementation =
        mparser::BuiltinImplementationKind::Context;
    undeclaredContext.requiredContext =
        mparser::BuiltinContextPermission::Workspace;
    undeclaredContext.handler = [](const mparser::BuiltinCall&) {
        return mparser::BuiltinResult::success();
    };
    const auto undeclaredContextResult =
        registry->registerBuiltin(std::move(undeclaredContext));
    require(!undeclaredContextResult.succeeded,
            "undeclared required context was accepted");

    mparser::CompiledModuleCompileOptions mutableOptions;
    mutableOptions.builtinRegistry = registry;
    const auto mutableModule = mparser::CompiledModule::compile(
        "value = custom_abs(-1);", mutableOptions);
    require(!mutableModule.valid() &&
                !mutableModule.diagnostics().empty() &&
                mutableModule.diagnostics().front().identifier ==
                    "MParser:MutableBuiltinRegistry",
            "mutable registry was accepted for module compilation");

    registry->freeze();
    mparser::BuiltinDescriptor late;
    late.name = "late_builtin";
    const auto lateResult =
        registry->registerBuiltin(std::move(late));
    require(!lateResult.succeeded,
            "frozen registry accepted a registration");
}

void runCrossEngineExtensionSmoke() {
    auto registry = makeCustomRegistry();
    registry->freeze();
    const auto run = mparser::test::runBuiltinConformance(
        R"(direct = custom_abs(-3);
alias_value = custom_abs_alias(-5);
f = @custom_abs;
handle_value = f(-4);
summary = direct + alias_value + handle_value;
)",
        registry);
    mparser::test::requireNoBuiltinErrors(run);
    mparser::test::requireBuiltinVariableParity(run, "direct");
    mparser::test::requireBuiltinVariableParity(run, "alias_value");
    mparser::test::requireBuiltinVariableParity(run, "handle_value");
    mparser::test::requireBuiltinVariableParity(run, "summary");
    requireNumber(
        mparser::test::findVariable(run.vm.variables, "summary"),
        12.0, "custom builtin cross-engine result mismatch");
    require(run.module.semantic().builtinRegistry.get() ==
                registry.get(),
            "compiled module did not retain its registry");

    const auto shadowed = mparser::test::runBuiltinConformance(
        R"(value = custom_abs(2);
function y = custom_abs(x)
y = x + 10;
end
)",
        registry);
    mparser::test::requireNoBuiltinErrors(shadowed);
    mparser::test::requireBuiltinVariableParity(shadowed, "value");
    requireNumber(
        mparser::test::findVariable(shadowed.vm.variables, "value"),
        12.0, "local function did not shadow custom builtin");
}

void runTypedDescriptorSmoke() {
    auto registry = makeCustomRegistry();
    registry->freeze();
    const auto run = mparser::test::runBuiltinConformance(
        R"(total = 0;
for i = 1:10
    total = total + custom_abs(i - 6);
end
)",
        registry);
    mparser::test::requireNoBuiltinErrors(run);
    requireNumber(
        mparser::test::findVariable(run.vm.variables, "total"),
        25.0, "custom builtin baseline result mismatch");

    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    const auto typedModule = builder.build(planner.plan(
        run.vm.profile, run.module.bytecode(), registry));
    require(!typedModule.regions.empty(),
            "custom typed descriptor produced no region");

    const auto runBackend = [&](mparser::TypedRegionBackend backend) {
        mparser::BytecodeVmOptions options;
        options.profiling =
            mparser::BytecodeVmProfilingMode::Disabled;
        options.typedRegionBackend = backend;
        mparser::BytecodeVm vm;
        const auto typed = vm.run(
            run.module.bytecode(), run.module.semantic(),
            typedModule, options);
        require(!mparser::hasErrorDiagnostics(typed.diagnostics),
                "custom typed descriptor execution failed");
        requireNumber(
            mparser::test::findVariable(typed.variables, "total"),
            25.0, "custom typed descriptor result mismatch");
        const auto execution = std::find_if(
            typed.typedRegionExecutions.begin(),
            typed.typedRegionExecutions.end(),
            [](const auto& profile) {
                return profile.target == "i";
            });
        require(
            execution != typed.typedRegionExecutions.end() &&
                execution->executionCount > 0 &&
                execution->fallbackCount == 0 &&
                execution->backend ==
                    mparser::typedRegionBackendName(backend),
            "custom descriptor did not execute in the requested typed tier");
        if (backend == mparser::TypedRegionBackend::Native) {
            require(execution->nativeCodeSize > 0,
                    "custom descriptor produced no native code");
        }
    };

    runBackend(mparser::TypedRegionBackend::Portable);
    if (mparser::nativeScalarJitAvailable()) {
        runBackend(mparser::TypedRegionBackend::Native);
    }
}

} // namespace

int main() {
    try {
        runDefaultCatalogSmoke();
        runRegistryBoundarySmoke();
        runCrossEngineExtensionSmoke();
        runTypedDescriptorSmoke();
        std::cout << "builtin registry smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Builtin registry smoke failure: "
                  << error.what() << "\n";
        return 1;
    }
}
