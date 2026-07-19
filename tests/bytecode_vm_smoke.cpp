#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_exception.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::BytecodeVmResult run(
    std::string_view source,
    const mparser::BytecodeVmOptions& options = {}) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);

    mparser::BytecodeVm vm;
    return vm.run(bytecode, semantic, options);
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const mparser::BytecodeFunctionProfile* findFunctionProfile(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& profile : result.profile.functions) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

const mparser::BytecodeLoopProfile* findLoopProfile(
    const mparser::BytecodeVmResult& result, std::string_view variable) {
    for (const auto& profile : result.profile.loops) {
        if (profile.variable == variable) {
            return &profile;
        }
    }
    return nullptr;
}

const mparser::BytecodeCallSiteProfile* findCallSiteProfile(
    const mparser::BytecodeVmResult& result, std::string_view kind,
    std::string_view target) {
    for (const auto& profile : result.profile.callSites) {
        if (profile.kind == kind && profile.target == target) {
            return &profile;
        }
    }
    return nullptr;
}

const mparser::BytecodeAssignmentProfile* findAssignmentProfile(
    const mparser::BytecodeVmResult& result, std::string_view kind,
    std::string_view target, size_t executionCount) {
    for (const auto& profile : result.profile.assignments) {
        if (profile.kind == kind && profile.target == target &&
            profile.executionCount == executionCount) {
            return &profile;
        }
    }
    return nullptr;
}

size_t instructionProfileExecutionTotal(
    const mparser::BytecodeVmResult& result) {
    size_t total = 0;
    for (const auto& profile : result.profile.instructions) {
        total += profile.executionCount;
    }
    return total;
}

void assertObservation(const mparser::BytecodeValueObservation& observation,
                       std::string_view kind, size_t rows, size_t columns,
                       size_t count) {
    assert(observation.kind == kind);
    assert(observation.numericClass == "double");
    assert(observation.rows == rows);
    assert(observation.columns == columns);
    assert(observation.observationCount == count);
    assert(observation.stable);
}

void assertNumber(const mparser::BytecodeVmResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

void assertVector(const mparser::BytecodeVmResult& result,
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

void assertMatrix(const mparser::BytecodeVmResult& result,
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

void runStraightLineFunctionSmoke() {
    const std::string source = R"(function y = f()
a = 1 + 2 * 3;
y = a + sin(pi / 2);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assert(result.executedInstructionCount > 0);
    assertNumber(result, "a", 7.0);
    assertNumber(result, "y", 8.0);
}

void runVectorAndIndexSmoke() {
    const std::string source = R"(function y = f()
A = 1:4;
B = A .* 2 + 1;
y = sum(B, "all") + B(2);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertVector(result, "A", {1.0, 2.0, 3.0, 4.0});
    assertVector(result, "B", {3.0, 5.0, 7.0, 9.0});
    assertNumber(result, "y", 29.0);
}

void runMatrixAndBuiltinSmoke() {
    const std::string source = R"(function y = f()
A = [1 2; 3 4];
B = A * A';
s = size(B);
y = B(2, 2) + sum(B, "all") + s(1) + s(2);
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertMatrix(result, "A", 2, 2, {1.0, 2.0, 3.0, 4.0});
    assertMatrix(result, "B", 2, 2, {5.0, 11.0, 11.0, 25.0});
    assertVector(result, "s", {2.0, 2.0});
    assertNumber(result, "y", 81.0);
}

void runScriptSkipsLocalFunctionSmoke() {
    const std::string source = R"(x = 2 + 3;
y = x * 4;

function z = helper()
z = 999;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "x", 5.0);
    assertNumber(result, "y", 20.0);
    assert(findVariable(result, "z") == nullptr);
}

void runForIfSmoke() {
    const std::string source = R"(function y = f()
y = 0;
for i = 1:4
    if i < 3
        y = y + i;
    elseif i == 3
        y = y + 10;
    else
        y = y + 100;
    end
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 4.0);
    assertNumber(result, "y", 113.0);
}

void runColonStepRangeSmoke() {
    const auto result = run(R"(function y = f()
forward = 0;
for i = 1:2:5
    forward = forward + i;
end
reverse = 0;
for j = 5:-2:1
    reverse = reverse + j;
end
emptyCount = 0;
for k = 5:1
    emptyCount = emptyCount + 1;
end
y = forward + reverse + emptyCount;
end
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "forward", 9.0);
    assertNumber(result, "reverse", 9.0);
    assertNumber(result, "emptyCount", 0.0);
    assertNumber(result, "i", 5.0);
    assertNumber(result, "j", 1.0);
    assert(findVariable(result, "k") == nullptr);
    assertNumber(result, "y", 18.0);
}

void runColonZeroStepDiagnosticSmoke() {
    const auto result = run(R"(function y = f()
y = 1:0:5;
end
)");

    assert(result.diagnostics.size() == 1);
    assert(result.diagnostics.front().message ==
           "bytecode colon range step cannot be zero");
    const auto* value = findVariable(result, "y");
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Missing);
}

void runWhileSmoke() {
    const std::string source = R"(function y = f()
i = 1;
y = 0;
while i <= 5
    y = y + i;
    i = i + 1;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 6.0);
    assertNumber(result, "y", 15.0);
}

void runBreakContinueSmoke() {
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
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 5.0);
    assertNumber(result, "y", 8.0);
}

void runReturnSmoke() {
    const std::string source = R"(function y = f()
y = 1;
return
y = 999;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "y", 1.0);
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
    assert(findVariable(result, "x") == nullptr);
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

void runMultiOutputFunctionSmoke() {
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
    assertVector(result, "s", {2.0, 2.0});
    assertNumber(result, "rows", 2.0);
    assertNumber(result, "cols", 2.0);
    assertNumber(result, "pages", 1.0);
    assertNumber(result, "vRows", 1.0);
    assertNumber(result, "vCols", 3.0);
    assertNumber(result, "ignoredCols", 2.0);
    assertNumber(result, "y", 15.0);
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
                 {11.0, 12.0, 13.0, 21.0, 99.0, 23.0});
    assertVector(result, "v", {7.0, 4.0, 7.0, 16.0});
    assertNumber(result, "y", 232.0);

    const auto* loopedAStore =
        findAssignmentProfile(result, "index", "A", 6);
    assert(loopedAStore != nullptr);
    assert(loopedAStore->inLoop);
    assertObservation(loopedAStore->valueObservation, "matrix", 2, 3, 6);
}

void runEndIndexingSmoke() {
    const std::string source = R"(function y = f()
v = [10 20 30 40 50];
A = [1 2 3; 4 5 6];

y = v(end) + v(end - 1) + sum(v(2:end - 1), "all") + sum(v([1 end]), "all");
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
                 {1.0, 2.0, 3.0, 4.0, 5.0, 42.0});
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
    assertMatrix(result, "block", 2, 2, {4.0, 6.0, 7.0, 9.0});
    assertVector(result, "linear",
                 {1.0, 4.0, 7.0, 2.0, 5.0, 8.0, 3.0, 6.0, 9.0});
    assertVector(result, "v", {7.0, 7.0, 7.0, 7.0});
    assertMatrix(result, "A", 3, 3,
                 {1.0, 2.0, 10.0, 4.0, 5.0, 10.0, 7.0, 8.0, 10.0});
    assertNumber(result, "y", 150.0);
}

void runBytecodeProfileSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * 2;
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 12.0);
    assertNumber(result, "y", 156.0);

    assert(result.profile.collected);
    assert(result.profile.hotLoopThreshold == 10);
    assert(!result.profile.instructions.empty());
    assert(instructionProfileExecutionTotal(result) ==
           result.executedInstructionCount);

    const auto* main = findFunctionProfile(result, "main");
    assert(main != nullptr);
    assert(main->callCount == 1);
    assert(main->executedInstructionCount > 0);

    const auto* kernel = findFunctionProfile(result, "kernel");
    assert(kernel != nullptr);
    assert(kernel->callCount == 12);
    assert(kernel->executedInstructionCount > 0);

    const auto* loop = findLoopProfile(result, "i");
    assert(loop != nullptr);
    assert(loop->entryCount == 1);
    assert(loop->iterationCount == 12);
    assert(loop->backedgeCount == 11);
    assert(loop->completionCount == 1);
    assert(loop->hot);
    assertObservation(loop->variableObservation, "number", 1, 1, 12);

    const auto* kernelCall =
        findCallSiteProfile(result, "function", "kernel");
    assert(kernelCall != nullptr);
    assert(kernelCall->executionCount == 12);
    assert(kernelCall->resultCount == 1);
    assert(kernelCall->argumentObservations.size() == 1);
    assert(kernelCall->resultObservations.size() == 1);
    assertObservation(kernelCall->argumentObservations[0], "number", 1, 1,
                      12);
    assertObservation(kernelCall->resultObservations[0], "number", 1, 1,
                      12);

    const auto* loopedYStore =
        findAssignmentProfile(result, "name", "y", 12);
    assert(loopedYStore != nullptr);
    assert(loopedYStore->inLoop);
    assertObservation(loopedYStore->valueObservation, "number", 1, 1, 12);
}

void runProfileDisabledSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * 2;
end
)";

    const auto profiled = run(source);
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    const auto steady = run(source, options);

    assert(profiled.diagnostics.empty());
    assert(steady.diagnostics.empty());
    assert(profiled.profile.collected);
    assert(!steady.profile.collected);
    assert(steady.profile.instructions.empty());
    assert(steady.profile.functions.empty());
    assert(steady.profile.loops.empty());
    assert(steady.profile.callSites.empty());
    assert(steady.profile.assignments.empty());
    assert(steady.executedInstructionCount ==
           profiled.executedInstructionCount);
    assertNumber(steady, "i", 12.0);
    assertNumber(steady, "y", 156.0);
}

void runInitialWorkspaceSmoke() {
    const std::string source = R"(y = y + 1;
)";
    mparser::RuntimeValue initialY;
    initialY.kind = mparser::RuntimeValueKind::Number;
    initialY.number = 41.0;

    mparser::BytecodeVmOptions profiledOptions;
    profiledOptions.initialWorkspace.push_back(
        mparser::RuntimeVariable{"y", initialY});
    const auto profiled = run(source, profiledOptions);
    assert(profiled.diagnostics.empty());
    assertNumber(profiled, "y", 42.0);
    assert(profiled.profile.workspaceInputs.size() == 1);
    assert(profiled.profile.workspaceInputs[0].name == "y");
    assertObservation(
        profiled.profile.workspaceInputs[0].valueObservation,
        "number", 1, 1, 1);

    auto steadyOptions = profiledOptions;
    steadyOptions.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    const auto steady = run(source, steadyOptions);
    assert(steady.diagnostics.empty());
    assertNumber(steady, "y", 42.0);
    assert(!steady.profile.collected);
    assert(steady.profile.workspaceInputs.empty());
}

void runNamedEntryFunctionSmoke() {
    const std::string source = R"(function y = ignored()
y = 999;
end

function [s, p] = kernel(x, n)
s = 0;
for i = 1:n
    s = s + x * i;
end
p = x * n;
end
)";

    mparser::RuntimeValue x;
    x.kind = mparser::RuntimeValueKind::Number;
    x.number = 2.0;
    mparser::RuntimeValue n;
    n.kind = mparser::RuntimeValueKind::Number;
    n.number = 4.0;
    mparser::BytecodeVmOptions options;
    options.entryFunction = "kernel";
    options.arguments = {x, n};

    const auto result = run(source, options);
    assert(result.diagnostics.empty());
    assert(result.entryFunction == "kernel");
    assert(result.outputNames.size() == 2);
    assert(result.outputNames[0] == "s");
    assert(result.outputNames[1] == "p");
    assert(result.outputs.size() == 2);
    assert(result.outputs[0].kind == mparser::RuntimeValueKind::Number);
    assert(result.outputs[0].number == 20.0);
    assert(result.outputs[1].number == 8.0);
    assertNumber(result, "s", 20.0);
    assertNumber(result, "p", 8.0);
    assert(findVariable(result, "y") == nullptr);

    assert(result.profile.functionEntries.size() == 1);
    const auto& entry = result.profile.functionEntries.front();
    assert(entry.name == "kernel");
    assert(entry.parameters.size() == 2);
    assert(entry.parameters[0] == "x");
    assert(entry.parameters[1] == "n");
    assert(entry.outputs == result.outputNames);
    assert(entry.invocationCount == 1);
    assert(entry.argumentObservations.size() == 2);
    assert(entry.resultObservations.size() == 2);
    assertObservation(entry.argumentObservations[0], "number", 1, 1, 1);
    assertObservation(entry.argumentObservations[1], "number", 1, 1, 1);
    assertObservation(entry.resultObservations[0], "number", 1, 1, 1);
    assertObservation(entry.resultObservations[1], "number", 1, 1, 1);

    auto steadyOptions = options;
    steadyOptions.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    const auto steady = run(source, steadyOptions);
    assert(steady.diagnostics.empty());
    assert(steady.outputs.size() == 2);
    assert(steady.outputs[0].number == 20.0);
    assert(!steady.profile.collected);
    assert(steady.profile.functionEntries.empty());

    auto wrongArguments = options;
    wrongArguments.arguments.pop_back();
    const auto mismatch = run(source, wrongArguments);
    assert(!mismatch.diagnostics.empty());

    auto unknownEntry = options;
    unknownEntry.entryFunction = "missing";
    const auto missing = run(source, unknownEntry);
    assert(!missing.diagnostics.empty());
}

void runWhileProfileSmoke() {
    const std::string source = R"(function y = main()
i = 1;
y = 0;
while i <= 11
    y = y + i;
    i = i + 1;
end
end
)";

    const auto result = run(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "i", 12.0);
    assertNumber(result, "y", 66.0);

    const auto* loop = findLoopProfile(result, "while");
    assert(loop != nullptr);
    assert(loop->iterationCount == 11);
    assert(loop->backedgeCount == 11);
    assert(loop->hot);
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
    assertNumber(result, "y", 13.0);

    const auto* exception = findVariable(result, "err");
    assert(exception != nullptr);
    assert(mparser::isRuntimeException(*exception));

    const auto* message = findVariable(result, "message");
    assert(message != nullptr);
    assert(message->kind == mparser::RuntimeValueKind::String);
    assert(message->text == "unknown bytecode runtime variable: missingName");
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
}

} // namespace

int main() {
    runStraightLineFunctionSmoke();
    runVectorAndIndexSmoke();
    runMatrixAndBuiltinSmoke();
    runScriptSkipsLocalFunctionSmoke();
    runForIfSmoke();
    runColonStepRangeSmoke();
    runColonZeroStepDiagnosticSmoke();
    runWhileSmoke();
    runBreakContinueSmoke();
    runReturnSmoke();
    runLocalFunctionSmoke();
    runLocalFunctionScopeSmoke();
    runScriptLocalFunctionSmoke();
    runMultiOutputFunctionSmoke();
    runBuiltinSizeMultiOutputSmoke();
    runIndexedAssignmentSmoke();
    runEndIndexingSmoke();
    runColonIndexingSmoke();
    runBytecodeProfileSmoke();
    runProfileDisabledSmoke();
    runInitialWorkspaceSmoke();
    runNamedEntryFunctionSmoke();
    runWhileProfileSmoke();
    runSwitchSmoke();
    runTryCatchSmoke();
    runSessionCommandSmoke();
    std::cout << "bytecode VM smoke tests passed\n";
    return 0;
}
