#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

mparser::InterpreterResult runInterpreter(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::Interpreter interpreter;
    return interpreter.run(semantic);
}

mparser::BytecodeVmResult runBytecode(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());

    mparser::BytecodeVm vm;
    return vm.run(bytecode, semantic);
}

template <typename Result>
const mparser::RuntimeValue* findVariable(const Result& result,
                                          std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

template <typename Result>
void assertNumber(const Result& result, std::string_view name,
                  double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

void assertFieldOrder(const mparser::RuntimeValue& value,
                      const std::vector<std::string>& expected) {
    assert(value.kind == mparser::RuntimeValueKind::Struct);
    assert(mparser::runtimeStructFieldOrder(value) == expected);
}

const mparser::RuntimeValue& requiredStructField(
    const mparser::RuntimeValue& value, std::string_view name,
    size_t logicalIndex = 0) {
    const auto offset =
        mparser::runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    assert(offset.has_value());
    const auto* field = mparser::runtimeStructField(value, name, *offset);
    assert(field != nullptr);
    return *field;
}

template <typename Result>
void assertNumericArray(const Result& result, std::string_view name,
                        const std::vector<double>& expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Vector ||
           value->kind == mparser::RuntimeValueKind::Matrix);
    assert(value->elements == expected);
}

template <typename Result>
void assertScalarStructResult(const Result& result) {
    assert(result.diagnostics.empty());
    assertNumber(result, "selected", 3);
    assertNumber(result, "nonstruct_flag", 0);
    assertNumber(result, "summary", 2193);

    const auto* structure = findVariable(result, "s");
    assert(structure != nullptr);
    assertFieldOrder(*structure, {"beta", "alpha", "gamma", "delta"});
    assert(mparser::runtimeStructElementCount(*structure) == 1);
    assert(requiredStructField(*structure, "beta").number == 2);
    assert(requiredStructField(*structure, "alpha").number == 1);
    assert(requiredStructField(*structure, "gamma").number == 3);
    assert(requiredStructField(*structure, "delta").kind ==
           mparser::RuntimeValueKind::Cell);
    assert(mparser::runtimeValueToString(*structure) ==
           "struct(beta=2, alpha=1, gamma=3, delta={4, 5})");

    const auto* names = findVariable(result, "names");
    assert(names != nullptr);
    assert(names->kind == mparser::RuntimeValueKind::Cell);
    assert(mparser::runtimeDimensions(*names) ==
           std::vector<size_t>({4, 1}));
    assert(names->cells.size() == 4);
    assert(mparser::runtimeTextScalarUtf8(names->cells[0]) == "beta");
    assert(mparser::runtimeTextScalarUtf8(names->cells[1]) == "alpha");
    assert(mparser::runtimeTextScalarUtf8(names->cells[2]) == "gamma");
    assert(mparser::runtimeTextScalarUtf8(names->cells[3]) == "delta");

    const auto* flags = findVariable(result, "flags");
    assert(flags != nullptr);
    assert(flags->kind == mparser::RuntimeValueKind::Vector);
    assert(flags->numericClass == mparser::RuntimeNumericClass::Logical);
    assert(flags->elements == std::vector<double>({1, 0, 1}));

    const auto* trimmed = findVariable(result, "trimmed");
    assert(trimmed != nullptr);
    assertFieldOrder(*trimmed, {"alpha", "gamma"});
    assert(mparser::runtimeStructField(*trimmed, "beta") == nullptr);
    assert(mparser::runtimeStructField(*trimmed, "delta") == nullptr);
    assert(mparser::runtimeStructField(*structure, "beta") != nullptr);
    assert(mparser::runtimeStructField(*structure, "delta") != nullptr);

    const auto* created = findVariable(result, "created");
    assert(created != nullptr);
    assertFieldOrder(*created, {"dynamic"});
    assert(requiredStructField(*created, "dynamic").number == 7);

    const auto* wrapped = findVariable(result, "wrapped");
    assert(wrapped != nullptr);
    assertFieldOrder(*wrapped, {"value"});
    assert(requiredStructField(*wrapped, "value").kind ==
           mparser::RuntimeValueKind::Number);
    assert(requiredStructField(*wrapped, "value").number == 9);
}

const std::string kScalarStructSource = R"(
s = struct("beta", 2, "alpha", 1);
field = "gamma";
s.(field) = 3;
s.delta = {4, 5};
created.dynamic = 7;
names = fieldnames(s);
flags = isfield(s, {"alpha", "missing", "gamma"});
trimmed = rmfield(s, {"beta", "delta"});
wrapped = struct("value", {9});
selected = s.(field);
nonstruct_flag = isfield(41, "alpha");
order_ok = strcmp(names{1}, "beta") + strcmp(names{2}, "alpha") + ...
           strcmp(names{3}, "gamma") + strcmp(names{4}, "delta");
summary = s.beta * 1000 + s.alpha * 100 + selected * 10 + ...
          created.dynamic + wrapped.value + trimmed.alpha + ...
          trimmed.gamma + sum(flags, "all") + order_ok * 10 + ...
          isstruct(s) + nonstruct_flag;
)";

void runScalarStructParitySmoke() {
    assertScalarStructResult(runInterpreter(kScalarStructSource));
    assertScalarStructResult(runBytecode(kScalarStructSource));
}

const std::string kStructArraySource = R"(
records = struct("value", {10, 20, 30, 40}, "label", "item");
second = records(2).value;
picked = records([4, 2]);
packed = [picked.value];
field_name = "value";
dynamic_packed = [picked.(field_name)];
[first_pick, second_pick] = picked.value;
called = combine(picked.value);
spread_cell = {picked.value};

replacement = struct("value", 99, "label", "replacement");
records(2) = replacement;
records([1, 3]) = replacement;
assigned = [records.value];

grown = records;
grown(6) = replacement;
grown_last = grown(6).value;
gap_empty = isempty(grown(5).value);
gap_is_double = strcmp(class(grown(5).value), "double");
deleted = grown;
deleted([2, 5]) = [];
deleted_values = [deleted.value];
single_deleted = struct("value", 1, "label", "single");
single_deleted(1) = [];

cell_values = reshape({1, 2, 3, 4}, 2, 2);
grid = struct("value", cell_values, "tag", "grid");
grid_21 = grid(2, 1).value;
grid_replacement = struct("value", 8, "tag", "changed");
grid(2, 2) = grid_replacement;
grid_22 = grid(2, 2).value;
grid_column = grid(:, 2);
grid_column_values = [grid_column.value];
subscripts = struct("value", {2, 2});
grid_via_csl = grid(subscripts.value).value;
column_values = reshape({7, 9}, 2, 1);
column_replacements = struct("value", column_values, "tag", "column");
grid_assigned = grid;
grid_assigned(:, 1) = column_replacements;
grid_assigned_values = [grid_assigned(:, 1).value];

empty_typed = struct("value", {});
empty_bare = struct([]);
empty_typed_flag = isempty(empty_typed);
empty_bare_flag = isempty(empty_bare);
empty_has_field = isfield(empty_typed, "value");
empty_names = fieldnames(empty_typed);
removed = rmfield(records, "label");
removed_values = [removed.value];

summary = sum(packed) + first_pick + second_pick + called + ...
          sum(dynamic_packed) + sum(assigned) + grown_last + ...
          gap_empty + gap_is_double + ...
          sum(deleted_values) + grid_21 + grid_22 + ...
          sum(grid_column_values) + grid_via_csl + ...
          sum(grid_assigned_values) + ...
          empty_typed_flag + empty_bare_flag + empty_has_field + ...
          sum(removed_values);

function out = combine(left, right)
out = left * 10 + right;
end
)";

template <typename Result>
void assertStructArrayResult(const Result& result) {
    assert(result.diagnostics.empty());
    assertNumber(result, "second", 20);
    assertNumber(result, "first_pick", 40);
    assertNumber(result, "second_pick", 20);
    assertNumber(result, "called", 420);
    assertNumber(result, "grown_last", 99);
    assertNumber(result, "gap_empty", 1);
    assertNumber(result, "gap_is_double", 1);
    assertNumber(result, "grid_21", 2);
    assertNumber(result, "grid_22", 8);
    assertNumber(result, "grid_via_csl", 8);
    assertNumber(result, "empty_typed_flag", 1);
    assertNumber(result, "empty_bare_flag", 1);
    assertNumber(result, "empty_has_field", 1);
    assertNumber(result, "summary", 1760);
    assertNumericArray(result, "packed", {40, 20});
    assertNumericArray(result, "dynamic_packed", {40, 20});
    assertNumericArray(result, "assigned", {99, 99, 99, 40});
    assertNumericArray(result, "deleted_values", {99, 99, 40, 99});
    assertNumericArray(result, "grid_column_values", {3, 8});
    assertNumericArray(result, "grid_assigned_values", {7, 9});
    assertNumericArray(result, "removed_values", {99, 99, 99, 40});

    const auto* records = findVariable(result, "records");
    assert(records != nullptr);
    assert(mparser::runtimeDimensions(*records) ==
           std::vector<size_t>({1, 4}));
    assertFieldOrder(*records, {"value", "label"});
    assert(requiredStructField(*records, "value", 0).number == 99);
    assert(requiredStructField(*records, "value", 3).number == 40);

    const auto* grid = findVariable(result, "grid");
    assert(grid != nullptr);
    assert(mparser::runtimeDimensions(*grid) ==
           std::vector<size_t>({2, 2}));
    assert(requiredStructField(*grid, "value", 0).number == 1);
    assert(requiredStructField(*grid, "value", 1).number == 2);
    assert(requiredStructField(*grid, "value", 2).number == 3);
    assert(requiredStructField(*grid, "value", 3).number == 8);

    const auto* gridAssigned = findVariable(result, "grid_assigned");
    assert(gridAssigned != nullptr);
    assert(mparser::runtimeDimensions(*gridAssigned) ==
           std::vector<size_t>({2, 2}));
    assert(requiredStructField(*gridAssigned, "value", 0).number == 7);
    assert(requiredStructField(*gridAssigned, "value", 1).number == 9);

    const auto* grown = findVariable(result, "grown");
    assert(grown != nullptr);
    const auto& grownGap = requiredStructField(*grown, "value", 4);
    assert(grownGap.kind == mparser::RuntimeValueKind::Matrix);
    assert(grownGap.numericClass == mparser::RuntimeNumericClass::Double);
    assert(mparser::runtimeDimensions(grownGap) ==
           std::vector<size_t>({0, 0}));

    const auto* emptyTyped = findVariable(result, "empty_typed");
    assert(emptyTyped != nullptr);
    assert(mparser::runtimeDimensions(*emptyTyped) ==
           std::vector<size_t>({0, 0}));
    assertFieldOrder(*emptyTyped, {"value"});
    assert(mparser::runtimeStructElementCount(*emptyTyped) == 0);

    const auto* singleDeleted = findVariable(result, "single_deleted");
    assert(singleDeleted != nullptr);
    assert(mparser::runtimeDimensions(*singleDeleted) ==
           std::vector<size_t>({0, 0}));
    assertFieldOrder(*singleDeleted, {"value", "label"});

    const auto* spreadCell = findVariable(result, "spread_cell");
    assert(spreadCell != nullptr);
    assert(spreadCell->kind == mparser::RuntimeValueKind::Cell);
    assert(spreadCell->cells.size() == 2);
    assert(spreadCell->cells[0].number == 40);
    assert(spreadCell->cells[1].number == 20);
}

void runStructArrayParitySmoke() {
    assertStructArrayResult(runInterpreter(kStructArraySource));
    assertStructArrayResult(runBytecode(kStructArraySource));
}

void runDynamicObjectMemberSmoke() {
    const std::string source = R"(
classdef DynamicMemberBox
    properties
        Value
    end
    methods
        function obj = DynamicMemberBox(value)
            obj.Value = value;
        end
    end
end

box = DynamicMemberBox(4);
property_name = "Value";
before = box.(property_name);
box.(property_name) = 9;
after = box.Value;
)";

    const auto result = runBytecode(source);
    assert(result.diagnostics.empty());
    assertNumber(result, "before", 4);
    assertNumber(result, "after", 9);
}

template <typename Result>
void assertDiagnostic(const Result& result, std::string_view fragment) {
    assert(!result.diagnostics.empty());
    bool found = false;
    for (const auto& diagnostic : result.diagnostics) {
        found = found || diagnostic.message.find(fragment) !=
                             std::string::npos;
    }
    assert(found);
}

void assertDiagnosticParity(std::string_view source,
                            std::string_view fragment) {
    assertDiagnostic(runInterpreter(source), fragment);
    assertDiagnostic(runBytecode(source), fragment);
}

void runStructDiagnosticSmoke() {
    assertDiagnosticParity(
        "s = struct(\"a\", 1, \"a\", 2);",
        "duplicate structure field name: a");
    assertDiagnosticParity(
        "s = struct(\"a\");",
        "struct constructor expects field/value pairs");
    assertDiagnosticParity(
        "s = struct(); name = \"not valid\"; s.(name) = 1;",
        "invalid structure field name: not valid");
    assertDiagnosticParity(
        "s = struct(\"a\", 1); value = s.(2);",
        "dynamic field name must be a character vector");
    assertDiagnosticParity(
        "s = struct(\"a\", 1); t = rmfield(s, \"missing\");",
        "structure field is not available: missing");
    assertDiagnosticParity(
        "a = reshape({1, 2}, 1, 2); b = reshape({3, 4}, 2, 1); "
        "s = struct(\"a\", a, \"b\", b);",
        "nonscalar Cell values in struct must have matching dimensions");
    assertDiagnosticParity(
        "s = struct(\"a\", {1, 2}); s.a = 4;",
        "direct field assignment requires a scalar structure");
    assertDiagnosticParity(
        "s = struct(\"a\", {1, 2}); value = s.a;",
        "comma-separated values where one was required");
    assertDiagnosticParity(
        "s = struct(\"a\", {1, 2}); t = struct(\"b\", 3); s(1) = t;",
        "subscripted assignment between dissimilar structures");
    assertDiagnosticParity(
        "s = struct(\"a\", reshape({1, 2, 3, 4}, 2, 2)); "
        "t = struct(\"a\", {5, 6, 7, 8}); s(:, :) = t;",
        "structure assignment dimensions do not match the right-hand value");
}

} // namespace

int main() {
    runScalarStructParitySmoke();
    runStructArrayParitySmoke();
    runDynamicObjectMemberSmoke();
    runStructDiagnosticSmoke();
    std::cout << "scalar struct runtime smoke tests passed\n";
    return 0;
}
