#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/semantic/semantic.h"

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
    require(parse.diagnostics.empty(), "array-transform source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "array-transform source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "array-transform source did not lower");

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

void requireCellNumber(const mparser::RuntimeValue& value, size_t offset,
                       double expected, std::string_view context) {
    require(value.kind == mparser::RuntimeValueKind::Cell, context);
    require(offset < value.cells.size(), context);
    requireNumber(value.cells[offset], expected, context);
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

template <typename Result>
void verifyTransforms(const Result& result) {
    requireArray(variable(result, "R"), {3, 2, 2},
                 {1, 7, 4, 10, 2, 8, 5, 11, 3, 9, 6, 12},
                 "reshape logical order");
    requireArray(variable(result, "RV"), {4, 3},
                 {1, 5, 9, 2, 6, 10, 3, 7, 11, 4, 8, 12},
                 "reshape size-vector order");
    requireArray(variable(result, "P"), {2, 3, 2},
                 {1, 4, 2, 5, 3, 6, 7, 10, 8, 11, 9, 12},
                 "permute coordinates");
    requireArray(variable(result, "I"), {3, 2, 2},
                 {1, 7, 4, 10, 2, 8, 5, 11, 3, 9, 6, 12},
                 "ipermute inversion");
    requireArray(variable(result, "S"), {6, 1}, {1, 2, 3, 4, 5, 6},
                 "squeeze vector orientation");
    requireArray(variable(result, "T"), {4, 2, 2},
                 {1, 1, 2, 2, 3, 3, 4, 4,
                  1, 1, 2, 2, 3, 3, 4, 4},
                 "repmat tiling");
    requireArray(variable(result, "C"), {2, 3, 4},
                 {1, 7, 101, 107, 3, 9, 103, 109, 5, 11, 105, 111,
                  2, 8, 102, 108, 4, 10, 104, 110, 6, 12, 106, 112},
                 "N-D concatenation");
    requireArray(variable(result, "H"), {2, 2}, {1, 3, 2, 4},
                 "horizontal concatenation");
    requireArray(variable(result, "V"), {2, 2}, {1, 2, 3, 4},
                 "vertical concatenation");

    const auto& kr = variable(result, "KR");
    require(kr.kind == mparser::RuntimeValueKind::Cell,
            "cell reshape kind");
    require(mparser::runtimeDimensions(kr) == std::vector<size_t>({2, 2}),
            "cell reshape dimensions");
    requireCellNumber(kr, 0, 5, "cell reshape first element");
    requireCellNumber(kr, 3, 7, "cell reshape last element");

    const auto& kp = variable(result, "KP");
    require(mparser::runtimeDimensions(kp) == std::vector<size_t>({2, 2}),
            "cell permute dimensions");
    requireCellNumber(kp, 0, 5, "cell permute first element");
    requireCellNumber(kp, 3, 7, "cell permute last element");

    const auto& kt = variable(result, "KT");
    require(mparser::runtimeDimensions(kt) ==
                std::vector<size_t>({2, 2, 2}),
            "cell repmat dimensions");
    requireCellNumber(kt, 0, 5, "cell repmat first tile");
    requireCellNumber(kt, 4, 5, "cell repmat second tile");

    const auto& kc = variable(result, "KC");
    require(mparser::runtimeDimensions(kc) ==
                std::vector<size_t>({2, 2, 2}),
            "cell cat dimensions");
    requireCellNumber(kc, 0, 5, "cell cat first input");
    requireCellNumber(kc, 4, 5, "cell cat second input");

    requireArray(variable(result, "E"), {0, 6}, {},
                 "zero-factor repmat");
    requireArray(variable(result, "Z"), {0, 2}, {}, "empty reshape");
    requireNumber(variable(result, "summary"), 2126,
                  "array-transform summary");
}

void runTransformSmoke() {
    const auto result = runBoth(R"(A = zeros(2, 3, 2);
for k = 1:numel(A)
    A(k) = k;
end
R = reshape(A, 3, [], 2);
RV = reshape(A, [4 3]);
P = permute(R, [3 1 2]);
I = ipermute(P, [3 1 2]);
S = squeeze(reshape(1:6, 1, 1, 6));
T = repmat([1 2; 3 4], [2 1 2]);
C = cat(3, A, A + 100);
H = horzcat([1; 2], [3; 4]);
V = vertcat([1 2], [3 4]);
K = cell(1, 2, 2);
K{1, 1, 1} = 5;
K{1, 2, 2} = 7;
KR = reshape(K, 2, 2);
KP = permute(K, [3 2 1]);
KT = repmat(K, [2 1 1]);
KC = cat(1, K, K);
E = repmat([1 2], 0, 3);
Z = reshape([], 0, 2);
numeric_score = sum(A, "all") + sum(R, "all") + sum(RV, "all") + ...
    sum(P, "all") + sum(I, "all") + sum(S, "all") + ...
    sum(T, "all") + sum(C, "all") + sum(H, "all") + sum(V, "all");
shape_score = sum(size(A), "all") + sum(size(R), "all") + ...
    sum(size(RV), "all") + sum(size(P), "all") + ...
    sum(size(I), "all") + sum(size(S), "all") + ...
    sum(size(T), "all") + sum(size(C), "all") + ...
    sum(size(H), "all") + sum(size(V), "all") + ...
    sum(size(KR), "all") + sum(size(KP), "all") + ...
    sum(size(KT), "all") + sum(size(KC), "all");
cell_score = KR{1} + KR{4} + KP{2, 2} + KT{2, 2, 2} + KC{2, 2, 2};
checks = R(1) + R(end) + RV(4, 3) + P(2, 3, 2) + I(3, 2, 2) + ...
    S(6) + T(4, 2, 2) + C(2, 3, 4) + H(2, 2) + V(2, 2);
summary = numeric_score + shape_score + cell_score + checks;
)");

    require(result.interpreter.diagnostics.empty(),
            "interpreter rejected valid array transforms");
    require(result.vm.diagnostics.empty(),
            "VM rejected valid array transforms");
    verifyTransforms(result.interpreter);
    verifyTransforms(result.vm);
}

void runReshapeDiagnosticSmoke() {
    const auto result = runBoth("bad = reshape(1:6, 4, 2);\n");
    require(hasDiagnostic(result.interpreter,
                          "reshape cannot change the number of elements"),
            "interpreter accepted an incompatible reshape");
    require(hasDiagnostic(result.vm,
                          "reshape cannot change the number of elements"),
            "VM accepted an incompatible reshape");
}

void runPermutationDiagnosticSmoke() {
    const auto result =
        runBoth("bad = permute(ones(2, 3, 2), [1 1 2]);\n");
    require(hasDiagnostic(result.interpreter,
                          "permutation of consecutive dimensions"),
            "interpreter accepted a duplicate permutation dimension");
    require(hasDiagnostic(result.vm,
                          "permutation of consecutive dimensions"),
            "VM accepted a duplicate permutation dimension");
}

void runConcatenationDiagnosticSmoke() {
    const auto result =
        runBoth("bad = cat(3, ones(2, 2), ones(3, 2));\n");
    require(hasDiagnostic(result.interpreter,
                          "dimensions must agree outside"),
            "interpreter accepted incompatible concatenation dimensions");
    require(hasDiagnostic(result.vm, "dimensions must agree outside"),
            "VM accepted incompatible concatenation dimensions");
}

} // namespace

int main() {
    try {
        runTransformSmoke();
        runReshapeDiagnosticSmoke();
        runPermutationDiagnosticSmoke();
        runConcatenationDiagnosticSmoke();
        std::cout << "Array transform smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Array transform smoke failure: " << error.what()
                  << "\n";
        return 1;
    }
}
