#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_object.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
    return {std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(const CompiledSource& compiled) {
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
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

void assertNumber(const mparser::BytecodeVmResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

void assertText(const mparser::BytecodeVmResult& result,
                std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    assert(text && *text == expected);
}

void assertLogicalArray(
    const mparser::BytecodeVmResult& result, std::string_view name,
    const std::vector<size_t>& dimensions,
    const std::vector<double>& expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(mparser::isRuntimeLogical(*value));
    if (mparser::runtimeDimensions(*value) != dimensions) {
        std::cerr << name << " dimensions:";
        for (const size_t dimension : mparser::runtimeDimensions(*value)) {
            std::cerr << ' ' << dimension;
        }
        std::cerr << '\n';
    }
    assert(mparser::runtimeDimensions(*value) == dimensions);
    assert(mparser::runtimeShapeElementCount(*value) == expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto element = mparser::runtimeNumericElement(*value, index);
        assert(element && *element == expected[index]);
    }
}

void runObjectArraySmoke() {
    const auto compiled = compile(R"(classdef ValueItem
    properties
        Value = 0
    end
    methods
        function obj = ValueItem(value)
        arguments
            value (1,1) double = 0
        end
            obj.Value = value;
        end
        function total = total(obj)
            total = sum([obj.Value]);
        end
    end
end

classdef HandleItem < handle
    properties
        Value = 0
    end
    methods
        function obj = HandleItem(value)
        arguments
            value (1,1) double = 0
        end
            obj.Value = value;
        end
    end
end

classdef Shape < matlab.mixin.Heterogeneous
    properties
        Code = 0
    end
    methods
        function obj = Shape(code)
        arguments
            code (1,1) double = 0
        end
            obj.Code = code;
        end
        function total = total(obj)
            total = sum([obj.Code]);
        end
    end
end

classdef Circle < Shape
    methods
        function obj = Circle(code)
        arguments
            code (1,1) double = 0
        end
            obj.Code = code;
        end
    end
end

classdef Square < Shape
    methods
        function obj = Square(code)
        arguments
            code (1,1) double = 0
        end
            obj.Code = code;
        end
    end
end

items = ValueItem(1);
items(2) = ValueItem(2);
items(3).Value = 30;
values = [items.Value];
items_total = items.total();
empty_items = items([]);
empty_class = class(empty_items);
empty_count = numel(empty_items);
masked = items(logical([1 0 1]));
masked_values = [masked.Value];
repeated = items;
repeated([2 2]) = [ValueItem(8), ValueItem(9)];
repeated_value = repeated(2).Value;
copy = items;
copy(1).Value = 99;
original_value = items(1).Value;
copied_value = copy(1).Value;

grid = [ValueItem(1), ValueItem(2); ValueItem(3), ValueItem(4)];
transposed = grid';
transpose_value = transposed(2, 1).Value;
flat = reshape(grid, 1, 4);
reshape_value = flat(2).Value;
cube = reshape(grid, 2, 1, 2);
cube_value = cube(2, 1, 2).Value;
permuted = permute(cube, [3 2 1]);
permuted_value = permuted(1, 1, 2).Value;
squeezed = squeeze(cube);
squeezed_value = squeezed(2, 2).Value;
stacked = cat(3, grid, grid);
stacked_value = stacked(1, 2, 2).Value;
tiles = repmat(ValueItem(6), 1, 2);
tiles(1).Value = 9;
tile_other_value = tiles(2).Value;
trimmed = grid;
trimmed(2, :) = [];
trimmed_values = [trimmed.Value];

h = HandleItem(7);
aliases = [h, h];
aliases(1).Value = 11;
alias_value = aliases(2).Value;
same_alias = aliases(1) == aliases(2);
grown = h;
grown(3) = HandleItem(30);
gap_value = grown(2).Value;
same_gap = grown(1) == grown(2);
handle_tiles = repmat(h, 1, 2);
handle_tiles(1).Value = 13;
handle_tile_other_value = handle_tiles(2).Value;
alias_mask = aliases == h;
grown_identity = grown == grown(1);
same_items = isequal(items, items);
same_copy = isequal(items, copy);
valid_before = isvalid(grown);
empty_valid = isvalid(grown([]));
delete(aliases);
alias_valid_after = isvalid(aliases);
grown_valid_mid = isvalid(grown);
delete(grown);
valid_after = isvalid(grown);

mixed = [Circle(5), Square(7)];
mixed_class = class(mixed);
first_class = class(mixed(1));
mixed_total = mixed.total();
mixed_kept = mixed;
mixed_kept(2) = [];
mixed_kept_class = class(mixed_kept);
)");

    const auto result = run(compiled);
    if (!result.diagnostics.empty()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << '\n';
        }
    }
    assert(result.diagnostics.empty());
    assertNumber(result, "items_total", 33);
    assertText(result, "empty_class", "ValueItem");
    assertNumber(result, "empty_count", 0);
    assertNumber(result, "repeated_value", 9);
    assertNumber(result, "original_value", 1);
    assertNumber(result, "copied_value", 99);
    assertNumber(result, "transpose_value", 2);
    assertNumber(result, "reshape_value", 3);
    assertNumber(result, "cube_value", 4);
    assertNumber(result, "permuted_value", 3);
    assertNumber(result, "squeezed_value", 4);
    assertNumber(result, "stacked_value", 2);
    assertNumber(result, "tile_other_value", 6);
    assertNumber(result, "handle_tile_other_value", 13);
    assertNumber(result, "alias_value", 11);
    assertNumber(result, "same_alias", 1);
    assertNumber(result, "gap_value", 0);
    assertNumber(result, "same_gap", 0);
    assertLogicalArray(result, "alias_mask", {1, 2}, {1, 1});
    assertLogicalArray(result, "grown_identity", {1, 3}, {1, 0, 0});
    assertNumber(result, "same_items", 1);
    assertNumber(result, "same_copy", 0);
    assertLogicalArray(result, "valid_before", {1, 3}, {1, 1, 1});
    assertLogicalArray(result, "empty_valid", {0, 0}, {});
    assertLogicalArray(result, "alias_valid_after", {1, 2}, {0, 0});
    assertLogicalArray(result, "grown_valid_mid", {1, 3}, {0, 1, 1});
    assertLogicalArray(result, "valid_after", {1, 3}, {0, 0, 0});
    assertText(result, "mixed_class", "Shape");
    assertText(result, "first_class", "Circle");
    assertText(result, "mixed_kept_class", "Circle");
    assertNumber(result, "mixed_total", 12);

    const auto* trimmedValues = findVariable(result, "trimmed_values");
    assert(trimmedValues != nullptr);
    assert(mparser::runtimeDimensions(*trimmedValues) ==
           std::vector<size_t>({1, 2}));
    assert(mparser::runtimeNumericElement(*trimmedValues, 0) == 1);
    assert(mparser::runtimeNumericElement(*trimmedValues, 1) == 2);

    const auto* maskedValues = findVariable(result, "masked_values");
    assert(maskedValues != nullptr);
    assert(mparser::runtimeDimensions(*maskedValues) ==
           std::vector<size_t>({1, 2}));
    assert(mparser::runtimeNumericElement(*maskedValues, 0) == 1);
    assert(mparser::runtimeNumericElement(*maskedValues, 1) == 30);

    const auto* items = findVariable(result, "items");
    const auto* mixed = findVariable(result, "mixed");
    assert(items && mparser::runtimeObjectElementCount(*items) == 3);
    assert(mparser::runtimeValueToString(*items) ==
           "<ValueItem 1x3>");
    assert(mixed && mixed->className == "Shape");
    assert(mparser::runtimeObjectElementCount(*mixed) == 2);
}

void runDefaultConstructionTransactionSmoke() {
    const auto compiled = compile(R"(classdef RequiredValue
    properties
        Value = 0
    end
    methods
        function obj = RequiredValue(value)
        arguments
            value (1,1) double
        end
            obj.Value = value;
        end
    end
end

original = RequiredValue(1);
original(3) = RequiredValue(3);
)");
    const auto result = run(compiled);
    assert(!result.diagnostics.empty());

    const auto* original = findVariable(result, "original");
    assert(original != nullptr);
    assert(mparser::isRuntimeScalarObject(*original));
    const auto* fields = mparser::runtimeObjectFields(*original);
    assert(fields != nullptr);
    const auto value = fields->find("RequiredValue::Value");
    assert(value != fields->end());
    assert(value->second.kind == mparser::RuntimeValueKind::Number);
    assert(value->second.number == 1);
}

void runNonscalarPropertyAssignmentRejectionSmoke() {
    const auto compiled = compile(R"(classdef ValueItem
    properties
        Value = 0
    end
    methods
        function obj = ValueItem(value)
            obj.Value = value;
        end
    end
end

items = [ValueItem(1), ValueItem(2)];
items.Value = 9;
)");
    const auto result = run(compiled);
    assert(!result.diagnostics.empty());
    const bool found = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const mparser::Diagnostic& diagnostic) {
            return diagnostic.message.find(
                       "property assignment requires a scalar object target") !=
                   std::string::npos;
        });
    assert(found);

    const auto* items = findVariable(result, "items");
    assert(items != nullptr);
    assert(mparser::runtimeObjectElementCount(*items) == 2);
    for (size_t index = 0; index < 2; ++index) {
        const auto* element =
            mparser::runtimeObjectLogicalElement(*items, index);
        assert(element != nullptr);
        const auto* fields = mparser::runtimeObjectFields(*element);
        assert(fields != nullptr);
        const auto value = fields->find("ValueItem::Value");
        assert(value != fields->end());
        assert(value->second.kind == mparser::RuntimeValueKind::Number);
        assert(value->second.number == static_cast<double>(index + 1));
    }
}

void runHomogeneousRejectionSmoke() {
    const auto compiled = compile(R"(classdef FirstValue
end
classdef SecondValue
end
values = [FirstValue(), SecondValue()];
)");
    const auto result = run(compiled);
    assert(!result.diagnostics.empty());
    bool found = false;
    for (const auto& diagnostic : result.diagnostics) {
        found = found ||
                diagnostic.message.find("ordinary object arrays cannot mix") !=
                    std::string::npos;
    }
    assert(found);
}

} // namespace

int main() {
    runObjectArraySmoke();
    runHomogeneousRejectionSmoke();
    runDefaultConstructionTransactionSmoke();
    runNonscalarPropertyAssignmentRejectionSmoke();
    return 0;
}
