#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_struct.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct RuntimeResults {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult bytecode;
};

RuntimeResults runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic);

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic);
    return RuntimeResults{std::move(interpreted), std::move(executed)};
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
void assertNumericArray(const Result& result, std::string_view name,
                        const std::vector<double>& expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Vector ||
           value->kind == mparser::RuntimeValueKind::Matrix);
    assert(value->elements == expected);
}

template <typename Check>
void assertSuccessfulParity(std::string_view source, Check check) {
    const auto results = runBoth(source);
    assert(results.interpreter.diagnostics.empty());
    assert(results.bytecode.diagnostics.empty());
    check(results.interpreter);
    check(results.bytecode);
}

void runCellValuedSwitchSmoke() {
    // Generalizes cap_101_ctl_switch beyond its numeric selector.
    constexpr std::string_view source = R"(
numeric_selector = 3;
switch numeric_selector
case {1, 2}
    numeric_case = 12;
case {3, 4}
    numeric_case = 34;
otherwise
    numeric_case = -1;
end

text_selector = 'beta';
switch text_selector
case {'alpha', 'beta'}
    text_case = 2;
otherwise
    text_case = -1;
end

switch 3
case {3, 4}
    first_match = 1;
case 3
    first_match = 2;
end

switch 9
case {1, 2}
    no_match = -1;
otherwise
    no_match = 9;
end
)";

    assertSuccessfulParity(source, [](const auto& result) {
        assertNumber(result, "numeric_case", 34);
        assertNumber(result, "text_case", 2);
        assertNumber(result, "first_match", 1);
        assertNumber(result, "no_match", 9);
    });
}

void runNestedFunctionSmoke() {
    // Generalizes cap_124_fun_nested_function across lexical call shapes.
    constexpr std::string_view source = R"(
read_result = read_outer();
[mutation_result, mutation_after] = mutate_outer(4);
first_result = first_outer();
second_result = second_outer();
sibling_result = sibling_outer();
[parameter_result, parameter_after] = parameter_outer(7);
handle_result = handle_outer();
deep_result = deep_outer();

function out = read_outer()
z = 3;
out = inner();
    function value = inner()
    value = z * 2;
    end
end

function [out, after] = mutate_outer(seed)
shared = seed;
out = inner();
after = shared;
    function value = inner()
    shared = shared + 3;
    value = shared;
    end
end

function out = first_outer()
bias = 10;
out = inner();
    function value = inner()
    value = bias + 1;
    end
end

function out = second_outer()
bias = 20;
out = inner();
    function value = inner()
    value = bias + 2;
    end
end

function out = sibling_outer()
base = 5;
out = left();
    function value = left()
    value = right() + 1;
    end
    function value = right()
    value = base * 2;
    end
end

function [out, after] = parameter_outer(seed)
shared = seed;
out = inner(2);
after = shared;
    function value = inner(delta)
    shared = shared + delta;
    value = shared;
    end
end

function out = handle_outer()
base = 8;
callable = @inner;
out = callable();
    function value = inner()
    value = base + 1;
    end
end

function out = deep_outer()
root = 2;
out = middle();
    function value = middle()
    offset = 3;
    value = inner();
        function nested_value = inner()
        nested_value = root + offset;
        end
    end
end
)";

    assertSuccessfulParity(source, [](const auto& result) {
        assertNumber(result, "read_result", 6);
        assertNumber(result, "mutation_result", 7);
        assertNumber(result, "mutation_after", 7);
        assertNumber(result, "first_result", 11);
        assertNumber(result, "second_result", 22);
        assertNumber(result, "sibling_result", 11);
        assertNumber(result, "parameter_result", 9);
        assertNumber(result, "parameter_after", 9);
        assertNumber(result, "handle_result", 9);
        assertNumber(result, "deep_result", 5);
    });

    const auto escaped = runBoth(R"(
callable = make_handle();
value = callable();

function out = make_handle()
captured = 4;
out = @inner;
    function value = inner()
    value = captured;
    end
end
)");
    assert(!escaped.interpreter.diagnostics.empty());
    assert(!escaped.bytecode.diagnostics.empty());
    assert(escaped.interpreter.diagnostics.front().message.find(
               "lexical parent is not active") != std::string::npos);
    assert(escaped.bytecode.diagnostics.front().message.find(
               "lexical parent is not active") != std::string::npos);
}

void runCommaSeparatedListSmoke() {
    // Generalizes cap_164_cell_comma_list across every shared expansion path.
    constexpr std::string_view source = R"(
values = {1, 2, 3};
expanded = [values{:}];
[left, right] = values{1:2};
reordered = [values{[3, 1]}];
called = pair(values{2:3});
wrapped = {values{1:2}};

function out = pair(first, second)
out = first * 10 + second;
end
)";

    assertSuccessfulParity(source, [](const auto& result) {
        assertNumericArray(result, "expanded", {1, 2, 3});
        assertNumber(result, "left", 1);
        assertNumber(result, "right", 2);
        assertNumericArray(result, "reordered", {3, 1});
        assertNumber(result, "called", 23);
        const auto* wrapped = findVariable(result, "wrapped");
        assert(wrapped != nullptr);
        assert(wrapped->kind == mparser::RuntimeValueKind::Cell);
        assert(wrapped->cells.size() == 2);
        assert(std::fabs(wrapped->cells[0].number - 1) < 1e-9);
        assert(std::fabs(wrapped->cells[1].number - 2) < 1e-9);
    });

    const auto mismatch = runBoth(R"(
values = {1};
[left, right] = values{1};
)");
    assert(!mismatch.interpreter.diagnostics.empty());
    assert(!mismatch.bytecode.diagnostics.empty());
}

template <typename Result>
void assertImplicitStructResult(const Result& result) {
    assertNumber(result, "first", 1);
    assertNumber(result, "third", 3);
    assertNumber(result, "middle_b", 20);
    assertNumber(result, "gap_a_empty", 1);
    assertNumber(result, "gap_b_empty", 1);
    assertNumber(result, "nested_value", 9);
    assertNumber(result, "nested_gap_empty", 1);
    assertNumber(result, "grid_value", 4);
    assertNumber(result, "dynamic_score", 7);

    const auto* structure = findVariable(result, "s");
    assert(structure != nullptr);
    assert(structure->kind == mparser::RuntimeValueKind::Struct);
    assert(mparser::runtimeDimensions(*structure) ==
           std::vector<size_t>({1, 3}));
    assert(mparser::runtimeStructFieldOrder(*structure) ==
           std::vector<std::string>({"a", "b"}));

    const auto* nested = findVariable(result, "nested");
    assert(nested != nullptr);
    assert(mparser::runtimeDimensions(*nested) ==
           std::vector<size_t>({1, 2}));

    const auto* grid = findVariable(result, "grid");
    assert(grid != nullptr);
    assert(mparser::runtimeDimensions(*grid) ==
           std::vector<size_t>({2, 2}));
}

void runImplicitStructCreationSmoke() {
    // Generalizes cap_177_struct_implicit_create across shape and field paths.
    constexpr std::string_view source = R"(
s(1).a = 1;
s(3).a = 3;
s(2).b = 20;
first = s(1).a;
third = s(3).a;
middle_b = s(2).b;
gap_a_empty = isempty(s(2).a);
gap_b_empty = isempty(s(1).b);

nested(2).child.value = 9;
nested_value = nested(2).child.value;
nested_gap_empty = isempty(nested(1).child);

grid(2, 2).value = 4;
grid_value = grid(2, 2).value;

field = 'score';
dynamic(2).(field) = 7;
dynamic_score = dynamic(2).score;
)";

    assertSuccessfulParity(source, [](const auto& result) {
        assertImplicitStructResult(result);
    });
}

} // namespace

int main() {
    runCellValuedSwitchSmoke();
    runNestedFunctionSmoke();
    runCommaSeparatedListSmoke();
    runImplicitStructCreationSmoke();
    std::cout << "dynamic language semantics smoke tests passed\n";
    return 0;
}
