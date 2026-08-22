#include "builtin_conformance.h"

#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/runtime/builtins/runtime_math.h"
#include "mparser/execution/jit/typed_ir.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
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

mparser::BuiltinDescriptor customArityDescriptor() {
    mparser::BuiltinDescriptor descriptor;
    descriptor.name = "custom_arity";
    descriptor.inputs = mparser::BuiltinArity::fixed(0);
    descriptor.outputs = mparser::BuiltinArity::range(0, 1);
    descriptor.implementation =
        mparser::BuiltinImplementationKind::Shared;
    descriptor.purity = mparser::BuiltinPurity::Pure;
    descriptor.determinism =
        mparser::BuiltinDeterminism::Deterministic;
    descriptor.threadSafety =
        mparser::BuiltinThreadSafety::Reentrant;
    descriptor.handler = [](const mparser::BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return mparser::BuiltinResult::success();
        }
        return mparser::BuiltinResult::success({
            mparser::makeRuntimeNumberValue(
                static_cast<double>(call.requestedOutputCount * 10 +
                                    call.callerNargout()))});
    };
    return descriptor;
}

std::shared_ptr<mparser::BuiltinRegistry> makeCustomRegistry() {
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    auto registration =
        registry->registerBuiltin(customAbsoluteDescriptor());
    require(registration.succeeded, registration.error);
    registration = registry->registerBuiltin(customArityDescriptor());
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
                mparser::kBuiltinSourceContractMinor == 8,
            "builtin source contract version changed");
    require(registry->descriptors().size() == 259,
            "default builtin descriptor catalog changed unexpectedly");
    require(registry->names().size() == 261,
            "default builtin name catalog changed unexpectedly");

    for (std::string_view name : {"eval", "evalc", "evalin"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    descriptor->purity ==
                        mparser::BuiltinPurity::Impure &&
                    descriptor->implicitOutputPolicy ==
                        (name == "evalc"
                             ? mparser::BuiltinImplicitOutputPolicy::
                                   FirstAvailable
                             : mparser::BuiltinImplicitOutputPolicy::None) &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->requiredContext,
                        mparser::BuiltinContextPermission::SourceEvaluation),
                "dynamic source builtin metadata mismatch");
    }
    const auto* assignin = registry->find("assignin");
    require(assignin && assignin->inputs.minimum == 3 &&
                assignin->inputs.maximum == 3 &&
                assignin->outputs.maximum == 0 &&
                mparser::hasBuiltinContextPermission(
                    assignin->requiredContext,
                    mparser::BuiltinContextPermission::Workspace),
            "assignin metadata mismatch");
    for (std::string_view name : {"load", "save"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.minimum == 0 &&
                    !descriptor->inputs.maximum &&
                    descriptor->outputs.minimum == 0 &&
                    descriptor->outputs.maximum ==
                        std::optional<size_t>(name == "load" ? 1 : 0) &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    descriptor->purity ==
                        mparser::BuiltinPurity::Impure &&
                    descriptor->implicitOutputPolicy ==
                        mparser::BuiltinImplicitOutputPolicy::None &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->requiredContext,
                        mparser::BuiltinContextPermission::Workspace) &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->requiredContext,
                        mparser::BuiltinContextPermission::SystemServices),
                "MAT-file builtin metadata mismatch");
    }
    for (std::string_view name : {"fgetl", "fgets", "fread", "fwrite",
                                  "feof", "ferror"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->requiredContext,
                        mparser::BuiltinContextPermission::SystemServices),
                "stream I/O builtin metadata mismatch");
    }
    require(registry->find("fread")->inputs.maximum == 5 &&
                registry->find("fread")->outputs.maximum == 2 &&
                registry->find("fwrite")->inputs.minimum == 2 &&
                registry->find("fwrite")->outputs.maximum == 1 &&
                registry->find("fgets")->outputs.maximum == 2,
            "stream I/O arity metadata mismatch");

    const auto* fileparts = registry->find("fileparts");
    require(fileparts && fileparts->inputs.minimum == 1 &&
                fileparts->inputs.maximum == 1 &&
                fileparts->outputs.minimum == 1 &&
                fileparts->outputs.maximum == 3 &&
                fileparts->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                fileparts->purity == mparser::BuiltinPurity::Pure &&
                fileparts->requiredContext ==
                    mparser::BuiltinContextPermission::None,
            "fileparts metadata mismatch");
    for (std::string_view name : {"isfile", "isfolder", "fileread",
                                  "tempname"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    descriptor->purity ==
                        mparser::BuiltinPurity::ReadOnly &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->requiredContext,
                        mparser::BuiltinContextPermission::SystemServices),
                "filesystem query builtin metadata mismatch");
    }
    for (std::string_view name : {"mkdir", "rmdir", "copyfile",
                                  "movefile"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->outputs.minimum == 0 &&
                    descriptor->outputs.maximum == 3 &&
                    descriptor->purity ==
                        mparser::BuiltinPurity::Impure &&
                    descriptor->sideEffects ==
                        mparser::BuiltinSideEffect::External &&
                    descriptor->implicitOutputPolicy ==
                        mparser::BuiltinImplicitOutputPolicy::None,
                "filesystem mutation builtin metadata mismatch");
    }

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
    const auto* complex = registry->find("complex");
    require(complex && complex->inputs.minimum == 1 &&
                complex->inputs.maximum == 2 &&
                complex->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                complex->purity == mparser::BuiltinPurity::Pure,
            "complex descriptor metadata mismatch");
    const auto* isreal = registry->find("isreal");
    require(isreal && isreal->outputs.maximum == 1 &&
                isreal->outputConstraints.size() == 1 &&
                isreal->outputConstraints.front().shape ==
                    mparser::BuiltinShapeConstraint::Scalar,
            "isreal descriptor metadata mismatch");
    for (std::string_view name : {"acosh", "nextpow2", "round",
                                  "isfinite"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.minimum == 1 &&
                    descriptor->inputs.maximum == 1 &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure,
                "shared unary math metadata mismatch");
    }
    for (std::string_view name : {"atan2", "hypot", "mod", "rem"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.minimum == 2 &&
                    descriptor->inputs.maximum == 2 &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared,
                "shared binary math metadata mismatch");
    }
    for (std::string_view name : {"isscalar", "isvector", "isrow",
                                  "iscolumn", "ismatrix"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.minimum == 1 &&
                    descriptor->inputs.maximum == 1 &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure,
                "shape predicate metadata mismatch");
    }
    for (std::string_view name : {"isequal", "isequaln"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.minimum == 2 &&
                    !descriptor->inputs.maximum &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared,
                "deep equality metadata mismatch");
    }
    const auto* epsilon = registry->find("eps");
    require(epsilon && epsilon->inputs.minimum == 0 &&
                epsilon->inputs.maximum == 1 &&
                epsilon->implementation ==
                    mparser::BuiltinImplementationKind::Shared,
            "eps descriptor metadata mismatch");
    const auto* missing = registry->find("missing");
    require(missing && missing->inputs.minimum == 0 &&
                missing->inputs.maximum == 0 &&
                missing->outputs.maximum == 1 &&
                missing->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                missing->purity == mparser::BuiltinPurity::Pure,
            "missing descriptor metadata mismatch");
    std::vector<mparser::RuntimeValue> noArguments;
    const auto missingResult = registry->invoke(
        "missing", mparser::BuiltinCall{noArguments, 1, {}, nullptr});
    require(missingResult.succeeded &&
                missingResult.outputs.size() == 1 &&
                missingResult.outputs.front().kind ==
                    mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeValueIsStorable(
                    missingResult.outputs.front()),
            "missing builtin result mismatch");
    const auto* randomState = registry->find("rng");
    require(randomState &&
                randomState->implicitOutputPolicy ==
                    mparser::BuiltinImplicitOutputPolicy::FirstWhenNoArguments &&
                randomState->implicitOutputCount(0) == 1 &&
                randomState->implicitOutputCount(1) == 0,
            "rng implicit query/set output metadata mismatch");
    for (std::string_view name : {"who", "whos", "dir", "pause",
                                  "system", "fprintf"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->implicitOutputCount(1) == 0,
                "command-style implicit output metadata mismatch");
    }
    const auto* str2double = registry->find("str2double");
    require(str2double && str2double->inputs.minimum == 1 &&
                str2double->inputs.maximum == 1 &&
                str2double->outputs.maximum == 1 &&
                str2double->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                str2double->purity == mparser::BuiltinPurity::Pure,
            "str2double descriptor metadata mismatch");
    for (std::string_view name : {"lower", "upper", "strtrim",
                                  "num2str", "strsplit", "regexp",
                                  "sort", "unique", "iscell",
                                  "struct2cell", "cell2struct"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure &&
                    descriptor->determinism ==
                        mparser::BuiltinDeterminism::Deterministic,
                "standard-library descriptor metadata mismatch");
    }
    const auto* cellfun = registry->find("cellfun");
    require(cellfun &&
                cellfun->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                cellfun->purity == mparser::BuiltinPurity::Impure &&
                mparser::hasBuiltinContextPermission(
                    cellfun->requiredContext,
                    mparser::BuiltinContextPermission::DynamicCall) &&
                mparser::hasBuiltinContextPermission(
                    cellfun->contextPermissions,
                    mparser::BuiltinContextPermission::ExecutionControl),
            "cellfun dynamic-call metadata mismatch");
    for (std::string_view name : {
             "factorial", "gcd", "isprime", "lcm", "logspace",
             "meshgrid", "primes"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->purity == mparser::BuiltinPurity::Pure &&
                    descriptor->determinism ==
                        mparser::BuiltinDeterminism::Deterministic &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->contextPermissions,
                        mparser::BuiltinContextPermission::ExecutionControl),
                "numeric utility metadata mismatch");
    }
    for (std::string_view name : {"flip", "fliplr", "flipud"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    descriptor->purity == mparser::BuiltinPurity::Pure &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->contextPermissions,
                        mparser::BuiltinContextPermission::ObjectArrayPolicy),
                "flip-family metadata mismatch");
    }
    for (std::string_view name : {"strfind", "strrep"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure,
                "text utility metadata mismatch");
    }
    const auto* randperm = registry->find("randperm");
    require(randperm &&
                randperm->purity == mparser::BuiltinPurity::Impure &&
                mparser::hasBuiltinSideEffect(
                    randperm->sideEffects,
                    mparser::BuiltinSideEffect::RandomState) &&
                mparser::hasBuiltinContextPermission(
                    randperm->contextPermissions,
                    mparser::BuiltinContextPermission::ExecutionControl),
            "randperm metadata mismatch");
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
    for (std::string_view name : {"size", "linspace", "zeros",
                                  "ones", "eye", "true", "false",
                                  "cell", "strings", "inf", "nan"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure,
                "shared array builtin metadata mismatch");
    }
    require(registry->find("Inf") == registry->find("inf") &&
                registry->find("NaN") == registry->find("nan"),
            "MATLAB special-value aliases are not canonicalized");
    const auto* display = registry->find("disp");
    require(display &&
                display->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                display->purity == mparser::BuiltinPurity::Impure &&
                mparser::hasBuiltinContextPermission(
                    display->requiredContext,
                    mparser::BuiltinContextPermission::Output),
            "display output builtin metadata mismatch");
    const auto* print = registry->find("fprintf");
    require(print &&
                print->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                print->purity == mparser::BuiltinPurity::Impure &&
                mparser::hasBuiltinContextPermission(
                    print->contextPermissions,
                    mparser::BuiltinContextPermission::Output) &&
                mparser::hasBuiltinContextPermission(
                    print->contextPermissions,
                    mparser::BuiltinContextPermission::SystemServices) &&
                print->requiredContext ==
                    mparser::BuiltinContextPermission::None,
            "formatted output builtin metadata mismatch");
    for (std::string_view name : {"filesep", "pathsep"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor && descriptor->inputs.maximum == 0 &&
                    descriptor->outputs.maximum == 1 &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity ==
                        mparser::BuiltinPurity::Pure &&
                    descriptor->contextPermissions ==
                        mparser::BuiltinContextPermission::None,
                "platform separator builtin metadata mismatch");
    }
    const auto* formatted = registry->find("sprintf");
    require(formatted &&
                formatted->implementation ==
                    mparser::BuiltinImplementationKind::Shared &&
                formatted->purity == mparser::BuiltinPurity::Pure &&
                formatted->outputs.maximum == 1,
            "sprintf metadata mismatch");
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
custom_arity();
implicit_arity = ans;
explicit_arity = custom_arity();
arity_handle = @custom_arity;
arity_handle();
handle_arity = ans;
feval(arity_handle);
feval_arity = ans;
summary = direct + alias_value + handle_value;
)",
        registry);
    mparser::test::requireNoBuiltinErrors(run);
    mparser::test::requireBuiltinVariableParity(run, "direct");
    mparser::test::requireBuiltinVariableParity(run, "alias_value");
    mparser::test::requireBuiltinVariableParity(run, "handle_value");
    mparser::test::requireBuiltinVariableParity(run, "implicit_arity");
    mparser::test::requireBuiltinVariableParity(run, "explicit_arity");
    mparser::test::requireBuiltinVariableParity(run, "handle_arity");
    mparser::test::requireBuiltinVariableParity(run, "feval_arity");
    mparser::test::requireBuiltinVariableParity(run, "summary");
    requireNumber(
        mparser::test::findVariable(run.vm.variables, "summary"),
        12.0, "custom builtin cross-engine result mismatch");
    for (std::string_view name : {"implicit_arity", "handle_arity",
                                  "feval_arity"}) {
        requireNumber(
            mparser::test::findVariable(run.vm.variables, name), 10.0,
            "implicit builtin nargout mismatch");
    }
    requireNumber(
        mparser::test::findVariable(run.vm.variables, "explicit_arity"),
        11.0, "explicit builtin nargout mismatch");
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
