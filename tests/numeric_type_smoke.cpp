#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/embedding/machine_protocol.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/semantic/semantic.h"
#include "mparser/execution/jit/typed_ir.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
    mparser::BytecodeProgram bytecode;
    mparser::SemanticResult semantic;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "numeric-type source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "numeric-type source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "numeric-type source did not lower");

    mparser::Interpreter interpreter;
    auto interpreterResult = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto vmResult = vm.run(bytecode, semantic);
    return RuntimePair{std::move(interpreterResult),
                       std::move(vmResult), std::move(bytecode),
                       std::move(semantic)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing numeric-type variable: " +
                             std::string(name));
}

template <typename Result>
bool hasDiagnostic(const Result& result, std::string_view fragment) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void requireScalar(const mparser::RuntimeValue& value,
                   mparser::RuntimeNumericClass numericClass,
                   double expected, std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Number, context);
    require(value.numericClass == numericClass, context);
    require(std::fabs(value.number - expected) < 1e-7, context);
}

void requireArray(const mparser::RuntimeValue& value,
                  mparser::RuntimeNumericClass numericClass,
                  const std::vector<double>& expected,
                  std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Vector ||
                value.kind == mparser::RuntimeValueKind::Matrix,
            context);
    require(value.numericClass == numericClass, context);
    require(value.elements == expected, context);
}

void requireIntegerBits(
    const mparser::RuntimeValue& value,
    const std::vector<std::uint64_t>& expected,
    std::string_view context) {
    require(mparser::runtimeNumericClassIsInteger(value.numericClass),
            context);
    require(mparser::runtimeShapeElementCount(value) == expected.size(),
            context);
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto element =
            mparser::runtimeNumericElementValue(value, index);
        require(element &&
                    element->integerRealBits == expected[index],
                context);
    }
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
    require(result.has_value(), "exact integer value could not be constructed");
    return std::move(*result);
}

template <typename Result>
void verify(const Result& result) {
    require(result.diagnostics.empty(),
            "numeric-type execution emitted diagnostics");

    requireScalar(variable(result, "single_value"),
                  mparser::RuntimeNumericClass::Single,
                  static_cast<double>(static_cast<float>(0.1)),
                  "single conversion did not round to binary32");
    requireArray(variable(result, "int8_values"),
                 mparser::RuntimeNumericClass::Int8,
                 {-128, -128, -128, -2, -1, 1, 2, 127, 127, 0},
                 "int8 conversion did not match MATLAB rounding/saturation");
    requireArray(variable(result, "uint8_values"),
                 mparser::RuntimeNumericClass::UInt8,
                 {0, 0, 0, 1, 2, 128, 255, 0},
                 "uint8 conversion did not match MATLAB rounding/saturation");
    requireArray(variable(result, "int16_values"),
                 mparser::RuntimeNumericClass::Int16,
                 {-32768, -32768, 32767, 32767},
                 "int16 conversion did not saturate");
    requireArray(variable(result, "uint16_values"),
                 mparser::RuntimeNumericClass::UInt16,
                 {0, 0, 65535, 65535},
                 "uint16 conversion did not saturate");
    requireArray(variable(result, "int32_values"),
                 mparser::RuntimeNumericClass::Int32,
                 {-2147483648.0, -2147483648.0,
                  2147483647.0, 2147483647.0},
                 "int32 conversion did not saturate");
    requireArray(variable(result, "uint32_values"),
                 mparser::RuntimeNumericClass::UInt32,
                 {0, 0, 4294967295.0, 4294967295.0},
                 "uint32 conversion did not saturate");
    requireScalar(variable(result, "hex_default"),
                  mparser::RuntimeNumericClass::UInt8, 42.0,
                  "default hexadecimal literal class mismatch");
    requireScalar(variable(result, "hex_widened"),
                  mparser::RuntimeNumericClass::UInt16, 256.0,
                  "widened hexadecimal literal class mismatch");
    requireScalar(variable(result, "binary_default"),
                  mparser::RuntimeNumericClass::UInt8, 42.0,
                  "default binary literal class mismatch");
    requireIntegerBits(
        variable(result, "signed_hex"),
        {std::numeric_limits<std::uint64_t>::max()},
        "signed hexadecimal literal did not sign-extend");
    requireIntegerBits(
        variable(result, "signed_min8"),
        {std::bit_cast<std::uint64_t>(std::int64_t{-128})},
        "signed binary int8 literal did not sign-extend");
    requireIntegerBits(
        variable(result, "signed_min16"),
        {std::bit_cast<std::uint64_t>(std::int64_t{-32768})},
        "signed hexadecimal int16 literal did not sign-extend");
    requireIntegerBits(
        variable(result, "signed_min32"),
        {std::bit_cast<std::uint64_t>(
            std::int64_t{std::numeric_limits<std::int32_t>::min()})},
        "signed hexadecimal int32 literal did not sign-extend");
    requireIntegerBits(
        variable(result, "signed_min64"),
        {std::bit_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::min())},
        "signed hexadecimal int64 literal mismatch");
    requireIntegerBits(
        variable(result, "maximum_hex"),
        {std::numeric_limits<std::uint64_t>::max()},
        "uint64 hexadecimal literal lost exact bits");
    requireIntegerBits(variable(result, "base_vector"),
                       {1, 2, 65535},
                       "base-prefixed literal vector lost exact bits");
    requireIntegerBits(
        variable(result, "int64_values"),
        {std::bit_cast<std::uint64_t>(
             std::numeric_limits<std::int64_t>::min()),
         std::bit_cast<std::uint64_t>(
             std::numeric_limits<std::int64_t>::min()),
         std::bit_cast<std::uint64_t>(
             std::numeric_limits<std::int64_t>::max()),
         std::bit_cast<std::uint64_t>(
             std::numeric_limits<std::int64_t>::max()),
         0},
        "int64 conversion did not retain exact saturated bits");
    requireIntegerBits(
        variable(result, "uint64_values"),
        {0, 0, 1, std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint64_t>::max(), 0},
        "uint64 conversion did not retain exact saturated bits");
    requireIntegerBits(
        variable(result, "uint64_from_negative"), {0},
        "signed-to-unsigned exact conversion did not saturate");
    requireIntegerBits(
        variable(result, "int64_from_uintmax"),
        {std::bit_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())},
        "unsigned-to-signed exact conversion did not saturate");
    requireArray(variable(result, "single_sum"),
                 mparser::RuntimeNumericClass::Single,
                 {static_cast<double>(static_cast<float>(
                      static_cast<double>(static_cast<float>(0.1)) + 0.25)),
                  static_cast<double>(static_cast<float>(
                      static_cast<double>(static_cast<float>(0.2)) + 0.25))},
                 "single arithmetic did not preserve binary32 class");
    requireArray(variable(result, "int8_sum"),
                 mparser::RuntimeNumericClass::Int8,
                 {127, -100},
                 "integer plus scalar double did not saturate");
    requireArray(variable(result, "uint8_product"),
                 mparser::RuntimeNumericClass::UInt8,
                 {255, 255},
                 "same-class integer multiplication did not saturate");
    requireArray(variable(result, "int16_matrix"),
                 mparser::RuntimeNumericClass::Int16,
                 {206, 18},
                 "integer matrix multiplication lost its class");
    requireArray(variable(result, "single_matrix"),
                 mparser::RuntimeNumericClass::Single,
                 {17, 39},
                 "single matrix multiplication lost its class");
    requireScalar(variable(result, "logical_sum"),
                  mparser::RuntimeNumericClass::Double, 2.0,
                  "logical arithmetic must produce double");
    requireScalar(variable(result, "int8_negated"),
                  mparser::RuntimeNumericClass::Int8, 127.0,
                  "integer unary minus did not saturate");
    requireScalar(variable(result, "uint8_negated"),
                  mparser::RuntimeNumericClass::UInt8, 0.0,
                  "unsigned unary minus did not saturate");
    requireScalar(variable(result, "single_negated"),
                  mparser::RuntimeNumericClass::Single,
                  static_cast<double>(static_cast<float>(
                      -static_cast<double>(static_cast<float>(0.1)))),
                  "single unary minus lost its class");

    for (std::string_view name : {"class_checks", "isa_checks",
                                  "predicate_checks", "comparison"}) {
        requireScalar(variable(result, name),
                      mparser::RuntimeNumericClass::Logical, 1.0,
                      "numeric class metadata check failed");
    }
}

void verifyExactIntegerStorage() {
    constexpr std::uint64_t beyondFlintmax = 9007199254740993ULL;
    mparser::RuntimeNumericElementValue element;
    element.numericClass = mparser::RuntimeNumericClass::UInt64;
    element.integerRealBits = beyondFlintmax;
    element.real = static_cast<double>(beyondFlintmax);

    auto value = mparser::runtimeNumericValueFromElements(
        {1, 1}, {element}, mparser::RuntimeNumericClass::UInt64);
    require(value.has_value(),
            "exact uint64 value could not be constructed");
    requireIntegerBits(*value, {beyondFlintmax},
                       "uint64 storage rounded through double");

    auto signedValue = mparser::runtimeConvertNumericClass(
        *value, mparser::RuntimeNumericClass::Int64);
    require(signedValue.has_value(),
            "exact uint64-to-int64 conversion failed");
    requireIntegerBits(
        *signedValue,
        {std::bit_cast<std::uint64_t>(
            static_cast<std::int64_t>(beyondFlintmax))},
        "uint64-to-int64 conversion rounded through double");

    auto doubleValue = mparser::runtimeConvertNumericClass(
        *value, mparser::RuntimeNumericClass::Double);
    require(doubleValue &&
                doubleValue->number ==
                    static_cast<double>(beyondFlintmax),
             "uint64-to-double conversion did not use the documented lossy boundary");
}

void verifyExactIntegerArithmetic() {
    constexpr std::uint64_t beyondFlintmax = 9007199254740993ULL;
    constexpr std::uint64_t uintMaximum =
        std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t intMinimumBits =
        std::bit_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::min());
    constexpr std::uint64_t intMaximumBits =
        std::bit_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());

    const auto beyond = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 1}, {beyondFlintmax});
    const auto one = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 1}, {1});
    const auto two = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 1}, {2});
    const auto maximum = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 1}, {uintMaximum});

    const auto sum = mparser::runtimeApplyNumericBinary("+", beyond, two);
    require(sum.succeeded, "exact uint64 addition failed");
    requireIntegerBits(sum.value, {beyondFlintmax + 2},
                       "exact uint64 addition rounded through double");

    const auto saturated =
        mparser::runtimeApplyNumericBinary("+", maximum, one);
    require(saturated.succeeded, "saturating uint64 addition failed");
    requireIntegerBits(saturated.value, {uintMaximum},
                       "uint64 addition did not saturate");

    const auto floored = mparser::runtimeApplyNumericBinary("-", one, two);
    require(floored.succeeded, "saturating uint64 subtraction failed");
    requireIntegerBits(floored.value, {0},
                       "uint64 subtraction did not saturate at zero");

    const auto signedMinimum = exactIntegerValue(
        mparser::RuntimeNumericClass::Int64, {1, 1}, {intMinimumBits});
    const auto negated =
        mparser::runtimeApplyNumericUnary("-", signedMinimum);
    require(negated.succeeded, "saturating int64 unary minus failed");
    requireIntegerBits(negated.value, {intMaximumBits},
                       "int64 unary minus did not saturate");

    const auto signedNegativeFive = exactIntegerValue(
        mparser::RuntimeNumericClass::Int64, {1, 1},
        {std::bit_cast<std::uint64_t>(std::int64_t{-5})});
    const auto signedTwo = exactIntegerValue(
        mparser::RuntimeNumericClass::Int64, {1, 1},
        {std::bit_cast<std::uint64_t>(std::int64_t{2})});
    const auto divided = mparser::runtimeApplyNumericBinary(
        "/", signedNegativeFive, signedTwo);
    require(divided.succeeded, "exact int64 division failed");
    requireIntegerBits(
        divided.value,
        {std::bit_cast<std::uint64_t>(std::int64_t{-3})},
        "integer division did not round halves away from zero");

    const auto uintComparison = mparser::runtimeApplyNumericBinary(
        "~=", maximum,
        mparser::makeRuntimeNumberValue(18446744073709551616.0));
    require(uintComparison.succeeded,
            "uint64 boundary comparison failed");
    requireScalar(uintComparison.value,
                  mparser::RuntimeNumericClass::Logical, 1.0,
                  "uint64 max compared equal to double 2^64");

    const auto signedMaximum = exactIntegerValue(
        mparser::RuntimeNumericClass::Int64, {1, 1}, {intMaximumBits});
    const auto intComparison = mparser::runtimeApplyNumericBinary(
        "~=", signedMaximum,
        mparser::makeRuntimeNumberValue(9223372036854775808.0));
    require(intComparison.succeeded,
            "int64 boundary comparison failed");
    requireScalar(intComparison.value,
                  mparser::RuntimeNumericClass::Logical, 1.0,
                  "int64 max compared equal to double 2^63");

    const auto left = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {1, 2},
        {beyondFlintmax, 1});
    const auto right = exactIntegerValue(
        mparser::RuntimeNumericClass::UInt64, {2, 1}, {1, 2});
    const auto product =
        mparser::runtimeApplyNumericBinary("*", left, right);
    require(product.succeeded, "exact uint64 matrix multiply failed");
    requireIntegerBits(product.value, {beyondFlintmax + 2},
                       "uint64 matrix multiply rounded through double");
}

template <typename Result>
void verifyExactArrayResult(const Result& result) {
    require(result.diagnostics.empty(),
            "exact integer array execution emitted diagnostics");
    constexpr std::uint64_t exact = 9007199254740993ULL;
    requireIntegerBits(variable(result, "indexed"), {exact},
                       "indexed uint64 value lost exact bits");
    requireIntegerBits(variable(result, "reshaped"), {exact, 7},
                       "reshape lost exact uint64 bits");
    requireIntegerBits(variable(result, "repeated"), {exact, exact},
                       "repmat lost exact uint64 bits");
    requireIntegerBits(variable(result, "joined"), {exact, 9},
                       "concatenation lost exact uint64 bits");
    requireIntegerBits(variable(result, "assigned"), {0, exact},
                       "indexed assignment lost exact uint64 bits");
    requireIntegerBits(variable(result, "grown"), {0, 0, exact},
                       "array growth lost exact uint64 bits");
    requireIntegerBits(variable(result, "deleted"), {exact, 3},
                       "array deletion lost exact uint64 bits");
}

void verifyExactArrayPaths() {
    const auto result = runBoth(R"(
exact = uint64(9007199254740992) + uint64(1);
values = [exact, uint64(7)];
indexed = values(1);
reshaped = reshape(values, 2, 1);
repeated = repmat(exact, 1, 2);
joined = [exact, uint64(9)];
assigned = uint64([0, 0]);
assigned(2) = exact;
grown = uint64(0);
grown(3) = exact;
deleted = [uint64(1), exact, uint64(3)];
deleted(1) = [];
)");
    verifyExactArrayResult(result.interpreter);
    verifyExactArrayResult(result.vm);
}

template <typename Result>
void verifyTypedColonResult(const Result& result) {
    require(result.diagnostics.empty(),
            "typed colon execution emitted diagnostics");
    constexpr std::uint64_t start = 9007199254740993ULL;
    requireIntegerBits(variable(result, "uint_range"),
                       {start, start + 1, start + 2},
                       "uint64 colon rounded through double");
    requireIntegerBits(
        variable(result, "signed_range"),
        {std::bit_cast<std::uint64_t>(std::int64_t{3}),
         std::bit_cast<std::uint64_t>(std::int64_t{2}),
         std::bit_cast<std::uint64_t>(std::int64_t{1})},
        "signed integer colon lost exact values");
    requireArray(variable(result, "single_range"),
                 mparser::RuntimeNumericClass::Single,
                 {1.0, 1.5, 2.0},
                 "single colon lost its class");
}

void verifyTypedColon() {
    const auto result = runBoth(R"(
uint_start = uint64(9007199254740992) + uint64(1);
uint_stop = uint_start + uint64(2);
uint_range = uint_start:uint_stop;
signed_range = int64(3):int64(-1):int64(1);
single_range = single(1):single(0.5):single(2);
)");
    verifyTypedColonResult(result.interpreter);
    verifyTypedColonResult(result.vm);

    const auto mixed = runBoth("bad = int8(1):uint8(3);\n");
    constexpr std::string_view message =
        "integer colon operands must use the same integer class";
    require(hasDiagnostic(mixed.interpreter, message),
            "interpreter accepted mixed integer colon classes");
    require(hasDiagnostic(mixed.vm, message),
            "VM accepted mixed integer colon classes");
}

template <typename Result>
void verifyNativeIntegerReductionResult(const Result& result) {
    require(result.diagnostics.empty(),
            "native integer reduction emitted diagnostics");
    constexpr std::uint64_t beyondFlintmax = 9007199254740993ULL;
    requireIntegerBits(variable(result, "uint_sum"),
                       {beyondFlintmax + 2},
                       "native uint64 sum rounded through double");
    requireIntegerBits(variable(result, "uint_product"),
                       {beyondFlintmax * 2},
                       "native uint64 product rounded through double");
    requireIntegerBits(
        variable(result, "signed_sum"),
        {std::bit_cast<std::uint64_t>(std::int64_t{-3})},
        "native int64 sum lost its signed result");
    requireIntegerBits(variable(result, "saturated_product"),
                       {static_cast<std::uint64_t>(
                           std::numeric_limits<std::int16_t>::max())},
                       "native int16 product did not saturate");
    requireIntegerBits(variable(result, "empty_sum"), {0},
                       "native empty integer sum was not zero");
    requireScalar(variable(result, "default_sum"),
                  mparser::RuntimeNumericClass::Double, 3.0,
                  "default integer sum did not return double");
}

void verifyNativeIntegerReductions() {
    const auto result = runBoth(R"(
base = uint64(9007199254740992) + uint64(1);
uint_sum = sum([base, uint64(2)], "all", "native");
uint_product = prod([base, uint64(2)], "all", "native");
signed_sum = sum(int64([-5, 2]), "all", "native");
saturated_product = prod(int16([200, 200]), "all", "native");
empty_sum = sum(uint64([]), "all", "native");
default_sum = sum(uint8([1, 2]), "all");
)");
    verifyNativeIntegerReductionResult(result.interpreter);
    verifyNativeIntegerReductionResult(result.vm);
}

template <typename Result>
void verifyExactMixedIntegerResult(const Result& result) {
    require(result.diagnostics.empty(),
            "mixed 64-bit integer execution emitted diagnostics");
    constexpr std::uint64_t base = 9007199254740993ULL;
    requireIntegerBits(variable(result, "plus_one"), {base + 1},
                       "uint64 plus scalar double lost one unit");
    requireIntegerBits(variable(result, "one_plus"), {base + 1},
                       "reversed scalar double addition lost one unit");
    requireIntegerBits(variable(result, "plus_half"), {base + 1},
                       "positive half tie did not round away from zero");
    requireIntegerBits(variable(result, "minus_half"), {base},
                       "subtracted half tie rounded the wrong way");
    requireIntegerBits(variable(result, "scaled"),
                       {13510798882111490ULL},
                       "uint64 multiplication lost extended precision");
    requireIntegerBits(variable(result, "divided"),
                       {4503599627370497ULL},
                       "uint64 division lost extended precision");
    requireIntegerBits(variable(result, "reverse_subtract"), {0},
                       "negative uint64 result did not saturate");
    requireIntegerBits(variable(result, "reverse_divide"), {0},
                       "fractional uint64 result did not round to zero");
    requireIntegerBits(variable(result, "mod_integer"), {1},
                       "exact uint64 modulus was rounded through double");
    requireIntegerBits(variable(result, "mod_fraction"), {1},
                       "dyadic uint64 modulus was incorrect");
    requireIntegerBits(variable(result, "fraction_mod"), {3},
                       "reversed dyadic modulus did not round correctly");
    requireIntegerBits(variable(result, "rem_fraction"), {1},
                       "dyadic uint64 remainder was incorrect");
    requireIntegerBits(variable(result, "mixed_grid"),
                       {base + 1, base + 3, base + 5, base + 7},
                       "N-D mixed uint64 arithmetic lost shape or bits");
    requireIntegerBits(variable(result, "nan_result"), {0},
                       "NaN mixed arithmetic did not convert to zero");
    requireIntegerBits(
        variable(result, "positive_inf"),
        {std::numeric_limits<std::uint64_t>::max()},
        "positive infinity did not saturate uint64");
    requireIntegerBits(variable(result, "negative_inf"), {0},
                       "negative infinity did not saturate uint64");
    requireIntegerBits(variable(result, "tiny_sum"), {base},
                       "subnormal addition changed an exact uint64");
    requireIntegerBits(variable(result, "tiny_product"), {0},
                       "subnormal multiplication did not round to zero");
    requireIntegerBits(
        variable(result, "tiny_divisor"),
        {std::numeric_limits<std::uint64_t>::max()},
        "subnormal divisor did not saturate uint64");
    requireIntegerBits(variable(result, "huge_sum"),
                       {std::numeric_limits<std::uint64_t>::max()},
                       "large double addition did not saturate uint64");
    requireIntegerBits(variable(result, "huge_subtract"), {0},
                       "large double subtraction did not saturate uint64");
    requireIntegerBits(
        variable(result, "max_plus_one"),
        {std::numeric_limits<std::uint64_t>::max()},
        "uint64 mixed addition did not saturate at intmax");
    requireIntegerBits(variable(result, "extended_even_tie"),
                       {0x8000000000000002ULL},
                       "binary80 even tie was rounded away from even");
    requireIntegerBits(variable(result, "extended_odd_tie"),
                       {0x8000000000000002ULL},
                       "binary80 odd tie did not round to even");
    requireIntegerBits(variable(result, "extended_subtract_tie"),
                       {0x8000000000000000ULL},
                       "binary80 subtraction tie did not round to even");
    requireIntegerBits(variable(result, "mod_infinity"), {base},
                       "mod with an infinite divisor lost uint64 bits");
    requireIntegerBits(variable(result, "rem_infinity"), {base},
                       "rem with an infinite divisor lost uint64 bits");
    requireIntegerBits(variable(result, "mod_zero"), {base},
                       "mod with a zero divisor lost uint64 bits");
    requireIntegerBits(variable(result, "rem_zero"), {0},
                       "rem with a zero divisor did not convert NaN to zero");

    const auto signedBits = [](std::int64_t value) {
        return std::bit_cast<std::uint64_t>(value);
    };
    requireIntegerBits(variable(result, "negative_plus_half"),
                       {signedBits(-3)},
                       "negative half tie did not round away from zero");
    requireIntegerBits(variable(result, "negative_plus_one_half"),
                       {signedBits(-2)},
                       "negative mixed addition rounded incorrectly");
    requireIntegerBits(variable(result, "negative_times_half"),
                       {signedBits(-2)},
                       "negative mixed multiplication rounded incorrectly");
    requireIntegerBits(variable(result, "negative_div_two"),
                       {signedBits(-2)},
                       "negative mixed division rounded incorrectly");
    requireIntegerBits(variable(result, "two_div_negative"),
                       {signedBits(-1)},
                       "reversed negative division rounded incorrectly");
    requireIntegerBits(
        variable(result, "positive_negative_zero"),
        {signedBits(std::numeric_limits<std::int64_t>::min())},
        "negative-zero divisor lost its saturation sign");
    requireIntegerBits(
        variable(result, "negative_negative_zero"),
        {signedBits(std::numeric_limits<std::int64_t>::max())},
        "two negative division signs did not produce positive saturation");
    requireIntegerBits(variable(result, "integer_power"), {8},
                       "integer base and double exponent failed");
    requireIntegerBits(variable(result, "double_power"), {16},
                       "double base and integer exponent failed");
    requireIntegerBits(
        variable(result, "signed_max_plus_one"),
        {signedBits(std::numeric_limits<std::int64_t>::max())},
        "int64 mixed positive overflow did not saturate");
    requireIntegerBits(
        variable(result, "signed_min_minus_one"),
        {signedBits(std::numeric_limits<std::int64_t>::min())},
        "int64 mixed negative overflow did not saturate");
}

void verifyMixedClassDiagnostics() {
    const auto mixedClass = runBoth(R"(
bad = int8([1, 2]) + single(1);
)");
    constexpr std::string_view mixedMessage =
        "integer arithmetic requires the same integer class or a scalar double operand";
    require(hasDiagnostic(mixedClass.interpreter, mixedMessage),
            "interpreter accepted mixed integer and single arithmetic");
    require(hasDiagnostic(mixedClass.vm, mixedMessage),
            "VM accepted mixed integer and single arithmetic");

    const auto doubleArray = runBoth(R"(
bad = int8([1, 2]) + [1, 2];
)");
    require(hasDiagnostic(doubleArray.interpreter, mixedMessage),
            "interpreter accepted integer plus a non-scalar double array");
    require(hasDiagnostic(doubleArray.vm, mixedMessage),
            "VM accepted integer plus a non-scalar double array");

    const auto exactMixed = runBoth(R"(
base = 0x0020000000000001u64;
plus_one = base + 1;
one_plus = 1 + base;
plus_half = base + 0.5;
minus_half = base - 0.5;
scaled = base * 1.5;
divided = base / 2;
reverse_subtract = 0.5 - base;
reverse_divide = 2 / base;
mod_integer = mod(base, 2);
mod_fraction = mod(base, 2.5);
fraction_mod = mod(2.5, base);
rem_fraction = rem(base, 2.5);
mixed_grid = reshape([base, base + uint64(2), ...
                      base + uint64(4), base + uint64(6)], 2, 2) + 0.5;
nan_result = base + NaN;
positive_inf = base + Inf;
negative_inf = base - Inf;
tiny_sum = base + 4.9406564584124654e-324;
tiny_product = base * 4.9406564584124654e-324;
tiny_divisor = base / 4.9406564584124654e-324;
huge_sum = base + 1e300;
huge_subtract = base - 1e300;
max_plus_one = 0xFFFFFFFFFFFFFFFFu64 + 1;
extended_even_tie = 0x8000000000000002u64 + 0.5;
extended_odd_tie = 0x8000000000000001u64 + 0.5;
extended_subtract_tie = 0x8000000000000001u64 - 0.5;
mod_infinity = mod(base, Inf);
rem_infinity = rem(base, Inf);
mod_zero = mod(base, 0);
rem_zero = rem(base, 0);
negative_plus_half = int64(-3) + 0.5;
negative_plus_one_half = int64(-3) + 1.5;
negative_times_half = int64(-3) * 0.5;
negative_div_two = int64(-3) / 2;
two_div_negative = 2 / int64(-3);
positive_negative_zero = int64(3) / (-0.0);
negative_negative_zero = int64(-3) / (-0.0);
integer_power = int64(2) ^ 3;
double_power = 2.5 ^ int64(3);
signed_max_plus_one = 0x7FFFFFFFFFFFFFFFs64 + 1;
signed_min_minus_one = 0x8000000000000000s64 - 1;
)");
    verifyExactMixedIntegerResult(exactMixed.interpreter);
    verifyExactMixedIntegerResult(exactMixed.vm);

    for (const std::string_view invalidPower : {
             "bad = int64(-3) ^ 2.5;\n",
             "bad = int64(2) ^ NaN;\n",
             "bad = 0.5 ^ int64(-3);\n"}) {
        const auto invalid = runBoth(invalidPower);
        require(!invalid.interpreter.diagnostics.empty() &&
                    !invalid.vm.diagnostics.empty(),
                "invalid mixed integer power was accepted");
    }
}

void verifyTypedFallback() {
    const auto result = runBoth(R"(
total = 0;
for item = single(1:12)
    total = total + double(item);
end
)");
    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected single loop range");
    require(result.vm.diagnostics.empty(),
            "VM rejected single loop range");

    const mparser::BytecodeLoopProfile* loop = nullptr;
    for (const auto& candidate : result.vm.profile.loops) {
        if (candidate.variable == "item") {
            loop = &candidate;
            break;
        }
    }
    require(loop && loop->hot,
            "single loop was not profiled as hot");
    require(loop->variableObservation.numericClass == "single",
            "single loop profile lost its numeric class");

    mparser::BytecodeOptimizationPlanner planner;
    const auto plan = planner.plan(result.vm.profile, result.bytecode);
    const mparser::BytecodeOptimizationCandidate* optimized = nullptr;
    for (const auto& candidate : plan.candidates) {
        if (candidate.kind == "hot-loop" &&
            candidate.target == "item") {
            optimized = &candidate;
            break;
        }
    }
    require(optimized && !optimized->guards.empty(),
            "single loop did not produce an optimization guard");
    require(optimized->guards.front().numericClass == "single",
            "single optimization guard lost its numeric class");

    mparser::BytecodeTypedIrBuilder builder;
    const auto typed = builder.build(plan);
    for (const auto& region : typed.regions) {
        if (region.sourcePc == optimized->pc) {
            require(region.kind != "scalar-loop",
                    "single loop entered the double-only typed path");
            return;
        }
    }
    throw std::runtime_error("single loop did not reach Typed IR");
}

} // namespace

int main() {
    try {
        constexpr std::string_view source = R"(
single_value = single(0.1);
int8_values = int8([-inf, -129, -128.5, -1.5, -0.5, 0.5, 1.5, 127.5, inf, nan]);
uint8_values = uint8([-inf, -1, -0.5, 0.5, 1.5, 127.5, inf, nan]);
int16_values = int16([-inf, -40000, 40000, inf]);
uint16_values = uint16([-inf, -1, 70000, inf]);
  int32_values = int32([-inf, -3000000000, 3000000000, inf]);
  uint32_values = uint32([-inf, -1, 5000000000, inf]);
  int64_values = int64([-inf, -9223372036854775808, 9223372036854775807, inf, nan]);
  uint64_values = uint64([-inf, -1, 0.5, 18446744073709551615, inf, nan]);
  uint64_from_negative = uint64(int64(-1));
  int64_from_uintmax = int64(uint64(inf));
  hex_default = 0x2A;
  hex_widened = 0x100;
  binary_default = 0B101010;
  signed_hex = 0xFFs8;
  signed_min8 = 0b10000000s8;
  signed_min16 = 0x8000s16;
  signed_min32 = 0x80000000s32;
  signed_min64 = 0x8000000000000000s64;
  maximum_hex = 0xFFFFFFFFFFFFFFFFu64;
  base_vector = [0b1u16, 0X2U16, 0xFFFFu16];
  single_sum = single([0.1, 0.2]) + 0.25;
  int8_sum = int8([120, -120]) + 20;
  uint8_product = uint8([20, 200]) .* uint8([20, 2]);
  int16_matrix = int16([100, 2; 3, 4]) * int16([2; 3]);
  single_matrix = single([1, 2; 3, 4]) * single([5; 6]);
  logical_sum = true + true;
  int8_negated = -int8(-128);
  uint8_negated = -uint8(10);
  single_negated = -single(0.1);
class_checks = strcmp(class(single_value), 'single') && ...
               strcmp(class(int8_values), 'int8') && ...
               strcmp(class(uint32_values), 'uint32');
isa_checks = isa(single_value, 'single') && ...
             isa(single_value, 'numeric') && ...
             isa(int8_values, 'int8') && ...
             isa(int8_values, 'numeric') && ...
             ~isa(true, 'numeric');
predicate_checks = isnumeric(single_value) && isfloat(single_value) && ...
                   ~isinteger(single_value) && isnumeric(int8_values) && ...
                   isinteger(int8_values) && ~isfloat(int8_values) && ...
                   ~isnumeric(true) && islogical(true);
comparison = int8(5) == 5;
)";

        const auto result = runBoth(source);
        verify(result.interpreter);
        verify(result.vm);
        verifyExactIntegerStorage();
        verifyExactIntegerArithmetic();
        verifyExactArrayPaths();
        verifyTypedColon();
        verifyNativeIntegerReductions();
        verifyMixedClassDiagnostics();
        verifyTypedFallback();

        struct LiteralCase {
            std::string_view text;
            mparser::RuntimeNumericClass numericClass;
            std::uint64_t bits;
        };
        const std::vector<LiteralCase> literalCases = {
            {"0x01u8", mparser::RuntimeNumericClass::UInt8, 1},
            {"0X0001U16", mparser::RuntimeNumericClass::UInt16, 1},
            {"0x00000001u32", mparser::RuntimeNumericClass::UInt32, 1},
            {"0x0000000000000001U64",
             mparser::RuntimeNumericClass::UInt64, 1},
            {"0xFFs8", mparser::RuntimeNumericClass::Int8,
             std::numeric_limits<std::uint64_t>::max()},
            {"0xFFFFS16", mparser::RuntimeNumericClass::Int16,
             std::numeric_limits<std::uint64_t>::max()},
            {"0xFFFFFFFFs32", mparser::RuntimeNumericClass::Int32,
             std::numeric_limits<std::uint64_t>::max()},
            {"0xFFFFFFFFFFFFFFFFS64",
             mparser::RuntimeNumericClass::Int64,
             std::numeric_limits<std::uint64_t>::max()},
        };
        for (const auto& literalCase : literalCases) {
            const auto value =
                mparser::runtimeParseNumericLiteral(literalCase.text);
            const auto element = value
                                     ? mparser::runtimeNumericElementValue(
                                           *value, 0)
                                     : std::nullopt;
            require(value && element &&
                        value->numericClass == literalCase.numericClass &&
                        element->integerRealBits == literalCase.bits,
                    "valid base-prefixed literal width or bits mismatch");
        }

        for (const std::string_view invalid : {
                 "0x", "0b102", "0x100s8", "0x100u8",
                 "0x10000u16", "0x100000000u32",
                 "0b100000000s8", "0x10000000000000000",
                 "0x1i", "0x1junk", "0x1_2"}) {
            require(!mparser::runtimeParseNumericLiteral(invalid),
                    "invalid base-prefixed literal was accepted");
            const auto invalidResult = runBoth(
                "bad = " + std::string(invalid) + ";\n");
            require(hasDiagnostic(invalidResult.interpreter,
                                  "unsupported literal") &&
                        hasDiagnostic(invalidResult.vm,
                                      "cannot load literal"),
                    "invalid base-prefixed source did not fail consistently");
        }

        require(mparser::runtimeNumericClassFromName("uint32") ==
                    mparser::RuntimeNumericClass::UInt32,
                "numeric class lookup failed");
        require(!mparser::runtimeNumericClassHasLegacyDoubleStorage(
                     mparser::RuntimeNumericClass::UInt64),
                "uint64 must not claim legacy double-only storage");

        mparser::ModuleInvocationResult machineResult;
        const auto exactProtocolValue = exactIntegerValue(
            mparser::RuntimeNumericClass::UInt64, {1, 2},
            {9007199254740993ULL,
             std::numeric_limits<std::uint64_t>::max()});
        require(mparser::runtimeValueToString(exactProtocolValue) ==
                    "[9007199254740993 18446744073709551615]",
                "human-readable output rounded exact uint64 values");
        machineResult.variables = {
            {"single_value", variable(result.vm, "single_value")},
            {"int8_values", variable(result.vm, "int8_values")},
            {"uint64_exact", exactProtocolValue},
        };
        const std::string json = mparser::serializeMachineResultJsonV1(
            machineResult, "numeric-type-test");
        require(json.find("\"class\":\"single\"") !=
                    std::string::npos,
                "machine protocol omitted single class");
        require(json.find("\"class\":\"int8\"") !=
                    std::string::npos,
                "machine protocol omitted int8 class");
        require(json.find(
                    "\"class\":\"uint64\",\"dimensions\":[1,2],\"data\":[9007199254740993,18446744073709551615]") !=
                    std::string::npos,
                "machine protocol rounded exact uint64 payloads");

        std::cout << "Numeric type smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Numeric type smoke failure: " << error.what()
                  << '\n';
        return 1;
    }
}
