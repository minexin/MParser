#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value.h"
#include "mparser/semantic.h"

#include <algorithm>
#include <cmath>
#include <complex>
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
            "complex numeric source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "complex numeric source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "complex numeric source did not lower");

    mparser::Interpreter interpreter;
    auto interpreterResult = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto vmResult = vm.run(bytecode, semantic);
    return RuntimePair{std::move(interpreterResult),
                       std::move(vmResult)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing complex numeric variable: " +
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

bool near(double left, double right, double tolerance = 1e-6) {
    const double scale = std::max({1.0, std::fabs(left),
                                   std::fabs(right)});
    return std::fabs(left - right) <= scale * tolerance;
}

void requireNumeric(
    const mparser::RuntimeValue& value,
    mparser::RuntimeNumericClass numericClass,
    const std::vector<size_t>& dimensions,
    const std::vector<std::complex<double>>& expected,
    bool expectedComplex, std::string_view context) {
    require(value.numericClass == numericClass, context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(value.numericComplex == expectedComplex, context);
    require(mparser::runtimeShapeElementCount(value) == expected.size(),
            context);
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto element =
            mparser::runtimeNumericElementValue(value, index);
        require(element.has_value(), context);
        require(near(element->real, expected[index].real()), context);
        require(near(element->imaginary,
                     expected[index].imag()),
                context);
    }
}

template <typename Result>
void verifySuccess(const Result& result) {
    if (!result.diagnostics.empty()) {
        throw std::runtime_error(
            "complex numeric execution emitted diagnostic: " +
            result.diagnostics.front().message + " at line " +
            std::to_string(
                result.diagnostics.front().span.begin.line));
    }
    using Class = mparser::RuntimeNumericClass;

    requireNumeric(variable(result, "literal_i"), Class::Double,
                   {1, 1}, {{0.0, 2.0}}, true,
                   "i-suffixed literal mismatch");
    requireNumeric(variable(result, "literal_j"), Class::Double,
                   {1, 1}, {{0.0, 0.35}}, true,
                   "j-suffixed literal mismatch");
    requireNumeric(variable(result, "cancelled_sum"), Class::Double,
                   {1, 1}, {{8.0, 0.0}}, false,
                   "complex cancellation did not become real");
    requireNumeric(variable(result, "product"), Class::Double,
                   {1, 1}, {{11.0, 2.0}}, true,
                   "complex product mismatch");
    requireNumeric(variable(result, "quotient"), Class::Double,
                   {1, 1}, {{-0.2, 0.4}}, true,
                   "complex quotient mismatch");
    requireNumeric(variable(result, "power"), Class::Double,
                   {1, 1}, {{0.0, 1.0}}, true,
                   "real-domain power did not promote to complex");

    requireNumeric(variable(result, "ctrans"), Class::Double,
                   {2, 1}, {{1.0, -2.0}, {3.0, 4.0}}, true,
                   "conjugate transpose mismatch");
    requireNumeric(variable(result, "ntrans"), Class::Double,
                   {2, 1}, {{1.0, 2.0}, {3.0, -4.0}}, true,
                   "nonconjugate transpose mismatch");
    requireNumeric(variable(result, "constructed"), Class::Single,
                   {1, 2}, {{1.0, 3.0}, {2.0, 3.0}}, true,
                   "complex implicit expansion or single class mismatch");
    requireNumeric(variable(result, "forced"), Class::Double,
                   {1, 1}, {{12.0, 0.0}}, true,
                   "one-input complex did not retain complex storage");
    requireNumeric(variable(result, "realpart"), Class::Double,
                   {1, 2}, {{1.0, 0.0}, {3.0, 0.0}}, false,
                   "real projection mismatch");
    requireNumeric(variable(result, "imagpart"), Class::Double,
                   {1, 2}, {{2.0, 0.0}, {-4.0, 0.0}}, false,
                   "imaginary projection mismatch");
    requireNumeric(variable(result, "conjugated"), Class::Double,
                   {1, 2}, {{1.0, -2.0}, {3.0, 4.0}}, true,
                   "conjugation mismatch");
    requireNumeric(variable(result, "magnitude"), Class::Double,
                   {1, 1}, {{5.0, 0.0}}, false,
                   "complex magnitude mismatch");

    requireNumeric(variable(result, "sqrt_neg"), Class::Double,
                   {1, 1}, {{0.0, 1.0}}, true,
                   "sqrt negative-domain result mismatch");
    requireNumeric(variable(result, "log_neg"), Class::Double,
                   {1, 1}, {{0.0, std::acos(-1.0)}}, true,
                   "log negative-domain result mismatch");
    requireNumeric(variable(result, "asin_two"), Class::Double,
                   {1, 1}, {{std::acos(-1.0) / 2.0,
                             -std::acosh(2.0)}},
                   true, "asin complex-domain result mismatch");
    requireNumeric(variable(result, "acos_two"), Class::Double,
                   {1, 1}, {{0.0, std::acosh(2.0)}}, true,
                   "acos complex-domain result mismatch");
    requireNumeric(variable(result, "single_sqrt"), Class::Single,
                   {1, 1}, {{0.0, 1.0}}, true,
                   "single complex math lost its class");
    const double pi = std::acos(-1.0);
    requireNumeric(variable(result, "hyper_acosh"), Class::Double,
                   {1, 1}, {{std::acosh(2.0), pi}}, true,
                   "acosh complex-domain result mismatch");
    requireNumeric(variable(result, "hyper_asinh"), Class::Double,
                   {1, 1}, {{std::acosh(2.0), pi / 2.0}}, true,
                   "asinh complex-domain result mismatch");
    requireNumeric(variable(result, "hyper_atanh"), Class::Double,
                   {1, 1}, {{0.5 * std::log(3.0), pi / 2.0}}, true,
                   "atanh complex-domain result mismatch");
    requireNumeric(variable(result, "decimal_log"), Class::Double,
                   {1, 1}, {{0.0, pi / std::log(10.0)}}, true,
                   "log10 complex-domain result mismatch");
    requireNumeric(variable(result, "binary_log"), Class::Double,
                   {1, 1}, {{0.0, pi / std::log(2.0)}}, true,
                   "log2 complex-domain result mismatch");
    requireNumeric(variable(result, "rounded_complex"), Class::Double,
                   {1, 2}, {{2.0, 2.0}, {-2.0, -3.0}}, true,
                   "complex rounding did not map both components");
    requireNumeric(variable(result, "floored_complex"), Class::Double,
                   {1, 1}, {{1.0, 2.0}}, true,
                   "complex floor did not map both components");
    requireNumeric(variable(result, "complex_sign"), Class::Double,
                   {1, 1}, {{0.6, 0.8}}, true,
                   "complex sign mismatch");
    requireNumeric(variable(result, "integer_round"), Class::Int16,
                   {1, 2}, {{-2.0, 0.0}, {3.0, 0.0}}, false,
                   "integer round lost its class");
    requireNumeric(variable(result, "integer_sign"), Class::Int16,
                   {1, 3}, {{-1.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}},
                   false, "integer sign lost its class");
    requireNumeric(variable(result, "finite_flags"), Class::Logical,
                   {1, 4}, {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                            {0.0, 0.0}},
                   false, "isfinite complex semantics mismatch");
    requireNumeric(variable(result, "infinite_flags"), Class::Logical,
                   {1, 4}, {{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0},
                            {1.0, 0.0}},
                   false, "isinf complex semantics mismatch");
    requireNumeric(variable(result, "nan_flags"), Class::Logical,
                   {1, 4}, {{0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0},
                            {0.0, 0.0}},
                   false, "isnan complex semantics mismatch");
    requireNumeric(variable(result, "single_hyperbolic"), Class::Single,
                   {1, 1}, {{static_cast<double>(
                                 std::tanh(1.0F)), 0.0}},
                   false, "single hyperbolic math lost its class");
    requireNumeric(variable(result, "angles"), Class::Single,
                   {1, 2}, {{std::atan2(1.0, 2.0), 0.0},
                            {0.0, 0.0}},
                   false, "atan2 implicit expansion or class mismatch");
    requireNumeric(variable(result, "distances"), Class::Single,
                   {2, 1}, {{5.0, 0.0}, {13.0, 0.0}}, false,
                   "hypot implicit expansion or class mismatch");
    requireNumeric(variable(result, "spacing"), Class::Single,
                   {1, 3},
                   {{std::numeric_limits<float>::denorm_min(), 0.0},
                    {std::numeric_limits<float>::epsilon(), 0.0},
                    {2.0 * std::numeric_limits<float>::epsilon(), 0.0}},
                   false, "single eps spacing mismatch");
    requireNumeric(variable(result, "complex_spacing"), Class::Double,
                   {1, 1}, {{2.0 * std::numeric_limits<double>::epsilon(),
                             0.0}},
                   false, "complex eps spacing mismatch");
    requireNumeric(variable(result, "reduce_sum"), Class::Single,
                   {1, 1}, {{1.0, 8.0}}, true,
                   "complex single sum mismatch");
    requireNumeric(variable(result, "reduce_product"), Class::Single,
                   {1, 1}, {{199.0, 32.0}}, true,
                   "complex single product mismatch");
    requireNumeric(variable(result, "reduce_mean"), Class::Single,
                   {1, 1}, {{1.0 / 3.0, 8.0 / 3.0}}, true,
                   "complex single mean mismatch");
    requireNumeric(variable(result, "reduce_minimum"), Class::Single,
                   {1, 1}, {{-3.0, -4.0}}, true,
                   "complex minimum did not use magnitude and phase");
    requireNumeric(variable(result, "reduce_minimum_index"),
                   Class::Double, {1, 1}, {{3.0, 0.0}}, false,
                   "complex minimum index mismatch");
    requireNumeric(variable(result, "reduce_maximum"), Class::Single,
                   {1, 1}, {{1.0, 8.0}}, true,
                   "complex maximum did not use magnitude and phase");
    requireNumeric(variable(result, "reduce_maximum_index"),
                   Class::Double, {1, 1}, {{2.0, 0.0}}, false,
                   "complex maximum index mismatch");
    requireNumeric(variable(result, "find_rows"), Class::Double,
                   {2, 1}, {{2.0, 0.0}, {1.0, 0.0}}, false,
                   "complex find row indices mismatch");
    requireNumeric(variable(result, "find_columns"), Class::Double,
                   {2, 1}, {{1.0, 0.0}, {2.0, 0.0}}, false,
                   "complex find column indices mismatch");
    requireNumeric(variable(result, "find_values"), Class::Double,
                   {2, 1}, {{4.0, -5.0}, {2.0, 3.0}}, true,
                   "complex find values lost their imaginary channel");
    requireNumeric(variable(result, "running_sum"), Class::Single,
                   {1, 3},
                   {{1.0, 2.0}, {4.0, -2.0}, {2.0, -1.0}}, true,
                   "complex cumulative sum mismatch");
    requireNumeric(variable(result, "running_product"), Class::Single,
                   {1, 3},
                   {{1.0, 2.0}, {11.0, 2.0}, {-24.0, 7.0}}, true,
                   "complex cumulative product mismatch");
    requireNumeric(variable(result, "running_minimum"), Class::Single,
                   {1, 3},
                   {{1.0, 2.0}, {1.0, 2.0}, {1.0, 2.0}}, true,
                   "complex cumulative minimum mismatch");
    requireNumeric(variable(result, "running_maximum"), Class::Single,
                   {1, 3},
                   {{1.0, 2.0}, {3.0, -4.0}, {3.0, -4.0}}, true,
                   "complex cumulative maximum mismatch");
    requireNumeric(variable(result, "complex_difference"), Class::Single,
                   {1, 2}, {{2.0, -6.0}, {-5.0, 5.0}}, true,
                   "complex difference lost its class or imaginary channel");
    requireNumeric(variable(result, "complex_grid"), Class::Double,
                   {1, 3},
                   {{1.0, 2.0}, {3.0, 0.0}, {5.0, -2.0}}, true,
                   "complex linspace mismatch");
    requireNumeric(variable(result, "single_grid"), Class::Single,
                   {1, 3}, {{1.0, 0.0}, {1.5, 0.0}, {2.0, 0.0}},
                   false, "single linspace lost its class");
    requireNumeric(variable(result, "typed_zeros"), Class::Single,
                   {2, 3}, std::vector<std::complex<double>>(6), false,
                   "typed zeros mismatch");
    requireNumeric(variable(result, "typed_ones"), Class::UInt64,
                   {1, 3}, {{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}},
                   false, "typed ones mismatch");
    requireNumeric(variable(result, "typed_eye"), Class::Int16,
                   {2, 3},
                   {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                    {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                   false, "typed eye mismatch");
    requireNumeric(variable(result, "scalar_zero"), Class::Double,
                   {1, 1}, {{0.0, 0.0}}, false,
                   "zero-argument zeros mismatch");
    const auto& typedInfinity = variable(result, "typed_infinity");
    require(typedInfinity.numericClass == Class::Single &&
                mparser::runtimeDimensions(typedInfinity) ==
                    std::vector<size_t>({1, 2}),
            "typed inf shape or class mismatch");
    for (size_t index = 0; index < 2; ++index) {
        const auto element =
            mparser::runtimeNumericElementValue(typedInfinity, index);
        require(element && std::isinf(element->real),
                "typed inf payload mismatch");
    }
    const auto& typedNan = variable(result, "typed_nan");
    require(typedNan.numericClass == Class::Double &&
                mparser::runtimeDimensions(typedNan) ==
                    std::vector<size_t>({2, 1}),
            "typed nan shape or class mismatch");
    for (size_t index = 0; index < 2; ++index) {
        const auto element =
            mparser::runtimeNumericElementValue(typedNan, index);
        require(element && std::isnan(element->real),
                "typed nan payload mismatch");
    }
    const auto& emptyCell = variable(result, "empty_cell");
    require(emptyCell.kind == mparser::RuntimeValueKind::Cell &&
                mparser::runtimeDimensions(emptyCell) ==
                    std::vector<size_t>({0, 0}) &&
                emptyCell.cells.empty(),
            "zero-argument cell mismatch");

    for (std::string_view name : {
             "cancel_is_real", "ordered", "equal",
             "not_equal", "reduce_any", "reduce_all"}) {
        requireNumeric(variable(result, name), Class::Logical,
                       {1, 1}, {{1.0, 0.0}}, false,
                       "complex predicate or comparison mismatch");
    }
    requireNumeric(variable(result, "forced_is_real"),
                   Class::Logical, {1, 1}, {{0.0, 0.0}}, false,
                   "isreal ignored forced complex storage");
    require(mparser::runtimeTextScalarUtf8(
                variable(result, "complex_text")) == "1+2i",
            "string conversion lost a complex component");
}

void verifyDiagnostics() {
    const auto condition = runBoth(R"(
if 1i
    value = 1;
end
)");
    require(hasDiagnostic(condition.interpreter,
                          "real numeric value without NaN"),
            "interpreter accepted a complex condition");
    require(hasDiagnostic(condition.vm,
                          "real numeric value without NaN"),
            "VM accepted a complex condition");

    const auto logical = runBoth("value = 1i & 1;\n");
    require(hasDiagnostic(logical.interpreter,
                          "logical operators require real"),
            "interpreter accepted complex logical arithmetic");
    require(hasDiagnostic(logical.vm,
                          "logical operators require real"),
            "VM accepted complex logical arithmetic");

    const auto integer = runBoth(R"(
left = complex(int8(1), int8(2));
right = complex(int8(3), int8(4));
value = left + right;
)");
    require(hasDiagnostic(integer.interpreter,
                          "complex integer values"),
            "interpreter accepted complex integer arithmetic");
    require(hasDiagnostic(integer.vm,
                          "complex integer values"),
            "VM accepted complex integer arithmetic");

    const auto integerReduction = runBoth(R"(
value = sum(complex(int8([1, 2]), int8([3, 4])));
)");
    require(hasDiagnostic(integerReduction.interpreter,
                          "complex integer values"),
            "interpreter accepted a complex integer reduction");
    require(hasDiagnostic(integerReduction.vm,
                          "complex integer values"),
            "VM accepted a complex integer reduction");

    const auto index = runBoth(R"(
values = [10, 20];
value = values(complex(1, 0));
)");
    require(hasDiagnostic(index.interpreter,
                          "index subscript must be real"),
            "interpreter accepted a complex index");
    require(hasDiagnostic(index.vm, "index subscript must be real"),
            "VM accepted a complex index");

    const auto reshape = runBoth(R"(
value = reshape(1:4, complex(2, 0), 2);
)");
    require(hasDiagnostic(reshape.interpreter,
                          "reshape dimensions must be real"),
            "interpreter accepted a complex reshape dimension");
    require(hasDiagnostic(reshape.vm,
                          "reshape dimensions must be real"),
            "VM accepted a complex reshape dimension");

    const auto character = runBoth("value = char(1 + 2i);\n");
    require(hasDiagnostic(character.interpreter,
                          "complex values cannot be converted to char"),
            "interpreter converted a complex value to char");
    require(hasDiagnostic(character.vm,
                          "complex values cannot be converted to char"),
            "VM converted a complex value to char");

    const auto sizeDimension = runBoth(R"(
value = size(ones(2, 3), complex(1, 0));
)");
    require(hasDiagnostic(sizeDimension.interpreter,
                          "positive real integer"),
            "interpreter accepted a complex size dimension");
    require(hasDiagnostic(sizeDimension.vm, "positive real integer"),
            "VM accepted a complex size dimension");

    const auto constructorDimension = runBoth(R"(
value = zeros(complex(2, 0), 3);
)");
    require(hasDiagnostic(constructorDimension.interpreter,
                          "dimensions must be real"),
            "interpreter accepted a complex constructor dimension");
    require(hasDiagnostic(constructorDimension.vm,
                          "dimensions must be real"),
            "VM accepted a complex constructor dimension");

    const auto linspaceCount = runBoth(R"(
value = linspace(0, 1, complex(3, 0));
)");
    require(hasDiagnostic(linspaceCount.interpreter,
                          "count must be a real"),
            "interpreter accepted a complex linspace count");
    require(hasDiagnostic(linspaceCount.vm,
                          "count must be a real"),
            "VM accepted a complex linspace count");

    const auto colon = runBoth("value = complex(1, 0):3;\n");
    require(hasDiagnostic(colon.interpreter,
                          "colon operands must be real"),
            "interpreter accepted a complex colon operand");
    require(hasDiagnostic(colon.vm,
                           "colon operands must be real"),
            "VM accepted a complex colon operand");

    const auto integerMath = runBoth("value = sin(int8(1));\n");
    require(hasDiagnostic(integerMath.interpreter,
                          "does not support the supplied numeric class"),
            "interpreter accepted integer transcendental math");
    require(hasDiagnostic(integerMath.vm,
                          "does not support the supplied numeric class"),
            "VM accepted integer transcendental math");

    const auto binaryComplex = runBoth("value = atan2(1 + 1i, 2);\n");
    require(hasDiagnostic(binaryComplex.interpreter,
                          "requires real floating-point inputs"),
            "interpreter accepted complex atan2 input");
    require(hasDiagnostic(binaryComplex.vm,
                          "requires real floating-point inputs"),
            "VM accepted complex atan2 input");

    const auto integerEpsilon = runBoth("value = eps(uint8(1));\n");
    require(hasDiagnostic(integerEpsilon.interpreter,
                          "eps accepts double, single"),
            "interpreter accepted integer eps input");
    require(hasDiagnostic(integerEpsilon.vm,
                          "eps accepts double, single"),
            "VM accepted integer eps input");

    const auto integerNan = runBoth("value = nan(2, 'uint8');\n");
    require(hasDiagnostic(integerNan.interpreter,
                          "trailing class name is not supported"),
            "interpreter constructed integer NaN values");
    require(hasDiagnostic(integerNan.vm,
                          "trailing class name is not supported"),
            "VM constructed integer NaN values");
}

} // namespace

int main() {
    try {
        const auto result = runBoth(R"(
literal_i = 2i;
literal_j = 3.5e-1j;
cancelled_sum = (3 + 4i) + (5 - 4i);
product = (1 + 2i) * (3 - 4i);
quotient = (1 + 2i) / (3 - 4i);
power = (-1) ^ 0.5;
vector = [1 + 2i, 3 - 4i];
ctrans = vector';
ntrans = vector.';
constructed = complex(single([1, 2]), single(3));
forced = complex(12);
forced_is_real = isreal(forced);
cancel_is_real = isreal(cancelled_sum);
complex_text = string(1 + 2i);
realpart = real(vector);
imagpart = imag(vector);
conjugated = conj(vector);
magnitude = abs(3 + 4i);
sqrt_neg = sqrt(-1);
log_neg = log(-1);
asin_two = asin(2);
acos_two = acos(2);
single_sqrt = sqrt(single(-1));
hyper_acosh = acosh(-2);
hyper_asinh = asinh(2i);
hyper_atanh = atanh(2);
decimal_log = log10(-1);
binary_log = log2(-1);
rounded_complex = round([1.5 + 2.4i, -1.5 - 2.6i]);
floored_complex = floor(1.2 + 2.8i);
complex_sign = sign(3 + 4i);
integer_round = round(int16([-2, 3]));
integer_sign = sign(int16([-2, 0, 3]));
predicate_input = [1, inf, nan, complex(1, inf)];
finite_flags = isfinite(predicate_input);
infinite_flags = isinf(predicate_input);
nan_flags = isnan(predicate_input);
single_hyperbolic = tanh(single(1));
angles = atan2(single([1, 0]), [2, 1]);
distances = hypot([3; 5], single([4; 12]));
spacing = eps(single([0, 1, 2]));
complex_spacing = eps(1 + 2i);
reduce_input = single([3 + 4i, 1 + 8i, -3 - 4i]);
reduce_sum = sum(reduce_input);
reduce_product = prod(reduce_input);
reduce_mean = mean(reduce_input);
[reduce_minimum, reduce_minimum_index] = min(reduce_input);
[reduce_maximum, reduce_maximum_index] = max(reduce_input);
reduce_any = any(reduce_input);
reduce_all = all(reduce_input);
[find_rows, find_columns, find_values] = ...
    find([0, 2 + 3i; 4 - 5i, 0]);
scan_input = single([1 + 2i, 3 - 4i, -2 + 1i]);
running_sum = cumsum(scan_input);
running_product = cumprod(scan_input);
running_minimum = cummin(scan_input);
running_maximum = cummax(scan_input);
complex_difference = diff(scan_input);
complex_grid = linspace(1 + 2i, 5 - 2i, uint64(3));
single_grid = linspace(single(1), single(2), uint8(3));
typed_zeros = zeros(uint64(2), uint64(3), "single");
typed_ones = ones([1, 3], "uint64");
typed_eye = eye(2, 3, "int16");
typed_infinity = inf(1, 2, "single");
typed_nan = nan(2, 1);
scalar_zero = zeros();
empty_cell = cell();
ordered = (1 + 9i) < (2 - 99i);
equal = (1 + 9i) == (1 + 9i);
not_equal = (1 + 9i) ~= (1 - 9i);
)");
        verifySuccess(result.interpreter);
        verifySuccess(result.vm);
        verifyDiagnostics();

        require(mparser::runtimeValueToString(
                    variable(result.vm, "vector")) ==
                    "[1+2i 3-4i]",
                "complex vector display omitted a component");
        require(mparser::runtimeValueToString(
                    variable(result.vm, "ctrans")) ==
                    "[1-2i; 3+4i]",
                "complex matrix display lost conjugation");

        require(mparser::runtimeParseNumericLiteral("2i").has_value(),
                "shared complex literal parser rejected 2i");
        require(!mparser::runtimeParseNumericLiteral("2ii"),
                "shared complex literal parser accepted malformed input");

        std::cout << "Complex numeric smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Complex numeric smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
