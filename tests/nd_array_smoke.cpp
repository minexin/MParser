#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_shape.h"
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
    require(parse.diagnostics.empty(), "N-D source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "N-D source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(), "N-D source did not lower");

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

void requireNumber(const mparser::RuntimeValue& value, double expected,
                   std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Number, context);
    require(std::fabs(value.number - expected) < 1e-9, context);
}

void requireArray(const mparser::RuntimeValue& value,
                  const std::vector<size_t>& dimensions,
                  const std::vector<double>& elements,
                  std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Vector ||
                value.kind == mparser::RuntimeValueKind::Matrix,
            context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(value.elements.size() == elements.size(), context);
    for (size_t index = 0; index < elements.size(); ++index) {
        require(std::fabs(value.elements[index] - elements[index]) < 1e-9,
                context);
    }
}

void requireCell(const mparser::RuntimeValue& value,
                 const std::vector<size_t>& dimensions,
                 std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Cell, context);
    require(mparser::runtimeDimensions(value) == dimensions, context);
    require(value.cells.size() == 12, context);
    requireNumber(value.cells[4], 13, context);
    requireNumber(value.cells[9], 42, context);
    requireNumber(value.cells[11], 23, context);
}

template <typename Result>
void verifyRuntime(const Result& result) {
    const std::vector<double> a = {1, 7, 3, 9, 5, 11,
                                   2, 8, 4, 10, 6, 12};
    requireArray(variable(result, "A"), {2, 3, 2}, a, "A layout");
    requireArray(variable(result, "shape"), {1, 3}, {2, 3, 2},
                 "size(A)");
    requireArray(variable(result, "by_dims"), {1, 3}, {2, 2, 1},
                 "size(A,[1 3 4])");
    requireArray(variable(result, "slice"), {2, 2}, {9, 11, 10, 12},
                 "N-D slice");
    requireArray(variable(result, "folded"), {2, 3},
                 {7, 9, 11, 8, 10, 12}, "folded slice");
    requireArray(variable(result, "cube"), {2, 2, 2},
                 {3, 9, 5, 11, 4, 10, 6, 12}, "N-D cube slice");
    requireArray(variable(result, "B"), {2, 3, 2},
                 {2, 8, 4, 10, 6, 12, 3, 9, 5, 11, 7, 13},
                 "N-D elementwise result");
    requireArray(variable(result, "Z"), {2, 1, 3},
                 std::vector<double>(6, 1.0), "shape-vector constructor");
    requireArray(variable(result, "M"), {2, 3, 2},
                 {0, 0, 0, 5, 0, 5, 0, 0, 0, 5, 0, 5},
                 "N-D scalar-fill assignment");
    requireCell(variable(result, "C"), {2, 3, 2}, "N-D cell array");
    requireArray(variable(result, "cell_shape"), {1, 3}, {2, 3, 2},
                 "cell size");

    requireNumber(variable(result, "same"), 30, "equivalent indices");
    requireNumber(variable(result, "rows"), 2, "size rows");
    requireNumber(variable(result, "tail"), 6, "size folded tail");
    requireNumber(variable(result, "r"), 2, "size output 1");
    requireNumber(variable(result, "c"), 3, "size output 2");
    requireNumber(variable(result, "p"), 2, "size output 3");
    requireNumber(variable(result, "e"), 1, "size trailing output");
    requireNumber(variable(result, "dimensions"), 3, "ndims");
    requireNumber(variable(result, "count"), 12, "numel");
    requireNumber(variable(result, "longest"), 3, "length");
    requireNumber(variable(result, "third_end"), 11, "end in dimension");
    requireNumber(variable(result, "folded_end"), 12, "folded end");
    requireNumber(variable(result, "linear_end"), 12, "linear end");
    requireNumber(variable(result, "extra"), 12, "trailing-one index");
    requireNumber(variable(result, "collapsed_ndims"), 2,
                  "trailing singleton normalization");
    requireNumber(variable(result, "empty_flag"), 1, "isempty");
    requireNumber(variable(result, "b_end"), 13,
                  "N-D elementwise linear index");
    requireNumber(variable(result, "m_sum"), 20, "N-D assignment sum");
    requireNumber(variable(result, "cell_folded"), 42,
                  "folded cell indexing");
    requireNumber(variable(result, "cell_linear"), 42,
                  "linear cell indexing");
    requireNumber(variable(result, "cell_dim_end"), 13,
                  "cell dimension end");
    requireNumber(variable(result, "cell_linear_end"), 23,
                  "cell linear end");
    requireNumber(variable(result, "cell_ndims"), 3, "cell ndims");
}

void verifyOptimizationShape(const RuntimePair& run) {
    const mparser::BytecodeAssignmentProfile* assignment = nullptr;
    for (const auto& candidate : run.vm.profile.assignments) {
        if (candidate.kind == "index" && candidate.target == "A" &&
            candidate.executionCount == 12) {
            assignment = &candidate;
            break;
        }
    }
    require(assignment != nullptr, "missing N-D assignment profile");
    require(assignment->valueObservation.dimensions ==
                std::vector<size_t>({2, 3, 2}),
            "profile lost N-D shape");

    mparser::BytecodeOptimizationPlanner planner;
    const auto plan = planner.plan(run.vm.profile, run.bytecode);
    const mparser::BytecodeOptimizationCandidate* store = nullptr;
    for (const auto& candidate : plan.candidates) {
        if (candidate.kind == "index-assignment" &&
            candidate.target == "A") {
            store = &candidate;
            break;
        }
    }
    require(store != nullptr, "missing N-D optimization candidate");
    require(!store->guards.empty(), "missing N-D optimization guard");
    require(store->guards.front().dimensions ==
                std::vector<size_t>({2, 3, 2}),
            "optimization guard lost N-D shape");

    mparser::BytecodeTypedIrBuilder builder;
    const auto typed = builder.build(plan);
    bool found = false;
    for (const auto& region : typed.regions) {
        if (region.target != "A") {
            continue;
        }
        for (const auto& guard : region.guards) {
            if (guard.value.dimensions == std::vector<size_t>({2, 3, 2})) {
                found = true;
            }
        }
    }
    require(found, "typed IR guard lost N-D shape");
}

void runNdArraySmoke() {
    const auto result = runBoth(R"(A = zeros(2, 3, 2);
for k = 1:numel(A)
    A(k) = k;
end

shape = size(A);
by_dims = size(A, [1 3 4]);
[rows, tail] = size(A);
[r, c, p, e] = size(A);
dimensions = ndims(A);
count = numel(A);
longest = length(A);

same = A(2, 2, 2) + A(2, 5) + A(10);
third_end = A(1, end, 2);
folded_end = A(2, end);
linear_end = A(end);
extra = A(2, 3, 2, 1);

slice = A(:, [2 3], 2);
folded = A(:, 4:6);
cube = A(:, 2:3, :);
B = A + ones(2, 3, 2);
b_end = B(end);

collapsed = zeros(2, 3, 1);
collapsed_ndims = ndims(collapsed);
Z = ones([2 1 3]);
empty_flag = isempty(zeros(0, 3, 2));

M = zeros(2, 3, 2);
M(:, 2:3, 2) = 5;
m_sum = sum(M, "all");

C = cell(2, 3, 2);
C{2, 2, 2} = 42;
C{1, 3, 1} = 13;
C{2, end, end} = 23;
cell_folded = C{2, 5};
cell_linear = C{10};
cell_dim_end = C{1, end, 1};
cell_linear_end = C{end};
cell_shape = size(C);
cell_ndims = ndims(C);
)");

    require(result.interpreter.diagnostics.empty(),
            "reference interpreter reported N-D diagnostics");
    require(result.vm.diagnostics.empty(),
            "bytecode VM reported N-D diagnostics");
    verifyRuntime(result.interpreter);
    verifyRuntime(result.vm);
    verifyOptimizationShape(result);
}

void runInvalidTrailingSubscriptSmoke() {
    const auto result = runBoth(R"(A = zeros(2, 3, 2);
bad = A(1, 1, 1, 2);
)");
    require(!result.interpreter.diagnostics.empty(),
            "interpreter accepted a non-singleton trailing subscript");
    require(!result.vm.diagnostics.empty(),
            "VM accepted a non-singleton trailing subscript");
    require(result.interpreter.diagnostics.front().message.find(
                "out of bounds") != std::string::npos,
            "unexpected interpreter trailing-subscript diagnostic");
    require(result.vm.diagnostics.front().message.find("out of bounds") !=
                std::string::npos,
            "unexpected VM trailing-subscript diagnostic");
}

void runDimensionOverflowSmoke() {
    const auto result = runBoth(R"(too_large = zeros(1e300, 2);
)");
    require(!result.interpreter.diagnostics.empty(),
            "interpreter accepted an unrepresentable array dimension");
    require(!result.vm.diagnostics.empty(),
            "VM accepted an unrepresentable array dimension");
    require(result.interpreter.diagnostics.front().message.find(
                "representable") != std::string::npos,
            "unexpected interpreter dimension-overflow diagnostic");
    require(result.vm.diagnostics.front().message.find("representable") !=
                std::string::npos,
            "unexpected VM dimension-overflow diagnostic");
}

template <typename Result>
void verifyAssignmentRuntime(const Result& result) {
    requireArray(variable(result, "B"), {4, 4},
                 {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 3, 4},
                 "matrix growth and block assignment");
    requireArray(variable(result, "v"), {1, 6}, {1, 2, 0, 40, 0, 60},
                 "row-vector linear growth");
    requireArray(variable(result, "L"), {2, 3}, {1, 2, 9, 3, 4, 0},
                 "matrix linear growth");
    requireArray(variable(result, "c"), {4, 1}, {1, 2, 0, 4},
                 "column-vector linear growth");
    requireArray(variable(result, "R"), {1, 2}, {40, 20},
                 "column-major repeated-index assignment");
    requireArray(variable(result, "N"), {3, 2, 3},
                 {0, 5, 0, 0, 0, 0, 0, 6, 0,
                  0, 0, 0, 0, 0, 0, 0, 0, 7},
                 "N-D assignment growth");
    requireArray(variable(result, "E"), {2, 3},
                 {11, 21, 31, 12, 22, 32},
                 "matrix implicit expansion");
    requireArray(variable(result, "Z"), {2, 3, 2},
                 {11, 11, 21, 21, 31, 31,
                  11, 11, 21, 21, 31, 31},
                 "N-D implicit expansion");
    requireArray(variable(result, "s"), {1, 3}, {1, 0, 5},
                 "scalar linear growth");
    requireArray(variable(result, "F"), {3, 2, 2},
                 {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 9},
                 "direct growth with a folded trailing subscript");
    requireNumber(variable(result, "summary"), 860,
                  "assignment demo summary");
}

void runAssignmentAndExpansionSmoke() {
    const auto result = runBoth(R"(B = zeros(2, 2);
B(3:4, 3:4) = [1 2; 3 4];
B(1, 4) = 8;
v = [1 2];
v([4 6]) = [40 60];
L = [1 2; 3 4];
L(5) = 9;
c = [1; 2];
c(4) = 4;
R = zeros(1, 2);
ids = [1 2; 2 1];
R(ids) = [10 20; 30 40];
N = zeros(2, 2, 2);
N(:, 1, 2) = [5; 6];
N(3, 2, 3) = 7;
E = [1; 2] + [10 20 30];
Z = ones(2, 1, 2) + [10 20 30];
s = 1;
s(3) = 5;
F = ones(2, 2, 2);
F(3, 4) = 9;
shape_score = sum(size(B), "all") + sum(size(L), "all") + ...
    sum(size(c), "all") + sum(size(N), "all") + ...
    sum(size(E), "all") + sum(size(Z), "all") + length(v);
value_score = sum(B, "all") + sum(v, "all") + sum(L, "all") + ...
    sum(c, "all") + sum(R, "all") + sum(N, "all") + ...
    sum(E, "all") + sum(Z, "all") + sum(s, "all");
checks = B(2, 2) + B(3, 3) + B(4, 4) + v(4) + L(1, 3) + ...
    c(4) + R(1) + R(2) + N(1, 1, 2) + N(2, 1, 2) + N(3, 2, 3) + ...
    E(2, 3) + Z(2, 3, 2) + s(3);
summary = shape_score + value_score + checks;
)");
    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected array assignment semantics");
    require(result.vm.diagnostics.empty(),
            "VM rejected array assignment semantics");
    verifyAssignmentRuntime(result.interpreter);
    verifyAssignmentRuntime(result.vm);
}

void runAssignmentShapeDiagnosticSmoke() {
    const auto result = runBoth(R"(A = zeros(2, 2);
A(3:4, 3:4) = [1 2 3 4];
)");
    require(!result.interpreter.diagnostics.empty(),
            "interpreter accepted an incompatible assignment shape");
    require(!result.vm.diagnostics.empty(),
            "VM accepted an incompatible assignment shape");
    require(result.interpreter.diagnostics.front().message.find(
                "dimensions do not match") != std::string::npos,
            "unexpected interpreter assignment-shape diagnostic");
    require(result.vm.diagnostics.front().message.find(
                "dimensions do not match") != std::string::npos,
            "unexpected VM assignment-shape diagnostic");
    requireArray(variable(result.interpreter, "A"), {2, 2},
                 {0, 0, 0, 0},
                 "interpreter assignment failure changed its target");
    requireArray(variable(result.vm, "A"), {2, 2}, {0, 0, 0, 0},
                 "VM assignment failure changed its target");
}

void runExpansionDiagnosticSmoke() {
    const auto result = runBoth(R"(A = ones(2, 2) + ones(1, 3);
)");
    require(!result.interpreter.diagnostics.empty(),
            "interpreter accepted incompatible implicit expansion");
    require(!result.vm.diagnostics.empty(),
            "VM accepted incompatible implicit expansion");
    require(result.interpreter.diagnostics.front().message.find(
                "incompatible dimensions") != std::string::npos,
            "unexpected interpreter expansion diagnostic");
    require(result.vm.diagnostics.front().message.find(
                "incompatible dimensions") != std::string::npos,
            "unexpected VM expansion diagnostic");
}

} // namespace

int main() {
    try {
        runNdArraySmoke();
        runInvalidTrailingSubscriptSmoke();
        runDimensionOverflowSmoke();
        runAssignmentAndExpansionSmoke();
        runAssignmentShapeDiagnosticSmoke();
        runExpansionDiagnosticSmoke();
        std::cout << "N-D array smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "N-D array smoke failure: " << error.what() << "\n";
        return 1;
    }
}
