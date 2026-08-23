#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/runtime_execution_control.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string readSource(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "v1.3 standard-library sample is unavailable");
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "v1.3 standard-library source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "v1.3 standard-library source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "v1.3 standard-library source did not lower");

    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic);
    return {std::move(interpreted), std::move(executed)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing v1.3 standard-library variable: " +
                             std::string(name));
}

template <typename Result>
void verifySample(const Result& result) {
    require(result.diagnostics.empty(),
            "v1.3 standard-library sample produced a diagnostic");
    const auto& summary = variable(result, "summary");
    require(summary.kind == mparser::RuntimeValueKind::Number &&
                summary.number == 31.0,
            "v1.3 standard-library sample summary mismatch");
    require(mparser::runtimeDimensions(variable(result, "grouped_cells")) ==
                std::vector<size_t>({1, 3}),
            "num2cell grouped output shape mismatch");
    require(mparser::runtimeDimensions(variable(result, "squares")) ==
                std::vector<size_t>({2, 2}),
            "arrayfun output shape mismatch");
    require(mparser::runtimeDimensions(variable(result, "combined")) ==
                std::vector<size_t>({1, 6}),
            "union NaN output shape mismatch");
}

template <typename Result>
void requireDiagnostic(const Result& result,
                       std::string_view identifier) {
    if (std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            [identifier](const auto& diagnostic) {
                return diagnostic.identifier == identifier;
            })) {
        return;
    }
    const std::string actual = result.diagnostics.empty()
                                   ? "<none>"
                                   : result.diagnostics.front().identifier;
    throw std::runtime_error(
        "invalid v1.3 standard-library call expected " +
        std::string(identifier) + " but reported " + actual);
}

mparser::RuntimeValue exactIntegerValue(
    mparser::RuntimeNumericClass numericClass,
    std::vector<size_t> dimensions,
    const std::vector<std::uint64_t>& bits) {
    std::vector<mparser::RuntimeNumericElementValue> elements;
    elements.reserve(bits.size());
    for (const std::uint64_t value : bits) {
        mparser::RuntimeNumericElementValue element;
        element.numericClass = numericClass;
        element.integerRealBits = value;
        elements.push_back(element);
    }
    auto result = mparser::runtimeNumericValueFromElements(
        std::move(dimensions), std::move(elements), numericClass);
    require(result.has_value(),
            "exact integer test value could not be constructed");
    return std::move(*result);
}

void requireIntegerBits(
    const mparser::RuntimeValue& value,
    const std::vector<std::uint64_t>& expected,
    std::string_view message) {
    require(mparser::runtimeShapeElementCount(value) == expected.size(),
            message);
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto element = mparser::runtimeNumericElementValue(
            value, index);
        require(element && element->integerRealBits == expected[index],
                message);
    }
}

void verifyInvalidCalls() {
    const auto arrayShape = runBoth(
        "bad = arrayfun(@(x,y)x+y, [1 2 3], 10);\n");
    requireDiagnostic(arrayShape.interpreter,
                      "MParser:InvalidArrayfunShape");
    requireDiagnostic(arrayShape.vm, "MParser:InvalidArrayfunShape");

    const auto characterMatrix = runBoth(
        "bad = contains(['ab'; 'cd'], 'a');\n");
    requireDiagnostic(characterMatrix.interpreter,
                      "MParser:InvalidTextQueryInput");
    requireDiagnostic(characterMatrix.vm,
                      "MParser:InvalidTextQueryInput");

    const auto incompatibleSet = runBoth(
        "bad = union({1,2}, {2,3});\n");
    requireDiagnostic(incompatibleSet.interpreter,
                      "MParser:InvalidSetInput");
    requireDiagnostic(incompatibleSet.vm, "MParser:InvalidSetInput");

    const auto rowWidth = runBoth(
        "bad = union([1 2], [1 2 3], 'rows');\n");
    requireDiagnostic(rowWidth.interpreter,
                      "MParser:IncompatibleSetInputs");
    requireDiagnostic(rowWidth.vm,
                      "MParser:IncompatibleSetInputs");

    const auto integerMissing = runBoth(
        "bad = union(int8(1), missing);\n");
    requireDiagnostic(integerMissing.interpreter,
                      "MParser:IncompatibleSetInputs");
    requireDiagnostic(integerMissing.vm,
                      "MParser:IncompatibleSetInputs");

    const auto cellShape = runBoth(
        "bad = cell2mat({[1 2]; [3 4 5]});\n");
    requireDiagnostic(cellShape.interpreter,
                      "MParser:InconsistentCell2matBlock");
    requireDiagnostic(cellShape.vm,
                      "MParser:InconsistentCell2matBlock");
}

void verifyRegistryAndScale() {
    const auto registry = mparser::defaultBuiltinRegistry();
    for (const std::string_view name : {
             "arrayfun", "cell2mat", "contains", "endsWith",
             "int2str", "intersect", "iscellstr", "ismember",
             "mat2str", "num2cell", "setdiff", "setxor",
             "startsWith", "str2num", "union"}) {
        require(registry->contains(name),
                "v1.3 builtin is absent from the registry");
    }

    const auto* arrayfun = registry->find("arrayfun");
    require(arrayfun &&
                arrayfun->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                mparser::hasBuiltinContextPermission(
                    arrayfun->requiredContext,
                    mparser::BuiltinContextPermission::DynamicCall),
            "arrayfun dynamic-call descriptor mismatch");
    const auto* set = registry->find("union");
    require(set && set->outputs.maximum == std::optional<size_t>(3) &&
                set->purity == mparser::BuiltinPurity::Pure &&
                mparser::hasBuiltinContextPermission(
                    set->contextPermissions,
                    mparser::BuiltinContextPermission::ExecutionControl),
            "set descriptor mismatch");
    const auto* query = registry->find("contains");
    require(query && query->outputs.maximum == std::optional<size_t>(1) &&
                query->determinism ==
                    mparser::BuiltinDeterminism::Deterministic,
            "text query descriptor mismatch");

    constexpr std::uint64_t kBeyondFlintmax = 9007199254740993ULL;
    constexpr std::uint64_t kUintMaximum =
        std::numeric_limits<std::uint64_t>::max();
    auto exactFormatting = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 2},
        {kUintMaximum - 1U, kUintMaximum});
    auto result = registry->invoke(
        "int2str",
        mparser::BuiltinCall{{exactFormatting}, 1, {}, nullptr});
    const auto intText = result.succeeded && result.outputs.size() == 1
                             ? mparser::runtimeTextScalarUtf8(
                                   result.outputs.front())
                             : std::nullopt;
    require(intText &&
                *intText ==
                    "18446744073709551614  18446744073709551615",
            "int2str rounded exact uint64 values through double");
    result = registry->invoke(
        "mat2str",
        mparser::BuiltinCall{{exactFormatting}, 1, {}, nullptr});
    const auto matrixText = result.succeeded && result.outputs.size() == 1
                                ? mparser::runtimeTextScalarUtf8(
                                      result.outputs.front())
                                : std::nullopt;
    require(matrixText &&
                *matrixText ==
                    "[18446744073709551614 18446744073709551615]",
            "mat2str rounded exact uint64 values through double");
    result = registry->invoke(
        "mat2str",
        mparser::BuiltinCall{
            {exactFormatting, mparser::makeRuntimeNumberValue(20.0),
             mparser::makeRuntimeCharacterVectorUtf8("class")},
            1, {}, nullptr});
    const auto exactClassText =
        result.succeeded && result.outputs.size() == 1
            ? mparser::runtimeTextScalarUtf8(result.outputs.front())
            : std::nullopt;
    require(exactClassText &&
                *exactClassText ==
                    "uint64([0xfffffffffffffffeu64 "
                    "0xffffffffffffffffu64])",
            "mat2str class output did not protect exact uint64 values");
    result = registry->invoke(
        "str2num",
        mparser::BuiltinCall{{result.outputs.front()}, 1, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 1,
            "exact uint64 mat2str text did not parse");
    requireIntegerBits(result.outputs.front(),
                       {kUintMaximum - 1U, kUintMaximum},
                       "mat2str class roundtrip lost exact uint64 bits");

    const auto signedMinimumBits = std::bit_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::min());
    const auto signedMaximumBits = std::bit_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    auto exactSignedFormatting = exactIntegerValue(
        mparser::RuntimeNumericClass::Int64, {1, 2},
        {signedMinimumBits, signedMaximumBits});
    result = registry->invoke(
        "int2str",
        mparser::BuiltinCall{{exactSignedFormatting}, 1, {}, nullptr});
    const auto signedIntText =
        result.succeeded && result.outputs.size() == 1
            ? mparser::runtimeTextScalarUtf8(result.outputs.front())
            : std::nullopt;
    require(signedIntText &&
                *signedIntText ==
                    "-9223372036854775808  9223372036854775807",
            "int2str lost exact signed int64 boundaries");
    result = registry->invoke(
        "mat2str",
        mparser::BuiltinCall{{exactSignedFormatting}, 1, {}, nullptr});
    const auto signedMatrixText =
        result.succeeded && result.outputs.size() == 1
            ? mparser::runtimeTextScalarUtf8(result.outputs.front())
            : std::nullopt;
    require(signedMatrixText &&
                *signedMatrixText ==
                    "[-9223372036854775808 9223372036854775807]",
            "mat2str lost exact signed int64 boundaries");
    result = registry->invoke(
        "mat2str",
        mparser::BuiltinCall{
            {exactSignedFormatting,
             mparser::makeRuntimeNumberValue(20.0),
             mparser::makeRuntimeCharacterVectorUtf8("class")},
            1, {}, nullptr});
    const auto exactSignedClassText =
        result.succeeded && result.outputs.size() == 1
            ? mparser::runtimeTextScalarUtf8(result.outputs.front())
            : std::nullopt;
    require(exactSignedClassText &&
                *exactSignedClassText ==
                    "int64([0x8000000000000000s64 "
                    "0x7fffffffffffffffs64])",
            "mat2str class output did not protect exact int64 values");
    result = registry->invoke(
        "str2num",
        mparser::BuiltinCall{{result.outputs.front()}, 1, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 1,
            "exact int64 mat2str text did not parse");
    requireIntegerBits(result.outputs.front(),
                       {signedMinimumBits, signedMaximumBits},
                       "mat2str class roundtrip lost exact int64 bits");

    auto exactSetInput = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 3},
        {kBeyondFlintmax - 1U, kBeyondFlintmax,
         kBeyondFlintmax - 1U});
    auto emptyExactSet = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {0, 0}, {});
    std::vector<mparser::RuntimeValue> exactSetArguments{
        exactSetInput, emptyExactSet,
        mparser::makeRuntimeCharacterVectorUtf8("stable")};
    result = registry->invoke(
        "union",
        mparser::BuiltinCall{exactSetArguments, 3, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 3,
            "exact uint64 union failed");
    requireIntegerBits(result.outputs[0],
                       {kBeyondFlintmax - 1U, kBeyondFlintmax},
                       "exact uint64 union did not deduplicate by bits");
    require(mparser::runtimeNumericElement(result.outputs[1], 0) ==
                    std::optional<double>(1.0) &&
                mparser::runtimeNumericElement(result.outputs[1], 1) ==
                    std::optional<double>(2.0) &&
                mparser::runtimeShapeElementCount(result.outputs[2]) == 0,
            "exact uint64 union first-index outputs mismatch");

    constexpr size_t kCount = 20000;
    std::vector<double> leftValues;
    leftValues.reserve(kCount);
    for (size_t index = 0; index < kCount; ++index) {
        leftValues.push_back(static_cast<double>(index % 200));
    }
    std::vector<double> rightValues(100);
    for (size_t index = 0; index < rightValues.size(); ++index) {
        rightValues[index] = static_cast<double>(index);
    }
    auto left = mparser::runtimeNumericValueFromLogicalOrder(
        {1, kCount}, std::move(leftValues),
        mparser::RuntimeNumericClass::Double);
    const size_t rightCount = rightValues.size();
    auto right = mparser::runtimeNumericValueFromLogicalOrder(
        {1, rightCount}, std::move(rightValues),
        mparser::RuntimeNumericClass::Double);
    require(left && right, "large ismember inputs could not be built");
    std::vector<mparser::RuntimeValue> arguments{*left, *right};
    result = registry->invoke(
        "ismember", mparser::BuiltinCall{arguments, 2, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 2 &&
                mparser::runtimeDimensions(result.outputs[0]) ==
                    std::vector<size_t>({1, kCount}) &&
                mparser::runtimeNumericElement(result.outputs[0], 99) ==
                    std::optional<double>(1.0) &&
                mparser::runtimeNumericElement(result.outputs[0], 100) ==
                    std::optional<double>(0.0) &&
                mparser::runtimeNumericElement(result.outputs[1], 99) ==
                    std::optional<double>(100.0) &&
                mparser::runtimeNumericElement(result.outputs[1], 100) ==
                    std::optional<double>(0.0),
            "large ismember lookup or shape mismatch");

    std::vector<mparser::RuntimeValue> missingArguments{
        mparser::makeRuntimeMissingArrayValue({1, 1000000000U}),
        mparser::makeRuntimeMissingArrayValue({1, 2})};
    result = registry->invoke(
        "union",
        mparser::BuiltinCall{missingArguments, 1, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 1 &&
                result.outputs.front().kind ==
                    mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(result.outputs.front()) ==
                    std::vector<size_t>({1, 1000000002U}) &&
                result.outputs.front().elements.empty(),
            "shape-only missing union was materialized or lost shape");
    result = registry->invoke(
        "union",
        mparser::BuiltinCall{missingArguments, 2, {}, nullptr});
    require(!result.succeeded && !result.diagnostics.empty() &&
                result.diagnostics.front().identifier ==
                    "MParser:SetInputTooLarge",
            "large missing index materialization was not bounded");

    mparser::RuntimeCancellationToken cancellation;
    cancellation.requestCancellation();
    mparser::RuntimeExecutionControl control({}, cancellation);
    mparser::BuiltinCallContext context;
    context.executionControl = &control;
    std::vector<mparser::RuntimeValue> conversionArguments{*left};
    result = registry->invoke(
        "int2str",
        mparser::BuiltinCall{conversionArguments, 1, {}, &context});
    require(!result.succeeded && !result.diagnostics.empty() &&
                result.diagnostics.front().identifier ==
                    "MParser:ExecutionStopped",
            "conversion execution cancellation was not enforced");
    result = registry->invoke(
        "union", mparser::BuiltinCall{arguments, 1, {}, &context});
    require(!result.succeeded && !result.diagnostics.empty() &&
                result.diagnostics.front().identifier ==
                    "MParser:ExecutionStopped",
            "set execution cancellation was not enforced");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2,
                "v1.3 standard-library smoke expects the sample path");
        const auto sample = runBoth(readSource(argv[1]));
        verifySample(sample.interpreter);
        verifySample(sample.vm);
        verifyInvalidCalls();
        verifyRegistryAndScale();
        std::cout << "conversion, set, callback, and text query smoke tests "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "V1.3 standard-library smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
