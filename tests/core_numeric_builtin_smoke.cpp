#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/semantic.h"

#include <algorithm>
#include <cmath>
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
            "core numeric builtin source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "core numeric builtin source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "core numeric builtin source did not lower");

    mparser::Interpreter interpreter;
    mparser::BytecodeVm vm;
    return RuntimePair{
        interpreter.run(semantic), vm.run(bytecode, semantic)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error(
        "missing core numeric builtin variable: " +
        std::string(name));
}

bool near(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) {
        return std::isnan(left) && std::isnan(right);
    }
    if (std::isinf(left) || std::isinf(right)) {
        return left == right;
    }
    const double scale = std::max({1.0, std::fabs(left),
                                   std::fabs(right)});
    return std::fabs(left - right) <= scale * 1e-6;
}

void requireNumeric(
    const mparser::RuntimeValue& value,
    mparser::RuntimeNumericClass numericClass,
    const std::vector<size_t>& dimensions,
    const std::vector<double>& expected,
    std::string_view context) {
    require(value.numericClass == numericClass, context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(mparser::runtimeShapeElementCount(value) == expected.size(),
            context);
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto element =
            mparser::runtimeNumericElementValue(value, index);
        require(element.has_value(), context);
        require(!element->complex, context);
        require(near(element->real, expected[index]), context);
    }
}

template <typename Result>
void requireLogical(const Result& result, std::string_view name,
                    bool expected) {
    const auto& value = variable(result, name);
    requireNumeric(value, mparser::RuntimeNumericClass::Logical,
                   {1, 1}, {expected ? 1.0 : 0.0}, name);
}

template <typename Result>
void verifySuccess(const Result& result) {
    if (!result.diagnostics.empty()) {
        throw std::runtime_error(
            "core numeric builtin execution emitted diagnostic: " +
            result.diagnostics.front().message);
    }

    requireLogical(result, "scalar_shape", true);
    requireLogical(result, "scalar_vector", true);
    requireLogical(result, "empty_matrix_vector", false);
    requireLogical(result, "empty_row_vector", true);
    requireLogical(result, "empty_row", true);
    requireLogical(result, "empty_column", true);
    requireLogical(result, "trailing_matrix", true);
    requireLogical(result, "cube_matrix", false);
    requireLogical(result, "nonscalar", false);

    requireLogical(result, "nan_equal", true);
    requireLogical(result, "nan_plain_unequal", true);
    requireLogical(result, "cross_float_equal", true);
    requireLogical(result, "cross_integer_equal", true);
    requireLogical(result, "cross_integer_different", true);
    requireLogical(result, "complex_nan_equal", true);
    requireLogical(result, "complex_nan_different", true);
    requireLogical(result, "cell_nan_equal", true);
    requireLogical(result, "struct_nan_equal", true);
    requireLogical(result, "variadic_equal", true);

    requireNumeric(variable(result, "mod_values"),
                   mparser::RuntimeNumericClass::Double, {1, 4},
                   {2.0, -2.0, 1.0, -1.0},
                   "floating mod values mismatch");
    requireNumeric(variable(result, "rem_values"),
                   mparser::RuntimeNumericClass::Double, {1, 4},
                   {-1.0, 1.0, 1.0, -1.0},
                   "floating rem values mismatch");
    requireNumeric(variable(result, "expanded_mod"),
                   mparser::RuntimeNumericClass::Double, {2, 2},
                   {2.0, 1.0, -1.0, -2.0},
                   "implicit expansion mod mismatch");
    requireNumeric(variable(result, "integer_mod"),
                   mparser::RuntimeNumericClass::Int16, {1, 2},
                   {2.0, -2.0}, "integer mod mismatch");
    requireNumeric(variable(result, "integer_zero"),
                   mparser::RuntimeNumericClass::Int16, {1, 2},
                   {5.0, 0.0}, "integer zero-divisor mismatch");
    requireNumeric(variable(result, "single_mod"),
                   mparser::RuntimeNumericClass::Single, {1, 2},
                   {1.0, 2.0}, "single mod mismatch");

    requireNumeric(variable(result, "next_values"),
                   mparser::RuntimeNumericClass::Double, {1, 7},
                   {0.0, -1.0, 0.0, 3.0, 3.0,
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()},
                   "double nextpow2 mismatch");
    requireNumeric(variable(result, "next_single"),
                   mparser::RuntimeNumericClass::Single, {1, 3},
                   {-1.0, 0.0, 3.0}, "single nextpow2 mismatch");
    requireNumeric(variable(result, "next_integer"),
                   mparser::RuntimeNumericClass::Int16, {1, 4},
                   {3.0, 0.0, 3.0, 3.0},
                   "integer nextpow2 mismatch");
    requireNumeric(variable(result, "next_complex"),
                   mparser::RuntimeNumericClass::Double, {1, 2},
                   {3.0, 3.0}, "complex nextpow2 mismatch");
    requireNumeric(variable(result, "next_logical"),
                   mparser::RuntimeNumericClass::Double, {1, 1},
                   {0.0}, "logical nextpow2 mismatch");
    requireNumeric(variable(result, "signed_absolute"),
                   mparser::RuntimeNumericClass::Double, {1, 3},
                   {1.0, 2.0, 3.0},
                   "whitespace-separated signed literal mismatch");
    requireNumeric(variable(result, "spaced_subtraction"),
                   mparser::RuntimeNumericClass::Double, {1, 1},
                   {-1.0}, "spaced subtraction mismatch");
    requireNumeric(variable(result, "spaced_unary_plus"),
                   mparser::RuntimeNumericClass::Double, {1, 2},
                   {1.0, 2.0}, "whitespace-separated unary plus mismatch");
}

template <typename Result>
void requireFailure(const Result& result, std::string_view fragment) {
    require(!result.diagnostics.empty(),
            "invalid core numeric builtin call unexpectedly succeeded");
    const bool found = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) {
            return diagnostic.message.find(fragment) !=
                   std::string::npos;
        });
    if (!found) {
        throw std::runtime_error(
            "core numeric builtin diagnostic mismatch: " +
            result.diagnostics.front().message);
    }
}

} // namespace

int main() {
    try {
        const auto success = runBoth(R"(
scalar_shape = isscalar(3);
scalar_vector = isvector(3);
empty_matrix_vector = isvector([]);
empty_row_vector = isvector(zeros(1, 0));
empty_row = isrow(zeros(1, 0));
empty_column = iscolumn(zeros(0, 1));
trailing_matrix = ismatrix(ones(2, 2, 1));
cube_matrix = ismatrix(ones(2, 2, 2));
nonscalar = isscalar([1, 2]);

nan_equal = isequaln(nan, nan);
nan_plain_unequal = ~isequal(nan, nan);
cross_float_equal = isequal(single(1), double(1));
cross_integer_equal = isequal(int8(1), uint8(1));
cross_integer_different = ~isequal(int8(-1), uint8(255));
complex_nan_equal = isequaln(complex(nan, 1), complex(nan, 1));
complex_nan_different = ~isequaln(complex(nan, 1), complex(nan, 2));
cell_nan_equal = isequaln({nan}, {nan});
struct_nan_equal = isequaln(struct('x', nan), struct('x', nan));
variadic_equal = isequal(1, single(1), uint8(1));

mod_values = mod([-7, 7, 7, -7], [3, -3, 3, -3]);
rem_values = rem([-7, 7, 7, -7], [3, -3, 3, -3]);
expanded_mod = mod([-7; 7], [3, -3]);
integer_mod = mod(int16([-7, 7]), int16([3, -3]));
integer_zero = [mod(int16(5), int16(0)), rem(int16(5), int16(0))];
single_mod = mod(single([7, -7]), single([3, 3]));

next_values = nextpow2([0, 0.5, 1, 7, 8, inf, nan]);
next_single = nextpow2(single([0.5, 1, 7]));
next_integer = nextpow2(int16([-8, 0, 7, 8]));
next_complex = nextpow2(complex([3, -3], [4, -4]));
next_logical = nextpow2(true);
signed_absolute = abs([-1 2 -3]);
spaced_subtraction = [1 - 2];
spaced_unary_plus = [1 +2];
)");
        verifySuccess(success.interpreter);
        verifySuccess(success.vm);

        const auto complexMod = runBoth("bad = mod(1 + 2i, 2);");
        requireFailure(complexMod.interpreter, "real numeric");
        requireFailure(complexMod.vm, "real numeric");

        const auto equalityArity = runBoth("bad = isequal(1);");
        requireFailure(equalityArity.interpreter, "at least 2");
        requireFailure(equalityArity.vm, "at least 2");

        const auto shapeArity = runBoth("bad = isscalar(1, 2);");
        requireFailure(shapeArity.interpreter, "expects 1 input");
        requireFailure(shapeArity.vm, "expects 1 input");

        std::cout << "core numeric builtin smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
