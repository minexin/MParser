#include "mparser/runtime/builtins/builtin_registry.h"

#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/builtins/array/runtime_collection_builtins.h"
#include "mparser/runtime/builtins/array/runtime_set_builtins.h"
#include "mparser/runtime/builtins/callback/runtime_callback_builtins.h"
#include "mparser/runtime/builtins/categorical/runtime_categorical_builtins.h"
#include "mparser/runtime/builtins/conversion/runtime_conversion_builtins.h"
#include "mparser/runtime/builtins/datetime/runtime_datetime_builtins.h"
#include "mparser/runtime/builtins/sparse/runtime_sparse_builtins.h"
#include "mparser/runtime/builtins/table/runtime_table_builtins.h"
#include "mparser/runtime/builtins/timetable/runtime_timetable_builtins.h"
#include "mparser/runtime/builtins/numeric/runtime_advanced_numeric.h"
#include "mparser/runtime/builtins/numeric/runtime_math.h"
#include "mparser/runtime/builtins/numeric/runtime_numeric_library_builtins.h"
#include "mparser/runtime/builtins/numeric/runtime_reduction.h"
#include "mparser/runtime/builtins/numeric/runtime_scan.h"
#include "mparser/runtime/builtins/system/runtime_mat_builtins.h"
#include "mparser/runtime/builtins/system/runtime_system_builtins.h"
#include "mparser/runtime/builtins/text/runtime_text_builtins.h"
#include "mparser/runtime/builtins/text/runtime_text_query_builtins.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"
#include "mparser/runtime/core/session/runtime_warning.h"

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
    "addcats",
    "addlistener",
    "addpath",
    "addprop",
    "all",
    "any",
    "asin",
    "asinh",
    "arrayfun",
    "array2table",
    "array2timetable",
    "assignin",
    "assert",
    "atan",
    "atan2",
    "atanh",
    "cat",
    "categorical",
    "categories",
    "cell",
    "cell2mat",
    "cell2struct",
    "cellfun",
    "cellstr",
    "cd",
    "ceil",
    "char",
    "class",
    "clc",
    "clear",
    "clock",
    "complex",
    "computer",
    "conj",
    "contains",
    "countcats",
    "conv",
    "copyfile",
    "cos",
    "cosh",
    "cross",
    "cummax",
    "cummin",
    "cumprod",
    "cumsum",
    "date",
    "datetime",
    "duration",
    "NaT",
    "day",
    "delete",
    "det",
    "disp",
    "diff",
    "dir",
    "double",
    "dot",
    "eig",
    "empty",
    "endsWith",
    "enumeration",
    "eps",
    "error",
    "eval",
    "evalc",
    "evalin",
    "events",
    "exist",
    "exp",
    "eye",
    "factorial",
    "false",
    "fieldnames",
    "fft",
    "fileattrib",
    "fileparts",
    "fileread",
    "filesep",
    "find",
    "feval",
    "findobj",
    "findprop",
    "fix",
    "flip",
    "fliplr",
    "flipud",
    "floor",
    "fclose",
    "feof",
    "ferror",
    "fgetl",
    "fgets",
    "fopen",
    "format",
    "fprintf",
    "fread",
    "frewind",
    "fscanf",
    "fseek",
    "ftell",
    "fwrite",
    "full",
    "fullfile",
    "func2str",
    "functions",
    "gcd",
    "getReport",
    "getenv",
    "height",
    "horzcat",
    "hypot",
    "hour",
    "i",
    "ifft",
    "imag",
    "inf",
    "int16",
    "int32",
    "int64",
    "int8",
    "int2str",
    "inv",
    "intersect",
    "ipermute",
    "isa",
    "iscell",
    "iscategorical",
    "iscellstr",
    "isenum",
    "isempty",
    "ischar",
    "iscolumn",
    "isdatetime",
    "isduration",
    "isfield",
    "isfile",
    "isfinite",
    "isfloat",
    "isfolder",
    "isinf",
    "isinteger",
    "islogical",
    "ismember",
    "ismatrix",
    "ismethod",
    "isnumeric",
    "isordinal",
    "isprime",
    "isprotected",
    "isnan",
    "isnat",
    "isreal",
    "isrow",
    "issparse",
    "isscalar",
    "isequal",
    "isequaln",
    "ismissing",
    "isprop",
    "isstring",
    "isStringScalar",
    "isstruct",
    "istable",
    "istimetable",
    "isundefined",
    "isvalid",
    "isvector",
    "j",
    "lastwarn",
    "lcm",
    "length",
    "linspace",
    "load",
    "listener",
    "log",
    "log10",
    "log2",
    "logspace",
    "logical",
    "lower",
    "max",
    "mean",
    "mergecats",
    "median",
    "minute",
    "month",
    "meshgrid",
    "mat2str",
    "metaclass",
    "metafunction",
    "methods",
    "missing",
    "min",
    "mkdir",
    "mod",
    "movefile",
    "nan",
    "nargin",
    "nargout",
    "ndims",
    "nextpow2",
    "nnz",
    "notify",
    "norm",
    "num2str",
    "num2cell",
    "numel",
    "nonzeros",
    "ones",
    "path",
    "pathsep",
    "pause",
    "permute",
    "pi",
    "plot",
    "polyfit",
    "polyval",
    "prod",
    "properties",
    "primes",
    "pwd",
    "rand",
    "randi",
    "randn",
    "randperm",
    "rank",
    "real",
    "regexp",
    "rem",
    "removecats",
    "renamecats",
    "reordercats",
    "repmat",
    "reshape",
    "rmdir",
    "rmfield",
    "rmpath",
    "rng",
    "round",
    "save",
    "second",
    "seconds",
    "setdiff",
    "setxor",
    "sign",
    "single",
    "sin",
    "sinh",
    "size",
    "sort",
    "sortrows",
    "spalloc",
    "speye",
    "spones",
    "sparse",
    "days",
    "hours",
    "minutes",
    "sqrt",
    "squeeze",
    "sprintf",
    "std",
    "startsWith",
    "str2double",
    "str2num",
    "str2func",
    "strcmp",
    "strcmpi",
    "strfind",
    "strrep",
    "strsplit",
    "strtrim",
    "string",
    "strings",
    "strlength",
    "struct",
    "struct2cell",
    "struct2table",
    "sum",
    "system",
    "table",
    "table2array",
    "table2struct",
    "table2timetable",
    "tan",
    "tanh",
    "throw",
    "throwAsCaller",
    "tic",
    "timetable",
    "timetable2table",
    "toc",
    "tempdir",
    "tempname",
    "trace",
    "trapz",
    "true",
    "rethrow",
    "uint16",
    "uint32",
    "uint64",
    "uint8",
    "unique",
    "union",
    "upper",
    "var",
    "vertcat",
    "version",
    "warning",
    "which",
    "who",
    "whos",
    "width",
    "year",
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
            const auto message =
                builtin == "atan2" || builtin == "hypot"
                    ? builtin + " requires real floating-point inputs"
                    : builtin +
                          " requires compatible real numeric inputs";
            return helperFailure(
                call.span, message,
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

BuiltinDescriptor missingDescriptor() {
    BuiltinDescriptor descriptor = baseDescriptor("missing");
    descriptor.inputs = BuiltinArity::fixed(0);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Scalar,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        "MATLAB-like scalar missing constructor for shaped missing arrays.";
    descriptor.handler = [](const BuiltinCall& call) {
        return call.requestedOutputCount == 0
                   ? BuiltinResult::success()
                   : BuiltinResult::success(
                         {makeRuntimeMissingArrayValue()});
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
        const auto target = runtimeNumericClassFromName(builtin);
        if (!target) {
            return helperFailure(
                call.span,
                builtin + " is not a supported numeric class",
                "MParser:UnsupportedNumericClass");
        }
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
        if (builtin == "double" &&
            isRuntimeStringArray(call.arguments.front())) {
            auto result =
                runtimeConvertStringToDouble(call.arguments.front());
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
        if (builtin == "double" &&
            isRuntimeCategoricalValue(call.arguments.front())) {
            auto result = runtimeCategoricalToDouble(
                call.arguments.front());
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
        if (call.arguments.front().kind ==
            RuntimeValueKind::MissingArray) {
            if (!runtimeNumericClassIsFloating(*target)) {
                return helperFailure(
                    call.span,
                    "missing cannot convert to class " + builtin,
                    "MParser:InvalidNumericConversion");
            }
            auto converted = runtimeNumericValueFromLogicalOrder(
                runtimeDimensions(call.arguments.front()),
                std::vector<double>(
                    runtimeShapeElementCount(call.arguments.front()),
                    std::numeric_limits<double>::quiet_NaN()),
                *target);
            if (!converted) {
                return helperFailure(
                    call.span,
                    "missing value could not convert to class " + builtin,
                    "MParser:InvalidNumericConversion");
            }
            return call.requestedOutputCount == 0
                       ? BuiltinResult::success()
                       : BuiltinResult::success(
                             {std::move(*converted)});
        }
        if (!isRuntimeNumericValue(call.arguments.front())) {
            return helperFailure(
                call.span, builtin + " expects one numeric argument",
                "MParser:InvalidNumericConversion");
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

BuiltinDescriptor str2doubleDescriptor() {
    BuiltinDescriptor descriptor = baseDescriptor("str2double");
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
        "Convert string, character, or cell text to double values.";
    descriptor.handler = [](const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        auto result = runtimeStr2Double(call.arguments.front());
        if (!result.succeeded) {
            return helperFailure(
                call.span, std::move(result.error),
                "MParser:InvalidNumericConversion");
        }
        return BuiltinResult::success({std::move(result.value)});
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

bool isShapePredicateBuiltin(std::string_view name) {
    return matches(name, {"isscalar", "isvector", "isrow",
                          "iscolumn", "ismatrix"});
}

BuiltinDescriptor shapePredicateDescriptor(std::string_view name) {
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
    descriptor.summary = "MATLAB-like value shape predicate.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        const auto result = runtimeShapePredicate(
            builtin, call.arguments.front());
        return result
                   ? BuiltinResult::success(
                         {makeRuntimeLogicalValue(*result)})
                   : helperFailure(
                         call.span, "unknown shape predicate: " + builtin,
                         "MParser:InvalidBuiltinArgument");
    };
    return descriptor;
}

BuiltinDescriptor equalityDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(2);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Scalar,
    }};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.summary =
        name == "isequaln"
            ? "Deep value equality with corresponding NaNs equal."
            : "Deep MATLAB-like value equality.";
    descriptor.handler = [equalNaNs = name == "isequaln"](
                             const BuiltinCall& call) {
        if (call.requestedOutputCount == 0) {
            return BuiltinResult::success();
        }
        const bool equal = std::all_of(
            call.arguments.begin() + 1, call.arguments.end(),
            [&](const RuntimeValue& value) {
                return runtimeValuesEqual(
                    call.arguments.front(), value,
                    equalNaNs ? RuntimeNaNEquality::Equal
                              : RuntimeNaNEquality::Unequal);
            });
        return BuiltinResult::success(
            {makeRuntimeLogicalValue(equal)});
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
    if (name == "sum") {
        descriptor.typedLowering = BuiltinTypedLowering::Sum;
    } else if (name == "prod") {
        descriptor.typedLowering = BuiltinTypedLowering::Product;
    } else if (name == "mean") {
        descriptor.typedLowering = BuiltinTypedLowering::Mean;
    }
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
    } else if (name == "squeeze" || name == "flipud" ||
               name == "fliplr") {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (name == "flip") {
        descriptor.inputs = BuiltinArity::range(1, 2);
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
    if (name == "inf") {
        descriptor.aliases = {"Inf"};
    } else if (name == "nan") {
        descriptor.aliases = {"NaN"};
    }
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
    descriptor.implicitOutputPolicy =
        name == "warning"
            ? BuiltinImplicitOutputPolicy::None
            : BuiltinImplicitOutputPolicy::FirstWhenNoArguments;
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
                ? call.context->warningContext->warning(
                      call.arguments, call.requestedOutputCount)
                : call.context->warningContext->lastWarning(
                      call.arguments, call.requestedOutputCount);
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

BuiltinDescriptor outputDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = name == "disp" ? BuiltinArity::fixed(1)
                                        : BuiltinArity::variadic(1);
    descriptor.outputs = name == "disp" ? BuiltinArity::fixed(0)
                                        : BuiltinArity::range(0, 1);
    if (name != "disp") {
        descriptor.outputConstraints = {{
            name == "sprintf" ? BuiltinValueConstraint::Text
                              : BuiltinValueConstraint::Numeric,
            name == "sprintf" ? BuiltinShapeConstraint::Any
                              : BuiltinShapeConstraint::Scalar,
        }};
    }
    descriptor.implementation =
        name == "sprintf" ? BuiltinImplementationKind::Shared
                          : BuiltinImplementationKind::Context;
    descriptor.purity =
        name == "sprintf" ? BuiltinPurity::Pure
                          : BuiltinPurity::Impure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety =
        name == "sprintf" ? BuiltinThreadSafety::Reentrant
                          : BuiltinThreadSafety::ContextBound;
    if (name != "sprintf") {
        descriptor.sideEffects = BuiltinSideEffect::Console;
        descriptor.contextPermissions =
            BuiltinContextPermission::Output |
            (name == "disp"
                 ? BuiltinContextPermission::DisplayFormat
                 : BuiltinContextPermission::None);
        descriptor.requiredContext = BuiltinContextPermission::Output;
    }
    descriptor.errorIdentifier = "MParser:InvalidFormattedOutput";
    descriptor.summary =
        name == "disp"
            ? "Display one value through the host-owned output sink."
            : "Format values into a character row vector.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        RuntimeFormatResult formatted;
        if (builtin == "disp") {
            RuntimeDisplayFormat displayFormat;
            if (call.context && call.context->displayFormat &&
                call.context->displayFormat->current) {
                displayFormat = call.context->displayFormat->current();
            }
            formatted = runtimeFormatDisplay(call.arguments.front(),
                                             displayFormat);
        } else {
            formatted = runtimeFormatPrintf(call.arguments);
        }
        if (!formatted.succeeded) {
            return helperFailure(
                call.span, std::move(formatted.error),
                "MParser:InvalidFormattedOutput");
        }
        if (builtin == "sprintf") {
            return call.requestedOutputCount == 0
                       ? BuiltinResult::success()
                       : BuiltinResult::success({
                             makeRuntimeCharacterVectorUtf8(formatted.text)});
        }

        const RuntimeOutputEvent event{
            builtin == "disp" ? RuntimeOutputKind::Display
                              : RuntimeOutputKind::StandardOutput,
            formatted.text,
            call.span};
        if (!(*call.context->outputSink)(event)) {
            return helperFailure(
                call.span,
                "host output sink rejected the emitted text",
                "MParser:OutputSinkRejected");
        }
        return BuiltinResult::success();
    };
    return descriptor;
}

bool isZeroOutputIntrinsic(std::string_view name) {
    return matches(name, {"assert", "clc", "error", "notify",
                          "rethrow", "throw",
                          "throwAsCaller"});
}

BuiltinDescriptor zeroOutputIntrinsicDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.outputs = BuiltinArity::fixed(0);
    descriptor.summary =
        "Engine intrinsic that does not return a value.";
    return descriptor;
}

BuiltinDescriptor systemDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.determinism = BuiltinDeterminism::ContextDependent;
    descriptor.threadSafety = BuiltinThreadSafety::ContextBound;
    descriptor.errorIdentifier = "MParser:InvalidSystemBuiltinCall";
    descriptor.summary =
        "Session-scoped MATLAB-like system and workspace operation.";

    if (name == "clear") {
        descriptor.inputs = BuiltinArity::variadic(0);
        descriptor.outputs = BuiltinArity::fixed(0);
        descriptor.purity = BuiltinPurity::Impure;
        descriptor.sideEffects = BuiltinSideEffect::Workspace;
        descriptor.contextPermissions = BuiltinContextPermission::Workspace;
        descriptor.requiredContext = BuiltinContextPermission::Workspace;
        descriptor.implicitOutputPolicy = BuiltinImplicitOutputPolicy::None;
    } else if (name == "assignin") {
        descriptor.inputs = BuiltinArity::fixed(3);
        descriptor.outputs = BuiltinArity::fixed(0);
        descriptor.argumentConstraints = {
            BuiltinArgumentConstraint{BuiltinValueConstraint::Text,
                                      BuiltinShapeConstraint::Any},
            BuiltinArgumentConstraint{BuiltinValueConstraint::Text,
                                      BuiltinShapeConstraint::Any},
            BuiltinArgumentConstraint{BuiltinValueConstraint::Any,
                                      BuiltinShapeConstraint::Any}};
        descriptor.purity = BuiltinPurity::Impure;
        descriptor.sideEffects = BuiltinSideEffect::Workspace;
        descriptor.contextPermissions = BuiltinContextPermission::Workspace;
        descriptor.requiredContext = BuiltinContextPermission::Workspace;
        descriptor.implicitOutputPolicy = BuiltinImplicitOutputPolicy::None;
    } else if (name == "eval" || name == "evalc" ||
               name == "evalin") {
        descriptor.inputs =
            name == "evalin" ? BuiltinArity::range(2, 3)
                             : BuiltinArity::range(1, 2);
        descriptor.outputs = BuiltinArity::variadic(0);
        descriptor.argumentConstraints.assign(
            *descriptor.inputs.maximum,
            BuiltinArgumentConstraint{BuiltinValueConstraint::Text,
                                      BuiltinShapeConstraint::Any});
        descriptor.purity = BuiltinPurity::Impure;
        descriptor.sideEffects =
            BuiltinSideEffect::Workspace |
            BuiltinSideEffect::Console |
            BuiltinSideEffect::WarningState |
            BuiltinSideEffect::Time |
            BuiltinSideEffect::ObjectState |
            BuiltinSideEffect::External |
            BuiltinSideEffect::RandomState |
            BuiltinSideEffect::DisplayState;
        descriptor.contextPermissions =
            BuiltinContextPermission::SourceEvaluation;
        descriptor.requiredContext =
            BuiltinContextPermission::SourceEvaluation;
        descriptor.implicitOutputPolicy =
            name == "evalc"
                ? BuiltinImplicitOutputPolicy::FirstAvailable
                : BuiltinImplicitOutputPolicy::None;
    } else if (name == "format") {
        descriptor.inputs = BuiltinArity::range(0, 2);
        descriptor.outputs = BuiltinArity::range(0, 1);
        descriptor.purity = BuiltinPurity::Impure;
        descriptor.sideEffects = BuiltinSideEffect::DisplayState;
        descriptor.contextPermissions =
            BuiltinContextPermission::DisplayFormat;
        descriptor.requiredContext =
            BuiltinContextPermission::DisplayFormat;
        descriptor.implicitOutputPolicy = BuiltinImplicitOutputPolicy::None;
    } else if (name == "fileparts") {
        descriptor.inputs = BuiltinArity::fixed(1);
        descriptor.outputs = BuiltinArity::range(1, 3);
        descriptor.implementation = BuiltinImplementationKind::Shared;
        descriptor.purity = BuiltinPurity::Pure;
        descriptor.determinism = BuiltinDeterminism::Deterministic;
        descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
        descriptor.contextPermissions = BuiltinContextPermission::None;
        descriptor.requiredContext = BuiltinContextPermission::None;
        descriptor.sideEffects = BuiltinSideEffect::None;
    } else if (name == "fullfile" || name == "filesep" ||
               name == "pathsep") {
        descriptor.inputs = name == "fullfile"
                                ? BuiltinArity::variadic(1)
                                : BuiltinArity::fixed(0);
        descriptor.outputs = BuiltinArity::fixed(1);
        descriptor.implementation = BuiltinImplementationKind::Shared;
        descriptor.purity = BuiltinPurity::Pure;
        descriptor.determinism = BuiltinDeterminism::Deterministic;
        descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
        descriptor.contextPermissions = BuiltinContextPermission::None;
        descriptor.requiredContext = BuiltinContextPermission::None;
        descriptor.sideEffects = BuiltinSideEffect::None;
    } else if (name == "fprintf") {
        descriptor.inputs = BuiltinArity::variadic(1);
        descriptor.outputs = BuiltinArity::range(0, 1);
        descriptor.purity = BuiltinPurity::Impure;
        descriptor.sideEffects =
            BuiltinSideEffect::External | BuiltinSideEffect::Console;
        descriptor.contextPermissions =
            BuiltinContextPermission::Output |
            BuiltinContextPermission::SystemServices;
        descriptor.requiredContext = BuiltinContextPermission::None;
        descriptor.implicitOutputPolicy =
            BuiltinImplicitOutputPolicy::None;
    } else if (name == "who" || name == "whos") {
        descriptor.inputs = BuiltinArity::variadic(0);
        descriptor.outputs = BuiltinArity::range(0, 1);
        descriptor.purity = BuiltinPurity::ReadOnly;
        descriptor.sideEffects = BuiltinSideEffect::Console;
        descriptor.contextPermissions =
            BuiltinContextPermission::Workspace |
            BuiltinContextPermission::Output;
        descriptor.requiredContext = BuiltinContextPermission::Workspace;
        descriptor.implicitOutputPolicy =
            BuiltinImplicitOutputPolicy::None;
    } else if (name == "exist") {
        descriptor.inputs = BuiltinArity::range(1, 2);
        descriptor.outputs = BuiltinArity::fixed(1);
        descriptor.purity = BuiltinPurity::ReadOnly;
        descriptor.contextPermissions =
            BuiltinContextPermission::Workspace |
            BuiltinContextPermission::SystemServices;
        descriptor.requiredContext = BuiltinContextPermission::Workspace;
    } else {
        descriptor.contextPermissions =
            BuiltinContextPermission::SystemServices;
        descriptor.requiredContext =
            BuiltinContextPermission::SystemServices;
        descriptor.purity = BuiltinPurity::ReadOnly;
        descriptor.outputs = BuiltinArity::range(0, 1);

        if (name == "isfile" || name == "isfolder") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(1);
        } else if (name == "fileread") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(1);
        } else if (name == "tempname") {
            descriptor.inputs = BuiltinArity::range(0, 1);
            descriptor.outputs = BuiltinArity::fixed(1);
        } else if (name == "delete") {
            descriptor.inputs = BuiltinArity::variadic(1);
            descriptor.outputs = BuiltinArity::fixed(0);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects =
                BuiltinSideEffect::External |
                BuiltinSideEffect::WarningState |
                BuiltinSideEffect::ObjectState;
            descriptor.contextPermissions =
                descriptor.contextPermissions |
                BuiltinContextPermission::WarningState;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
            descriptor.summary =
                "MATLAB-like file deletion with VM object-delete dispatch.";
        } else if (name == "fileattrib") {
            descriptor.inputs = BuiltinArity::range(0, 4);
            descriptor.outputs = BuiltinArity::range(0, 3);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects =
                BuiltinSideEffect::External |
                BuiltinSideEffect::Console;
            descriptor.contextPermissions =
                descriptor.contextPermissions |
                BuiltinContextPermission::Output;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
            descriptor.summary =
                "Cross-platform MATLAB-like file attribute query and update.";
        } else if (name == "mkdir" || name == "rmdir") {
            descriptor.inputs = BuiltinArity::range(1, 2);
            descriptor.outputs = BuiltinArity::range(0, 3);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "copyfile" || name == "movefile") {
            descriptor.inputs = BuiltinArity::range(1, 3);
            descriptor.outputs = BuiltinArity::range(0, 3);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "fopen") {
            descriptor.inputs = BuiltinArity::range(1, 4);
            descriptor.outputs = BuiltinArity::range(0, 4);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fclose") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::range(0, 1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "feof") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(1);
        } else if (name == "ferror") {
            descriptor.inputs = BuiltinArity::range(1, 2);
            descriptor.outputs = BuiltinArity::range(1, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fgetl") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fgets") {
            descriptor.inputs = BuiltinArity::range(1, 2);
            descriptor.outputs = BuiltinArity::range(1, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fread") {
            descriptor.inputs = BuiltinArity::range(1, 5);
            descriptor.outputs = BuiltinArity::range(1, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fwrite") {
            descriptor.inputs = BuiltinArity::range(2, 5);
            descriptor.outputs = BuiltinArity::range(0, 1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fscanf") {
            descriptor.inputs = BuiltinArity::range(2, 3);
            descriptor.outputs = BuiltinArity::range(1, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "fseek") {
            descriptor.inputs = BuiltinArity::fixed(3);
            descriptor.outputs = BuiltinArity::range(0, 1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "ftell") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(1);
        } else if (name == "frewind") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::fixed(0);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "path") {
            descriptor.inputs = BuiltinArity::range(0, 1);
            descriptor.purity = BuiltinPurity::Contextual;
            descriptor.sideEffects = BuiltinSideEffect::External;
        } else if (name == "addpath" || name == "rmpath") {
            descriptor.inputs = BuiltinArity::variadic(1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::External;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "pwd" || name == "tempdir" ||
                   name == "date" || name == "clock" ||
                   name == "version") {
            descriptor.inputs = BuiltinArity::fixed(0);
        } else if (name == "computer") {
            descriptor.inputs = BuiltinArity::fixed(0);
            descriptor.outputs = BuiltinArity::range(0, 3);
            descriptor.determinism = BuiltinDeterminism::Deterministic;
        } else if (name == "cd" || name == "dir") {
            descriptor.inputs = BuiltinArity::range(0, 1);
            if (name == "cd") {
                descriptor.purity = BuiltinPurity::Impure;
                descriptor.sideEffects = BuiltinSideEffect::External;
            } else {
                descriptor.sideEffects = BuiltinSideEffect::Console;
                descriptor.implicitOutputPolicy =
                    BuiltinImplicitOutputPolicy::None;
                descriptor.contextPermissions =
                    descriptor.contextPermissions |
                    BuiltinContextPermission::Output;
            }
        } else if (name == "getenv" || name == "which") {
            descriptor.inputs = BuiltinArity::fixed(1);
            if (name == "which") {
                descriptor.contextPermissions =
                    descriptor.contextPermissions |
                    BuiltinContextPermission::Workspace;
            }
        } else if (name == "pause") {
            descriptor.inputs = BuiltinArity::range(0, 1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects = BuiltinSideEffect::Time;
            descriptor.contextPermissions =
                descriptor.contextPermissions |
                BuiltinContextPermission::ExecutionControl;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        } else if (name == "rand" || name == "randn") {
            descriptor.inputs = BuiltinArity::variadic(0);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.determinism =
                BuiltinDeterminism::Nondeterministic;
            descriptor.sideEffects = BuiltinSideEffect::RandomState;
        } else if (name == "randi") {
            descriptor.inputs = BuiltinArity::variadic(1);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.determinism =
                BuiltinDeterminism::Nondeterministic;
            descriptor.sideEffects = BuiltinSideEffect::RandomState;
        } else if (name == "randperm") {
            descriptor.inputs = BuiltinArity::range(1, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.determinism =
                BuiltinDeterminism::Nondeterministic;
            descriptor.sideEffects = BuiltinSideEffect::RandomState;
            descriptor.contextPermissions =
                descriptor.contextPermissions |
                BuiltinContextPermission::ExecutionControl;
        } else if (name == "rng") {
            descriptor.inputs = BuiltinArity::range(0, 2);
            descriptor.purity = BuiltinPurity::Contextual;
            descriptor.sideEffects = BuiltinSideEffect::RandomState;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::FirstWhenNoArguments;
        } else if (name == "system") {
            descriptor.inputs = BuiltinArity::fixed(1);
            descriptor.outputs = BuiltinArity::range(0, 2);
            descriptor.purity = BuiltinPurity::Impure;
            descriptor.sideEffects =
                BuiltinSideEffect::External |
                BuiltinSideEffect::Console;
            descriptor.contextPermissions =
                descriptor.contextPermissions |
                BuiltinContextPermission::Output;
            descriptor.implicitOutputPolicy =
                BuiltinImplicitOutputPolicy::None;
        }
    }

    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeSystemBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor matFileDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(0);
    descriptor.outputs = name == "save" ? BuiltinArity::fixed(0)
                                        : BuiltinArity::range(0, 1);
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Impure;
    descriptor.determinism = BuiltinDeterminism::ContextDependent;
    descriptor.threadSafety = BuiltinThreadSafety::ContextBound;
    descriptor.sideEffects = BuiltinSideEffect::External |
                             (name == "load"
                                  ? BuiltinSideEffect::Workspace
                                  : BuiltinSideEffect::None);
    descriptor.contextPermissions =
        BuiltinContextPermission::Workspace |
        BuiltinContextPermission::SystemServices;
    descriptor.requiredContext = descriptor.contextPermissions;
    descriptor.implicitOutputPolicy = BuiltinImplicitOutputPolicy::None;
    descriptor.errorIdentifier = "MParser:InvalidMatFileCall";
    descriptor.summary =
        "Session-scoped MATLAB MAT v5 workspace persistence operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeMatBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor textLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "lower" || name == "upper" || name == "strtrim") {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (name == "num2str") {
        descriptor.inputs = BuiltinArity::range(1, 2);
    } else if (name == "regexp") {
        descriptor.inputs = BuiltinArity::variadic(2);
    } else if (name == "strfind") {
        descriptor.inputs = BuiltinArity::fixed(2);
    } else if (name == "strrep") {
        descriptor.inputs = BuiltinArity::fixed(3);
    } else {
        descriptor.inputs = BuiltinArity::variadic(1);
    }
    descriptor.outputs = BuiltinArity::range(
        0, name == "strsplit" ? 2 : name == "regexp" ? 5 : 1);
    if (name == "strfind" || name == "strrep") {
        descriptor.argumentConstraints.assign(
            *descriptor.inputs.maximum,
            BuiltinArgumentConstraint{BuiltinValueConstraint::Text,
                                      BuiltinShapeConstraint::Any});
        descriptor.argumentConstraints.front().value =
            BuiltinValueConstraint::Any;
    } else {
        descriptor.argumentConstraints = {{
            name == "num2str" ? BuiltinValueConstraint::Numeric
                               : BuiltinValueConstraint::Text,
            BuiltinShapeConstraint::Any,
        }};
    }
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{BuiltinValueConstraint::Any,
                                BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidTextBuiltinCall";
    descriptor.summary =
        "MATLAB-like text transformation, conversion, splitting, or "
        "regular-expression operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeTextLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor numericLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "factorial" || name == "isprime" || name == "primes") {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (name == "gcd" || name == "lcm") {
        descriptor.inputs = BuiltinArity::fixed(2);
    } else if (name == "logspace") {
        descriptor.inputs = BuiltinArity::range(2, 3);
    } else {
        descriptor.inputs = BuiltinArity::range(1, 3);
    }
    descriptor.outputs = BuiltinArity::range(
        0, name == "meshgrid" ? 3 : 1);
    descriptor.argumentConstraints.assign(
        *descriptor.inputs.maximum,
        BuiltinArgumentConstraint{BuiltinValueConstraint::Numeric,
                                  BuiltinShapeConstraint::Any});
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{BuiltinValueConstraint::Numeric,
                                BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidNumericLibraryCall";
    descriptor.summary =
        "MATLAB-like number theory, logarithmic spacing, or grid operation.";
    descriptor.contextPermissions =
        BuiltinContextPermission::ExecutionControl;
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeNumericLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor advancedNumericDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (matches(name, {"det", "inv", "trace"})) {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (matches(name, {"norm", "rank"})) {
        descriptor.inputs = BuiltinArity::range(1, 2);
    } else if (name == "eig") {
        descriptor.inputs = BuiltinArity::fixed(1);
    } else if (matches(name, {"dot", "cross", "conv"})) {
        descriptor.inputs = BuiltinArity::range(2, 3);
    } else if (name == "polyval") {
        descriptor.inputs = BuiltinArity::fixed(2);
    } else if (matches(name, {"fft", "ifft", "trapz"})) {
        descriptor.inputs = BuiltinArity::range(1, 3);
    } else if (name == "polyfit") {
        descriptor.inputs = BuiltinArity::fixed(3);
    } else {
        descriptor.inputs = BuiltinArity::range(1, 4);
    }
    descriptor.outputs = BuiltinArity::range(
        0, name == "eig" ? 2 : name == "polyfit" ? 3 : 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.minimum,
        BuiltinArgumentConstraint{BuiltinValueConstraint::Numeric,
                                  BuiltinShapeConstraint::Any});
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{BuiltinValueConstraint::Any,
                                BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.contextPermissions =
        BuiltinContextPermission::ExecutionControl;
    descriptor.errorIdentifier =
        "MParser:InvalidAdvancedNumericCall";
    descriptor.summary =
        "MATLAB-like statistics, dense linear algebra, polynomial, "
        "convolution, or Fourier operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeAdvancedNumericBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor collectionLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "iscell" || name == "struct2cell") {
        descriptor.inputs = BuiltinArity::fixed(1);
        descriptor.outputs = BuiltinArity::range(0, 1);
    } else if (name == "cell2struct") {
        descriptor.inputs = BuiltinArity::range(2, 3);
        descriptor.outputs = BuiltinArity::range(0, 1);
    } else if (name == "sort") {
        descriptor.inputs = BuiltinArity::variadic(1);
        descriptor.outputs = BuiltinArity::range(0, 2);
    } else if (name == "unique") {
        descriptor.inputs = BuiltinArity::variadic(1);
        descriptor.outputs = BuiltinArity::range(0, 3);
    } else {
        descriptor.inputs = BuiltinArity::variadic(2);
        descriptor.outputs = BuiltinArity::variadic(0);
    }
    descriptor.implementation = name == "cellfun"
                                    ? BuiltinImplementationKind::Context
                                    : BuiltinImplementationKind::Shared;
    descriptor.purity = name == "cellfun" ? BuiltinPurity::Impure
                                           : BuiltinPurity::Pure;
    descriptor.determinism =
        name == "cellfun" ? BuiltinDeterminism::ContextDependent
                          : BuiltinDeterminism::Deterministic;
    descriptor.threadSafety =
        name == "cellfun" ? BuiltinThreadSafety::ContextBound
                          : BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidCollectionBuiltinCall";
    descriptor.summary =
        "MATLAB-like sorting, uniqueness, Cell, or structure operation.";
    if (descriptor.outputs.maximum) {
        descriptor.outputConstraints.assign(
            *descriptor.outputs.maximum,
            BuiltinOutputConstraint{BuiltinValueConstraint::Any,
                                    BuiltinShapeConstraint::Any});
    }
    if (name == "cellfun") {
        descriptor.sideEffects = BuiltinSideEffect::External;
        descriptor.contextPermissions =
            BuiltinContextPermission::DynamicCall |
            BuiltinContextPermission::ObjectArrayPolicy |
            BuiltinContextPermission::ExecutionControl;
        descriptor.requiredContext =
            BuiltinContextPermission::DynamicCall;
    }
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeCollectionLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor conversionLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "mat2str") {
        descriptor.inputs = BuiltinArity::range(1, 3);
    } else if (name == "num2cell") {
        descriptor.inputs = BuiltinArity::range(1, 2);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {{
        name == "int2str" ? BuiltinValueConstraint::Numeric
        : name == "str2num" ? BuiltinValueConstraint::Text
                             : BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Any,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidConversionBuiltinCall";
    descriptor.summary =
        "MATLAB-like numeric/text representation or Cell conversion.";
    descriptor.contextPermissions =
        BuiltinContextPermission::ExecutionControl;
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeConversionLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor callbackLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(2);
    descriptor.outputs = BuiltinArity::variadic(0);
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Impure;
    descriptor.determinism = BuiltinDeterminism::ContextDependent;
    descriptor.threadSafety = BuiltinThreadSafety::ContextBound;
    descriptor.sideEffects = BuiltinSideEffect::External;
    descriptor.contextPermissions =
        BuiltinContextPermission::DynamicCall |
        BuiltinContextPermission::ObjectArrayPolicy |
        BuiltinContextPermission::ExecutionControl;
    descriptor.requiredContext = BuiltinContextPermission::DynamicCall;
    descriptor.errorIdentifier = "MParser:InvalidCallbackBuiltinCall";
    descriptor.summary =
        "MATLAB-like element-wise callback mapping over dense arrays.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeCallbackLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor setLibraryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(2);
    descriptor.outputs = BuiltinArity::range(
        0, name == "ismember" || name == "setdiff" ? 2 : 3);
    descriptor.argumentConstraints = {
        {BuiltinValueConstraint::Any, BuiltinShapeConstraint::Any},
        {BuiltinValueConstraint::Any, BuiltinShapeConstraint::Any},
    };
    descriptor.outputConstraints.assign(
        *descriptor.outputs.maximum,
        BuiltinOutputConstraint{BuiltinValueConstraint::Any,
                                BuiltinShapeConstraint::Any});
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.contextPermissions =
        BuiltinContextPermission::ExecutionControl;
    descriptor.errorIdentifier = "MParser:InvalidSetBuiltinCall";
    descriptor.summary =
        "MATLAB-like membership and set operation over dense values or "
        "rows.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeSetLibraryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor textQueryDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.inputs = BuiltinArity::variadic(2);
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints = {
        {BuiltinValueConstraint::Any, BuiltinShapeConstraint::Any},
        {BuiltinValueConstraint::Any, BuiltinShapeConstraint::Any},
    };
    descriptor.outputConstraints = {{
        BuiltinValueConstraint::Numeric,
        BuiltinShapeConstraint::Any,
    }};
    descriptor.implementation = BuiltinImplementationKind::Context;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.contextPermissions =
        BuiltinContextPermission::ExecutionControl;
    descriptor.errorIdentifier = "MParser:InvalidTextQueryCall";
    descriptor.summary =
        "MATLAB-like contains, startsWith, or endsWith text query.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeTextQueryBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor temporalDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "datetime") {
        descriptor.inputs = BuiltinArity::range(1, 6);
    } else if (name == "duration") {
        descriptor.inputs = BuiltinArity::fixed(3);
    } else if (name == "NaT") {
        descriptor.inputs = BuiltinArity::variadic(0);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.maximum.value_or(descriptor.inputs.minimum),
        BuiltinArgumentConstraint{BuiltinValueConstraint::Any,
                                  BuiltinShapeConstraint::Any});
    descriptor.outputConstraints = {{BuiltinValueConstraint::Any,
                                    BuiltinShapeConstraint::Any}};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidTemporalCall";
    descriptor.summary =
        "Native MATLAB-like datetime/duration construction, component, "
        "unit, and missing-value operation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeDateTimeBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor sparseDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "sparse") {
        descriptor.inputs = BuiltinArity::range(1, 6);
    } else if (name == "spalloc") {
        descriptor.inputs = BuiltinArity::fixed(3);
    } else if (name == "speye") {
        descriptor.inputs = BuiltinArity::range(1, 2);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.maximum.value_or(descriptor.inputs.minimum),
        BuiltinArgumentConstraint{
            name == "issparse" ? BuiltinValueConstraint::Any
                               : BuiltinValueConstraint::Numeric,
                                  BuiltinShapeConstraint::Any});
    if (name == "nnz" || name == "issparse") {
        descriptor.outputConstraints = {{BuiltinValueConstraint::Numeric,
                                         BuiltinShapeConstraint::Scalar}};
    } else if (name == "full" || name == "nonzeros") {
        descriptor.outputConstraints = {{BuiltinValueConstraint::Numeric,
                                         BuiltinShapeConstraint::Any}};
    } else {
        descriptor.outputConstraints = {{BuiltinValueConstraint::Any,
                                         BuiltinShapeConstraint::Any}};
    }
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidSparseCall";
    descriptor.summary =
        "Native C++ CSC sparse construction, inspection, and dense fallback.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeSparseBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor categoricalDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "categorical") {
        descriptor.inputs = BuiltinArity::variadic(1);
    } else if (name == "addcats") {
        descriptor.inputs = BuiltinArity::range(2, 4);
    } else if (name == "removecats" || name == "reordercats") {
        descriptor.inputs = BuiltinArity::fixed(2);
    } else if (name == "renamecats" || name == "mergecats") {
        descriptor.inputs = BuiltinArity::fixed(3);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.maximum.value_or(descriptor.inputs.minimum),
        BuiltinArgumentConstraint{BuiltinValueConstraint::Any,
                                  BuiltinShapeConstraint::Any});
    if (name == "iscategorical" || name == "isundefined" ||
        name == "isordinal" || name == "isprotected") {
        descriptor.outputConstraints = {
            {BuiltinValueConstraint::Numeric,
             name == "isundefined" ? BuiltinShapeConstraint::Any
                                     : BuiltinShapeConstraint::Scalar}};
    } else if (name == "countcats") {
        descriptor.outputConstraints = {
            {BuiltinValueConstraint::Numeric,
             BuiltinShapeConstraint::Any}};
    } else {
        descriptor.outputConstraints = {
            {BuiltinValueConstraint::Any,
             BuiltinShapeConstraint::Any}};
    }
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidCategoricalCall";
    descriptor.summary =
        "Native C++ categorical dictionary, undefined, indexing, and category management.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeCategoricalBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor tableDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "table") {
        descriptor.inputs = BuiltinArity::variadic();
    } else if (name == "sortrows") {
        descriptor.inputs = BuiltinArity::variadic(1);
    } else if (name == "array2table" || name == "struct2table") {
        descriptor.inputs = BuiltinArity::variadic(1);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = name == "sortrows"
                             ? BuiltinArity::range(0, 2)
                             : BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.maximum.value_or(descriptor.inputs.minimum),
        BuiltinArgumentConstraint{BuiltinValueConstraint::Any,
                                  BuiltinShapeConstraint::Any});
    if (name == "height" || name == "width" || name == "istable") {
        descriptor.outputConstraints = {{BuiltinValueConstraint::Numeric,
                                         BuiltinShapeConstraint::Scalar}};
    } else {
        descriptor.outputConstraints = {{BuiltinValueConstraint::Any,
                                         BuiltinShapeConstraint::Any}};
    }
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidTableCall";
    descriptor.summary =
        "Native C++ table construction, shape query, indexing, and conversion.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeTableBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor timetableDescriptor(std::string_view name) {
    BuiltinDescriptor descriptor = baseDescriptor(name);
    if (name == "timetable") {
        descriptor.inputs = BuiltinArity::variadic();
    } else if (name == "array2timetable" ||
               name == "table2timetable" ||
               name == "timetable2table") {
        descriptor.inputs = BuiltinArity::variadic(1);
    } else {
        descriptor.inputs = BuiltinArity::fixed(1);
    }
    descriptor.outputs = BuiltinArity::range(0, 1);
    descriptor.argumentConstraints.assign(
        descriptor.inputs.maximum.value_or(descriptor.inputs.minimum),
        BuiltinArgumentConstraint{BuiltinValueConstraint::Any,
                                  BuiltinShapeConstraint::Any});
    descriptor.outputConstraints = name == "istimetable"
        ? std::vector<BuiltinOutputConstraint>{
              {BuiltinValueConstraint::Numeric,
               BuiltinShapeConstraint::Scalar}}
        : std::vector<BuiltinOutputConstraint>{
              {BuiltinValueConstraint::Any,
               BuiltinShapeConstraint::Any}};
    descriptor.implementation = BuiltinImplementationKind::Shared;
    descriptor.purity = BuiltinPurity::Pure;
    descriptor.determinism = BuiltinDeterminism::Deterministic;
    descriptor.threadSafety = BuiltinThreadSafety::Reentrant;
    descriptor.errorIdentifier = "MParser:InvalidTimetableCall";
    descriptor.summary =
        "Native C++ timetable RowTimes, indexing, conversion, and concatenation.";
    descriptor.handler = [builtin = std::string(name)](
                             const BuiltinCall& call) {
        return invokeRuntimeTimetableBuiltin(builtin, call);
    };
    return descriptor;
}

BuiltinDescriptor descriptorFor(std::string_view name) {
    if (isRuntimeSystemBuiltin(name)) {
        return systemDescriptor(name);
    }
    if (isRuntimeMatBuiltin(name)) {
        return matFileDescriptor(name);
    }
    if (isRuntimeTextLibraryBuiltin(name)) {
        return textLibraryDescriptor(name);
    }
    if (isRuntimeCollectionLibraryBuiltin(name)) {
        return collectionLibraryDescriptor(name);
    }
    if (isRuntimeConversionLibraryBuiltin(name)) {
        return conversionLibraryDescriptor(name);
    }
    if (isRuntimeCallbackLibraryBuiltin(name)) {
        return callbackLibraryDescriptor(name);
    }
    if (isRuntimeSetLibraryBuiltin(name)) {
        return setLibraryDescriptor(name);
    }
    if (isRuntimeTextQueryBuiltin(name)) {
        return textQueryDescriptor(name);
    }
    if (isRuntimeDateTimeBuiltin(name)) {
        return temporalDescriptor(name);
    }
    if (isRuntimeSparseBuiltin(name)) {
        return sparseDescriptor(name);
    }
    if (isRuntimeCategoricalBuiltin(name)) {
        return categoricalDescriptor(name);
    }
    if (isRuntimeTableBuiltin(name)) {
        return tableDescriptor(name);
    }
    if (isRuntimeTimetableBuiltin(name)) {
        return timetableDescriptor(name);
    }
    if (isRuntimeNumericLibraryBuiltin(name)) {
        return numericLibraryDescriptor(name);
    }
    if (isRuntimeAdvancedNumericBuiltin(name)) {
        return advancedNumericDescriptor(name);
    }
    if (name == "missing") {
        return missingDescriptor();
    }
    if (isNumericConversionBuiltin(name)) {
        return numericConversionDescriptor(name);
    }
    if (name == "str2double") {
        return str2doubleDescriptor();
    }
    if (isNumericPredicateBuiltin(name)) {
        return numericPredicateDescriptor(name);
    }
    if (isShapePredicateBuiltin(name)) {
        return shapePredicateDescriptor(name);
    }
    if (name == "isequal" || name == "isequaln") {
        return equalityDescriptor(name);
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
    if (name == "disp" || name == "sprintf") {
        return outputDescriptor(name);
    }
    if (isZeroOutputIntrinsic(name)) {
        return zeroOutputIntrinsicDescriptor(name);
    }

    BuiltinDescriptor descriptor = baseDescriptor(name);
    descriptor.sideEffects = BuiltinSideEffect::External;
    if (matches(name, {"empty", "plot", "rand", "randn"})) {
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
    return isRuntimeNumericValue(value);
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
    if (permission == BuiltinContextPermission::Output) {
        return "output sink";
    }
    if (permission == BuiltinContextPermission::SystemServices) {
        return "system-services context";
    }
    if (permission == BuiltinContextPermission::DisplayFormat) {
        return "display-format state";
    }
    if (permission == BuiltinContextPermission::SourceEvaluation) {
        return "source evaluator";
    }
    return "runtime context";
}

bool contextAvailable(const BuiltinCallContext* context,
                      BuiltinContextPermission permission) {
    if (!context) {
        return false;
    }
    if (permission == BuiltinContextPermission::Workspace) {
        return context->workspace != nullptr &&
               context->workspace->variables != nullptr;
    }
    if (permission == BuiltinContextPermission::WarningState) {
        return context->warningContext != nullptr;
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
    if (permission == BuiltinContextPermission::Output) {
        return context->outputSink != nullptr &&
               static_cast<bool>(*context->outputSink);
    }
    if (permission == BuiltinContextPermission::SystemServices) {
        return context->systemContext != nullptr;
    }
    if (permission == BuiltinContextPermission::DisplayFormat) {
        return context->displayFormat != nullptr &&
               static_cast<bool>(context->displayFormat->current) &&
               static_cast<bool>(context->displayFormat->replace);
    }
    if (permission == BuiltinContextPermission::SourceEvaluation) {
        return static_cast<bool>(context->sourceEvaluator);
    }
    return true;
}

} // namespace

bool builtinTypedLoweringIsElementwiseUnary(
    BuiltinTypedLowering lowering) {
    switch (lowering) {
    case BuiltinTypedLowering::Absolute:
    case BuiltinTypedLowering::ArcCosine:
    case BuiltinTypedLowering::ArcSine:
    case BuiltinTypedLowering::ArcTangent:
    case BuiltinTypedLowering::Cosine:
    case BuiltinTypedLowering::Exponential:
    case BuiltinTypedLowering::Logarithm:
    case BuiltinTypedLowering::Sine:
    case BuiltinTypedLowering::SquareRoot:
    case BuiltinTypedLowering::Tangent:
        return true;
    case BuiltinTypedLowering::None:
    case BuiltinTypedLowering::Sum:
    case BuiltinTypedLowering::Product:
    case BuiltinTypedLowering::Mean:
        return false;
    }
    return false;
}

bool builtinTypedLoweringIsReduction(BuiltinTypedLowering lowering) {
    return lowering == BuiltinTypedLowering::Sum ||
           lowering == BuiltinTypedLowering::Product ||
           lowering == BuiltinTypedLowering::Mean;
}

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

size_t BuiltinCall::callerNargout() const {
    return callerOutputCount.value_or(requestedOutputCount);
}

size_t BuiltinDescriptor::implicitOutputCount(
    size_t suppliedInputCount) const {
    switch (implicitOutputPolicy) {
    case BuiltinImplicitOutputPolicy::None:
        return 0;
    case BuiltinImplicitOutputPolicy::FirstWhenNoArguments:
        if (suppliedInputCount != 0) {
            return 0;
        }
        break;
    case BuiltinImplicitOutputPolicy::FirstAvailable:
        break;
    }
    if (outputs.accepts(1)) {
        return 1;
    }
    return outputs.accepts(0) ? 0 : 1;
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
    if (descriptor.implicitOutputPolicy ==
            BuiltinImplicitOutputPolicy::None &&
        !descriptor.outputs.accepts(0)) {
        return {false, "builtin implicit output policy requires zero-output "
                       "support: " + descriptor.name};
    }
    if (descriptor.implicitOutputPolicy ==
            BuiltinImplicitOutputPolicy::FirstWhenNoArguments &&
        (!descriptor.inputs.accepts(0) ||
         !descriptor.outputs.accepts(0) ||
         !descriptor.outputs.accepts(1))) {
        return {false, "builtin query/set implicit output policy is "
                       "incompatible with its arity: " + descriptor.name};
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
        const bool loweringContractSupported =
            builtinTypedLoweringIsElementwiseUnary(
                descriptor.typedLowering) ||
            builtinTypedLoweringIsReduction(
                descriptor.typedLowering);
        if (!loweringContractSupported ||
            descriptor.implementation !=
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
             BuiltinContextPermission::ExecutionControl,
             BuiltinContextPermission::Output,
             BuiltinContextPermission::SystemServices,
             BuiltinContextPermission::DisplayFormat,
             BuiltinContextPermission::SourceEvaluation}) {
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
