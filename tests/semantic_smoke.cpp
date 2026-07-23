#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::SemanticResult analyze(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    return analyzer.analyze(*parseResult.root);
}

bool hasSymbol(const mparser::SemanticResult& result, mparser::SymbolKind kind,
               std::string_view name) {
    for (const auto& symbol : result.symbols) {
        if (symbol.kind == kind && symbol.name == name) {
            return true;
        }
    }
    return false;
}

bool containsHirKind(const mparser::HirNode& node, mparser::HirKind kind) {
    if (node.kind == kind) {
        return true;
    }

    for (const auto& child : node.children) {
        if (containsHirKind(*child, kind)) {
            return true;
        }
    }

    return false;
}

bool hasDiagnostic(const mparser::SemanticResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::HirNode* findNameRef(const mparser::HirNode& node,
                                    std::string_view name) {
    if (node.kind == mparser::HirKind::NameRef && node.label == name) {
        return &node;
    }

    for (const auto& child : node.children) {
        if (const auto* found = findNameRef(*child, name)) {
            return found;
        }
    }

    return nullptr;
}

const mparser::HirNode* findHirNode(const mparser::HirNode& node,
                                    mparser::HirKind kind,
                                    std::string_view label) {
    if (node.kind == kind && node.label == label) {
        return &node;
    }

    for (const auto& child : node.children) {
        if (const auto* found = findHirNode(*child, kind, label)) {
            return found;
        }
    }

    return nullptr;
}

const mparser::HirNode* findBoundNode(const mparser::HirNode& node,
                                      mparser::HirKind kind,
                                      mparser::BindingKind binding) {
    if (node.kind == kind && node.binding.kind == binding) {
        return &node;
    }

    for (const auto& child : node.children) {
        if (const auto* found = findBoundNode(*child, kind, binding)) {
            return found;
        }
    }

    return nullptr;
}

void analyzeFunctionSmoke() {
    const std::string source = R"(function y = g(A, obj)
for i = 1:10
    y = A(i) + obj.Value;
end
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(result.scopes.size() == 2);
    assert(hasSymbol(result, mparser::SymbolKind::Function, "g"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionOutput, "y"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionParameter, "A"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionParameter, "obj"));
    assert(hasSymbol(result, mparser::SymbolKind::Variable, "i"));

    assert(containsHirKind(*result.root, mparser::HirKind::ControlHeader));
    assert(containsHirKind(*result.root, mparser::HirKind::CallOrIndex));
    assert(containsHirKind(*result.root, mparser::HirKind::MemberAccess));

    const auto* nameA = findNameRef(*result.root, "A");
    assert(nameA != nullptr);
    assert(nameA->binding.kind == mparser::BindingKind::FunctionParameter);

    const auto* nameObj = findNameRef(*result.root, "obj");
    assert(nameObj != nullptr);
    assert(nameObj->binding.kind == mparser::BindingKind::FunctionParameter);
}

void analyzeForwardFunctionSmoke() {
    const std::string source = R"(function y = main(x)
y = helper(x);
end

function y = helper(x)
y = x;
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(hasSymbol(result, mparser::SymbolKind::Function, "main"));
    assert(hasSymbol(result, mparser::SymbolKind::Function, "helper"));

    const auto* helperName = findNameRef(*result.root, "helper");
    assert(helperName != nullptr);
    assert(helperName->binding.kind == mparser::BindingKind::Function);

    const auto* helperCall =
        findBoundNode(*result.root, mparser::HirKind::CallOrIndex,
                      mparser::BindingKind::Function);
    assert(helperCall != nullptr);
}

void analyzeFunctionHandleSmoke() {
    const std::string source = R"(function y = outer(x)
h = @(t) t + x;
y = h(1);
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(result.scopes.size() == 3);
    assert(hasSymbol(result, mparser::SymbolKind::FunctionParameter, "x"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionParameter, "t"));
    assert(containsHirKind(*result.root, mparser::HirKind::FunctionHandle));
    assert(containsHirKind(*result.root, mparser::HirKind::ParameterList));

    const auto* nameT = findNameRef(*result.root, "t");
    assert(nameT != nullptr);
    assert(nameT->binding.kind == mparser::BindingKind::FunctionParameter);

    const auto* nameX = findNameRef(*result.root, "x");
    assert(nameX != nullptr);
    assert(nameX->binding.kind == mparser::BindingKind::FunctionParameter);
}

void analyzeBuiltinSmoke() {
    const std::string source = R"(function y = use_builtin(x)
y = sin(x) + pi;
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(hasSymbol(result, mparser::SymbolKind::Builtin, "sin"));
    assert(hasSymbol(result, mparser::SymbolKind::Builtin, "pi"));

    const auto* sinName = findNameRef(*result.root, "sin");
    assert(sinName != nullptr);
    assert(sinName->binding.kind == mparser::BindingKind::Builtin);

    const auto* sinCall =
        findBoundNode(*result.root, mparser::HirKind::CallOrIndex,
                      mparser::BindingKind::Builtin);
    assert(sinCall != nullptr);

    const auto* piName = findNameRef(*result.root, "pi");
    assert(piName != nullptr);
    assert(piName->binding.kind == mparser::BindingKind::Builtin);
}

void analyzeBuiltinShadowSmoke() {
    const std::string source = R"(function y = shadow_builtin(x)
sin = x;
y = sin + pi;
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(hasSymbol(result, mparser::SymbolKind::Variable, "sin"));
    assert(hasSymbol(result, mparser::SymbolKind::Builtin, "pi"));

    const auto* sinName = findNameRef(*result.root, "sin");
    assert(sinName != nullptr);
    assert(sinName->binding.kind == mparser::BindingKind::LocalVariable);

    const auto* piName = findNameRef(*result.root, "pi");
    assert(piName != nullptr);
    assert(piName->binding.kind == mparser::BindingKind::Builtin);
}

void analyzeClassSmoke() {
    const std::string source = R"(classdef Demo
    properties
        Value double = 0
    end

    methods
        function obj = Demo(v)
            obj.Value = v;
        end

        y = compute(obj, x)
    end
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(result.scopes.size() == 3);
    assert(hasSymbol(result, mparser::SymbolKind::Class, "Demo"));
    assert(hasSymbol(result, mparser::SymbolKind::Property, "Value"));
    assert(hasSymbol(result, mparser::SymbolKind::Method, "Demo"));
    assert(hasSymbol(result, mparser::SymbolKind::Method, "compute"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionParameter, "v"));
    assert(hasSymbol(result, mparser::SymbolKind::FunctionOutput, "obj"));

    const auto* valueAccess =
        findHirNode(*result.root, mparser::HirKind::MemberAccess, "Value");
    assert(valueAccess != nullptr);
    assert(valueAccess->binding.kind == mparser::BindingKind::Property);

    const auto* objName = findNameRef(*result.root, "obj");
    assert(objName != nullptr);
    assert(objName->binding.kind == mparser::BindingKind::FunctionOutput);
}

void analyzeWorkspaceDeclarationSmoke() {
    const std::string source = R"(global shared
shared = 1;
function y = f(x)
persistent count
global shared
count = count + 1;
shared = shared + x;
y = count + shared;
end
)";

    auto result = analyze(source);
    assert(result.diagnostics.empty());
    assert(containsHirKind(*result.root,
                           mparser::HirKind::GlobalDeclaration));
    assert(containsHirKind(*result.root,
                           mparser::HirKind::PersistentDeclaration));
    assert(hasSymbol(result, mparser::SymbolKind::GlobalVariable,
                     "shared"));
    assert(hasSymbol(result, mparser::SymbolKind::PersistentVariable,
                     "count"));
    assert(findBoundNode(*result.root, mparser::HirKind::NameRef,
                         mparser::BindingKind::GlobalVariable) != nullptr);
    assert(findBoundNode(*result.root, mparser::HirKind::NameRef,
                         mparser::BindingKind::PersistentVariable) != nullptr);

    const auto localShadow = analyze(R"(global shared
function y = local_shadow()
shared = 4;
y = shared;
end
)");
    assert(localShadow.diagnostics.empty());
    assert(findBoundNode(*localShadow.root, mparser::HirKind::NameRef,
                         mparser::BindingKind::LocalVariable) != nullptr);

    const auto topLevelPersistent = analyze("persistent count\n");
    assert(hasDiagnostic(topLevelPersistent,
                         "only valid inside a function"));

    const auto parameterConflict = analyze(R"(function y = bad(x)
global x
y = x;
end
)");
    assert(hasDiagnostic(parameterConflict,
                         "conflicts with existing FunctionParameter"));

    const auto mixedDeclaration = analyze(R"(function y = bad()
global value
persistent value
y = 0;
end
)");
    assert(hasDiagnostic(mixedDeclaration,
                         "both global and persistent"));

    const auto lateDeclaration = analyze(R"(function y = bad()
y = value;
persistent value
end
)");
    assert(hasDiagnostic(lateDeclaration,
                         "must be declared before it is referenced"));
}

} // namespace

int main() {
    analyzeFunctionSmoke();
    analyzeForwardFunctionSmoke();
    analyzeFunctionHandleSmoke();
    analyzeBuiltinSmoke();
    analyzeBuiltinShadowSmoke();
    analyzeClassSmoke();
    analyzeWorkspaceDeclarationSmoke();
    std::cout << "semantic smoke tests passed\n";
    return 0;
}
