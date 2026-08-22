#include "mparser/execution/bytecode/adaptive_bytecode_vm.h"
#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"
#include "mparser/execution/jit/typed_ir.h"

#include <cmath>
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
    mparser::SemanticResult semantic;
};

struct ProgramFixture {
    mparser::BytecodeProgram bytecode;
    mparser::SemanticResult semantic;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

ProgramFixture compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(), "logical-index source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "logical-index source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "logical-index source did not lower");

    return ProgramFixture{std::move(bytecode), std::move(semantic)};
}

RuntimePair runBoth(std::string_view source) {
    auto fixture = compile(source);

    mparser::Interpreter interpreter;
    auto interpreterResult = interpreter.run(fixture.semantic);
    mparser::BytecodeVm vm;
    auto vmResult = vm.run(fixture.bytecode, fixture.semantic);
    return RuntimePair{std::move(interpreterResult), std::move(vmResult),
                       std::move(fixture.bytecode),
                       std::move(fixture.semantic)};
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
                    const std::vector<double>& elements,
                    mparser::RuntimeNumericClass numericClass,
                    std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Vector ||
                value.kind == mparser::RuntimeValueKind::Matrix,
            context);
    require(value.numericClass == numericClass, context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(value.elements.size() == elements.size(), context);
    for (size_t index = 0; index < elements.size(); ++index) {
        require(std::fabs(value.elements[index] - elements[index]) < 1e-9,
                context);
    }
}

void requireScalar(const mparser::RuntimeValue& value, double expected,
                   mparser::RuntimeNumericClass numericClass,
                   std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Number, context);
    require(value.numericClass == numericClass, context);
    require(std::fabs(value.number - expected) < 1e-9, context);
}

template <typename Result>
void verifyLogicalBehavior(const Result& result) {
    const auto& mask = variable(result, "mask");
    requireNumeric(mask, {3, 4},
                   {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1},
                   mparser::RuntimeNumericClass::Logical,
                   "comparison did not produce a logical matrix");
    requireNumeric(variable(result, "picked"), {7, 1},
                   {9, 6, 10, 7, 11, 8, 12},
                   mparser::RuntimeNumericClass::Double,
                   "matrix logical indexing order or shape is wrong");
    requireNumeric(variable(result, "rowPicked"), {1, 2}, {10, 30},
                   mparser::RuntimeNumericClass::Double,
                   "row-vector logical indexing lost target orientation");
    requireNumeric(variable(result, "columnPicked"), {2, 1}, {10, 30},
                   mparser::RuntimeNumericClass::Double,
                   "column-vector logical indexing lost target orientation");
    requireNumeric(variable(result, "shortPicked"), {2, 1}, {1, 9},
                   mparser::RuntimeNumericClass::Double,
                   "short logical mask did not produce a column result");
    requireScalar(variable(result, "longFalsePicked"), 1,
                  mparser::RuntimeNumericClass::Double,
                  "false logical indices beyond the extent were rejected");
    requireNumeric(variable(result, "block"), {2, 2}, {2, 3, 10, 11},
                   mparser::RuntimeNumericClass::Double,
                   "multi-subscript logical selection is wrong");
    requireNumeric(variable(result, "A"), {3, 4},
                   {1, 2, 3, 4, 5, 100, 100, 100,
                    100, 100, 100, 100},
                   mparser::RuntimeNumericClass::Double,
                   "logical indexed scalar assignment is wrong");
    requireNumeric(variable(result, "B"), {3, 4},
                   {1, 20, 30, 4, 5, 6, 7, 8,
                    9, 100, 110, 12},
                   mparser::RuntimeNumericClass::Double,
                   "multi-subscript logical assignment is wrong");
    requireNumeric(variable(result, "L"), {1, 4}, {1, 1, 1, 1},
                   mparser::RuntimeNumericClass::Logical,
                   "logical target assignment did not coerce values");
    requireNumeric(variable(result, "LD"), {1, 4}, {1, 1, 1, 1},
                   mparser::RuntimeNumericClass::Double,
                   "double conversion did not change numeric class");
    requireNumeric(variable(result, "literal"), {2, 2}, {1, 0, 0, 1},
                   mparser::RuntimeNumericClass::Logical,
                   "logical matrix literal lost its class");
    requireNumeric(variable(result, "truths"), {2, 2, 2},
                   std::vector<double>(8, 1.0),
                   mparser::RuntimeNumericClass::Logical,
                   "N-dimensional true constructor is wrong");
    requireNumeric(variable(result, "falses"), {2, 3},
                   std::vector<double>(6, 0.0),
                   mparser::RuntimeNumericClass::Logical,
                   "shape-vector false constructor is wrong");

    require(mparser::runtimeTextScalarUtf8(
                variable(result, "logicalClass")) == "logical",
            "class(logical) returned the wrong class");
    require(mparser::runtimeTextScalarUtf8(
                variable(result, "doubleClass")) == "double",
            "class(double) returned the wrong class");
    requireScalar(variable(result, "q1"), 1,
                  mparser::RuntimeNumericClass::Logical,
                  "islogical did not return logical true");
    requireScalar(variable(result, "q2"), 1,
                  mparser::RuntimeNumericClass::Logical,
                  "isa logical query failed");
    requireScalar(variable(result, "q3"), 1,
                  mparser::RuntimeNumericClass::Logical,
                  "isa double query failed");

    for (const std::string_view name : {"R", "P", "T", "C", "notMask",
                                        "andMask", "shortLogical"}) {
        require(variable(result, name).numericClass ==
                    mparser::RuntimeNumericClass::Logical,
                "logical array transform lost its numeric class");
    }
    require(variable(result, "arithmetic").numericClass ==
                mparser::RuntimeNumericClass::Double,
            "logical arithmetic did not produce double output");
    require(variable(result, "positiveMask").numericClass ==
                mparser::RuntimeNumericClass::Double,
            "unary plus on logical data did not produce double output");
}

void runLogicalBehaviorSmoke() {
    const auto result = runBoth(R"(A = [1 2 3 4; 5 6 7 8; 9 10 11 12];
B = A;
mask = A > 5;
picked = A(mask);
row = [10 20 30 40];
rowMask = logical([1 0 1 0]);
rowPicked = row(rowMask);
column = [10; 20; 30; 40];
columnPicked = column(rowMask);
shortPicked = B(logical([1 0 1]));
longMask = false(1, 14);
longMask(1) = true;
longFalsePicked = B(longMask);
rowsMask = logical([1 0 1]);
columnsMask = logical([0 1 1 0]);
block = B(rowsMask, columnsMask);
A(mask) = 100;
B(rowsMask, columnsMask) = [20 30; 100 110];
L = logical([0 1 0 1]);
L(rowMask) = [2 5];
LD = double(L);
logicalClass = class(mask);
doubleClass = class(LD);
q1 = islogical(mask);
q2 = isa(mask, "logical");
q3 = isa(LD, "double");
R = reshape(mask, 4, 3);
P = permute(R, [2 1]);
T = repmat(rowMask, [2 1]);
C = cat(1, rowMask, logical([0 1 0 1]));
notMask = ~mask;
arithmetic = mask + 1;
positiveMask = +mask;
literal = [true false; false true];
truths = true(2, 2, 2);
falses = false([2 3]);
andMask = mask & true;
shortLogical = true && false;
)");

    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected valid logical indexing");
    require(result.vm.diagnostics.empty(),
            "VM rejected valid logical indexing");
    verifyLogicalBehavior(result.interpreter);
    verifyLogicalBehavior(result.vm);
}

void runLogicalBoundsSmoke() {
    const auto result = runBoth(R"(A = 1:4;
mask = false(1, 5);
mask(5) = true;
bad = A(mask);
)");
    require(hasDiagnostic(result.interpreter,
                          "true value outside the target extent"),
            "interpreter accepted a true logical index past the extent");
    require(hasDiagnostic(result.vm,
                          "true value outside the target extent"),
            "VM accepted a true logical index past the extent");
}

void runNumericIndexDistinctionSmoke() {
    const auto result = runBoth("A = 1:4; bad = A([1 0]);\n");
    require(hasDiagnostic(result.interpreter,
                          "index must be a positive integer"),
            "interpreter treated a numeric 0/1 array as a logical mask");
    require(hasDiagnostic(result.vm,
                          "index must be a positive integer"),
            "VM treated a numeric 0/1 array as a logical mask");
}

void runTransactionalLogicalAssignmentSmoke() {
    const auto result = runBoth(R"(L = logical([0 1 0]);
L(logical([1 0 1])) = [1 nan];
)");
    require(hasDiagnostic(result.interpreter,
                          "NaN cannot be converted to logical"),
            "interpreter accepted NaN assignment into logical storage");
    require(hasDiagnostic(result.vm,
                          "NaN cannot be converted to logical"),
            "VM accepted NaN assignment into logical storage");
    requireNumeric(variable(result.interpreter, "L"), {1, 3}, {0, 1, 0},
                   mparser::RuntimeNumericClass::Logical,
                   "interpreter partially mutated logical storage");
    requireNumeric(variable(result.vm, "L"), {1, 3}, {0, 1, 0},
                   mparser::RuntimeNumericClass::Logical,
                   "VM partially mutated logical storage");
}

void runJitNumericClassSmoke() {
    const auto result = runBoth(R"(total = 0;
for flag = true(1, 12)
    total = total + double(flag);
end
)");
    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected logical for-loop range");
    require(result.vm.diagnostics.empty(),
            "VM rejected logical for-loop range");
    requireScalar(variable(result.interpreter, "flag"), 1,
                  mparser::RuntimeNumericClass::Logical,
                  "interpreter lost logical loop-variable class");
    requireScalar(variable(result.vm, "flag"), 1,
                  mparser::RuntimeNumericClass::Logical,
                  "VM lost logical loop-variable class");

    const mparser::BytecodeLoopProfile* logicalLoop = nullptr;
    for (const auto& loop : result.vm.profile.loops) {
        if (loop.variable == "flag") {
            logicalLoop = &loop;
            break;
        }
    }
    require(logicalLoop != nullptr && logicalLoop->hot,
            "logical loop was not profiled as hot");
    require(logicalLoop->variableObservation.numericClass == "logical",
            "JIT profile erased the logical numeric class");

    mparser::BytecodeOptimizationPlanner planner;
    const auto plan = planner.plan(result.vm.profile, result.bytecode);
    const mparser::BytecodeOptimizationCandidate* loopCandidate = nullptr;
    for (const auto& candidate : plan.candidates) {
        if (candidate.kind == "hot-loop" && candidate.target == "flag") {
            loopCandidate = &candidate;
            break;
        }
    }
    require(loopCandidate != nullptr && !loopCandidate->guards.empty(),
            "logical loop did not produce an optimization guard");
    require(loopCandidate->guards.front().numericClass == "logical",
            "optimization guard erased the logical numeric class");

    mparser::BytecodeTypedIrBuilder builder;
    const auto typed = builder.build(plan);
    for (const auto& region : typed.regions) {
        if (region.sourcePc == loopCandidate->pc) {
            require(region.kind != "scalar-loop",
                    "logical loop entered the double scalar fast path");
            return;
        }
    }
    throw std::runtime_error("logical loop did not reach Typed IR");
}

void runAdaptiveNumericClassMergeSmoke() {
    const auto fixture = compile(R"(function y = kernel(flag)
y = double(flag);
end
)");

    mparser::RuntimeValue logicalArgument;
    logicalArgument.kind = mparser::RuntimeValueKind::Number;
    logicalArgument.number = 1.0;
    logicalArgument.numericClass = mparser::RuntimeNumericClass::Logical;
    mparser::setRuntimeDimensions(logicalArgument, {1, 1});

    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 1000;
    options.entryFunction = "kernel";
    options.arguments = {logicalArgument};
    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic, options);
    const auto first = session.run();
    require(first.runtime.diagnostics.empty(),
            "adaptive logical invocation failed");

    auto doubleArgument = logicalArgument;
    doubleArgument.numericClass = mparser::RuntimeNumericClass::Double;
    session.setArguments({doubleArgument});
    const auto second = session.run();
    require(second.runtime.diagnostics.empty(),
            "adaptive double invocation failed");

    const mparser::BytecodeFunctionEntryProfile* entry = nullptr;
    for (const auto& candidate :
         session.accumulatedProfile().functionEntries) {
        if (candidate.name == "kernel") {
            entry = &candidate;
            break;
        }
    }
    require(entry != nullptr && entry->argumentObservations.size() == 1,
            "adaptive entry argument was not profiled");
    const auto& observation = entry->argumentObservations.front();
    require(observation.observationCount == 2,
            "adaptive entry observation count is wrong");
    require(!observation.stable && observation.kind == "mixed" &&
                observation.numericClass.empty(),
            "adaptive merge reused a guard across numeric classes");
}

} // namespace

int main() {
    try {
        runLogicalBehaviorSmoke();
        runLogicalBoundsSmoke();
        runNumericIndexDistinctionSmoke();
        runTransactionalLogicalAssignmentSmoke();
        runJitNumericClassSmoke();
        runAdaptiveNumericClassMergeSmoke();
        std::cout << "Logical indexing smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Logical indexing smoke failure: " << error.what()
                  << "\n";
        return 1;
    }
}
