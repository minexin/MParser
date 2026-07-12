#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::InterpreterResult run(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::Interpreter interpreter;
    return interpreter.run(semantic);
}

const mparser::RuntimeValue* findVariable(const mparser::InterpreterResult& result,
                                          std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

void assertNumber(const mparser::InterpreterResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

void assertString(const mparser::InterpreterResult& result,
                  std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::String);
    assert(value->text == expected);
}

void assertVector(const mparser::InterpreterResult& result,
                  std::string_view name,
                  std::initializer_list<double> expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Vector);
    assert(value->elements.size() == expected.size());

    size_t index = 0;
    for (double element : expected) {
        assert(std::fabs(value->elements[index] - element) < 1e-9);
        ++index;
    }
}

void assertMatrix(const mparser::InterpreterResult& result,
                  std::string_view name, size_t rows, size_t columns,
                  std::initializer_list<double> expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Matrix);
    assert(value->rows == rows);
    assert(value->columns == columns);
    assert(value->elements.size() == expected.size());

    size_t index = 0;
    for (double element : expected) {
        assert(std::fabs(value->elements[index] - element) < 1e-9);
        ++index;
    }
}

void runForLoopSmoke() {
    const std::string source = R"(function y = f()
y = 0;
for i = 1:3
    y = y + i;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 3.0);
    assertNumber(result, "y", 6.0);
}

void runStepRangeSmoke() {
    const std::string source = R"(function y = f()
y = 0;
for i = 1:2:5
    y = y + i;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 5.0);
    assertNumber(result, "y", 9.0);
}

void runIfAndBuiltinSmoke() {
    const std::string source = R"(function y = f()
if sin(pi / 2) > 0
    y = sqrt(9);
else
    y = 0;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "y", 3.0);
}

void runVectorLiteralAndIndexSmoke() {
    const std::string source = R"(function y = f()
A = [1 2 3];
B = A .* 2 + 1;
y = sum(A) + B(3) + length(B);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertVector(result, "A", {1.0, 2.0, 3.0});
    assertVector(result, "B", {3.0, 5.0, 7.0});
    assertNumber(result, "y", 16.0);
}

void runCellLiteralAndBraceIndexSmoke() {
    const std::string source = R"(function y = f()
C = {1, "two"};
C{3} = [3 4];
first = C{1};
label = C{2};
tail = C{3};
y = first + tail(2);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    const auto* cell = findVariable(result, "C");
    assert(cell != nullptr);
    assert(cell->kind == mparser::RuntimeValueKind::Cell);
    assert(cell->cells.size() == 3);
    assertNumber(result, "first", 1.0);
    assertString(result, "label", "two");
    assertVector(result, "tail", {3.0, 4.0});
    assertNumber(result, "y", 5.0);
}

void runVectorRangeAndBuiltinSmoke() {
    const std::string source = R"(function y = f()
A = 1:3;
B = sqrt(A .^ 2);
y = sum(B) + numel(A);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertVector(result, "A", {1.0, 2.0, 3.0});
    assertVector(result, "B", {1.0, 2.0, 3.0});
    assertNumber(result, "y", 9.0);
}

void runForLoopOverVectorSmoke() {
    const std::string source = R"(function y = f()
y = 0;
for i = [2 4 6]
    y = y + i;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 6.0);
    assertNumber(result, "y", 12.0);
}

void runMatrixShapeSmoke() {
    const std::string source = R"(function y = f()
A = [1 2; 3 4];
B = A .* 2;
C = A';
D = A * C;
s = size(A);
y = A(2, 1) + A(4) + sum(B) + s(1) + s(2) + D(2, 2);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 2, {1.0, 2.0, 3.0, 4.0});
    assertMatrix(result, "B", 2, 2, {2.0, 4.0, 6.0, 8.0});
    assertMatrix(result, "C", 2, 2, {1.0, 3.0, 2.0, 4.0});
    assertMatrix(result, "D", 2, 2, {5.0, 11.0, 11.0, 25.0});
    assertVector(result, "s", {2.0, 2.0});
    assertNumber(result, "y", 56.0);
}

void runLocalFunctionSmoke() {
    const std::string source = R"(function y = main()
A = [1 2; 3 4];
y = pick(A, 2) + scale(3);
end

function y = pick(A, row)
y = A(row, 1) + A(row, 2);
end

function y = scale(x)
y = x * 2;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 2, {1.0, 2.0, 3.0, 4.0});
    assertNumber(result, "y", 13.0);
    assert(findVariable(result, "row") == nullptr);
}

void runLocalFunctionScopeSmoke() {
    const std::string source = R"(function y = main()
x = 10;
y = helper(2) + x;
end

function y = helper(x)
x = x + 1;
y = x;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "x", 10.0);
    assertNumber(result, "y", 13.0);
}

void runScriptLocalFunctionSmoke() {
    const std::string source = R"(x = 4;
y = helper(x);

function z = helper(a)
z = a + 1;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "x", 4.0);
    assertNumber(result, "y", 5.0);
    assert(findVariable(result, "a") == nullptr);
}

void runLocalFunctionMultiOutputSmoke() {
    const std::string source = R"(function y = main()
A = [1 2; 3 4];
[total, rows, cols] = summarize(A);
[~, doubled] = pair(total);
firstOnly = pair(5);
y = total + rows + cols + doubled + firstOnly;
end

function [total, rows, cols] = summarize(A)
total = sum(A);
[rows, cols] = size(A);
end

function [original, doubled] = pair(x)
original = x;
doubled = x * 2;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 2, {1.0, 2.0, 3.0, 4.0});
    assertNumber(result, "total", 10.0);
    assertNumber(result, "rows", 2.0);
    assertNumber(result, "cols", 2.0);
    assertNumber(result, "doubled", 20.0);
    assertNumber(result, "firstOnly", 5.0);
    assertNumber(result, "y", 39.0);
    assert(findVariable(result, "original") == nullptr);
    assert(findVariable(result, "x") == nullptr);
}

void runBuiltinSizeMultiOutputSmoke() {
    const std::string source = R"(function y = f()
A = [1 2; 3 4];
v = [5 6 7];
s = size(A);
[rows, cols, pages] = size(A);
[vRows, vCols] = size(v);
[~, ignoredCols] = size(A);
y = s(1) + s(2) + rows + cols + pages + vRows + vCols + ignoredCols;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 2, {1.0, 2.0, 3.0, 4.0});
    assertVector(result, "v", {5.0, 6.0, 7.0});
    assertVector(result, "s", {2.0, 2.0});
    assertNumber(result, "rows", 2.0);
    assertNumber(result, "cols", 2.0);
    assertNumber(result, "pages", 1.0);
    assertNumber(result, "vRows", 1.0);
    assertNumber(result, "vCols", 3.0);
    assertNumber(result, "ignoredCols", 2.0);
    assertNumber(result, "y", 15.0);
}

void runArrayConstructorBuiltinSmoke() {
    const std::string source = R"(function y = f()
Z = zeros(2, 3);
O = ones(1, 4);
I = eye(3);
R = eye(2, 3);
S = zeros(2);
shape = [2 3];
P = ones(shape);
y = sum(Z) + sum(O) + sum(I) + sum(R) + sum(S) + sum(P);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "Z", 2, 3, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    assertVector(result, "O", {1.0, 1.0, 1.0, 1.0});
    assertMatrix(result, "I", 3, 3,
                 {1.0, 0.0, 0.0,
                  0.0, 1.0, 0.0,
                  0.0, 0.0, 1.0});
    assertMatrix(result, "R", 2, 3,
                 {1.0, 0.0, 0.0,
                  0.0, 1.0, 0.0});
    assertMatrix(result, "S", 2, 2,
                 {0.0, 0.0,
                  0.0, 0.0});
    assertVector(result, "shape", {2.0, 3.0});
    assertMatrix(result, "P", 2, 3, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
    assertNumber(result, "y", 15.0);
}

void runLinspaceBuiltinSmoke() {
    const std::string source = R"(function y = f()
defaultGrid = linspace(1, 3);
grid = linspace(0, 1, 5);
onePoint = linspace(5, 7, 1);
emptyGrid = linspace(1, 2, 0);
y = length(defaultGrid) + defaultGrid(1) + defaultGrid(100) + sum(grid) + onePoint(1) + numel(emptyGrid);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    const auto* defaultGrid = findVariable(result, "defaultGrid");
    assert(defaultGrid != nullptr);
    assert(defaultGrid->kind == mparser::RuntimeValueKind::Vector);
    assert(defaultGrid->elements.size() == 100);
    assert(std::fabs(defaultGrid->elements.front() - 1.0) < 1e-9);
    assert(std::fabs(defaultGrid->elements.back() - 3.0) < 1e-9);
    assertVector(result, "grid", {0.0, 0.25, 0.5, 0.75, 1.0});
    assertVector(result, "onePoint", {7.0});
    assertVector(result, "emptyGrid", {});
    assertNumber(result, "y", 113.5);
}

void runIndexedAssignmentSmoke() {
    const std::string source = R"(function y = f()
A = zeros(2, 3);
for r = 1:2
    for c = 1:3
        A(r, c) = r * 10 + c;
    end
end
A(4) = 99;
v = zeros(1, 4);
for i = 1:4
    v(i) = i * i;
end
v([1 3]) = 7;
y = A(2, 2) + A(4) + sum(v);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 3,
                 {11.0, 12.0, 13.0,
                  21.0, 99.0, 23.0});
    assertVector(result, "v", {7.0, 4.0, 7.0, 16.0});
    assertNumber(result, "r", 2.0);
    assertNumber(result, "c", 3.0);
    assertNumber(result, "i", 4.0);
    assertNumber(result, "y", 232.0);
}

void runEndIndexingSmoke() {
    const std::string source = R"(function y = f()
v = [10 20 30 40 50];
A = [1 2 3; 4 5 6];
y = v(end) + v(end - 1) + sum(v(2:end - 1)) + sum(v([1 end]));
y = y + A(end, 2) + A(1, end) + A(end);
v(end) = 99;
A(end, end) = 42;
y = y + v(end) + A(end, end);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertVector(result, "v", {10.0, 20.0, 30.0, 40.0, 99.0});
    assertMatrix(result, "A", 2, 3,
                 {1.0, 2.0, 3.0,
                  4.0, 5.0, 42.0});
    assertNumber(result, "y", 395.0);
}

void runColonIndexingSmoke() {
    const std::string source = R"(function y = f()
A = [1 2 3; 4 5 6; 7 8 9];
col = A(:, 2);
row = A(1, :);
block = A(2:3, [1 3]);
linear = A(:);
v = [1 2 3 4];
v(:) = 7;
A(:, 3) = 10;
y = sum(col) + sum(row) + sum(block) + sum(linear) + sum(v) + sum(A(:, 3));
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "col", 3, 1, {2.0, 5.0, 8.0});
    assertVector(result, "row", {1.0, 2.0, 3.0});
    assertMatrix(result, "block", 2, 2,
                 {4.0, 6.0,
                  7.0, 9.0});
    assertVector(result, "linear",
                 {1.0, 4.0, 7.0, 2.0, 5.0, 8.0, 3.0, 6.0, 9.0});
    assertVector(result, "v", {7.0, 7.0, 7.0, 7.0});
    assertMatrix(result, "A", 3, 3,
                 {1.0, 2.0, 10.0,
                  4.0, 5.0, 10.0,
                  7.0, 8.0, 10.0});
    assertNumber(result, "y", 150.0);
}

void runWhileLoopSmoke() {
    const std::string source = R"(function y = f()
i = 1;
y = 0;
v = zeros(1, 4);
while i <= 4
    v(i) = i;
    y = y + v(i);
    i = i + 1;
end
guard = 0;
while guard < 0
    guard = 1;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertVector(result, "v", {1.0, 2.0, 3.0, 4.0});
    assertNumber(result, "i", 5.0);
    assertNumber(result, "guard", 0.0);
    assertNumber(result, "y", 10.0);
}

void runLoopControlSmoke() {
    const std::string source = R"(function y = f()
y = 0;
for i = 1:6
    if i == 2
        continue
    end
    if i > 4
        break
    end
    y = y + i;
end

j = 0;
while j < 5
    j = j + 1;
    if j == 3
        continue
    end
    if j == 5
        break
    end
    y = y + 10 * j;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 5.0);
    assertNumber(result, "j", 5.0);
    assertNumber(result, "y", 78.0);
}

void runSwitchSmoke() {
    const std::string source = R"(function y = f()
mode = "beta";
y = 0;
switch mode
    case "alpha"
        y = 1;
    case "beta"
        y = 2;
    otherwise
        y = 3;
end

n = 3;
switch n
    case 1
        y = y + 10;
    case 3
        y = y + 30;
    otherwise
        y = y + 100;
end

v = [1 2];
switch v
    case [2 1]
        y = y + 1000;
    case [1 2]
        y = y + 300;
end

switch 9
    case 1
        y = y + 1000;
    otherwise
        y = y + 4000;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertString(result, "mode", "beta");
    assertVector(result, "v", {1.0, 2.0});
    assertNumber(result, "y", 4332.0);
}

void runTryCatchSmoke() {
    const std::string source = R"(function y = f()
y = 0;
try
    for k = 1:3
        y = y + k;
        x = missingName;
        y = 999;
    end
catch err
    message = err;
    y = y + 10;
end

try
    y = y + 2;
catch err2
    y = 999;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "k", 1.0);
    assertString(result, "err", "unknown runtime variable: missingName");
    assertString(result, "message", "unknown runtime variable: missingName");
    assert(findVariable(result, "err2") == nullptr);
    assertNumber(result, "y", 13.0);
}

void runStringCompareSmoke() {
    const std::string source = R"(function y = f()
mode = "fast";
y = 0;
if mode == "fast"
    y = y + 1;
end
if mode ~= "slow"
    y = y + 10;
end
if strcmp(mode, "fast")
    y = y + 100;
end
if strcmp(mode, "slow")
    y = 999;
else
    y = y + 1000;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertString(result, "mode", "fast");
    assertNumber(result, "y", 1111.0);
}

void runShortCircuitSmoke() {
    const std::string source = R"(function y = f()
y = 0;
if false && missingName
    y = 999;
else
    y = y + 1;
end
if true || missingName
    y = y + 10;
end

try
    if true && missingName
        y = 999;
    end
catch err
    y = y + 100;
end

try
    if false || missingName
        y = 999;
    end
catch err2
    y = y + 1000;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertString(result, "err", "unknown runtime variable: missingName");
    assertString(result, "err2", "unknown runtime variable: missingName");
    assertNumber(result, "y", 1111.0);
}

void runReturnSmoke() {
    const std::string source = R"(function y = f()
y = 1;
for i = 1:5
    if i == 3
        y = y + helper();
        return
    end
    y = y + i;
end
y = 999;
end

function out = helper()
out = 100;
return
out = 1000;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 3.0);
    assertNumber(result, "y", 104.0);
}

} // namespace

int main() {
    runForLoopSmoke();
    runStepRangeSmoke();
    runIfAndBuiltinSmoke();
    runVectorLiteralAndIndexSmoke();
    runCellLiteralAndBraceIndexSmoke();
    runVectorRangeAndBuiltinSmoke();
    runForLoopOverVectorSmoke();
    runMatrixShapeSmoke();
    runLocalFunctionSmoke();
    runLocalFunctionScopeSmoke();
    runScriptLocalFunctionSmoke();
    runLocalFunctionMultiOutputSmoke();
    runBuiltinSizeMultiOutputSmoke();
    runArrayConstructorBuiltinSmoke();
    runLinspaceBuiltinSmoke();
    runIndexedAssignmentSmoke();
    runEndIndexingSmoke();
    runColonIndexingSmoke();
    runWhileLoopSmoke();
    runLoopControlSmoke();
    runSwitchSmoke();
    runTryCatchSmoke();
    runStringCompareSmoke();
    runShortCircuitSmoke();
    runReturnSmoke();
    std::cout << "interpreter smoke tests passed\n";
    return 0;
}
