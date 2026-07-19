#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_shape.h"
#include "mparser/semantic.h"

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
    assert(value.fieldOrder == expected);
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
    assert(structure->fields.size() == 4);
    assert(structure->fields.at("beta").number == 2);
    assert(structure->fields.at("alpha").number == 1);
    assert(structure->fields.at("gamma").number == 3);
    assert(structure->fields.at("delta").kind ==
           mparser::RuntimeValueKind::Cell);
    assert(mparser::runtimeValueToString(*structure) ==
           "struct(beta=2, alpha=1, gamma=3, delta={4, 5})");

    const auto* names = findVariable(result, "names");
    assert(names != nullptr);
    assert(names->kind == mparser::RuntimeValueKind::Cell);
    assert(mparser::runtimeDimensions(*names) ==
           std::vector<size_t>({4, 1}));
    assert(names->cells.size() == 4);
    assert(names->cells[0].text == "beta");
    assert(names->cells[1].text == "alpha");
    assert(names->cells[2].text == "gamma");
    assert(names->cells[3].text == "delta");

    const auto* flags = findVariable(result, "flags");
    assert(flags != nullptr);
    assert(flags->kind == mparser::RuntimeValueKind::Vector);
    assert(flags->numericClass == mparser::RuntimeNumericClass::Logical);
    assert(flags->elements == std::vector<double>({1, 0, 1}));

    const auto* trimmed = findVariable(result, "trimmed");
    assert(trimmed != nullptr);
    assertFieldOrder(*trimmed, {"alpha", "gamma"});
    assert(!trimmed->fields.contains("beta"));
    assert(!trimmed->fields.contains("delta"));
    assert(structure->fields.contains("beta"));
    assert(structure->fields.contains("delta"));

    const auto* created = findVariable(result, "created");
    assert(created != nullptr);
    assertFieldOrder(*created, {"dynamic"});
    assert(created->fields.at("dynamic").number == 7);

    const auto* wrapped = findVariable(result, "wrapped");
    assert(wrapped != nullptr);
    assertFieldOrder(*wrapped, {"value"});
    assert(wrapped->fields.at("value").kind ==
           mparser::RuntimeValueKind::Number);
    assert(wrapped->fields.at("value").number == 9);
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
        "s = struct(\"a\", {1, 2});",
        "nonscalar Cell values create structure arrays");
    assertDiagnosticParity(
        "s = struct(\"a\");",
        "scalar struct constructor expects field/value pairs");
    assertDiagnosticParity(
        "s = struct(); name = \"not valid\"; s.(name) = 1;",
        "invalid structure field name: not valid");
    assertDiagnosticParity(
        "s = struct(\"a\", 1); value = s.(2);",
        "dynamic field name must be a character vector");
    assertDiagnosticParity(
        "s = struct(\"a\", 1); t = rmfield(s, \"missing\");",
        "structure field is not available: missing");
}

} // namespace

int main() {
    runScalarStructParitySmoke();
    runDynamicObjectMemberSmoke();
    runStructDiagnosticSmoke();
    std::cout << "scalar struct runtime smoke tests passed\n";
    return 0;
}
