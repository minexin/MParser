#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
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
        if (const auto* found = findNode(*child, kind, label)) {
            return found;
        }
    }
    return nullptr;
}

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view message) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message == message) {
            return true;
        }
    }
    return false;
}

void assertNumber(const mparser::BytecodeVmResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

void runSingleInheritanceSmoke() {
    constexpr std::string_view source = R"(classdef CounterEntity < Entity
    properties
        Count
    end
    methods
        function obj = CounterEntity(id, count)
            obj.Id = id;
            obj.Count = count;
        end
        function value = score(obj)
            value = obj.Id + obj.Count;
        end
    end
end

classdef Entity < handle
    properties
        Id
    end
    methods
        function setId(obj, id)
            obj.Id = id;
        end
        function value = describe(obj)
            value = obj.Id;
        end
        function value = score(obj)
            value = obj.Id;
        end
    end
    methods (Static)
        function value = category()
            value = 10;
        end
    end
end

item = CounterEntity(3, 4);
alias = item;
alias.setId(8);
inherited_value = item.describe();
override_value = item.score();
static_value = CounterEntity.category();
base_property = item.Id;
derived_property = item.Count;
)";

    const auto compiled = compile(source);
    const auto* derived = findNode(*compiled.semantic.root,
                                   mparser::HirKind::Class, "CounterEntity");
    assert(derived != nullptr);
    const auto* constructor = findNode(*derived, mparser::HirKind::Function,
                                       "CounterEntity");
    assert(constructor != nullptr);
    const auto* inheritedAccess = findNode(
        *constructor, mparser::HirKind::MemberAccess, "Id");
    assert(inheritedAccess != nullptr);
    assert(inheritedAccess->binding.kind == mparser::BindingKind::Property);
    assert(inheritedAccess->binding.symbolId >= 0);
    assert(compiled.semantic
               .symbols[static_cast<size_t>(inheritedAccess->binding.symbolId)]
               .name == "Id");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    const auto* item = findVariable(result, "item");
    const auto* alias = findVariable(result, "alias");
    assert(item != nullptr);
    assert(alias != nullptr);
    assert(item->kind == mparser::RuntimeValueKind::Object);
    assert(item->className == "CounterEntity");
    assert(item->handleObject);
    assert(item->sharedFields != nullptr);
    assert(item->sharedFields == alias->sharedFields);
    assertNumber(result, "inherited_value", 8);
    assertNumber(result, "override_value", 12);
    assertNumber(result, "static_value", 10);
    assertNumber(result, "base_property", 8);
    assertNumber(result, "derived_property", 4);
}

void runSharedDiamondSmoke() {
    const auto compiled = compile(R"(classdef Diamond < LeftBranch & RightBranch
end
classdef LeftBranch < RootClass
end
classdef RightBranch < RootClass
end
classdef RootClass
    methods
        function value = token(obj)
            value = 5;
        end
    end
end
diamond = Diamond();
diamond_value = diamond.token();
)");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    assertNumber(result, "diamond_value", 5);
}

void runMostSpecificMethodSmoke() {
    const auto compiled = compile(R"(classdef SpecificDiamond < PlainBranch & OverrideBranch
    methods
        function value = invoke(obj)
            value = obj.token();
        end
    end
end
classdef PlainBranch < MethodRoot
end
classdef OverrideBranch < MethodRoot
    methods
        function value = token(obj)
            value = 7;
        end
    end
end
classdef MethodRoot
    methods
        function value = token(obj)
            value = 5;
        end
    end
end
specific = SpecificDiamond();
specific_value = specific.invoke();
)");

    const auto* derived = findNode(*compiled.semantic.root,
                                   mparser::HirKind::Class,
                                   "SpecificDiamond");
    assert(derived != nullptr);
    const auto* invoke = findNode(*derived, mparser::HirKind::Function,
                                  "invoke");
    assert(invoke != nullptr);
    const auto* token = findNode(*invoke, mparser::HirKind::MemberAccess,
                                 "token");
    assert(token != nullptr);
    assert(token->binding.kind == mparser::BindingKind::Method);
    assert(token->binding.symbolId >= 0);
    const auto& tokenSymbol = compiled.semantic.symbols[
        static_cast<size_t>(token->binding.symbolId)];
    assert(tokenSymbol.scopeId >= 0);
    assert(compiled.semantic.scopes[static_cast<size_t>(tokenSymbol.scopeId)]
               .label == "OverrideBranch");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    assertNumber(result, "specific_value", 7);
}

void runConflictResolutionSmoke() {
    const auto resolved = compile(R"(classdef Resolved < LeftChoice & RightChoice
    methods
        function value = choose(obj)
            value = 3;
        end
    end
end
classdef LeftChoice
    methods
        function value = choose(obj)
            value = 1;
        end
    end
end
classdef RightChoice
    methods
        function value = choose(obj)
            value = 2;
        end
    end
end
item = Resolved();
resolved_value = item.choose();
)");
    const auto resolvedResult = run(resolved);
    assert(resolvedResult.diagnostics.empty());
    assertNumber(resolvedResult, "resolved_value", 3);

    const auto ambiguous = compile(R"(classdef Ambiguous < LeftChoice & RightChoice
end
classdef LeftChoice
    methods
        function value = choose(obj)
            value = 1;
        end
    end
end
classdef RightChoice
    methods
        function value = choose(obj)
            value = 2;
        end
    end
end
)");
    const auto ambiguousResult = run(ambiguous);
    assert(hasDiagnostic(
        ambiguousResult,
        "ambiguous inherited method: Ambiguous.choose from LeftChoice and "
        "RightChoice"));
}

void runPropertyHierarchySmoke() {
    const auto shared = compile(R"(classdef PropertyDiamond < LeftProperty & RightProperty
end
classdef LeftProperty < PropertyRoot
end
classdef RightProperty < PropertyRoot
end
classdef PropertyRoot
    properties
        Value
    end
end
item = PropertyDiamond();
item.Value = 6;
property_value = item.Value;
)");
    const auto sharedResult = run(shared);
    assert(sharedResult.diagnostics.empty());
    assertNumber(sharedResult, "property_value", 6);

    const auto ambiguous = compile(R"(classdef BadProperty < LeftValue & RightValue
end
classdef LeftValue
    properties
        Value
    end
end
classdef RightValue
    properties
        Value
    end
end
)");
    const auto ambiguousResult = run(ambiguous);
    assert(hasDiagnostic(
        ambiguousResult,
        "ambiguous inherited property: BadProperty.Value from LeftValue and "
        "RightValue"));

    const auto redeclared = compile(R"(classdef Redeclared < PropertyBase
    properties
        Value
    end
end
classdef PropertyBase
    properties
        Value
    end
end
)");
    const auto redeclaredResult = run(redeclared);
    assert(hasDiagnostic(
        redeclaredResult,
        "inherited property cannot be redeclared: Redeclared.Value"));
}

void runInvalidHierarchySmoke() {
    const auto missing = compile("classdef Broken < MissingBase\nend\n");
    const auto missingResult = run(missing);
    assert(hasDiagnostic(
        missingResult,
        "superclass is not available: MissingBase (required by Broken)"));

    const auto cyclic = compile(
        "classdef CycleA < CycleB\nend\n"
        "classdef CycleB < CycleA\nend\n");
    const auto cyclicResult = run(cyclic);
    assert(!cyclicResult.diagnostics.empty());
    bool foundCycle = false;
    for (const auto& diagnostic : cyclicResult.diagnostics) {
        if (diagnostic.message.find("cyclic class inheritance involving:") ==
            0) {
            foundCycle = true;
        }
    }
    assert(foundCycle);
}

} // namespace

int main() {
    runSingleInheritanceSmoke();
    runSharedDiamondSmoke();
    runMostSpecificMethodSmoke();
    runConflictResolutionSmoke();
    runPropertyHierarchySmoke();
    runInvalidHierarchySmoke();
    std::cout << "class inheritance smoke tests passed\n";
    return 0;
}
