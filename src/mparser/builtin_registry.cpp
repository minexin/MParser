#include "mparser/builtin_registry.h"

#include "mparser/runtime_array_ops.h"
#include "mparser/runtime_math.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_object.h"
#include "mparser/runtime_reduction.h"
#include "mparser/runtime_scan.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_warning.h"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace mparser {
namespace {

constexpr std::string_view kBuiltinNames[] = {
    "MException",
    "abs",
    "acos",
    "acosh",
    "addCause",
    "addCorrection",
    "addlistener",
    "addprop",
    "all",
    "any",
    "asin",
    "asinh",
    "assert",
    "atan",
    "atan2",
    "atanh",
    "cat",
    "cell",
    "cellstr",
    "ceil",
    "char",
    "class",
    "clc",
    "clear",
    "complex",
    "conj",
    "cos",
    "cosh",
    "cummax",
    "cummin",
    "cumprod",
    "cumsum",
    "delete",
    "disp",
    "diff",
    "double",
    "empty",
    "enumeration",
    "eps",
    "error",
    "events",
    "exp",
    "eye",
    "false",
    "fieldnames",
    "find",
    "feval",
    "findobj",
    "findprop",
    "fix",
    "floor",
    "fprintf",
    "func2str",
    "functions",
    "getReport",
    "horzcat",
    "hypot",
    "i",
    "imag",
    "inf",
    "int16",
    "int32",
    "int64",
    "int8",
    "ipermute",
    "isa",
    "isenum",
    "isempty",
    "ischar",
    "isfield",
    "isfinite",
    "isfloat",
    "isinf",
    "isinteger",
    "islogical",
    "ismethod",
    "isnumeric",
    "isnan",
    "isreal",
    "isequal",
    "ismissing",
    "isprop",
    "isstring",
    "isStringScalar",
    "isstruct",
    "isvalid",
    "j",
    "lastwarn",
    "length",
    "linspace",
    "listener",
    "log",
    "log10",
    "log2",
    "logical",
    "max",
    "mean",
    "metaclass",
    "metafunction",
    "methods",
    "min",
    "nan",
    "nargin",
    "nargout",
    "ndims",
    "notify",
    "numel",
    "ones",
    "permute",
    "pi",
    "plot",
    "prod",
    "properties",
    "rand",
    "randn",
    "real",
    "repmat",
    "reshape",
    "rmfield",
    "round",
    "sign",
    "single",
    "sin",
    "sinh",
    "size",
    "sqrt",
    "squeeze",
    "str2func",
    "strcmp",
    "strcmpi",
    "string",
    "strings",
    "strlength",
    "struct",
    "sum",
    "table",
    "tan",
    "tanh",
    "throw",
    "throwAsCaller",
    "tic",
    "toc",
    "true",
    "rethrow",
    "uint16",
    "uint32",
    "uint64",
    "uint8",
    "vertcat",
    "warning",
    "zeros",
    "matlab.metadata.Class.fromName",
    "meta.class.fromName",
    "event.proplistener",
};

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinTypedLowering typedLowering(std::string_view name) {
    if (name == "abs") {
        return BuiltinTypedLowering::Absolute;
    }
    if (name == "acos") {
        return BuiltinTypedLowering::ArcCosine;
    }
    if (name == "asin") {
        return BuiltinTypedLowering::ArcSine;
    }
    if (name == "atan") {
        return BuiltinTypedLowering::ArcTangent;
    }
    if (name == "cos") {
        return BuiltinTypedLowering::Cosine;
    }
    if (name == "exp") {
        return BuiltinTypedLowering::Exponential;
    }
    if (name == "log") {
        return BuiltinTypedLowering::Logarithm;
    }
    if (name == "sin") {
        return BuiltinTypedLowering::Sine;
    }
    if (name == "sqrt") {
        return BuiltinTypedLowering::SquareRoot;
    }
    if (name == "tan") {
        return BuiltinTypedLowering::Tangent;
    }
    return BuiltinTypedLowering::None;
}

BuiltinResult helperFailure(SourceSpan span, std::string error,
                            std::string identifier) {
    return BuiltinResult::failure(span, std::move(error),
                                  std::move(identifier));
}

BuiltinDescriptor baseDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor;
    descriptor.name = std::string(name);
    descriptor.summary =
        "Engine intrinsic provided by the MParser runtime.";
    return descriptor;
}

BuiltinDescriptor mathDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::fixed(1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.typedLowering = typedLowering(name);
    descriptor.summary =
        "Pure element-wise numeric unary operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeApplyPureUnaryMathBuiltin(
            builtin, call.arguments.front());
        if (!result) {
            return helperFailure(
                call.span,
                builtin +
                    " does not support the supplied numeric class or value",
                "MParser:InvalidBuiltinArgument");
        }
        return BuiltinResult::success({std::move(*result)});
    };
    return descriptor;
}

BuiltinDescriptor binaryMathDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::fixed(2);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {
        {BuiltinValueConstraint::Numeric, BuiltinShapeConstraint::Any},
        {BuiltinValueConstraint::Numeric, BuiltinShapeConstraint::Any},
    };
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "Pure element-wise numeric binary operation with implicit expansion.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeApplyPureBinaryMathBuiltin(
            builtin, call.arguments[0], call.arguments[1]);
        if (!result) {
            return helperFailure(
                call.span,
                builtin +
                    " requires real floating-point inputs with compatible shapes",
                "MParser:InvalidBuiltinArgument");
        }
        return BuiltinResult::success({std::move(*result)});
    };
    return descriptor;
}

BuiltinDescriptor epsilonDescriptor() {
    BuiltinDescriptor descriptor = baseDescriptor("eps");
    descriptor.inputs = BuiltinArity::range(0, 1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "Floating-point spacing for double or single values.";
    descriptor.handler = [](const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeEpsilonBuiltin(call.arguments);
        if (!result) {
            return helperFailure(
                call.span,
                "eps accepts double, single, or a floating-point class name",
                "MParser:InvalidBuiltinArgument");
        }
        return BuiltinResult::success({std::move(*result)});
    };
    return descriptor;
}

bool isNumericConversionBuiltin(std::string_view name) {
    return matches(name, {"double", "logical", "single", "int8",
                          "uint8", "int16", "uint16", "int32",
                          "uint32", "int64", "uint64"});
}

BuiltinDescriptor numericConversionDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::fixed(1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "MATLAB-like numeric class conversion with class-specific rounding.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (builtin == "double" &&
            isRuntimeCharacterArray(call.arguments.front())) {
            auto result = runtimeCharacterCodes(call.arguments.front());
            if (!result.succeeded) {
                return helperFailure(
                    call.span, std::move(result.error),
                    "MParser:InvalidNumericConversion");
            }
            return call.requestedOutputCount == 0
                       ? BuiltinResult::success()
                       : BuiltinResult::success(
                             {std::move(result.value)});
        }
        if (!isRuntimeNumericValue(call.arguments.front())) {
            return helperFailure(
                call.span, builtin + " expects one numeric argument",
                "MParser:InvalidNumericConversion");
        }
        const auto target = runtimeNumericClassFromName(builtin);
        if (!target) {
            return helperFailure(
                call.span,
                builtin + " is not a supported numeric class",
                "MParser:UnsupportedNumericClass");
        }
        auto converted = runtimeConvertNumericClass(
            call.arguments.front(), *target);
        if (!converted) {
            return helperFailure(
                call.span,
                builtin == "logical"
                    ? "NaN cannot be converted to class logical"
                    : "numeric value cannot be converted to class " +
                          builtin,
                "MParser:InvalidNumericConversion");
        }
        return call.requestedOutputCount == 0
                   ? BuiltinResult::success()
                   : BuiltinResult::success(
                         {std::move(*converted)});
    };
    return descriptor;
}

bool isNumericPredicateBuiltin(std::string_view name) {
    return matches(name, {"isnumeric", "isfloat", "isinteger",
                          "islogical"});
}

BuiltinDescriptor numericPredicateDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::fixed(1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Scalar,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary = "Numeric class-family predicate.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        return BuiltinResult::success({makeRuntimeLogicalValue(
            runtimeNumericPredicate(builtin, call.arguments.front()))});
    };
    return descriptor;
}

BuiltinDescriptor complexNumericDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = name == "complex"
                            ? BuiltinArity::range(1, 2)
                            : BuiltinArity::fixed(1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        name == "complex" ? 2 : 1,
        BuiltinArgumentConstraint{
            BuiltinValueConstraint::Numeric,
            BuiltinShapeConstraint::Any});
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        name == "isreal" ? BuiltinShapeConstraint::Scalar
                         : BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "MATLAB-like complex construction, projection, conjugation, or predicate.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeApplyComplexNumericBuiltin(
            builtin, call.arguments);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidComplexNumericOperation");
        }
        return BuiltinResult::success(
            {std::move(result.value)});
    };
    return descriptor;
}

BuiltinDescriptor reductionDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(1);
    descriptor.outputs = BuiltinArity::range(
        0, name == "find" ? 3 :
           (name == "min" || name == "max") ? 2 : 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{
            BuiltinValueConstraint::Numeric,
            BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "Numeric reduction with MATLAB-like dimension and output rules.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        auto result = runtimeReductionBuiltin(
            builtin, call.arguments, call.requestedOutputCount);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidReduction");
        }
        return BuiltinResult::success(std::move(result.outputs));
    };
    return descriptor;
}

BuiltinDescriptor scanDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(1);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "Numeric cumulative scan or finite difference operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        auto result = runtimeScanBuiltin(
            builtin, call.arguments, call.requestedOutputCount);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidScan");
        }
        return BuiltinResult::success(std::move(result.outputs));
    };
    return descriptor;
}

BuiltinDescriptor arrayDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "permute" || name == "ipermute") {
        descriptor.inputs = BuiltinArity::fixed(2);
    } else if (name == "squeeze") {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (name == "reshape" || name == "repmat" ||
               name == "cat") {
        descriptor.inputs = BuiltinArity::variadic(2);
    } else {
        descriptor.inputs = BuiltinArity::variadic();
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.contextPermissions =
        BuiltinContextPermission::ObjectArrayPolicy;
    descriptor.summary =
        "Shape-preserving array transformation or concatenation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        const RuntimeObjectArrayPolicy defaultPolicy;
        const RuntimeObjectArrayPolicy& policy =
            call.context && call.context->objectArrayPolicy
                ? *call.context->objectArrayPolicy
                : defaultPolicy;
        auto result = runtimeArrayOperationBuiltin(
            builtin, call.arguments, policy);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidArrayOperation");
        }
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        return BuiltinResult::success({std::move(result.value)});
    };
    return descriptor;
}

BuiltinDescriptor arrayConstructorDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic();
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidArrayConstructor";
    descriptor.summary =
        "MATLAB-like numeric, logical, cell, or string array constructor.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeArrayConstructorBuiltin(
            builtin, call.arguments);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidArrayConstructor");
        }
        return BuiltinResult::success({std::move(result.value)});
    };
    return descriptor;
}

BuiltinDescriptor linspaceDescriptor() {
    BuiltinDescriptor descriptor = baseDescriptor("linspace");
    descriptor.inputs = BuiltinArity::range(2, 3);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        BuiltinValueConstraint::ScalarNumeric,
        BuiltinShapeConstraint::Scalar,
    }, {
        BuiltinValueConstraint::ScalarNumeric,
        BuiltinShapeConstraint::Scalar,
    }, {
        BuiltinValueConstraint::ScalarNumeric,
        BuiltinShapeConstraint::Scalar,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidLinspace";
    descriptor.summary =
        "Evenly spaced real or complex floating-point row vector.";
    descriptor.handler = [](const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeLinspaceBuiltin(call.arguments);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidLinspace");
        }
        return BuiltinResult::success({std::move(result.value)});
    };
    return descriptor;
}

BuiltinDescriptor sizeDescriptor() {
    BuiltinDescriptor descriptor = baseDescriptor("size");
    descriptor.inputs = BuiltinArity::range(1, 2);
    descriptor.outputs = BuiltinArity::variadic();
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidSize";
    descriptor.summary =
        "Array dimensions with scalar, vector, or multiple-output queries.";
    descriptor.handler = [](const BuiltinCall& call) {
        auto result = runtimeSizeBuiltin(
            call.arguments, call.requestedOutputCount);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidSize");
        }
        return BuiltinResult::success(std::move(result.outputs));
    };
    return descriptor;
}

BuiltinDescriptor warningDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic();
    descriptor.outputs = BuiltinArity::range(
        0, name == "lastwarn" ? 2 : 1);
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{
            BuiltinValueConstraint::Any,
            BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Impure;
    descriptor.determinism =
        BuiltinDeterminism::ContextDependent;
    descriptor.threadSafety = BuiltinThreadSafety::ContextBound;
    descriptor.sideEffects = BuiltinSideEffect::WarningState;
    descriptor.contextPermissions =
        BuiltinContextPermission::WarningState;
    descriptor.requiredContext =
        BuiltinContextPermission::WarningState;
    descriptor.errorIdentifier = "MParser:InvalidWarning";
    descriptor.summary =
        "Per-runtime warning state query, update, or emission.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        RuntimeWarningOperationResult result =
            builtin == "warning"
                ? runtimeWarning(call.arguments,
                                 call.requestedOutputCount,
                                 *call.context->warningState)
                : runtimeLastWarning(call.arguments,
                                     call.requestedOutputCount,
                                     *call.context->warningState);
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidWarning");
        }
        std::vector<Diagnostic> diagnostics;
        if (result.emitted) {
            diagnostics.push_back(Diagnostic{
                call.span,
                std::move(result.emitted->message),
                std::move(result.emitted->identifier),
                DiagnosticSeverity::Warning});
        }
        return BuiltinResult::success(
            std::move(result.outputs), std::move(diagnostics));
    };
    return descriptor;
}

BuiltinDescriptor descriptorFor(std::string_view name) {
    if (isNumericConversionBuiltin(name)) {
        return numericConversionDescriptor(name);
    }
    if (isNumericPredicateBuiltin(name)) {
        return numericPredicateDescriptor(name);
    }
    if (isRuntimeComplexNumericBuiltin(name)) {
        return complexNumericDescriptor(name);
    }
    if (isRuntimePureUnaryMathBuiltin(name)) {
        return mathDescriptor(name);
    }
    if (isRuntimePureBinaryMathBuiltin(name)) {
        return binaryMathDescriptor(name);
    }
    if (name == "eps") {
        return epsilonDescriptor();
    }
    if (isRuntimeReductionBuiltin(name)) {
        return reductionDescriptor(name);
    }
    if (isRuntimeScanBuiltin(name)) {
        return scanDescriptor(name);
    }
    if (isRuntimeArrayOperationBuiltin(name)) {
        return arrayDescriptor(name);
    }
    if (isRuntimeArrayConstructorBuiltin(name)) {
        return arrayConstructorDescriptor(name);
    }
    if (name == "linspace") {
        return linspaceDescriptor();
    }
    if (name == "size") {
        return sizeDescriptor();
    }
    if (name == "warning" || name == "lastwarn") {
        return warningDescriptor(name);
    }

    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.sideEffects = BuiltinSideEffect::External;
    if (matches(name, {"disp", "empty", "fprintf", "plot",
                       "rand", "randn", "table"})) {
        descriptor.implementation =
            BuiltinImplementationKind::Unsupported;
        descriptor.sideEffects = BuiltinSideEffect::None;
        descriptor.errorIdentifier = "MParser:UnsupportedBuiltin";
        descriptor.summary =
            "Recognized compatibility name without a v0.80 implementation.";
    }
    return descriptor;
}

Diagnostic contractDiagnostic(SourceSpan span, std::string message) {
    return Diagnostic{span, std::move(message),
                      "MParser:BuiltinContractViolation"};
}

bool isNumeric(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number ||
           value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isText(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::CharacterArray ||
           value.kind == RuntimeValueKind::StringArray;
}

bool matchesConstraint(const RuntimeValue& value,
                       const BuiltinArgumentConstraint& constraint) {
    switch (constraint.value) {
    case BuiltinValueConstraint::Any:
        break;
    case BuiltinValueConstraint::Numeric:
        if (!isNumeric(value)) {
            return false;
        }
        break;
    case BuiltinValueConstraint::ScalarNumeric:
        if (value.kind != RuntimeValueKind::Number) {
            return false;
        }
        break;
    case BuiltinValueConstraint::Text:
        if (!isText(value)) {
            return false;
        }
        break;
    case BuiltinValueConstraint::FunctionHandle:
        if (value.kind != RuntimeValueKind::FunctionHandle) {
            return false;
        }
        break;
    }

    switch (constraint.shape) {
    case BuiltinShapeConstraint::Any:
        return true;
    case BuiltinShapeConstraint::Scalar:
        return runtimeShapeElementCount(value) == 1;
    case BuiltinShapeConstraint::DenseArray:
        return isNumeric(value) || isText(value);
    }
    return false;
}

std::string missingContextName(
    BuiltinContextPermission permission) {
    if (permission == BuiltinContextPermission::Workspace) {
        return "workspace";
    }
    if (permission == BuiltinContextPermission::WarningState) {
        return "warning state";
    }
    if (permission ==
        BuiltinContextPermission::ObjectArrayPolicy) {
        return "object-array policy";
    }
    if (permission == BuiltinContextPermission::DynamicCall) {
        return "dynamic invoker";
    }
    if (permission ==
        BuiltinContextPermission::ExecutionControl) {
        return "execution control";
    }
    return "runtime context";
}

bool contextAvailable(const BuiltinCallContext* context,
                      BuiltinContextPermission permission) {
    if (!context) {
        return false;
    }
    if (permission == BuiltinContextPermission::Workspace) {
        return context->workspace != nullptr;
    }
    if (permission == BuiltinContextPermission::WarningState) {
        return context->warningState != nullptr;
    }
    if (permission ==
        BuiltinContextPermission::ObjectArrayPolicy) {
        return context->objectArrayPolicy != nullptr;
    }
    if (permission == BuiltinContextPermission::DynamicCall) {
        return static_cast<bool>(context->dynamicInvoker);
    }
    if (permission ==
        BuiltinContextPermission::ExecutionControl) {
        return context->executionControl != nullptr;
    }
    return true;
}

} // namespace

BuiltinArity BuiltinArity::fixed(size_t count) {
    return BuiltinArity{count, count};
}

BuiltinArity BuiltinArity::range(size_t minimum, size_t maximum) {
    return BuiltinArity{minimum, maximum};
}

BuiltinArity BuiltinArity::variadic(size_t minimum) {
    return BuiltinArity{minimum, std::nullopt};
}

bool BuiltinArity::accepts(size_t count) const {
    return count >= minimum &&
           (!maximum || count <= *maximum);
}

std::string BuiltinArity::describe() const {
    if (!maximum) {
        return "at least " + std::to_string(minimum);
    }
    if (minimum == *maximum) {
        return std::to_string(minimum);
    }
    return std::to_string(minimum) + ".." +
           std::to_string(*maximum);
}

BuiltinSideEffect operator|(BuiltinSideEffect left,
                            BuiltinSideEffect right) {
    return static_cast<BuiltinSideEffect>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

BuiltinContextPermission operator|(BuiltinContextPermission left,
                                   BuiltinContextPermission right) {
    return static_cast<BuiltinContextPermission>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

bool hasBuiltinSideEffect(BuiltinSideEffect value,
                          BuiltinSideEffect expected) {
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(expected)) != 0;
}

bool hasBuiltinContextPermission(BuiltinContextPermission value,
                                 BuiltinContextPermission expected) {
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(expected)) != 0;
}

BuiltinResult BuiltinResult::success(
    std::vector<RuntimeValue> outputs,
    std::vector<Diagnostic> diagnostics) {
    return BuiltinResult{
        true, std::move(outputs), std::move(diagnostics)};
}

BuiltinResult BuiltinResult::failure(SourceSpan span,
                                     std::string message,
                                     std::string identifier) {
    return BuiltinResult{
        false, {},
        {Diagnostic{span, std::move(message),
                    std::move(identifier)}}};
}

BuiltinRegistrationResult BuiltinRegistry::registerBuiltin(
    BuiltinDescriptor descriptor) {
    if (frozen_) {
        return {false, "builtin registry is frozen"};
    }
    if (descriptor.name.empty()) {
        return {false, "builtin name cannot be empty"};
    }
    if (descriptor.inputs.maximum &&
        descriptor.inputs.minimum > *descriptor.inputs.maximum) {
        return {false, "builtin input arity is invalid: " +
                           descriptor.name};
    }
    if (descriptor.outputs.maximum &&
        descriptor.outputs.minimum > *descriptor.outputs.maximum) {
        return {false, "builtin output arity is invalid: " +
                           descriptor.name};
    }
    if (descriptor.inputs.maximum &&
        descriptor.argumentConstraints.size() >
            *descriptor.inputs.maximum) {
        return {false, "builtin has constraints for impossible inputs: " +
                           descriptor.name};
    }
    if (descriptor.outputs.maximum &&
        descriptor.outputConstraints.size() >
            *descriptor.outputs.maximum) {
        return {false,
                "builtin has constraints for impossible outputs: " +
                    descriptor.name};
    }
    if (descriptor.errorIdentifier.empty()) {
        return {false, "builtin error identifier cannot be empty: " +
                           descriptor.name};
    }
    if (descriptors_.contains(descriptor.name) ||
        aliases_.contains(descriptor.name)) {
        return {false, "builtin name is already registered: " +
                           descriptor.name};
    }
    if ((descriptor.implementation ==
             BuiltinImplementationKind::Shared ||
         descriptor.implementation ==
             BuiltinImplementationKind::Context) &&
        !descriptor.handler) {
        return {false, "executable builtin has no handler: " +
                           descriptor.name};
    }
    if ((descriptor.implementation ==
             BuiltinImplementationKind::Intrinsic ||
         descriptor.implementation ==
             BuiltinImplementationKind::Unsupported) &&
        descriptor.handler) {
        return {false, "non-handler builtin declares a handler: " +
                           descriptor.name};
    }

    const auto contextBits = [](BuiltinContextPermission value) {
        return static_cast<std::uint32_t>(value);
    };
    if ((contextBits(descriptor.requiredContext) &
         ~contextBits(descriptor.contextPermissions)) != 0) {
        return {false, "builtin requires undeclared context: " +
                           descriptor.name};
    }
    if (descriptor.implementation ==
            BuiltinImplementationKind::Shared &&
        (descriptor.contextPermissions !=
             BuiltinContextPermission::None ||
         descriptor.requiredContext !=
             BuiltinContextPermission::None)) {
        return {false, "shared builtin declares runtime context: " +
                           descriptor.name};
    }

    if (descriptor.typedLowering != BuiltinTypedLowering::None) {
        const bool numericFirstArgument =
            !descriptor.argumentConstraints.empty() &&
            (descriptor.argumentConstraints.front().value ==
                 BuiltinValueConstraint::Numeric ||
             descriptor.argumentConstraints.front().value ==
                 BuiltinValueConstraint::ScalarNumeric);
        const bool numericFirstOutput =
            !descriptor.outputConstraints.empty() &&
            (descriptor.outputConstraints.front().value ==
                 BuiltinValueConstraint::Numeric ||
             descriptor.outputConstraints.front().value ==
                 BuiltinValueConstraint::ScalarNumeric);
        if (descriptor.implementation !=
                BuiltinImplementationKind::Shared ||
            descriptor.purity != BuiltinPurity::Pure ||
            descriptor.determinism !=
                BuiltinDeterminism::Deterministic ||
            descriptor.threadSafety !=
                BuiltinThreadSafety::Reentrant ||
            descriptor.sideEffects != BuiltinSideEffect::None ||
            descriptor.contextPermissions !=
                BuiltinContextPermission::None ||
            !descriptor.inputs.accepts(1) ||
            !descriptor.outputs.accepts(1) ||
            !numericFirstArgument || !numericFirstOutput) {
            return {false, "typed builtin metadata is not safely lowerable: " +
                               descriptor.name};
        }
    }

    std::set<std::string, std::less<>> pendingAliases;
    for (const std::string& alias : descriptor.aliases) {
        if (alias.empty()) {
            return {false, "builtin alias cannot be empty: " +
                               descriptor.name};
        }
        if (alias == descriptor.name ||
            !pendingAliases.insert(alias).second ||
            descriptors_.contains(alias) ||
            aliases_.contains(alias)) {
            return {false, "builtin alias is already registered: " +
                               alias};
        }
    }

    const std::string name = descriptor.name;
    const std::vector<std::string> aliases = descriptor.aliases;
    descriptors_.emplace(name, std::move(descriptor));
    for (const std::string& alias : aliases) {
        aliases_.emplace(alias, name);
    }
    return {true, {}};
}

const BuiltinDescriptor* BuiltinRegistry::find(
    std::string_view name) const {
    if (const auto descriptor = descriptors_.find(name);
        descriptor != descriptors_.end()) {
        return &descriptor->second;
    }
    const auto alias = aliases_.find(name);
    if (alias == aliases_.end()) {
        return nullptr;
    }
    const auto descriptor = descriptors_.find(alias->second);
    return descriptor == descriptors_.end()
               ? nullptr
               : &descriptor->second;
}

bool BuiltinRegistry::contains(std::string_view name) const {
    return find(name) != nullptr;
}

std::vector<std::string> BuiltinRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(descriptors_.size() + aliases_.size());
    for (const auto& [name, descriptor] : descriptors_) {
        (void)descriptor;
        result.push_back(name);
    }
    for (const auto& [alias, name] : aliases_) {
        (void)name;
        result.push_back(alias);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::reference_wrapper<const BuiltinDescriptor>>
BuiltinRegistry::descriptors() const {
    std::vector<std::reference_wrapper<const BuiltinDescriptor>>
        result;
    result.reserve(descriptors_.size());
    for (const auto& [name, descriptor] : descriptors_) {
        (void)name;
        result.push_back(std::cref(descriptor));
    }
    return result;
}

BuiltinResult BuiltinRegistry::invoke(
    std::string_view name, const BuiltinCall& call) const {
    const BuiltinDescriptor* descriptor = find(name);
    if (!descriptor) {
        return BuiltinResult::failure(
            call.span, "unknown builtin: " + std::string(name),
            "MParser:UnknownBuiltin");
    }
    if (descriptor->implementation ==
        BuiltinImplementationKind::Unsupported) {
        return BuiltinResult::failure(
            call.span,
            "builtin is recognized but not implemented: " +
                std::string(name),
            "MParser:UnsupportedBuiltin");
    }
    if (descriptor->implementation ==
        BuiltinImplementationKind::Intrinsic) {
        return BuiltinResult::failure(
            call.span,
            "builtin requires engine intrinsic dispatch: " +
                std::string(name),
            "MParser:BuiltinIntrinsicRequired");
    }
    if (!descriptor->inputs.accepts(call.arguments.size())) {
        return BuiltinResult::failure(
            call.span,
            std::string(name) + " expects " +
                descriptor->inputs.describe() +
                " input argument(s), received " +
                std::to_string(call.arguments.size()),
            descriptor->errorIdentifier);
    }
    if (!descriptor->outputs.accepts(
            call.requestedOutputCount)) {
        std::string expectation;
        if (descriptor->outputs.maximum &&
            descriptor->outputs.minimum == 0) {
            expectation =
                *descriptor->outputs.maximum == 1
                    ? "supports at most one output"
                    : "supports at most " +
                          std::to_string(
                              *descriptor->outputs.maximum) +
                          " outputs";
        } else if (descriptor->outputs.maximum &&
                   descriptor->outputs.minimum ==
                       *descriptor->outputs.maximum) {
            expectation =
                "expects exactly " +
                std::to_string(
                    descriptor->outputs.minimum) +
                " output value(s)";
        } else {
            expectation =
                "supports " +
                descriptor->outputs.describe() +
                " output value(s)";
        }
        return BuiltinResult::failure(
            call.span,
            std::string(name) + " " + expectation +
                ", requested " +
                std::to_string(call.requestedOutputCount),
            descriptor->errorIdentifier);
    }

    const size_t constraintCount = std::min(
        call.arguments.size(),
        descriptor->argumentConstraints.size());
    for (size_t index = 0; index < constraintCount; ++index) {
        if (!matchesConstraint(
                call.arguments[index],
                descriptor->argumentConstraints[index])) {
            return BuiltinResult::failure(
                call.span,
                std::string(name) + " argument " +
                    std::to_string(index + 1) +
                    " does not satisfy its value/shape constraint",
                descriptor->errorIdentifier);
        }
    }

    for (const auto permission : {
             BuiltinContextPermission::Workspace,
             BuiltinContextPermission::WarningState,
             BuiltinContextPermission::ObjectArrayPolicy,
             BuiltinContextPermission::DynamicCall,
             BuiltinContextPermission::ExecutionControl}) {
        if (hasBuiltinContextPermission(
                descriptor->requiredContext, permission) &&
            !contextAvailable(call.context, permission)) {
            return BuiltinResult::failure(
                call.span,
                std::string(name) + " requires a " +
                    missingContextName(permission),
                "MParser:MissingBuiltinContext");
        }
    }

    BuiltinResult result;
    try {
        result = descriptor->handler(call);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return BuiltinResult::failure(
            call.span,
            std::string(name) +
                " host handler threw an exception: " + error.what(),
            "MParser:BuiltinHostException");
    } catch (...) {
        return BuiltinResult::failure(
            call.span,
            std::string(name) +
                " host handler threw a non-standard exception",
            "MParser:BuiltinHostException");
    }

    if (!result.succeeded) {
        const bool hasError = std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            isErrorDiagnostic);
        if (!result.outputs.empty() || !hasError) {
            return BuiltinResult{
                false, {},
                {contractDiagnostic(
                    call.span,
                    std::string(name) +
                        (!result.outputs.empty()
                             ? " returned outputs after failure"
                             : " failed without an error diagnostic"))}};
        }
        return result;
    }
    if (std::any_of(result.diagnostics.begin(),
                    result.diagnostics.end(),
                    isErrorDiagnostic)) {
        return BuiltinResult{
            false, {},
            {contractDiagnostic(
                call.span,
                std::string(name) +
                    " reported an error diagnostic after success")}};
    }
    if (result.outputs.size() != call.requestedOutputCount) {
        return BuiltinResult{
            false, {},
            {contractDiagnostic(
                call.span,
                std::string(name) + " returned " +
                    std::to_string(result.outputs.size()) +
                    " output value(s), expected " +
                    std::to_string(call.requestedOutputCount))}};
    }
    for (size_t index = 0; index < result.outputs.size(); ++index) {
        if (!runtimeValueIsStorable(result.outputs[index])) {
            return BuiltinResult{
                false, {},
                {contractDiagnostic(
                    call.span,
                    std::string(name) + " output " +
                        std::to_string(index + 1) +
                        " has transient ownership")}};
        }
        const auto contract =
            validateRuntimeValueContract(result.outputs[index]);
        if (!contract.valid) {
            return BuiltinResult{
                false, {},
                {contractDiagnostic(
                    call.span,
                    std::string(name) + " output " +
                        std::to_string(index + 1) +
                        " violates RuntimeValue at " +
                        contract.path + ": " + contract.error)}};
        }
        if (index < descriptor->outputConstraints.size() &&
            !matchesConstraint(
                result.outputs[index],
                descriptor->outputConstraints[index])) {
            return BuiltinResult{
                false, {},
                {contractDiagnostic(
                    call.span,
                    std::string(name) + " output " +
                        std::to_string(index + 1) +
                        " does not satisfy its value/shape constraint")}};
        }
    }
    return result;
}

void BuiltinRegistry::freeze() {
    frozen_ = true;
}

bool BuiltinRegistry::frozen() const {
    return frozen_;
}

std::shared_ptr<BuiltinRegistry>
createBuiltinRegistryWithDefaults() {
    auto registry = std::make_shared<BuiltinRegistry>();
    for (const std::string_view name : kBuiltinNames) {
        auto registered =
            registry->registerBuiltin(descriptorFor(name));
        if (!registered.succeeded) {
            throw std::logic_error(registered.error);
        }
    }
    return registry;
}

std::shared_ptr<const BuiltinRegistry>
defaultBuiltinRegistry() {
    static const std::shared_ptr<const BuiltinRegistry> registry = [] {
        auto result = createBuiltinRegistryWithDefaults();
        result->freeze();
        return std::shared_ptr<const BuiltinRegistry>(
            std::move(result));
    }();
    return registry;
}

} // namespace mparser
