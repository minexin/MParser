#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/semantic/semantic.h"

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
    require(parse.diagnostics.empty(), "scan source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "scan source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(), "scan source did not lower");

    mparser::Interpreter interpreter;
    auto interpreterResult = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto vmResult = vm.run(bytecode, semantic);
    return RuntimePair{std::move(interpreterResult), std::move(vmResult)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing runtime variable: " +
                             std::string(name));
}

template <typename Result>
bool hasDiagnostic(const Result& result, std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void requireNumeric(const mparser::RuntimeValue& value,
                    const std::vector<size_t>& dimensions,
                    const std::vector<double>& logicalValues,
                    mparser::RuntimeNumericClass numericClass,
                    std::string_view context) {
    require(mparser::isRuntimeNumericValue(value), context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(mparser::runtimeShapeElementCount(value) == logicalValues.size(),
            context);
    require(value.numericClass == numericClass, context);
    for (size_t index = 0; index < logicalValues.size(); ++index) {
        const auto actual = mparser::runtimeNumericElement(value, index);
        require(actual.has_value(), context);
        if (std::isnan(logicalValues[index])) {
            require(std::isnan(*actual), context);
        } else {
            require(*actual == logicalValues[index], context);
        }
    }
}

template <typename Result>
void verifyCumulativeBehavior(const Result& result) {
    requireNumeric(variable(result, "columnSums"), {2, 3, 2},
                   {1, 3, 3, 7, 5, 11, 7, 15, 9, 19, 11, 23},
                   mparser::RuntimeNumericClass::Double,
                   "default cumulative sum is wrong");
    requireNumeric(variable(result, "rowProducts"), {2, 3, 2},
                   {1, 2, 3, 8, 15, 48, 7, 8, 63, 80, 693, 960},
                   mparser::RuntimeNumericClass::Double,
                   "dimension cumulative product is wrong");
    requireNumeric(variable(result, "reversePages"), {2, 3, 2},
                   {8, 10, 12, 14, 16, 18, 7, 8, 9, 10, 11, 12},
                   mparser::RuntimeNumericClass::Double,
                   "reverse N-D cumulative sum is wrong");
    requireNumeric(variable(result, "unchanged"), {2, 3, 2},
                   {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
                   mparser::RuntimeNumericClass::Double,
                   "cumulative dimension beyond ndims changed the input");
    requireNumeric(variable(result, "runningMin"), {2, 3},
                   {3, 2, 1, 2, 1, 2},
                   mparser::RuntimeNumericClass::Double,
                   "cumulative minimum is wrong");
    requireNumeric(variable(result, "reverseMax"), {2, 3},
                   {7, 7, 6, 6, 5, 5},
                   mparser::RuntimeNumericClass::Double,
                   "reverse cumulative maximum is wrong");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    requireNumeric(variable(result, "includeSums"), {2, 3},
                   {nan, nan, 2, nan, nan, nan},
                   mparser::RuntimeNumericClass::Double,
                   "included NaN cumulative sum is wrong");
    requireNumeric(variable(result, "omitSums"), {2, 3},
                   {0, 1, 2, 2, 0, 3},
                   mparser::RuntimeNumericClass::Double,
                   "omitted NaN cumulative sum is wrong");
    requireNumeric(variable(result, "omitProducts"), {2, 3},
                   {1, 1, 2, 1, 2, 3},
                   mparser::RuntimeNumericClass::Double,
                   "omitted NaN cumulative product is wrong");
    requireNumeric(variable(result, "omitMinima"), {2, 3},
                   {nan, 1, 2, 1, 2, 1},
                   mparser::RuntimeNumericClass::Double,
                   "default omitted NaN cumulative minimum is wrong");
    requireNumeric(variable(result, "includeMaxima"), {2, 3},
                   {nan, 1, nan, nan, nan, nan},
                   mparser::RuntimeNumericClass::Double,
                   "included NaN cumulative maximum is wrong");
    requireNumeric(variable(result, "reverseOmit"), {1, 3},
                   {4, 3, 3}, mparser::RuntimeNumericClass::Double,
                   "reverse omitted cumulative sum is wrong");

    requireNumeric(variable(result, "logicalSums"), {1, 3},
                   {1, 1, 2}, mparser::RuntimeNumericClass::Double,
                   "logical cumulative sum did not return double");
    requireNumeric(variable(result, "logicalProducts"), {1, 3},
                   {1, 1, 0}, mparser::RuntimeNumericClass::Double,
                   "logical cumulative product did not return double");
    requireNumeric(variable(result, "logicalMinima"), {1, 3},
                   {1, 0, 0}, mparser::RuntimeNumericClass::Logical,
                   "logical cumulative minimum did not preserve class");
    requireNumeric(variable(result, "emptyScan"), {0, 3}, {},
                   mparser::RuntimeNumericClass::Double,
                   "empty cumulative result shape is wrong");
    requireNumeric(variable(result, "emptyRowScan"), {1, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "empty row cumulative result shape is wrong");
}

template <typename Result>
void verifyDifferenceBehavior(const Result& result) {
    requireNumeric(variable(result, "firstDifference"), {1, 3},
                   {3, 5, 7}, mparser::RuntimeNumericClass::Double,
                   "first difference is wrong");
    requireNumeric(variable(result, "secondDifference"), {1, 3},
                   {5, 5, 5}, mparser::RuntimeNumericClass::Double,
                   "second difference is wrong");
    requireNumeric(variable(result, "matrixDifference"), {2, 2},
                   {2, 5, 3, 6}, mparser::RuntimeNumericClass::Double,
                   "matrix dimension difference is wrong");
    requireNumeric(variable(result, "pageDifference"), {2, 3},
                   {6, 6, 6, 6, 6, 6},
                   mparser::RuntimeNumericClass::Double,
                   "N-D page difference is wrong");
    requireNumeric(variable(result, "logicalDifference"), {1, 2},
                   {-1, 1}, mparser::RuntimeNumericClass::Double,
                   "logical difference did not return double");
    requireNumeric(variable(result, "emptyOrderDifference"), {1, 2},
                   {3, 5}, mparser::RuntimeNumericClass::Double,
                   "empty diff order did not select the default order");
    requireNumeric(variable(result, "highOrder"), {0, 3}, {},
                   mparser::RuntimeNumericClass::Double,
                   "high-order difference did not stay on one dimension");
    requireNumeric(variable(result, "pastDimension"), {2, 3, 2, 1, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "difference beyond ndims has the wrong shape");
    requireNumeric(variable(result, "emptyDifference"), {0, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "0-by-0 difference shape is wrong");
    requireNumeric(variable(result, "scalarDifference"), {0, 1}, {},
                   mparser::RuntimeNumericClass::Double,
                   "scalar difference shape is wrong");
}

void runBehaviorSmoke() {
    const auto result = runBoth(R"(A = reshape(1:12, 2, 3, 2);
columnSums = cumsum(A);
rowProducts = cumprod(A, 2);
reversePages = cumsum(A, 3, "reverse");
unchanged = cumsum(A, 5);

X = [3 1 5; 2 6 4];
runningMin = cummin(X, 2);
reverseMax = cummax([3 1 2; 7 6 5], 1, "reverse");

N = [nan 2 nan; 1 nan 3];
includeSums = cumsum(N, 1, "includenan");
omitSums = cumsum(N, 1, "omitnan");
omitProducts = cumprod(N, 2, "omitmissing");
omitMinima = cummin(N, 2);
includeMaxima = cummax(N, 2, "includemissing");
reverseOmit = cumsum([1 nan 3], 2, "reverse", "omitnan");

logicalSums = cumsum(logical([1 0 1]));
logicalProducts = cumprod(logical([1 1 0]));
logicalMinima = cummin(logical([1 0 1]));
emptyScan = cumsum(zeros(0, 3));
emptyRowScan = cumprod(zeros(1, 0), "reverse");

firstDifference = diff([1 4 9 16]);
secondDifference = diff([0 5 15 30 50], 2);
matrixDifference = diff([1 3 6; 10 15 21], 1, 2);
pageDifference = diff(A, 1, 3);
logicalDifference = diff(logical([1 0 1]));
emptyOrderDifference = diff([1 4 9], [], 2);
highOrder = diff([1 2 3; 4 5 6], 3);
pastDimension = diff(A, 1, 5);
emptyDifference = diff([]);
scalarDifference = diff(4);
)");

    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected valid scan operations");
    require(result.vm.diagnostics.empty(),
            "VM rejected valid scan operations");
    verifyCumulativeBehavior(result.interpreter);
    verifyCumulativeBehavior(result.vm);
    verifyDifferenceBehavior(result.interpreter);
    verifyDifferenceBehavior(result.vm);
}

void requireBothDiagnostic(std::string_view source,
                           std::string_view diagnostic,
                           std::string_view context) {
    const auto result = runBoth(source);
    require(hasDiagnostic(result.interpreter, diagnostic), context);
    require(hasDiagnostic(result.vm, diagnostic), context);
}

void runDiagnosticSmoke() {
    requireBothDiagnostic("A = [1 2]; bad = cumsum(A, 0);\n",
                          "positive integer scalar",
                          "dimension zero was accepted");
    requireBothDiagnostic("A = [1 2]; bad = cumsum(A, [1 2]);\n",
                          "positive integer scalar",
                          "dimension vector was accepted");
    requireBothDiagnostic("A = [1 2]; bad = cumsum(A, \"sideways\");\n",
                          "unsupported cumulative option",
                          "invalid cumulative direction was accepted");
    requireBothDiagnostic(
        "A = [1 2]; bad = cumsum(A, \"forward\", \"reverse\");\n",
        "direction was specified more than once",
        "duplicate cumulative direction was accepted");
    requireBothDiagnostic(
        "A = [1 2]; bad = cumsum(A, \"omitnan\", \"includenan\");\n",
        "missing-value policy was specified more than once",
        "duplicate missing policy was accepted");
    requireBothDiagnostic("A = [1 2]; [a, b] = cummax(A);\n",
                          "supports at most one output",
                          "multiple cumulative outputs were accepted");
    requireBothDiagnostic("A = [1 2]; bad = diff(A, 0);\n",
                          "positive integer scalar or []",
                          "zero diff order was accepted");
    requireBothDiagnostic("A = [1 2]; bad = diff(A, 1.5);\n",
                          "positive integer scalar or []",
                          "fractional diff order was accepted");
    requireBothDiagnostic("A = [1 2]; bad = diff(A, 1, 0);\n",
                          "dimension must be a positive integer scalar",
                          "zero diff dimension was accepted");
    requireBothDiagnostic("A = [1 2]; [a, b] = diff(A);\n",
                          "supports at most one output",
                          "multiple diff outputs were accepted");
}

} // namespace

int main() {
    try {
        runBehaviorSmoke();
        runDiagnosticSmoke();
        std::cout << "Scan/diff smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scan/diff smoke failure: " << error.what() << '\n';
        return 1;
    }
}
