#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct CompiledSource {
    mparser::ParseResult parsed;
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
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    auto compiled = compile(source);
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

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void assertValueObjectSlots(const mparser::BytecodeVmResult& result,
                            std::string_view variable,
                            std::initializer_list<std::string_view> keys) {
    const auto* object = findVariable(result, variable);
    assert(object != nullptr);
    assert(object->kind == mparser::RuntimeValueKind::Object);
    assert(!object->handleObject);
    for (const auto key : keys) {
        assert(object->fields.contains(std::string(key)));
    }
}

void assertHandleObjectSlots(const mparser::BytecodeVmResult& result,
                             std::string_view variable,
                             std::initializer_list<std::string_view> keys) {
    const auto* object = findVariable(result, variable);
    assert(object != nullptr);
    assert(object->kind == mparser::RuntimeValueKind::Object);
    assert(object->handleObject);
    assert(object->sharedFields != nullptr);
    for (const auto key : keys) {
        assert(object->sharedFields->contains(std::string(key)));
    }
}

void executeValuePropertyIdentitySmoke() {
    const auto result = run(R"(classdef PrivateBase
    properties (Access = private)
        Value(1,1) double {mustBePositive} = 2
    end
    methods
        function value = baseValue(obj)
            value = obj.Value;
        end
        function obj = setBaseValue(obj, value)
            obj.Value = value;
        end
    end
end

classdef PrivateChild < PrivateBase
    properties
        Value(1,1) double = 1
    end
    methods
        function value = childValue(obj)
            value = obj.Value;
        end
    end
end

obj = PrivateChild();
base_default = obj.baseValue();
child_default = obj.Value;

copy = obj;
copy = copy.setBaseValue(8);
copy.Value = 9;
copy_base = copy.baseValue();
copy_child = copy.childValue();
original_base = obj.baseValue();
original_child = obj.Value;

try
    copy = copy.setBaseValue(-1);
catch
end
copy_base_after_reject = copy.baseValue();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_default", 2);
    assertNumber(result, "child_default", 1);
    assertNumber(result, "copy_base", 8);
    assertNumber(result, "copy_child", 9);
    assertNumber(result, "original_base", 2);
    assertNumber(result, "original_child", 1);
    assertNumber(result, "copy_base_after_reject", 8);
    assertValueObjectSlots(
        result, "obj", {"PrivateBase::Value", "PrivateChild::Value"});
    assertValueObjectSlots(
        result, "copy", {"PrivateBase::Value", "PrivateChild::Value"});
}

void executeHandlePropertyIdentitySmoke() {
    const auto result = run(R"(classdef HandlePrivateBase < handle
    properties (Access = private)
        Count = 10
    end
    methods
        function setBaseCount(obj, value)
            obj.Count = value;
        end
        function value = baseCount(obj)
            value = obj.Count;
        end
    end
end

classdef HandlePrivateChild < HandlePrivateBase
    properties
        Count = 20
    end
    methods
        function setChildCount(obj, value)
            obj.Count = value;
        end
        function value = childCount(obj)
            value = obj.Count;
        end
    end
end

obj = HandlePrivateChild();
alias = obj;
obj.setBaseCount(11);
alias.setChildCount(22);
base_count = alias.baseCount();
child_count = obj.Count;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_count", 11);
    assertNumber(result, "child_count", 22);
    assertHandleObjectSlots(
        result, "obj",
        {"HandlePrivateBase::Count", "HandlePrivateChild::Count"});
    const auto* object = findVariable(result, "obj");
    const auto* alias = findVariable(result, "alias");
    assert(object != nullptr && alias != nullptr);
    assert(object->sharedFields == alias->sharedFields);
}

void executeConstructorPropertyIdentitySmoke() {
    const auto result = run(R"(classdef ConstructedBase
    properties (GetAccess = private, SetAccess = private)
        Value = 2
    end
    methods
        function obj = ConstructedBase(value)
            obj.Value = value;
        end
        function value = baseValue(obj)
            value = obj.Value;
        end
    end
end

classdef ConstructedChild < ConstructedBase
    properties
        Value = 1
    end
    methods
        function obj = ConstructedChild(baseValue, childValue)
            obj = obj@ConstructedBase(baseValue);
            obj.Value = childValue;
        end
    end
end

obj = ConstructedChild(6, 7);
base_value = obj.baseValue();
child_value = obj.Value;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_value", 6);
    assertNumber(result, "child_value", 7);
    assertValueObjectSlots(
        result, "obj",
        {"ConstructedBase::Value", "ConstructedChild::Value"});
}

void executeQualifiedAccessorIdentitySmoke() {
    const auto result = run(R"(classdef AccessorBase
    properties (Access = private)
        Raw = 3
    end
    properties (Access = private, Dependent)
        Value
    end
    methods
        function value = get.Value(obj)
            value = obj.Raw + 100;
        end
        function obj = set.Value(obj, value)
            obj.Raw = value;
        end
        function value = baseValue(obj)
            value = obj.Value;
        end
        function obj = setBaseValue(obj, value)
            obj.Value = value;
        end
    end
end

classdef AccessorChild < AccessorBase
    properties (Access = private)
        Raw = 4
    end
    properties (Dependent)
        Value
    end
    methods
        function value = get.Value(obj)
            value = obj.Raw * 2;
        end
        function obj = set.Value(obj, value)
            obj.Raw = value;
        end
    end
end

obj = AccessorChild();
base_initial = obj.baseValue();
child_initial = obj.Value;
obj = obj.setBaseValue(7);
obj.Value = 9;
base_after = obj.baseValue();
child_after = obj.Value;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_initial", 103);
    assertNumber(result, "child_initial", 8);
    assertNumber(result, "base_after", 107);
    assertNumber(result, "child_after", 18);
    assertValueObjectSlots(
        result, "obj", {"AccessorBase::Raw", "AccessorChild::Raw"});
}

void executePrivateConstantIdentitySmoke() {
    const auto result = run(R"(classdef ConstantBase
    properties (Access = private, Constant)
        Code = 2
    end
    methods
        function value = baseCode(obj)
            value = obj.Code;
        end
        function value = qualifiedChildCode(obj)
            value = ConstantChild.Code;
        end
    end
end

classdef ConstantChild < ConstantBase
    properties (Constant)
        Code = 3
    end
end

obj = ConstantChild();
base_code = obj.baseCode();
child_code = obj.Code;
class_code = ConstantChild.Code;
qualified_child_code = obj.qualifiedChildCode();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_code", 2);
    assertNumber(result, "child_code", 3);
    assertNumber(result, "class_code", 3);
    assertNumber(result, "qualified_child_code", 3);
}

void executeMultipleInheritancePrivateSlotsSmoke() {
    const auto result = run(R"(classdef LeftPrivate
    properties (Access = private)
        Shared = 1
    end
    methods
        function value = leftValue(obj)
            value = obj.Shared;
        end
    end
end

classdef RightPrivate
    properties (Access = private)
        Shared = 2
    end
    methods
        function value = rightValue(obj)
            value = obj.Shared;
        end
    end
end

classdef PublicProperty
    properties
        Shared = 4
    end
end

classdef AllPrivateCombined < LeftPrivate & RightPrivate
    properties
        Shared = 3
    end
end

classdef PublicAndPrivateCombined < LeftPrivate & PublicProperty
end

all_private = AllPrivateCombined();
left_value = all_private.leftValue();
right_value = all_private.rightValue();
combined_value = all_private.Shared;

mixed = PublicAndPrivateCombined();
mixed_private = mixed.leftValue();
mixed_public = mixed.Shared;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "left_value", 1);
    assertNumber(result, "right_value", 2);
    assertNumber(result, "combined_value", 3);
    assertNumber(result, "mixed_private", 1);
    assertNumber(result, "mixed_public", 4);
    assertValueObjectSlots(
        result, "all_private",
        {"LeftPrivate::Shared", "RightPrivate::Shared",
         "AllPrivateCombined::Shared"});
    assertValueObjectSlots(
        result, "mixed",
        {"LeftPrivate::Shared", "PublicProperty::Shared"});
}

void executeEmptyListPrivateRedeclarationSmoke() {
    const auto result = run(R"(classdef EmptyPrivateBase
    properties (Access = {})
        Token = 2
    end
    methods
        function value = baseToken(obj)
            value = obj.Token;
        end
    end
end

classdef EmptyPrivateChild < EmptyPrivateBase
    properties
        Token = 1
    end
end

obj = EmptyPrivateChild();
base_token = obj.baseToken();
child_token = obj.Token;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_token", 2);
    assertNumber(result, "child_token", 1);
}

void rejectNonPrivateRedeclarationSmoke() {
    auto result = run(R"(classdef SetPrivateBase
    properties (SetAccess = private)
        Value = 1
    end
end

classdef SetPrivateChild < SetPrivateBase
    properties
        Value = 2
    end
end
)");
    assert(hasDiagnostic(
        result,
        "inherited property cannot be redeclared: SetPrivateChild.Value"));

    result = run(R"(classdef NamedAccessBase
    properties (Access = ?NamedFriend)
        Value = 1
    end
end

classdef NamedFriend
end

classdef NamedAccessChild < NamedAccessBase
    properties
        Value = 2
    end
end
)");
    assert(hasDiagnostic(
        result,
        "inherited property cannot be redeclared: NamedAccessChild.Value"));
}

} // namespace

int main() {
    executeValuePropertyIdentitySmoke();
    executeHandlePropertyIdentitySmoke();
    executeConstructorPropertyIdentitySmoke();
    executeQualifiedAccessorIdentitySmoke();
    executePrivateConstantIdentitySmoke();
    executeMultipleInheritancePrivateSlotsSmoke();
    executeEmptyListPrivateRedeclarationSmoke();
    rejectNonPrivateRedeclarationSmoke();
    std::cout << "class property identity smoke tests passed\n";
    return 0;
}
