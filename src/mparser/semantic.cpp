#include "mparser/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace mparser {
namespace {

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\v\f");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> splitCommaList(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (char c : text) {
        if (c == '(') {
            ++parenDepth;
        } else if (c == ')' && parenDepth > 0) {
            --parenDepth;
        } else if (c == '[') {
            ++bracketDepth;
        } else if (c == ']' && bracketDepth > 0) {
            --bracketDepth;
        } else if (c == '{') {
            ++braceDepth;
        } else if (c == '}' && braceDepth > 0) {
            --braceDepth;
        }

        if (c == ',' && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    parts.push_back(trim(current));
    return parts;
}

bool isIdentifierText(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(text.front());
    if (std::isalpha(first) == 0 && text.front() != '_') {
        return false;
    }

    return std::all_of(text.begin() + 1, text.end(), [](char c) {
        const unsigned char value = static_cast<unsigned char>(c);
        return std::isalnum(value) != 0 || c == '_';
    });
}

BindingKind bindingKindForSymbol(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Variable:
        return BindingKind::LocalVariable;
    case SymbolKind::FunctionParameter:
        return BindingKind::FunctionParameter;
    case SymbolKind::FunctionOutput:
        return BindingKind::FunctionOutput;
    case SymbolKind::Function:
        return BindingKind::Function;
    case SymbolKind::Method:
        return BindingKind::Method;
    case SymbolKind::Property:
        return BindingKind::Property;
    case SymbolKind::Class:
        return BindingKind::Class;
    case SymbolKind::Builtin:
        return BindingKind::Builtin;
    }
    return BindingKind::Unresolved;
}

bool isKnownBuiltinName(const std::string& name) {
    static constexpr const char* kBuiltinNames[] = {
        "abs",      "acos",     "all",      "any",     "asin",
        "assert",   "atan",     "cell",     "char",    "class",
        "cos",      "disp",     "double",   "empty",   "eps",
        "error",    "exp",      "eye",      "false",   "fprintf",
        "inf",      "isa",      "isempty",  "isfield", "length",
        "linspace", "log",      "logical",  "max",     "mean",
        "min",      "nan",      "numel",    "ones",    "pi",
        "plot",     "rand",     "randn",    "single",  "sin",
        "size",     "sqrt",     "strcmp",   "string",  "struct",
        "sum",      "table",    "tan",      "true",    "zeros",
        "nargin",   "nargout",
    };

    for (const char* builtin : kBuiltinNames) {
        if (name == builtin) {
            return true;
        }
    }

    return false;
}

struct FunctionSignature {
    std::vector<std::string> outputs;
    std::vector<std::string> parameters;
    bool hasVarargout = false;
    bool hasVarargin = false;
};

FunctionSignature parseFunctionSignature(const SyntaxNode& functionNode) {
    FunctionSignature signature;
    std::string text = trim(functionNode.raw);

    const auto equal = text.find('=');
    std::string declaration = text;
    if (equal != std::string::npos) {
        std::string outputs = trim(text.substr(0, equal));
        declaration = trim(text.substr(equal + 1));

        if (outputs.size() >= 2 && outputs.front() == '[' &&
            outputs.back() == ']') {
            outputs = outputs.substr(1, outputs.size() - 2);
        }

        for (const auto& part : splitCommaList(outputs)) {
            if (part == "varargout") {
                signature.hasVarargout = true;
            } else if (isIdentifierText(part)) {
                signature.outputs.push_back(part);
            }
        }
    }

    const auto namePosition = declaration.find(functionNode.label);
    if (namePosition == std::string::npos) {
        return signature;
    }

    const auto open = declaration.find('(', namePosition + functionNode.label.size());
    if (open == std::string::npos) {
        return signature;
    }

    const auto close = declaration.find_last_of(')');
    if (close == std::string::npos || close <= open) {
        return signature;
    }

    const std::string parameters =
        declaration.substr(open + 1, close - open - 1);
    for (const auto& part : splitCommaList(parameters)) {
        if (part == "varargin") {
            signature.hasVarargin = true;
        } else if (isIdentifierText(part) && part != "~") {
            signature.parameters.push_back(part);
        }
    }

    return signature;
}

class AnalyzerContext {
public:
    SemanticResult analyze(const SyntaxNode& root) {
        result_.root = makeNode(HirKind::Module, root);
        pushScope(ScopeKind::Module, "module");
        predeclareModuleSymbols(root);
        lowerChildren(root, *result_.root);
        popScope();
        return std::move(result_);
    }

private:
    void predeclareModuleSymbols(const SyntaxNode& root) {
        for (const auto& child : root.children) {
            if (child->kind == SyntaxKind::FunctionDef) {
                declareSymbol(SymbolKind::Function, child->label, child->span);
            } else if (child->kind == SyntaxKind::ClassDef) {
                declareSymbol(SymbolKind::Class, child->label, child->span,
                              child->label);
            }
        }
    }

    void predeclareClassMembers(const SyntaxNode& classNode) {
        for (const auto& block : classNode.children) {
            if (block->kind == SyntaxKind::PropertiesBlock) {
                for (const auto& child : block->children) {
                    if (child->kind == SyntaxKind::PropertyDecl) {
                        declareSymbol(SymbolKind::Property, child->label,
                                      child->span);
                    }
                }
                continue;
            }

            if (block->kind == SyntaxKind::MethodsBlock) {
                for (const auto& child : block->children) {
                    if (child->kind == SyntaxKind::FunctionDef ||
                        child->kind == SyntaxKind::MethodPrototype) {
                        declareSymbol(SymbolKind::Method, child->label,
                                      child->span);
                    }
                }
            }
        }
    }

    std::unique_ptr<HirNode> lower(const SyntaxNode& syntax) {
        switch (syntax.kind) {
        case SyntaxKind::CompilationUnit:
            return lowerTransparent(syntax, HirKind::Module);
        case SyntaxKind::ClassDef:
            return lowerClass(syntax);
        case SyntaxKind::FunctionDef:
            return lowerFunction(syntax);
        case SyntaxKind::PropertyDecl:
            return lowerDeclaration(syntax, HirKind::Property, SymbolKind::Property);
        case SyntaxKind::MethodPrototype:
            return lowerDeclaration(syntax, HirKind::MethodPrototype,
                                    SymbolKind::Method);
        case SyntaxKind::ForBlock:
        case SyntaxKind::IfBlock:
        case SyntaxKind::ParforBlock:
        case SyntaxKind::WhileBlock:
        case SyntaxKind::SwitchBlock:
        case SyntaxKind::TryBlock:
        case SyntaxKind::SpmdBlock:
            return lowerGeneric(syntax, HirKind::Control);
        case SyntaxKind::ControlHeader:
            return lowerGeneric(syntax, HirKind::ControlHeader);
        case SyntaxKind::ControlArm:
            return lowerGeneric(syntax, HirKind::ControlArm);
        case SyntaxKind::AssignmentStatement:
            return lowerAssignment(syntax);
        case SyntaxKind::ExpressionStatement:
        case SyntaxKind::Statement:
        case SyntaxKind::ArgumentsBlock:
        case SyntaxKind::PropertiesBlock:
        case SyntaxKind::MethodsBlock:
        case SyntaxKind::EventsBlock:
        case SyntaxKind::EnumerationBlock:
        case SyntaxKind::EventDecl:
        case SyntaxKind::EnumMember:
        case SyntaxKind::SuperclassList:
        case SyntaxKind::Superclass:
            return lowerGeneric(syntax, HirKind::Statement);
        case SyntaxKind::OutputList:
            return lowerGeneric(syntax, HirKind::OutputList);
        case SyntaxKind::ParameterList:
            return lowerGeneric(syntax, HirKind::ParameterList);
        case SyntaxKind::IdentifierExpr:
            return lowerNameRef(syntax);
        case SyntaxKind::NumberLiteralExpr:
        case SyntaxKind::StringLiteralExpr:
        case SyntaxKind::EndExpr:
        case SyntaxKind::ColonExpr:
        case SyntaxKind::IgnoredOutputExpr:
            return lowerGeneric(syntax, HirKind::Literal);
        case SyntaxKind::UnaryExpr:
            return lowerGeneric(syntax, HirKind::Unary);
        case SyntaxKind::BinaryExpr:
            return lowerGeneric(syntax, HirKind::Binary);
        case SyntaxKind::PostfixExpr:
            return lowerGeneric(syntax, HirKind::Postfix);
        case SyntaxKind::ParenthesizedExpr:
            return lowerTransparent(syntax, HirKind::Statement);
        case SyntaxKind::MatrixExpr:
            return lowerGeneric(syntax, HirKind::Matrix);
        case SyntaxKind::MatrixRow:
            return lowerGeneric(syntax, HirKind::MatrixRow);
        case SyntaxKind::CellExpr:
            return lowerGeneric(syntax, HirKind::Cell);
        case SyntaxKind::MemberAccessExpr:
            return lowerMemberAccess(syntax);
        case SyntaxKind::CallOrIndexExpr:
            return lowerCallOrIndex(syntax);
        case SyntaxKind::BraceIndexExpr:
            return lowerGeneric(syntax, HirKind::BraceIndex);
        case SyntaxKind::FunctionHandleExpr:
            return lowerFunctionHandle(syntax);
        case SyntaxKind::MetaClassExpr:
            return lowerMetaClass(syntax);
        case SyntaxKind::Error:
            return lowerGeneric(syntax, HirKind::Unknown);
        }

        return lowerGeneric(syntax, HirKind::Unknown);
    }

    std::unique_ptr<HirNode> lowerClass(const SyntaxNode& syntax) {
        declareSymbol(SymbolKind::Class, syntax.label, syntax.span, syntax.label);
        auto node = makeNode(HirKind::Class, syntax);
        const int classScope = pushScope(ScopeKind::Class, syntax.label);
        classScopeByName_[syntax.label] = classScope;
        classStack_.push_back(syntax.label);
        predeclareClassMembers(syntax);
        lowerChildren(syntax, *node);
        classStack_.pop_back();
        popScope();
        return node;
    }

    std::unique_ptr<HirNode> lowerFunction(const SyntaxNode& syntax) {
        const bool method = currentScope().kind == ScopeKind::Class;
        const std::string className = method && !classStack_.empty()
                                          ? classStack_.back()
                                          : std::string{};
        declareSymbol(method ? SymbolKind::Method : SymbolKind::Function,
                      syntax.label, syntax.span);

        auto node = makeNode(HirKind::Function, syntax);
        pushScope(ScopeKind::Function, syntax.label);

        const FunctionSignature signature = parseFunctionSignature(syntax);
        for (const auto& output : signature.outputs) {
            const std::string typeName =
                method && syntax.label == className ? className : std::string{};
            declareSymbol(SymbolKind::FunctionOutput, output, syntax.span,
                          typeName);
        }
        for (size_t i = 0; i < signature.parameters.size(); ++i) {
            const std::string typeName =
                method && i == 0 ? className : std::string{};
            declareSymbol(SymbolKind::FunctionParameter, signature.parameters[i],
                          syntax.span, typeName);
        }
        if (signature.hasVarargin) {
            declareSymbol(SymbolKind::FunctionParameter, "varargin",
                          syntax.span);
        }
        if (signature.hasVarargout) {
            declareSymbol(SymbolKind::FunctionOutput, "varargout",
                          syntax.span);
        }

        lowerChildren(syntax, *node);
        popScope();
        return node;
    }

    std::unique_ptr<HirNode> lowerDeclaration(const SyntaxNode& syntax, HirKind kind,
                                              SymbolKind symbolKind) {
        declareSymbol(symbolKind, syntax.label, syntax.span);
        return lowerGeneric(syntax, kind);
    }

    std::unique_ptr<HirNode> lowerAssignment(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::Assignment, syntax);
        if (!syntax.children.empty()) {
            declareAssignmentTargets(*syntax.children.front());
        }
        lowerChildren(syntax, *node);
        return node;
    }

    std::unique_ptr<HirNode> lowerNameRef(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::NameRef, syntax);
        node->binding = resolveName(syntax.label);
        return node;
    }

    std::unique_ptr<HirNode> lowerMemberAccess(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::MemberAccess, syntax);
        lowerChildren(syntax, *node);

        if (!node->children.empty()) {
            const std::string className =
                classNameForBinding(node->children.front()->binding);
            if (!className.empty()) {
                if (const auto member =
                        resolveClassMember(className, syntax.label)) {
                    node->binding = *member;
                }
            }
        }

        return node;
    }

    std::unique_ptr<HirNode> lowerCallOrIndex(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::CallOrIndex, syntax);
        lowerChildren(syntax, *node);

        if (!node->children.empty()) {
            const BindingKind calleeKind = node->children.front()->binding.kind;
            if (calleeKind == BindingKind::Function ||
                calleeKind == BindingKind::Method ||
                calleeKind == BindingKind::Class ||
                calleeKind == BindingKind::Builtin) {
                node->binding = node->children.front()->binding;
            }
        }

        return node;
    }

    std::unique_ptr<HirNode> lowerMetaClass(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::MetaClass, syntax);
        node->binding = resolveClass(syntax.label);
        lowerChildren(syntax, *node);
        return node;
    }

    std::unique_ptr<HirNode> lowerFunctionHandle(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::FunctionHandle, syntax);
        pushScope(ScopeKind::Function, "<anonymous>");

        if (!syntax.children.empty() &&
            syntax.children.front()->kind == SyntaxKind::ParameterList) {
            for (const auto& parameter : splitCommaList(syntax.children.front()->raw)) {
                if (isIdentifierText(parameter) && parameter != "~") {
                    declareSymbol(SymbolKind::FunctionParameter, parameter,
                                  syntax.children.front()->span);
                }
            }
        }

        lowerChildren(syntax, *node);
        popScope();
        return node;
    }

    std::unique_ptr<HirNode> lowerGeneric(const SyntaxNode& syntax, HirKind kind) {
        auto node = makeNode(kind, syntax);
        lowerChildren(syntax, *node);
        return node;
    }

    std::unique_ptr<HirNode> lowerTransparent(const SyntaxNode& syntax,
                                              HirKind kind) {
        auto node = makeNode(kind, syntax);
        lowerChildren(syntax, *node);
        return node;
    }

    void lowerChildren(const SyntaxNode& syntax, HirNode& node) {
        for (const auto& child : syntax.children) {
            node.children.push_back(lower(*child));
        }
    }

    void declareAssignmentTargets(const SyntaxNode& syntax) {
        switch (syntax.kind) {
        case SyntaxKind::IdentifierExpr:
            declareVariableIfUnresolved(syntax.label, syntax.span);
            break;
        case SyntaxKind::OutputList:
            for (const auto& child : syntax.children) {
                declareAssignmentTargets(*child);
            }
            break;
        case SyntaxKind::CallOrIndexExpr:
        case SyntaxKind::BraceIndexExpr:
        case SyntaxKind::MemberAccessExpr:
            if (!syntax.children.empty()) {
                declareAssignmentTargets(*syntax.children.front());
            }
            break;
        default:
            break;
        }
    }

    std::unique_ptr<HirNode> makeNode(HirKind kind, const SyntaxNode& syntax) {
        auto node = std::make_unique<HirNode>(kind);
        node->label = syntax.label;
        node->raw = syntax.raw;
        node->span = syntax.span;
        return node;
    }

    int pushScope(ScopeKind kind, std::string label) {
        const int id = static_cast<int>(result_.scopes.size());
        const int parent = scopeStack_.empty() ? -1 : scopeStack_.back();
        result_.scopes.push_back(
            SemanticScope{id, parent, kind, std::move(label), {}});
        scopeStack_.push_back(id);
        return id;
    }

    void popScope() {
        scopeStack_.pop_back();
    }

    SemanticScope& currentScope() {
        return result_.scopes[static_cast<size_t>(scopeStack_.back())];
    }

    int declareSymbol(SymbolKind kind, const std::string& name, SourceSpan span,
                      std::string typeName = {}) {
        if (name.empty()) {
            return -1;
        }

        for (int symbolId : currentScope().symbols) {
            auto& symbol = result_.symbols[static_cast<size_t>(symbolId)];
            if (symbol.kind == kind && symbol.name == name) {
                if (symbol.typeName.empty() && !typeName.empty()) {
                    symbol.typeName = std::move(typeName);
                }
                return symbol.id;
            }
        }

        const int id = static_cast<int>(result_.symbols.size());
        result_.symbols.push_back(
            SemanticSymbol{id, kind, name, std::move(typeName), currentScope().id,
                           span});
        currentScope().symbols.push_back(id);
        return id;
    }

    void declareVariableIfUnresolved(const std::string& name, SourceSpan span) {
        if (resolveLocal(name).kind == BindingKind::Unresolved) {
            declareSymbol(SymbolKind::Variable, name, span);
        }
    }

    BindingRef resolveLocal(const std::string& name) const {
        for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
            const auto& scope = result_.scopes[static_cast<size_t>(*it)];
            for (int symbolId : scope.symbols) {
                const auto& symbol = result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.name == name) {
                    return BindingRef{bindingKindForSymbol(symbol.kind), symbol.id};
                }
            }
        }

        return BindingRef{};
    }

    BindingRef resolveName(const std::string& name) {
        const BindingRef local = resolveLocal(name);
        if (local.kind != BindingKind::Unresolved) {
            return local;
        }

        return resolveBuiltin(name);
    }

    BindingRef resolveBuiltin(const std::string& name) {
        if (!isKnownBuiltinName(name)) {
            return BindingRef{};
        }

        if (const auto existing = builtinSymbols_.find(name);
            existing != builtinSymbols_.end()) {
            return BindingRef{BindingKind::Builtin, existing->second};
        }

        const int id = static_cast<int>(result_.symbols.size());
        result_.symbols.push_back(
            SemanticSymbol{id, SymbolKind::Builtin, name, {}, -1, {}});
        builtinSymbols_[name] = id;
        return BindingRef{BindingKind::Builtin, id};
    }

    BindingRef resolveClass(const std::string& name) const {
        for (const auto& symbol : result_.symbols) {
            if (symbol.kind == SymbolKind::Class && symbol.name == name) {
                return BindingRef{BindingKind::Class, symbol.id};
            }
        }

        return BindingRef{};
    }

    std::optional<BindingRef> resolveClassMember(const std::string& className,
                                                 const std::string& memberName) const {
        const auto scope = classScopeByName_.find(className);
        if (scope == classScopeByName_.end()) {
            return std::nullopt;
        }

        const auto& classScope =
            result_.scopes[static_cast<size_t>(scope->second)];
        for (int symbolId : classScope.symbols) {
            const auto& symbol = result_.symbols[static_cast<size_t>(symbolId)];
            if (symbol.name == memberName &&
                (symbol.kind == SymbolKind::Property ||
                 symbol.kind == SymbolKind::Method)) {
                return BindingRef{bindingKindForSymbol(symbol.kind), symbol.id};
            }
        }

        return std::nullopt;
    }

    std::string classNameForBinding(BindingRef binding) const {
        if (binding.symbolId < 0) {
            return {};
        }

        const auto& symbol =
            result_.symbols[static_cast<size_t>(binding.symbolId)];
        if (symbol.kind == SymbolKind::Class) {
            return symbol.name;
        }
        return symbol.typeName;
    }

    SemanticResult result_;
    std::vector<int> scopeStack_;
    std::vector<std::string> classStack_;
    std::unordered_map<std::string, int> classScopeByName_;
    std::unordered_map<std::string, int> builtinSymbols_;
};

} // namespace

SemanticResult SemanticAnalyzer::analyze(const SyntaxNode& root) {
    AnalyzerContext context;
    return context.analyze(root);
}

const char* hirKindName(HirKind kind) {
    switch (kind) {
    case HirKind::Module:
        return "Module";
    case HirKind::Class:
        return "Class";
    case HirKind::Function:
        return "Function";
    case HirKind::Property:
        return "Property";
    case HirKind::MethodPrototype:
        return "MethodPrototype";
    case HirKind::Control:
        return "Control";
    case HirKind::ControlHeader:
        return "ControlHeader";
    case HirKind::ControlArm:
        return "ControlArm";
    case HirKind::Statement:
        return "Statement";
    case HirKind::Assignment:
        return "Assignment";
    case HirKind::OutputList:
        return "OutputList";
    case HirKind::ParameterList:
        return "ParameterList";
    case HirKind::NameRef:
        return "NameRef";
    case HirKind::Literal:
        return "Literal";
    case HirKind::Unary:
        return "Unary";
    case HirKind::Binary:
        return "Binary";
    case HirKind::Postfix:
        return "Postfix";
    case HirKind::Matrix:
        return "Matrix";
    case HirKind::MatrixRow:
        return "MatrixRow";
    case HirKind::Cell:
        return "Cell";
    case HirKind::MemberAccess:
        return "MemberAccess";
    case HirKind::CallOrIndex:
        return "CallOrIndex";
    case HirKind::BraceIndex:
        return "BraceIndex";
    case HirKind::FunctionHandle:
        return "FunctionHandle";
    case HirKind::MetaClass:
        return "MetaClass";
    case HirKind::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* bindingKindName(BindingKind kind) {
    switch (kind) {
    case BindingKind::Unresolved:
        return "Unresolved";
    case BindingKind::LocalVariable:
        return "LocalVariable";
    case BindingKind::FunctionParameter:
        return "FunctionParameter";
    case BindingKind::FunctionOutput:
        return "FunctionOutput";
    case BindingKind::Function:
        return "Function";
    case BindingKind::Method:
        return "Method";
    case BindingKind::Property:
        return "Property";
    case BindingKind::Class:
        return "Class";
    case BindingKind::Builtin:
        return "Builtin";
    }
    return "Unresolved";
}

const char* symbolKindName(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Variable:
        return "Variable";
    case SymbolKind::FunctionParameter:
        return "FunctionParameter";
    case SymbolKind::FunctionOutput:
        return "FunctionOutput";
    case SymbolKind::Function:
        return "Function";
    case SymbolKind::Method:
        return "Method";
    case SymbolKind::Property:
        return "Property";
    case SymbolKind::Class:
        return "Class";
    case SymbolKind::Builtin:
        return "Builtin";
    }
    return "Variable";
}

const char* scopeKindName(ScopeKind kind) {
    switch (kind) {
    case ScopeKind::Module:
        return "Module";
    case ScopeKind::Class:
        return "Class";
    case ScopeKind::Function:
        return "Function";
    }
    return "Module";
}

} // namespace mparser
