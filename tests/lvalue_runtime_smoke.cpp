#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

const std::string_view kLvalueSource = R"(
root.inner.value = 7;
root.inner.data = [10, 20, 30];
root.inner.data(2) = 99;
root.inner.data(end) = 77;

root.items = struct("value", {1, 2, 3});
root.items(2).value = 20;
field_name = "value";
root.items(3).(field_name) = 31;
root.items(5).value = 55;
root.items(2).tag = 9;
items_sum = sum([root.items.value]);
item_tag = root.items(2).tag;
tag_gap_empty = isempty(root.items(1).tag);

root.created(3).value = 6;
created_value = root.created(3).value;
created_gap_empty = isempty(root.created(2).value);

root.cells = {struct("value", 4), struct("value", 5)};
root.cells{2}.value = 50;
root.more_cells{3}.value = 12;
more_cell_value = root.more_cells{3}.value;

root.grid = reshape({1, 2, 3, 4}, 2, 2);
root.grid{2, 1} = 22;
root.grid(1, 2) = {33};
grid_column = root.grid(:, 2);
grid_column_sum = grid_column{1} + grid_column{2};

root.inner.data(1) = [];
root.cells(1) = [];

caught = 0;
try
    root.inner.data(1).bad = 4;
catch err
    caught = 1;
end
unchanged = root.inner.data(1);

growth_caught = 0;
try
    root.items(8).value.bad = 4;
catch growth_err
    growth_caught = 1;
end
item_count = size(root.items, 2);

summary = root.inner.value + sum(root.inner.data) + ...
          items_sum + item_tag + tag_gap_empty + ...
          created_value + created_gap_empty + ...
          root.cells{1}.value + more_cell_value + ...
          root.grid{1, 1} + root.grid{2, 1} + ...
          root.grid{1, 2} + root.grid{2, 2} + ...
          grid_column_sum + caught + unchanged + ...
          growth_caught + item_count;
)";

struct CompiledSource {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

CompiledSource compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    return CompiledSource{std::move(semantic), std::move(bytecode)};
}

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
    auto compiled = compile(source);
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
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

template <typename Result>
void assertLvalueResult(const Result& result) {
    assert(result.diagnostics.empty());
    assertNumber(result, "summary", 572);
    assertNumber(result, "items_sum", 107);
    assertNumber(result, "item_tag", 9);
    assertNumber(result, "tag_gap_empty", 1);
    assertNumber(result, "created_value", 6);
    assertNumber(result, "created_gap_empty", 1);
    assertNumber(result, "more_cell_value", 12);
    assertNumber(result, "grid_column_sum", 37);
    assertNumber(result, "caught", 1);
    assertNumber(result, "unchanged", 99);
    assertNumber(result, "growth_caught", 1);
    assertNumber(result, "item_count", 5);
}

bool containsOp(const mparser::BytecodeProgram& program,
                mparser::BytecodeOp op) {
    for (const auto& instruction : program.instructions) {
        if (instruction.op == op) {
            return true;
        }
    }
    return false;
}

void runLvalueParitySmoke() {
    assertLvalueResult(runInterpreter(kLvalueSource));
    assertLvalueResult(runBytecode(kLvalueSource));

    const auto compiled = compile(kLvalueSource);
    assert(containsOp(compiled.bytecode, mparser::BytecodeOp::BeginLvalue));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::BeginLvalueIndexContext));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::LvalueDescendMember));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::LvalueDescendIndex));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::LvalueDescendBrace));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::StorePathMember));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::StorePathIndex));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::StorePathBrace));
}

void runObjectCopybackSmoke() {
    const auto result = runBytecode(R"(
classdef LvalueLeaf
    properties
        Value
    end
    methods
        function obj = LvalueLeaf(value)
            obj.Value = value;
        end
    end
end

classdef LvalueHolder
    properties
        Child
    end
    methods
        function obj = LvalueHolder(child)
            obj.Child = child;
        end
    end
end

classdef LvalueHandleHolder < handle
    properties
        Child
    end
    methods
        function obj = LvalueHandleHolder(child)
            obj.Child = child;
        end
    end
end

value_holder = LvalueHolder(LvalueLeaf(3));
value_copy = value_holder;
value_copy.Child.Value = 8;
value_new = value_copy.Child.Value;
value_old = value_holder.Child.Value;

handle_holder = LvalueHandleHolder(LvalueLeaf(4));
handle_alias = handle_holder;
handle_alias.Child.Value = 9;
handle_seen = handle_holder.Child.Value;
summary = value_new * 100 + value_old * 10 + handle_seen;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "value_new", 8);
    assertNumber(result, "value_old", 3);
    assertNumber(result, "handle_seen", 9);
    assertNumber(result, "summary", 839);
}

} // namespace

int main() {
    runLvalueParitySmoke();
    runObjectCopybackSmoke();
    std::cout << "lvalue runtime smoke tests passed\n";
    return 0;
}
