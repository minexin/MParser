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

const mparser::SyntaxNode* findSyntax(const mparser::SyntaxNode& node,
                                      mparser::SyntaxKind kind,
                                      std::string_view label) {
    if (node.kind == kind && node.label == label) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = findSyntax(*child, kind, label)) {
            return found;
        }
    }
    return nullptr;
}

const mparser::AttributeSyntax* findAttribute(
    const mparser::SyntaxNode& node, std::string_view name) {
    for (const auto& attribute : node.attributes) {
        if (attribute.name == name) {
            return &attribute;
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

void parseStructuredMetaClassListsSmoke() {
    const auto compiled = compile(R"(classdef (AllowedSubclasses = {?AllowedLeaf, ?pkg.Future}) PolicyBase
    properties (GetAccess = {?PolicyReader, ?pkg.Reader}, SetAccess = {})
        Value = 1
    end
    methods (Access = ?PolicyFactory)
        function obj = PolicyBase()
        end
    end
end
)");

    const auto* klass = findSyntax(*compiled.parsed.root,
                                   mparser::SyntaxKind::ClassDef,
                                   "PolicyBase");
    assert(klass != nullptr);
    const auto* allowed = findAttribute(*klass, "AllowedSubclasses");
    assert(allowed != nullptr);
    assert(allowed->hasMetaClassList);
    assert(allowed->metaClassNames.size() == 2);
    assert(allowed->metaClassNames[0] == "AllowedLeaf");
    assert(allowed->metaClassNames[1] == "pkg.Future");

    const auto* property = findSyntax(*compiled.parsed.root,
                                      mparser::SyntaxKind::PropertyDecl,
                                      "Value");
    assert(property != nullptr);
    const auto* getAccess = findAttribute(*property, "GetAccess");
    const auto* setAccess = findAttribute(*property, "SetAccess");
    assert(getAccess != nullptr && getAccess->hasMetaClassList);
    assert(getAccess->metaClassNames.size() == 2);
    assert(getAccess->metaClassNames[0] == "PolicyReader");
    assert(getAccess->metaClassNames[1] == "pkg.Reader");
    assert(setAccess != nullptr && setAccess->hasMetaClassList);
    assert(setAccess->metaClassNames.empty());

    const auto* constructor = findSyntax(*compiled.parsed.root,
                                         mparser::SyntaxKind::FunctionDef,
                                         "PolicyBase");
    assert(constructor != nullptr);
    const auto* access = findAttribute(*constructor, "Access");
    assert(access != nullptr && access->hasMetaClassList);
    assert(access->metaClassNames.size() == 1);
    assert(access->metaClassNames.front() == "PolicyFactory");
}

constexpr std::string_view kVaultClasses = R"(classdef Vault < handle
    properties (GetAccess = {?VaultReader}, SetAccess = {?VaultWriter})
        Value = 0
    end
    methods (Access = ?VaultFactory)
        function obj = Vault(seed)
            obj.Value = seed;
        end
    end
    methods (Access = {?VaultReader})
        function value = secretCode(obj)
            value = obj.Value + 100;
        end
    end
end

classdef VaultFactory
    methods (Static)
        function obj = create(seed)
            obj = Vault(seed);
        end
    end
end

classdef VaultReader
    methods (Static)
        function value = read(obj)
            value = obj.Value;
        end
        function value = secret(obj)
            value = obj.secretCode();
        end
    end
end

classdef PrivilegedReader < VaultReader
    methods (Static)
        function value = peek(obj)
            value = obj.Value;
        end
    end
end

classdef VaultWriter
    methods (Static)
        function write(obj, value)
            obj.Value = value;
        end
    end
end

classdef VaultIntruder
    methods (Static)
        function value = peek(obj)
            value = obj.Value;
        end
    end
end
)";

void executeFriendAndDescendantAccessSmoke() {
    const auto result = run(std::string(kVaultClasses) + R"(
vault = VaultFactory.create(7);
initial = VaultReader.read(vault);
secret = VaultReader.secret(vault);
VaultWriter.write(vault, 11);
updated = PrivilegedReader.peek(vault);
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "initial", 7);
    assertNumber(result, "secret", 107);
    assertNumber(result, "updated", 11);
}

void rejectUnlistedClassAccessSmoke() {
    auto result = run(std::string(kVaultClasses) + R"(
vault = VaultFactory.create(7);
value = vault.Value;
)");
    assert(hasDiagnostic(result, "property get access is denied"));

    result = run(std::string(kVaultClasses) + R"(
vault = VaultFactory.create(7);
vault.Value = 9;
)");
    assert(hasDiagnostic(result, "property set access is denied"));

    result = run(std::string(kVaultClasses) + R"(
vault = VaultFactory.create(7);
value = vault.secretCode();
)");
    assert(hasDiagnostic(result, "method access is denied"));

    result = run(std::string(kVaultClasses) + R"(
vault = VaultFactory.create(7);
value = VaultIntruder.peek(vault);
)");
    assert(hasDiagnostic(result, "property get access is denied"));

    result = run(std::string(kVaultClasses) + R"(
vault = Vault(7);
)");
    assert(hasDiagnostic(result, "constructor access is denied"));
}

void enforceDefiningClassListSemanticsSmoke() {
    auto result = run(R"(classdef HiddenBox
    properties (GetAccess = ?MissingReader)
        Value = 4
    end
    methods
        function value = read(obj)
            value = obj.Value;
        end
    end
end

obj = HiddenBox();
inside = obj.read();
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "inside", 4);

    result = run(R"(classdef HiddenBox
    properties (GetAccess = ?MissingReader)
        Value = 4
    end
end

obj = HiddenBox();
outside = obj.Value;
)");
    assert(hasDiagnostic(result, "property get access is denied"));

    result = run(R"(classdef Owner
    properties (GetAccess = ?Friend)
        Value = 5
    end
end

classdef Friend
end

classdef OwnerChild < Owner
    methods
        function value = peek(obj)
            value = obj.Value;
        end
    end
end

obj = OwnerChild();
value = obj.peek();
)");
    assert(hasDiagnostic(result, "property get access is denied"));
}

void executeAllowedSubclassChainSmoke() {
    const auto result = run(R"(classdef (AllowedSubclasses = ?ExtensionPoint) RestrictedRoot
    methods
        function value = code(obj)
            value = 23;
        end
    end
end

classdef ExtensionPoint < RestrictedRoot
end

classdef ExtensionLeaf < ExtensionPoint
end

obj = ExtensionLeaf();
value = obj.code();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "value", 23);
}

void rejectDisallowedAndEmptySubclassListsSmoke() {
    auto result = run(R"(classdef (AllowedSubclasses = {?AllowedChild}) RestrictedBase
end

classdef AllowedChild < RestrictedBase
end

classdef RejectedChild < RestrictedBase
end
)");
    assert(hasDiagnostic(result, "class is not listed in AllowedSubclasses"));

    result = run(R"(classdef (AllowedSubclasses = {}) ClosedBase
end

classdef ClosedChild < ClosedBase
end
)");
    assert(hasDiagnostic(result, "sealed class cannot be subclassed"));

    result = run(R"(classdef (AllowedSubclasses = ?MissingChild) MissingClosedBase
end

classdef ExistingChild < MissingClosedBase
end
)");
    assert(hasDiagnostic(result, "sealed class cannot be subclassed"));
}

void enforceClassListMethodOverrideSmoke() {
    auto result = run(R"(classdef OverrideBase
    methods (Access = ?AuthorizedChild)
        function value = compute(obj)
            value = 1;
        end
    end
end

classdef AuthorizedChild < OverrideBase
    methods (Access = ?AuthorizedChild)
        function value = compute(obj)
            value = 2;
        end
    end
    methods
        function value = run(obj)
            value = obj.compute();
        end
    end
end

obj = AuthorizedChild();
value = obj.run();
)");
    assert(result.diagnostics.empty());
    assertNumber(result, "value", 2);

    result = run(R"(classdef OverrideBase
    methods (Access = ?AuthorizedChild)
        function value = compute(obj)
            value = 1;
        end
    end
end

classdef AuthorizedChild < OverrideBase
end

classdef UnauthorizedChild < OverrideBase
    methods (Access = ?AuthorizedChild)
        function value = compute(obj)
            value = 3;
        end
    end
end
)");
    assert(hasDiagnostic(
        result, "subclass cannot override inaccessible method"));

    result = run(R"(classdef MissingListBase
    methods (Access = ?MissingFriend)
        function value = compute(obj)
            value = 1;
        end
    end
end

classdef MissingListChild < MissingListBase
    methods (Access = ?MissingFriend)
        function value = compute(obj)
            value = 4;
        end
    end
end
)");
    assert(hasDiagnostic(
        result, "subclass cannot override inaccessible method"));
}

void rejectMalformedClassPoliciesSmoke() {
    auto result = run(R"(classdef BadAccessList
    properties (GetAccess = {?Friend, public})
        Value = 1
    end
end

classdef Friend
end
)");
    assert(hasDiagnostic(result, "unsupported access attribute value"));

    result = run(R"(classdef (AllowedSubclasses = public) BadRestriction
end
)");
    assert(hasDiagnostic(
        result, "AllowedSubclasses requires ?Class or a cell array"));
}

} // namespace

int main() {
    parseStructuredMetaClassListsSmoke();
    executeFriendAndDescendantAccessSmoke();
    rejectUnlistedClassAccessSmoke();
    enforceDefiningClassListSemanticsSmoke();
    executeAllowedSubclassChainSmoke();
    rejectDisallowedAndEmptySubclassListsSmoke();
    enforceClassListMethodOverrideSmoke();
    rejectMalformedClassPoliciesSmoke();
    std::cout << "class access policy smoke tests passed\n";
    return 0;
}
