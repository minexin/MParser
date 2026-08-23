#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
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
    require(parse.diagnostics.empty(), "reduction source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "reduction source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "reduction source did not lower");

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
void verifyReductionBehavior(const Result& result) {
    requireNumeric(variable(result, "columnTotals"), {1, 3, 4},
                   {3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47},
                   mparser::RuntimeNumericClass::Double,
                   "default N-D sum is wrong");
    requireNumeric(variable(result, "rowTotals"), {2, 1, 4},
                   {9, 12, 27, 30, 45, 48, 63, 66},
                   mparser::RuntimeNumericClass::Double,
                   "explicit-dimension sum is wrong");
    requireNumeric(variable(result, "pageTotals"), {1, 1, 4},
                   {21, 57, 93, 129},
                   mparser::RuntimeNumericClass::Double,
                   "dimension-vector sum is wrong");
    requireNumeric(variable(result, "grandTotal"), {1, 1}, {300},
                   mparser::RuntimeNumericClass::Double,
                   "all-dimension sum is wrong");
    requireNumeric(variable(result, "pageMeans"), {2, 3},
                   {10, 11, 12, 13, 14, 15},
                   mparser::RuntimeNumericClass::Double,
                   "N-D mean is wrong");
    requireNumeric(variable(result, "vectorProduct"), {1, 1}, {24},
                   mparser::RuntimeNumericClass::Double,
                   "vector product is wrong");

    requireNumeric(variable(result, "minByColumn"), {1, 3}, {2, 1, 4},
                   mparser::RuntimeNumericClass::Double,
                   "minimum values are wrong");
    requireNumeric(variable(result, "minRow"), {1, 3}, {2, 1, 2},
                   mparser::RuntimeNumericClass::Double,
                   "minimum indices are wrong");
    requireNumeric(variable(result, "maxByRow"), {2, 1}, {5, 7},
                   mparser::RuntimeNumericClass::Double,
                   "maximum values are wrong");
    requireNumeric(variable(result, "maxColumn"), {2, 1}, {2, 3},
                   mparser::RuntimeNumericClass::Double,
                   "maximum dimension indices are wrong");
    requireNumeric(variable(result, "maxLinearIndex"), {2, 1}, {3, 6},
                   mparser::RuntimeNumericClass::Double,
                   "maximum linear indices are wrong");
    requireNumeric(variable(result, "maximum"), {1, 1}, {24},
                   mparser::RuntimeNumericClass::Double,
                   "all-dimension maximum is wrong");
    requireNumeric(variable(result, "maximumIndex"), {1, 1}, {24},
                   mparser::RuntimeNumericClass::Double,
                   "all-dimension maximum index is wrong");
    requireNumeric(variable(result, "elementwiseMax"), {2, 3},
                   {4, 4, 4, 4, 5, 4},
                   mparser::RuntimeNumericClass::Double,
                   "elementwise maximum is wrong");

    requireNumeric(variable(result, "omitTotals"), {1, 2}, {4, 4},
                   mparser::RuntimeNumericClass::Double,
                   "omitnan sum is wrong");
    requireNumeric(variable(result, "includeTotals"), {1, 2},
                   {4, std::numeric_limits<double>::quiet_NaN()},
                   mparser::RuntimeNumericClass::Double,
                   "includenan sum is wrong");
    requireNumeric(variable(result, "nanSafeMax"), {1, 2}, {3, 4},
                   mparser::RuntimeNumericClass::Double,
                   "default NaN extrema policy is wrong");
    requireNumeric(variable(result, "includeMax"), {1, 2},
                   {3, std::numeric_limits<double>::quiet_NaN()},
                   mparser::RuntimeNumericClass::Double,
                   "included NaN maximum is wrong");

    requireNumeric(variable(result, "nonzeroRows"), {2, 1}, {1, 1},
                   mparser::RuntimeNumericClass::Logical,
                   "dimension-aware any is wrong");
    requireNumeric(variable(result, "fullyNonzeroColumns"), {1, 3},
                   {0, 0, 0}, mparser::RuntimeNumericClass::Logical,
                   "dimension-aware all is wrong");
    requireNumeric(variable(result, "nanTruth"), {1, 1}, {1},
                   mparser::RuntimeNumericClass::Logical,
                   "any treated NaN as false");
    requireNumeric(variable(result, "nanAll"), {1, 1}, {1},
                   mparser::RuntimeNumericClass::Logical,
                   "all treated NaN as false");
    requireNumeric(variable(result, "logicalSum"), {1, 1}, {2},
                   mparser::RuntimeNumericClass::Double,
                   "default logical sum did not return double");
    requireNumeric(variable(result, "nativeLogicalSum"), {1, 1}, {1},
                   mparser::RuntimeNumericClass::Logical,
                   "native logical sum did not preserve its class");

    requireNumeric(variable(result, "emptySum"), {1, 1}, {0},
                   mparser::RuntimeNumericClass::Double,
                   "empty sum identity is wrong");
    requireNumeric(variable(result, "emptyProduct"), {1, 1}, {1},
                   mparser::RuntimeNumericClass::Double,
                   "empty product identity is wrong");
    requireNumeric(variable(result, "emptyMean"), {1, 1},
                   {std::numeric_limits<double>::quiet_NaN()},
                   mparser::RuntimeNumericClass::Double,
                   "empty mean is wrong");
    requireNumeric(variable(result, "emptyMaximum"), {0, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "empty maximum shape is wrong");
    requireNumeric(variable(result, "unchanged"), {2, 3},
                   {0, -2, 5, 0, 0, 7},
                   mparser::RuntimeNumericClass::Double,
                   "reduction beyond ndims changed values");
}

template <typename Result>
void verifyFindBehavior(const Result& result) {
    requireNumeric(variable(result, "linearIndices"), {3, 1}, {2, 3, 6},
                   mparser::RuntimeNumericClass::Double,
                   "matrix find indices are wrong");
    requireNumeric(variable(result, "lastRows"), {2, 1}, {1, 2},
                   mparser::RuntimeNumericClass::Double,
                   "last find row indices are wrong");
    requireNumeric(variable(result, "lastColumns"), {2, 1}, {2, 3},
                   mparser::RuntimeNumericClass::Double,
                   "last find column indices are wrong");
    requireNumeric(variable(result, "lastValues"), {2, 1}, {5, 7},
                   mparser::RuntimeNumericClass::Double,
                   "last find values are wrong");
    requireNumeric(variable(result, "rowIndices"), {1, 2}, {2, 4},
                   mparser::RuntimeNumericClass::Double,
                   "row-vector find orientation is wrong");
    requireNumeric(variable(result, "none"), {0, 1}, {},
                   mparser::RuntimeNumericClass::Double,
                   "empty matrix find orientation is wrong");
    requireNumeric(variable(result, "rowNone"), {1, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "empty row find orientation is wrong");
    requireNumeric(variable(result, "zeroScalar"), {0, 0}, {},
                   mparser::RuntimeNumericClass::Double,
                   "zero scalar find shape is wrong");
    requireNumeric(variable(result, "tensorRows"), {3, 1}, {2, 1, 1},
                   mparser::RuntimeNumericClass::Double,
                   "N-D find row indices are wrong");
    requireNumeric(variable(result, "tensorColumns"), {3, 1}, {1, 3, 4},
                   mparser::RuntimeNumericClass::Double,
                   "N-D find folded column indices are wrong");
    requireNumeric(variable(result, "tensorValues"), {3, 1}, {1, 2, 3},
                   mparser::RuntimeNumericClass::Double,
                   "N-D find values are wrong");
}

void runBehaviorSmoke() {
    const auto result = runBoth(R"(A = reshape(1:24, 2, 3, 4);
columnTotals = sum(A);
rowTotals = sum(A, 2);
pageTotals = sum(A, [1 2]);
grandTotal = sum(A, "all");
pageMeans = mean(A, 3);
vectorProduct = prod([1 2 3 4]);

X = [0 5 0; -2 0 7];
[maxByRow, maxColumn] = max(X, [], 2);
[maxByRowLinear, maxLinearIndex] = max(X, [], 2, "linear");
linearIndices = find(X);
[lastRows, lastColumns, lastValues] = find(X, 2, "last");
nonzeroRows = any(X, 2);
fullyNonzeroColumns = all(X ~= 0, 1);

Y = [3 1 5; 2 1 4];
[minByColumn, minRow] = min(Y);
elementwiseMax = max(Y, 4);
[maximum, maximumIndex] = max(A, [], "all");
rowIndices = find([0 4 0 8]);
none = find(zeros(2, 2));
rowNone = find([0 0]);
zeroScalar = find(0);

T = reshape([0 1 0 0 2 0 3 0], 2, 2, 2);
[tensorRows, tensorColumns, tensorValues] = find(T);

N = [1 nan; 3 4];
omitTotals = sum(N, 1, "omitnan");
includeTotals = sum(N, 1, "includenan");
nanSafeMax = max(N, [], 1);
includeMax = max(N, [], 1, "includenan");
nanTruth = any(nan);
nanAll = all(nan);

logicalSum = sum(logical([1 1 0]));
nativeLogicalSum = sum(logical([1 1 0]), "native");
emptySum = sum([]);
emptyProduct = prod([]);
emptyMean = mean([]);
emptyMaximum = max([]);
unchanged = sum(X, 3);
)");

    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected valid reductions");
    require(result.vm.diagnostics.empty(),
            "VM rejected valid reductions");
    verifyReductionBehavior(result.interpreter);
    verifyReductionBehavior(result.vm);
    verifyFindBehavior(result.interpreter);
    verifyFindBehavior(result.vm);
}

void runDiagnosticSmoke() {
    const auto zeroDimension =
        runBoth("A = [1 2; 3 4]; bad = sum(A, 0);\n");
    require(hasDiagnostic(zeroDimension.interpreter,
                          "positive integers"),
            "interpreter accepted dimension zero");
    require(hasDiagnostic(zeroDimension.vm, "positive integers"),
            "VM accepted dimension zero");

    const auto duplicate =
        runBoth("A = [1 2; 3 4]; bad = sum(A, [1 1]);\n");
    require(hasDiagnostic(duplicate.interpreter, "must not repeat"),
            "interpreter accepted duplicate dimensions");
    require(hasDiagnostic(duplicate.vm, "must not repeat"),
            "VM accepted duplicate dimensions");

    const auto direction = runBoth(
        "A = [1 0 2]; bad = find(A, 1, \"middle\");\n");
    require(hasDiagnostic(direction.interpreter,
                          "direction must be"),
            "interpreter accepted an invalid find direction");
    require(hasDiagnostic(direction.vm, "direction must be"),
            "VM accepted an invalid find direction");

    const auto outputCount = runBoth(
        "A = [1 2; 3 4]; [left, right] = sum(A);\n");
    require(hasDiagnostic(outputCount.interpreter,
                          "at most one output"),
            "interpreter accepted multiple sum outputs");
    require(hasDiagnostic(outputCount.vm, "at most one output"),
            "VM accepted multiple sum outputs");

    const auto vectorIndex = runBoth(
        "A = reshape(1:8, 2, 2, 2); [v, i] = max(A, [], [1 2]);\n");
    require(hasDiagnostic(vectorIndex.interpreter,
                          "requires \"linear\""),
            "interpreter guessed a vector-dimension extrema index");
    require(hasDiagnostic(vectorIndex.vm, "requires \"linear\""),
            "VM guessed a vector-dimension extrema index");
}

} // namespace

int main() {
    try {
        runBehaviorSmoke();
        runDiagnosticSmoke();
        std::cout << "Reduction/find smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Reduction/find smoke failure: " << error.what()
                  << "\n";
        return 1;
    }
}
