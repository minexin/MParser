#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

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

const mparser::SyntaxNode* findSyntax(const mparser::SyntaxNode& node,
                                      mparser::SyntaxKind kind,
                                      std::string_view label = {}) {
    if (node.kind == kind && (label.empty() || node.label == label)) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = findSyntax(*child, kind, label)) {
            return found;
        }
    }
    return nullptr;
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

constexpr std::string_view kAccessClasses = R"(classdef AccessBase
    properties (Access = private)
        Secret(1,1) double = 11
    end
    properties (Access = protected)
        Code(1,1) double = 7
    end
    methods
        function value = reveal(obj)
            value = obj.Secret + obj.hiddenValue();
        end
    end
    methods (Access = private)
        function value = hiddenValue(obj)
            value = obj.Secret;
        end
    end
    methods (Access = protected)
        function value = protectedValue(obj)
            value = obj.Code;
        end
    end
end

classdef AccessChild < AccessBase
    methods
        function value = childCode(obj)
            value = obj.Code;
        end
        function value = childCall(obj)
            value = obj.protectedValue();
        end
    end
end
)";

void parseQualifiedAccessorsAndMethodAttributesSmoke() {
    auto compiled = compile(R"(classdef ParsedAccessors
    properties (Dependent)
        Value
    end
    methods
        function value = get.Value(obj)
            value = 1;
        end
    end
    methods (Access = private, Static)
        function value = helper()
            value = 2;
        end
    end
end
)");

    const auto* getter = findSyntax(*compiled.parsed.root,
                                    mparser::SyntaxKind::FunctionDef,
                                    "get.Value");
    assert(getter != nullptr);
    assert(getter->attributes.empty());
    const auto* helper = findSyntax(*compiled.parsed.root,
                                    mparser::SyntaxKind::FunctionDef,
                                    "helper");
    assert(helper != nullptr);
    assert(helper->attributes.size() == 2);
    assert(helper->attributes[0].name == "Access");
    assert(helper->attributes[0].value.find("private") != std::string::npos);
    assert(helper->attributes[1].name == "Static");
}

void executePropertyAndMethodAccessSmoke() {
    const auto result = run(std::string(kAccessClasses) + R"(
obj = AccessChild();
revealed = obj.reveal();
child_code = obj.childCode();
child_call = obj.childCall();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "revealed", 22);
    assertNumber(result, "child_code", 7);
    assertNumber(result, "child_call", 7);
}

void rejectExternalAndSubclassPrivateAccessSmoke() {
    auto result = run(std::string(kAccessClasses) + R"(
obj = AccessChild();
value = obj.Secret;
)");
    assert(hasDiagnostic(result, "property get access is denied"));

    result = run(std::string(kAccessClasses) + R"(
obj = AccessChild();
obj.Code = 3;
)");
    assert(hasDiagnostic(result, "property set access is denied"));

    result = run(std::string(kAccessClasses) + R"(
obj = AccessChild();
value = obj.hiddenValue();
)");
    assert(hasDiagnostic(result, "method access is denied"));

    result = run(std::string(kAccessClasses) + R"(
classdef InvalidPrivateChild < AccessBase
    methods
        function value = readSecret(obj)
            value = obj.Secret;
        end
    end
end
obj = InvalidPrivateChild();
value = obj.readSecret();
)");
    assert(hasDiagnostic(result, "property get access is denied"));
}

void executePrivateConstructorFactorySmoke() {
    const auto source = R"(classdef FactoryOnly
    properties
        Value(1,1) double
    end
    methods (Access = private)
        function obj = FactoryOnly(value)
            obj.Value = value;
        end
    end
    methods (Static)
        function obj = create(value)
            obj = FactoryOnly(value);
        end
    end
end
)";

    auto result = run(std::string(source) + R"(
obj = FactoryOnly.create(9);
value = obj.Value;
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "value", 9);

    result = run(std::string(source) + R"(
obj = FactoryOnly(9);
)");
    assert(hasDiagnostic(result, "constructor access is denied"));

    result = run(R"(classdef ProtectedBase
    properties
        Value
    end
    methods (Access = protected)
        function obj = ProtectedBase(value)
            obj.Value = value;
        end
    end
end

classdef PublicDerived < ProtectedBase
    methods
        function obj = PublicDerived(value)
            obj = obj@ProtectedBase(value);
        end
    end
end

obj = PublicDerived(12);
value = obj.Value;
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "value", 12);

    result = run(R"(classdef ProtectedOnly
    methods (Access = protected)
        function obj = ProtectedOnly()
        end
    end
end
obj = ProtectedOnly();
)");
    assert(hasDiagnostic(result, "constructor access is denied"));
}

void executeConstantAndImmutablePropertiesSmoke() {
    const auto result = run(R"(classdef NamedValues
    properties (Constant)
        Scale(1,1) double = 4
        Offset(1,1) double = NamedValues.Scale + 2
    end
    properties (Constant, GetAccess = private)
        Hidden(1,1) double = 3
    end
    properties (Constant)
        FromHidden(1,1) double = NamedValues.Hidden + 5
    end
end

classdef DerivedValues < NamedValues
end

classdef Identity
    properties (SetAccess = immutable)
        Id(1,1) double
    end
    methods
        function obj = Identity(value)
            obj.Id = value;
        end
    end
end

scale = NamedValues.Scale;
offset = DerivedValues.Offset;
from_hidden = NamedValues.FromHidden;
obj = Identity(17);
id = obj.Id;
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "scale", 4);
    assertNumber(result, "offset", 6);
    assertNumber(result, "from_hidden", 8);
    assertNumber(result, "id", 17);

    auto rejected = run(R"(classdef NamedValues
    properties (Constant)
        Scale = 4
    end
end
obj = NamedValues();
obj.Scale = 8;
)");
    assert(hasDiagnostic(rejected, "constant property cannot be assigned"));

    rejected = run(R"(classdef NamedValues
    properties (Constant)
        Scale = 4
    end
end
NamedValues.Scale = 8;
)");
    assert(hasDiagnostic(rejected, "constant property cannot be assigned"));

    rejected = run(R"(classdef Identity
    properties (SetAccess = immutable)
        Id
    end
    methods
        function obj = Identity(value)
            obj.Id = value;
        end
    end
end
obj = Identity(1);
obj.Id = 2;
)");
    assert(hasDiagnostic(rejected, "property set access is denied"));
}

constexpr std::string_view kAccessorClass = R"(classdef ScaledValue
    properties (Access = private)
        Raw(1,1) double = 1
    end
    properties (Dependent)
        Value(1,1) double {mustBePositive}
    end
    methods
        function value = get.Value(obj)
            value = obj.Raw * 2;
        end
        function obj = set.Value(obj, value)
            obj.Raw = value / 2;
        end
    end
end
)";

void executeDependentAccessorsAndValidationSmoke() {
    const auto result = run(std::string(kAccessorClass) + R"(
obj = ScaledValue();
initial = obj.Value;
obj.Value = 10;
stored = obj.Value;
try
    obj.Value = -2;
catch
    end
    after_rejection = obj.Value;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "initial", 2);
    assertNumber(result, "stored", 10);
    assertNumber(result, "after_rejection", 10);
}

void inheritPropertyAccessorsSmoke() {
    const auto result = run(std::string(kAccessorClass) + R"(
classdef DerivedScaledValue < ScaledValue
end
obj = DerivedScaledValue();
obj.Value = 14;
value = obj.Value;
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "value", 14);
}

void executeHandleSetterAndAbortSetSmoke() {
    const auto result = run(R"(classdef ObservableValue < handle
    properties (AbortSet)
        Value(1,1) double = 1
    end
    properties (SetAccess = private)
        ChangeCount(1,1) double = 0
    end
    methods
        function set.Value(obj, value)
            obj.Value = value;
            obj.ChangeCount = obj.ChangeCount + 1;
        end
        function value = count(obj)
            value = obj.ChangeCount;
        end
    end
end

obj = ObservableValue();
alias = obj;
obj.Value = 5;
obj.Value = 5;
count = alias.count();
value = alias.Value;
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "count", 1);
    assertNumber(result, "value", 5);
}

void rejectMalformedMemberContractsSmoke() {
    auto result = run(R"(classdef BadDependent
    properties (Dependent)
        Value
    end
end
obj = BadDependent();
)");
    assert(hasDiagnostic(result, "dependent property requires a get method"));

    result = run(R"(classdef BadConstant
    properties (Constant)
        Value
    end
end
obj = BadConstant();
)");
    assert(hasDiagnostic(result, "constant property requires a default value"));

    result = run(R"(classdef BadAccessor
    properties (Dependent)
        Value
    end
    methods (Access = private)
        function value = get.Value(obj)
            value = 1;
        end
    end
end
obj = BadAccessor();
)");
    assert(hasDiagnostic(
        result,
        "property access methods must be declared in a methods block without "
        "attributes"));

    result = run(R"(classdef BadAccess
    properties (GetAccess = internal)
        Value = 1
    end
end
obj = BadAccess();
)");
    assert(hasDiagnostic(result, "unsupported access attribute value"));

    result = run(R"(classdef PublicMethodBase
    methods
        function value = execute(obj)
            value = 1;
        end
    end
end
classdef PrivateOverride < PublicMethodBase
    methods (Access = private)
        function value = execute(obj)
            value = 2;
        end
    end
end
obj = PrivateOverride();
)");
    assert(hasDiagnostic(result,
                         "overriding method must preserve Access"));
}

} // namespace

int main() {
    parseQualifiedAccessorsAndMethodAttributesSmoke();
    executePropertyAndMethodAccessSmoke();
    rejectExternalAndSubclassPrivateAccessSmoke();
    executePrivateConstructorFactorySmoke();
    executeConstantAndImmutablePropertiesSmoke();
    executeDependentAccessorsAndValidationSmoke();
    inheritPropertyAccessorsSmoke();
    executeHandleSetterAndAbortSetSmoke();
    rejectMalformedMemberContractsSmoke();
    std::cout << "class member access smoke tests passed\n";
    return 0;
}
