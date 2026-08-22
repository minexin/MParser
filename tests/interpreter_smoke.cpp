#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_exception.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

mparser::InterpreterResult run(
    std::string_view source,
    const mparser::InterpreterOptions& options = {}) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::Interpreter interpreter;
    return interpreter.run(semantic, options);
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
    assert(mparser::runtimeTextScalarUtf8(*value) == expected);
}

void assertException(const mparser::InterpreterResult& result,
                     std::string_view name,
                     std::string_view expectedMessage) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(mparser::isRuntimeException(*value));
    const auto* message =
        mparser::runtimeExceptionProperty(*value, "message");
    assert(message != nullptr);
    const auto actualMessage = mparser::runtimeTextScalarUtf8(*message);
    if (actualMessage != expectedMessage) {
        std::cerr << "unexpected exception message for " << name
                  << ": expected '" << expectedMessage << "', got '"
                  << actualMessage.value_or("<non-scalar text>") << "'\n";
    }
    assert(actualMessage == expectedMessage);
}

void assertVector(const mparser::InterpreterResult& result,
                  std::string_view name,
                  std::initializer_list<double> expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(mparser::isRuntimeNumericValue(*value));
    const auto dimensions = mparser::runtimeDimensions(*value);
    assert(dimensions.size() == 2);
    assert(dimensions[0] == 1);
    assert(dimensions[1] == expected.size());

    size_t index = 0;
    for (double element : expected) {
        const auto actual =
            mparser::runtimeNumericElementValue(*value, index);
        assert(actual.has_value());
        assert(std::fabs(actual->real - element) < 1e-9);
        ++index;
    }
}

void assertMatrix(const mparser::InterpreterResult& result,
                  std::string_view name, size_t rows, size_t columns,
                  std::initializer_list<double> expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(mparser::isRuntimeNumericValue(*value));
    const auto dimensions = mparser::runtimeDimensions(*value);
    assert(dimensions.size() == 2);
    assert(dimensions[0] == rows);
    assert(dimensions[1] == columns);
    assert(rows * columns == expected.size());

    auto expectedElement = expected.begin();
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            const auto actual = mparser::runtimeNumericElementValue(
                *value, column * rows + row);
            assert(actual.has_value());
            assert(std::fabs(actual->real - *expectedElement) < 1e-9);
            ++expectedElement;
        }
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
reverse = 0;
for j = 5:-2:1
    reverse = reverse + j;
end
emptyCount = 0;
for k = 5:1
    emptyCount = emptyCount + 1;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 5.0);
    assertNumber(result, "y", 9.0);
    assertNumber(result, "j", 1.0);
    assertNumber(result, "reverse", 9.0);
    assertNumber(result, "emptyCount", 0.0);
    assert(findVariable(result, "k") == nullptr);
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
y = sum(A, "all") + B(3) + length(B);
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
y = sum(B, "all") + numel(A);
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
y = A(2, 1) + A(4) + sum(B, "all") + s(1) + s(2) + D(2, 2);
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
total = sum(A, "all");
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
y = sum(Z, "all") + sum(O, "all") + sum(I, "all") + ...
    sum(R, "all") + sum(S, "all") + sum(P, "all");
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
y = length(defaultGrid) + defaultGrid(1) + defaultGrid(100) + ...
    sum(grid, "all") + onePoint(1) + numel(emptyGrid);
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
y = A(2, 2) + A(4) + sum(v, "all");
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
y = v(end) + v(end - 1) + sum(v(2:end - 1), "all") + ...
    sum(v([1 end]), "all");
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
y = sum(col, "all") + sum(row, "all") + sum(block, "all") + ...
    sum(linear, "all") + sum(v, "all") + sum(A(:, 3), "all");
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "col", 3, 1, {2.0, 5.0, 8.0});
    assertVector(result, "row", {1.0, 2.0, 3.0});
    assertMatrix(result, "block", 2, 2,
                 {4.0, 6.0,
                  7.0, 9.0});
    assertMatrix(result, "linear", 9, 1,
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
    message = err.message;
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
    assertException(result, "err", "unknown runtime variable: missingName");
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
    assertException(result, "err", "unknown runtime variable: missingName");
    assertException(result, "err2", "unknown runtime variable: missingName");
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

void runSessionCommandSmoke() {
    const auto result = run(R"(stale = 7;
clear;
clc;
tic;
total = 0;
for i = 1:100
    total = total + i;
end
elapsed = toc;
)");

    assert(result.diagnostics.empty());
    assert(findVariable(result, "stale") == nullptr);
    assertNumber(result, "i", 100.0);
    assertNumber(result, "total", 5050.0);
    const auto* elapsed = findVariable(result, "elapsed");
    assert(elapsed != nullptr);
    assert(elapsed->kind == mparser::RuntimeValueKind::Number);
    assert(elapsed->number >= 0.0);

    const auto missingTimer = run("elapsed = toc;");
    assert(missingTimer.diagnostics.size() == 1);
    assert(missingTimer.diagnostics.front().message.find(
               "preceding tic") != std::string::npos);

    const auto selective = run(R"(keep = 11;
drop = 22;
tempOne = 1;
tempTwo = 2;
clear drop
clear -regexp ^temp
all = 9;
clear all
echo_fn 5

function echo_fn(value)
assert(strcmp(value, '5'));
end
)");
    assert(selective.diagnostics.empty());
    assertNumber(selective, "keep", 11.0);
    assert(findVariable(selective, "drop") == nullptr);
    assert(findVariable(selective, "tempOne") == nullptr);
    assert(findVariable(selective, "tempTwo") == nullptr);
    assert(findVariable(selective, "all") == nullptr);

    const auto clearVariables = run(R"(first = 1;
second = 2;
clear variables
after = 3;
)");
    assert(clearVariables.diagnostics.empty());
    assert(findVariable(clearVariables, "first") == nullptr);
    assert(findVariable(clearVariables, "second") == nullptr);
    assertNumber(clearVariables, "after", 3.0);

    const auto existence = run(R"(shadowed = 4;
variable_code = exist('shadowed');
variable_filter = exist('shadowed', 'var');
builtin_code = exist('sin', 'builtin');
builtin_not_variable = exist('sin', 'var');
local_function_code = exist('exist_probe', 'file');
missing_code = exist('definitely_absent_mparser_name');
clear shadowed
cleared_code = exist('shadowed', 'var');

function value = exist_probe()
value = 1;
end
)");
    assert(existence.diagnostics.empty());
    assertNumber(existence, "variable_code", 1.0);
    assertNumber(existence, "variable_filter", 1.0);
    assertNumber(existence, "builtin_code", 5.0);
    assertNumber(existence, "builtin_not_variable", 0.0);
    assertNumber(existence, "local_function_code", 2.0);
    assertNumber(existence, "missing_code", 0.0);
    assertNumber(existence, "cleared_code", 0.0);

    const auto formatting = run(R"(format long
pi
format short
pi
format shortE
pi
format bank
pi
format rational
pi
format default
previous = format('long');
previous_numeric = previous.NumericFormat;
current = format();
current_numeric = current.NumericFormat;
format(previous);
restored = format();
restored_numeric = restored.NumericFormat;
format compact
compact_state = format;
compact_spacing = compact_state.LineSpacing;
pi
disp(pi)
format loose
loose_state = format;
loose_spacing = loose_state.LineSpacing;
pi
disp(pi)
format long
bare_state = format;
bare_numeric = bare_state.NumericFormat;
format
reset_state = format();
reset_numeric = reset_state.NumericFormat;
reset_spacing = reset_state.LineSpacing;
)");
    assert(formatting.diagnostics.empty());
    assert(formatting.expressionResults.size() == 7);
    assert(formatting.expressionResults[0].displayText ==
           "3.141592653589793");
    assert(formatting.expressionResults[1].displayText == "3.1416");
    assert(formatting.expressionResults[2].displayText == "3.1416e+00");
    assert(formatting.expressionResults[3].displayText == "3.14");
    assert(formatting.expressionResults[4].displayText == "355/113");
    assert(formatting.expressionResults[5].displayText == "3.1416");
    assert(formatting.expressionResults[5].lineSpacing ==
           mparser::RuntimeLineSpacing::Compact);
    assert(formatting.expressionResults[6].displayText == "3.1416");
    assert(formatting.expressionResults[6].lineSpacing ==
           mparser::RuntimeLineSpacing::Loose);
    assertString(formatting, "previous_numeric", "short");
    assertString(formatting, "current_numeric", "long");
    assertString(formatting, "restored_numeric", "short");
    assertString(formatting, "compact_spacing", "compact");
    assertString(formatting, "loose_spacing", "loose");
    assertString(formatting, "bare_numeric", "long");
    assertString(formatting, "reset_numeric", "short");
    assertString(formatting, "reset_spacing", "loose");
    assert(formatting.outputEvents.size() == 2);
    assert(formatting.outputEvents[0].text == "3.1416\n");
    assert(formatting.outputEvents[1].text == "3.1416\n\n");
}

void runV11CoreCompatibilitySmoke() {
    const auto result = run(R"(function summary = main()
power = 2^3^2;
dotPower = 2.^3.^2;
if power == 64, branch = 10; else, branch = -1; end
switch branch, case 10, switched = 20; otherwise, switched = -1; end
A = [1 2 3; 4 5 6];
sumFirst = 0;
for col = A, sumFirst = sumFirst + col(1); end
count = 0;
while count < 3, count = count + 1; end
linear = A(:);
v = [7 8 9];
vlinear = v(:);
summary = power + dotPower + branch + switched + sumFirst + count + ...
    sum(linear, "all");
end
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "power", 64.0);
    assertNumber(result, "dotPower", 64.0);
    assertNumber(result, "branch", 10.0);
    assertNumber(result, "switched", 20.0);
    assertNumber(result, "sumFirst", 6.0);
    assertNumber(result, "count", 3.0);
    assertMatrix(result, "col", 2, 1, {3.0, 6.0});
    assertMatrix(result, "linear", 6, 1,
                 {1.0, 4.0, 2.0, 5.0, 3.0, 6.0});
    assertMatrix(result, "vlinear", 3, 1, {7.0, 8.0, 9.0});
    assertNumber(result, "summary", 188.0);
}

void runHostOutputAndExpressionSmoke() {
    std::vector<mparser::RuntimeOutputEvent> observed;
    mparser::InterpreterOptions options;
    options.outputSink = [&observed](
                             const mparser::RuntimeOutputEvent& event) {
        observed.push_back(event);
        return true;
    };
    const auto result = run(R"(formatted = sprintf("value=%d", 42);
disp(formatted)
written = fprintf("pi=%.1f\n", 3.14);
40 + 2
41 + 2;
zero_handle = @no_output;
one_handle = @one_output;
zero_handle()
one_handle()

function no_output()
end

function value = one_output()
value = 44;
end
)", options);

    assert(result.diagnostics.empty());
    assertString(result, "formatted", "value=42");
    assertNumber(result, "written", 7.0);
    assertNumber(result, "ans", 44.0);
    assert(result.outputEvents.size() == 2);
    assert(observed.size() == result.outputEvents.size());
    assert(result.outputEvents[0].kind ==
           mparser::RuntimeOutputKind::Display);
    assert(result.outputEvents[0].text == "value=42\n\n");
    assert(result.outputEvents[0].sequence == 0);
    assert(result.outputEvents[1].kind ==
           mparser::RuntimeOutputKind::StandardOutput);
    assert(result.outputEvents[1].text == "pi=3.1\n");
    assert(result.outputEvents[1].sequence == 1);
    assert(result.expressionResults.size() == 3);
    assert(result.expressionResults[0].value.kind ==
           mparser::RuntimeValueKind::Number);
    assert(result.expressionResults[0].value.number == 42.0);
    assert(!result.expressionResults[0].outputSuppressed);
    assert(result.expressionResults[0].sequence == 2);
    assert(result.expressionResults[1].value.number == 43.0);
    assert(result.expressionResults[1].outputSuppressed);
    assert(result.expressionResults[1].sequence == 3);
    assert(result.expressionResults[2].value.number == 44.0);
    assert(!result.expressionResults[2].outputSuppressed);
    assert(result.expressionResults[2].sequence == 4);

    options.outputSink = [](const mparser::RuntimeOutputEvent&) {
        return false;
    };
    const auto rejected = run("disp(1);", options);
    assert(rejected.outputEvents.size() == 1);
    assert(rejected.diagnostics.size() == 1);
    assert(rejected.diagnostics.front().identifier ==
           "MParser:OutputSinkRejected");
}

void runImplicitStatementNargoutSmoke() {
    const auto result = run(R"(ans = 999;
probe();
suppressed_value = ans;
ans = 999;
probe()
visible_value = ans;
probe_handle = @probe;
ans = 999;
probe_handle();
handle_value = ans;
ans = 999;
feval(probe_handle);
feval_value = ans;

function value = probe()
value = 10 + nargout;
end
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "suppressed_value", 10.0);
    assertNumber(result, "visible_value", 10.0);
    assertNumber(result, "handle_value", 10.0);
    assertNumber(result, "feval_value", 10.0);
    assertNumber(result, "ans", 10.0);
    assert(result.expressionResults.size() == 4);
    assert(result.expressionResults[0].outputSuppressed);
    assert(result.expressionResults[0].value.number == 10.0);
    assert(!result.expressionResults[1].outputSuppressed);
    assert(result.expressionResults[1].value.number == 10.0);
    assert(result.expressionResults[2].outputSuppressed);
    assert(result.expressionResults[2].value.number == 10.0);
    assert(result.expressionResults[3].outputSuppressed);
    assert(result.expressionResults[3].value.number == 10.0);
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
    runSessionCommandSmoke();
    runV11CoreCompatibilitySmoke();
    runHostOutputAndExpressionSmoke();
    runImplicitStatementNargoutSmoke();
    std::cout << "interpreter smoke tests passed\n";
    return 0;
}
