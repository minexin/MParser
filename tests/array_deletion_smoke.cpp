#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_shape.h"
#include "mparser/semantic.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
    mparser::BytecodeProgram bytecode;
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
    require(parse.diagnostics.empty(), "array-deletion source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "array-deletion source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "array-deletion source did not lower");

    mparser::Interpreter interpreter;
    auto interpreterResult = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto vmResult = vm.run(bytecode, semantic);
    return RuntimePair{std::move(interpreterResult), std::move(vmResult),
                       std::move(bytecode)};
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

void requireArray(const mparser::RuntimeValue& value,
                  const std::vector<size_t>& dimensions,
                  const std::vector<double>& elements,
                  mparser::RuntimeNumericClass numericClass,
                  std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Vector ||
                value.kind == mparser::RuntimeValueKind::Matrix,
            context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(value.elements == elements, context);
    require(value.numericClass == numericClass, context);
}

template <typename Result>
void verifyDeletion(const Result& result) {
    requireArray(variable(result, "row"), {1, 3}, {1, 3, 5},
                 mparser::RuntimeNumericClass::Double,
                 "row-vector deletion is wrong");
    requireArray(variable(result, "column"), {2, 1}, {1, 3},
                 mparser::RuntimeNumericClass::Double,
                 "column-vector logical deletion is wrong");
    requireArray(variable(result, "A"), {1, 4}, {2, 5, 8, 11},
                 mparser::RuntimeNumericClass::Double,
                 "matrix row deletion is wrong");
    requireArray(variable(result, "B"), {3, 2},
                 {1, 7, 2, 8, 3, 9},
                 mparser::RuntimeNumericClass::Double,
                 "matrix column deletion is wrong");
    requireArray(variable(result, "C"), {2, 3, 2},
                 {7, 19, 9, 21, 11, 23,
                  8, 20, 10, 22, 12, 24},
                 mparser::RuntimeNumericClass::Double,
                 "N-dimensional slice deletion is wrong");
    requireArray(variable(result, "L"), {1, 2}, {1, 0},
                 mparser::RuntimeNumericClass::Logical,
                 "logical deletion lost the numeric class");
    requireArray(variable(result, "whole"), {0, 0}, {},
                 mparser::RuntimeNumericClass::Double,
                 "whole-array deletion did not produce 0-by-0");
    requireArray(variable(result, "empty"), {0, 0}, {},
                 mparser::RuntimeNumericClass::Double,
                 "literal [] is not a 0-by-0 array");
    requireArray(variable(result, "unchanged"), {1, 3}, {4, 5, 6},
                 mparser::RuntimeNumericClass::Double,
                 "empty logical deletion changed the target");
}

void runDeletionBehaviorSmoke() {
    const auto result = runBoth(R"(row = [1 2 3 4 5];
row([2 4]) = [];
column = [1; 2; 3; 4];
column(logical([0 1 0 1])) = [];
A = reshape(1:12, 3, 4);
A([1 3], :) = [];
B = reshape(1:12, 3, 4);
B(:, logical([0 1 0 1])) = [];
C = reshape(1:24, 2, 3, 4);
C(:, :, [1 3]) = [];
L = logical([1 0 1 0]);
L([2 3]) = [];
whole = [1 2; 3 4];
whole(:) = [];
empty = [];
unchanged = [4 5 6];
unchanged(logical([0 0 0])) = [];
)");

    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected valid null assignment");
    require(result.vm.diagnostics.empty(),
            "VM rejected valid null assignment");
    verifyDeletion(result.interpreter);
    verifyDeletion(result.vm);

    size_t nullStoreCount = 0;
    bool foundColumnMetadata = false;
    for (const auto& instruction : result.bytecode.instructions) {
        if (instruction.op != mparser::BytecodeOp::StoreIndex ||
            !instruction.nullAssignment) {
            continue;
        }
        ++nullStoreCount;
        if (instruction.operand == "B") {
            foundColumnMetadata =
                instruction.colonSubscripts ==
                std::vector<bool>({true, false});
        }
    }
    require(nullStoreCount == 8,
            "bytecode did not preserve every direct null assignment");
    require(foundColumnMetadata,
            "bytecode did not preserve direct colon syntax");
}

void runDirectSyntaxDistinctionSmoke() {
    const auto result = runBoth(R"(A = [1 2 3];
rhs = [];
A(2) = rhs;
)");
    require(hasDiagnostic(result.interpreter,
                          "same number of elements"),
            "interpreter treated an empty variable as null syntax");
    require(hasDiagnostic(result.vm, "same number of elements"),
            "VM treated an empty variable as null syntax");
    requireArray(variable(result.interpreter, "A"), {1, 3}, {1, 2, 3},
                 mparser::RuntimeNumericClass::Double,
                 "interpreter mutated a failed ordinary assignment");
    requireArray(variable(result.vm, "A"), {1, 3}, {1, 2, 3},
                 mparser::RuntimeNumericClass::Double,
                 "VM mutated a failed ordinary assignment");
}

void runInvalidSliceSmoke() {
    const auto multiple = runBoth(
        "A = [1 2; 3 4]; A(1, 1) = [];\n");
    require(hasDiagnostic(multiple.interpreter,
                          "only one non-colon subscript"),
            "interpreter accepted two deletion dimensions");
    require(hasDiagnostic(multiple.vm,
                          "only one non-colon subscript"),
            "VM accepted two deletion dimensions");

    const auto folded = runBoth(
        "A = reshape(1:24, 2, 3, 4); A(:, 1) = [];\n");
    require(hasDiagnostic(folded.interpreter,
                          "one subscript per dimension"),
            "interpreter accepted folded N-D deletion");
    require(hasDiagnostic(folded.vm,
                          "one subscript per dimension"),
            "VM accepted folded N-D deletion");
}

} // namespace

int main() {
    try {
        runDeletionBehaviorSmoke();
        runDirectSyntaxDistinctionSmoke();
        runInvalidSliceSmoke();
        std::cout << "Array deletion smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Array deletion smoke failure: " << error.what()
                  << "\n";
        return 1;
    }
}
