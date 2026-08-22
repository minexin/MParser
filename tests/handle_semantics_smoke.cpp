#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

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

const mparser::HirNode* findNode(const mparser::HirNode& node,
                                 mparser::HirKind kind,
                                 std::string_view label) {
    if (node.kind == kind && node.label == label) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* result = findNode(*child, kind, label)) {
            return result;
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

void runHandleAliasSmoke() {
    constexpr std::string_view source = R"(classdef MutableBox < handle
    properties
        Value
        LastNargout
    end
    methods
        function obj = MutableBox(value)
            obj.Value = value;
            obj.LastNargout = 99;
        end
        function setValue(obj, value)
            obj.Value = value;
            obj.LastNargout = nargout;
        end
    end
end

original = MutableBox(1);
alias = original;
alias.setValue(9);
shared_value = original.Value;
statement_nargout = original.LastNargout;
)";

    const auto compiled = compile(source);
    const auto* klass = findNode(*compiled.semantic.root,
                                 mparser::HirKind::Class, "MutableBox");
    assert(klass != nullptr);
    assert(klass->superclasses.size() == 1);
    assert(klass->superclasses.front() == "handle");
    assert(findNode(*klass, mparser::HirKind::Statement, "handle") == nullptr);

    size_t valueParameterCount = 0;
    bool foundMethodReceiver = false;
    for (const auto& symbol : compiled.semantic.symbols) {
        if (symbol.kind != mparser::SymbolKind::FunctionParameter) {
            continue;
        }
        if (symbol.name == "value") {
            ++valueParameterCount;
            assert(symbol.typeName.empty());
        }
        if (symbol.name == "obj") {
            foundMethodReceiver = true;
            assert(symbol.typeName == "MutableBox");
        }
    }
    assert(valueParameterCount == 2);
    assert(foundMethodReceiver);

    const auto implicitOutputCalls = std::count_if(
        compiled.bytecode.instructions.begin(),
        compiled.bytecode.instructions.end(), [](const auto& instruction) {
            return instruction.op == mparser::BytecodeOp::CallOrIndex &&
                   instruction.resultCount == 1 &&
                   instruction.implicitExpressionOutput;
        });
    assert(implicitOutputCalls == 1);

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    const auto* original = findVariable(result, "original");
    const auto* alias = findVariable(result, "alias");
    assert(original != nullptr);
    assert(alias != nullptr);
    assert(original->kind == mparser::RuntimeValueKind::Object);
    assert(alias->kind == mparser::RuntimeValueKind::Object);
    assert(original->handleObject);
    assert(alias->handleObject);
    assert(original->sharedFields != nullptr);
    assert(original->sharedFields == alias->sharedFields);
    assertNumber(result, "shared_value", 9);
    assertNumber(result, "statement_nargout", 0);
}

void runValueCopySmoke() {
    constexpr std::string_view source = R"(classdef ValueBox
    properties
        Value
    end
    methods
        function obj = ValueBox(value)
            obj.Value = value;
        end
    end
end

first = ValueBox(2);
second = first;
second.Value = 7;
first_value = first.Value;
second_value = second.Value;
)";

    const auto compiled = compile(source);
    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    const auto* first = findVariable(result, "first");
    const auto* second = findVariable(result, "second");
    assert(first != nullptr);
    assert(second != nullptr);
    assert(first->kind == mparser::RuntimeValueKind::Object);
    assert(second->kind == mparser::RuntimeValueKind::Object);
    assert(!first->handleObject);
    assert(!second->handleObject);
    assert(first->sharedFields == nullptr);
    assert(second->sharedFields == nullptr);
    assertNumber(result, "first_value", 2);
    assertNumber(result, "second_value", 7);
}

} // namespace

int main() {
    runHandleAliasSmoke();
    runValueCopySmoke();
    std::cout << "handle semantics smoke tests passed\n";
    return 0;
}
