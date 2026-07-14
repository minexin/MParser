#include "mparser/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

bool logicalAttributeEnabled(
    const std::vector<AttributeSyntax>& attributes,
    std::string_view expectedName) {
    for (const auto& attribute : attributes) {
        if (attribute.name.size() != expectedName.size() ||
            !std::equal(attribute.name.begin(), attribute.name.end(),
                        expectedName.begin(), expectedName.end(),
                        [](char left, char right) {
                            return std::tolower(
                                       static_cast<unsigned char>(left)) ==
                                   std::tolower(
                                       static_cast<unsigned char>(right));
                        })) {
            continue;
        }
        if (attribute.negated) {
            return false;
        }
        if (attribute.value.empty()) {
            return true;
        }
        std::string value = trim(attribute.value);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value == "true";
    }
    return false;
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

std::string unqualifiedClassName(std::string_view className) {
    const size_t dot = className.find_last_of('.');
    return std::string(dot == std::string_view::npos
                           ? className
                           : className.substr(dot + 1));
}

std::optional<std::string> dottedReferenceName(const HirNode& node) {
    if (node.kind == HirKind::NameRef && !node.label.empty() &&
        (node.binding.kind == BindingKind::Unresolved ||
         node.binding.kind == BindingKind::Class ||
         node.binding.kind == BindingKind::Function)) {
        return node.label;
    }
    if (node.kind != HirKind::MemberAccess || node.children.empty() ||
        !isIdentifierText(node.label)) {
        return std::nullopt;
    }
    auto prefix = dottedReferenceName(*node.children.front());
    if (!prefix) {
        return std::nullopt;
    }
    return *prefix + "." + node.label;
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
    case SymbolKind::Event:
        return BindingKind::Event;
    case SymbolKind::EnumerationMember:
        return BindingKind::EnumerationMember;
    case SymbolKind::Class:
        return BindingKind::Class;
    case SymbolKind::Builtin:
        return BindingKind::Builtin;
    }
    return BindingKind::Unresolved;
}

bool isKnownBuiltinName(const std::string& name) {
    static constexpr const char* kBuiltinNames[] = {
        "abs",      "acos",     "addlistener", "all",      "any",
        "asin",     "assert",   "atan",        "cat",      "cell",
        "char",     "class",    "cos",         "delete",   "disp",
        "double",   "empty",    "enumeration", "eps",      "error",
        "events",   "exp",      "eye",         "false",    "fprintf",
        "horzcat",  "inf",      "ipermute",    "isa",      "isenum",
        "isempty",  "isfield",  "isvalid",     "length",   "linspace",
        "listener", "log",      "logical",     "max",      "mean",
        "min",      "nan",      "nargin",      "nargout",  "ndims",
        "notify",   "numel",    "ones",        "permute",  "pi",
        "plot",     "rand",     "randn",       "repmat",   "reshape",
        "single",   "sin",      "size",        "sqrt",     "squeeze",
        "strcmp",   "string",   "struct",      "sum",      "table",
        "tan",      "true",     "vertcat",     "zeros",
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

    std::string_view sourceName = functionNode.label;
    if (const size_t local = sourceName.find_last_of('>');
        local != std::string_view::npos) {
        sourceName.remove_prefix(local + 1);
    } else if (declaration.find(sourceName) == std::string::npos) {
        if (const size_t qualified = sourceName.find_last_of('.');
            qualified != std::string_view::npos) {
            sourceName.remove_prefix(qualified + 1);
        }
    }

    const auto namePosition = declaration.find(sourceName);
    if (namePosition == std::string::npos) {
        return signature;
    }

    const auto open =
        declaration.find('(', namePosition + sourceName.size());
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
    SemanticResult analyze(const SyntaxNode& root,
                           const std::vector<SourceUnit>& sources) {
        for (size_t sourceId = 0; sourceId < sources.size(); ++sourceId) {
            if (!sources[sourceId].classPrivateFunctionOwner.empty()) {
                lexicalClassBySource_[sourceId] =
                    sources[sourceId].classPrivateFunctionOwner;
            }
        }
        result_.root = makeNode(HirKind::Module, root);
        pushScope(ScopeKind::Module, "module");
        predeclareModuleSymbols(root);
        registerExternalFunctionBindings(sources);
        predeclareClassScopes(root);
        collectImportsForCurrentScope(root);
        lowerChildren(root, *result_.root);
        popScope();
        resolveDeferredBindings(*result_.root);
        validateImports();
        validateSuperclassCalls(*result_.root);
        return std::move(result_);
    }

private:
    struct NameResolution {
        BindingRef binding;
        std::string canonicalName;
    };

    struct ImportScope {
        std::unordered_map<std::string, std::string> explicitTargets;
        std::vector<std::string> wildcardTargets;
    };

    struct RegisteredImport {
        std::string target;
        SourceSpan span;
    };

    void registerSourceFunctionAlias(const SyntaxNode& function,
                                     int symbolId) {
        if (symbolId < 0 ||
            function.span.begin.sourceId == kInvalidSourceId) {
            return;
        }
        const size_t local = function.label.find_last_of('>');
        const size_t dot = function.label.find_last_of('.');
        const size_t separator = local != std::string::npos ? local : dot;
        if (separator == std::string::npos ||
            separator + 1 >= function.label.size()) {
            return;
        }
        const std::string alias = function.label.substr(separator + 1);
        auto& aliases =
            sourceFunctionAliases_[function.span.begin.sourceId];
        const auto [existing, inserted] = aliases.emplace(
            alias, BindingRef{BindingKind::Function, symbolId});
        if (!inserted && existing->second.symbolId != symbolId) {
            result_.diagnostics.push_back(Diagnostic{
                function.span,
                "duplicate source-local function: " + alias});
        }
    }

    void registerExternalFunctionBindings(
        const std::vector<SourceUnit>& sources) {
        std::unordered_set<std::string> classPrivateTargets;
        for (const auto& source : sources) {
            if (!source.classPrivateFunctionOwner.empty() &&
                !source.primaryFunctionIdentity.empty()) {
                classPrivateTargets.insert(source.primaryFunctionIdentity);
            }
        }

        for (size_t sourceId = 0; sourceId < sources.size(); ++sourceId) {
            for (const auto& binding : sources[sourceId].functionBindings) {
                SourceSpan span;
                span.begin.sourceId = sourceId;
                span.end.sourceId = sourceId;
                const BindingRef target = resolveFunction(binding.target);
                if (target.kind != BindingKind::Function) {
                    result_.diagnostics.push_back(Diagnostic{
                        span, "source function target is not available: " +
                                  binding.target});
                    continue;
                }

                auto& aliases = classPrivateTargets.contains(binding.target)
                                    ? classPrivateFunctionAliases_[sourceId]
                                    : externalFunctionAliases_[sourceId];
                const auto [existing, inserted] =
                    aliases.emplace(binding.alias, target);
                if (!inserted &&
                    existing->second.symbolId != target.symbolId) {
                    result_.diagnostics.push_back(Diagnostic{
                        span, "conflicting source function bindings for: " +
                                  binding.alias});
                }
            }
        }
    }

    void collectImportsForCurrentScope(const SyntaxNode& owner) {
        collectImportsForCurrentScopeChildren(owner);
    }

    void collectImportsForCurrentScopeChildren(const SyntaxNode& node) {
        for (const auto& child : node.children) {
            if (child->kind == SyntaxKind::ImportStatement) {
                for (const auto& item : child->children) {
                    registerImport(*item);
                }
                continue;
            }
            if (child->kind == SyntaxKind::FunctionDef ||
                child->kind == SyntaxKind::ClassDef) {
                continue;
            }
            collectImportsForCurrentScopeChildren(*child);
        }
    }

    void registerImport(const SyntaxNode& item) {
        if (item.kind != SyntaxKind::ImportItem || item.label.empty()) {
            return;
        }
        auto& imports = importsByScope_[currentScope().id];
        if (item.label.ends_with(".*")) {
            const std::string target =
                item.label.substr(0, item.label.size() - 2);
            if (std::find(imports.wildcardTargets.begin(),
                          imports.wildcardTargets.end(), target) ==
                imports.wildcardTargets.end()) {
                imports.wildcardTargets.push_back(target);
            }
            return;
        }

        const std::string alias = unqualifiedClassName(item.label);
        const auto [existing, inserted] =
            imports.explicitTargets.emplace(alias, item.label);
        if (!inserted && existing->second != item.label) {
            result_.diagnostics.push_back(Diagnostic{
                item.span, "conflicting explicit imports for: " + alias});
            return;
        }
        if (inserted) {
            registeredImports_.push_back(
                RegisteredImport{item.label, item.span});
        }
    }

    void predeclareModuleSymbols(const SyntaxNode& root) {
        for (const auto& child : root.children) {
            if (child->kind == SyntaxKind::FunctionDef) {
                const int symbolId = declareSymbol(
                    SymbolKind::Function, child->label, child->span);
                registerSourceFunctionAlias(*child, symbolId);
            } else if (child->kind == SyntaxKind::ClassDef) {
                declareSymbol(SymbolKind::Class, child->label, child->span,
                              child->label);
            }
        }
    }

    void predeclareClassMembers(const SyntaxNode& classNode) {
        std::unordered_set<std::string> occupiedNames;
        for (const auto& block : classNode.children) {
            if (block->kind == SyntaxKind::PropertiesBlock) {
                for (const auto& child : block->children) {
                    if (child->kind == SyntaxKind::PropertyDecl) {
                        occupiedNames.insert(child->label);
                        declareSymbol(SymbolKind::Property, child->label,
                                      child->span,
                                      child->property.className);
                    }
                }
                continue;
            }

            if (block->kind == SyntaxKind::MethodsBlock) {
                for (const auto& child : block->children) {
                    if (child->kind == SyntaxKind::FunctionDef ||
                        child->kind == SyntaxKind::MethodPrototype) {
                        occupiedNames.insert(child->label);
                        const std::string memberName =
                            child->label ==
                                    unqualifiedClassName(classNode.label)
                                ? classNode.label
                                : child->label;
                        const int symbolId = declareSymbol(
                            SymbolKind::Method, memberName, child->span);
                        if (symbolId >= 0 &&
                            logicalAttributeEnabled(child->attributes,
                                                    "Static")) {
                            staticMethodSymbols_.insert(symbolId);
                        }
                    }
                }
                continue;
            }

        }

        for (const auto& block : classNode.children) {
            if (block->kind != SyntaxKind::EventsBlock) {
                continue;
            }
            for (const auto& child : block->children) {
                if (child->kind != SyntaxKind::EventDecl ||
                    child->label.empty()) {
                    continue;
                }
                if (child->label ==
                    unqualifiedClassName(classNode.label)) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "event cannot have the class name: " +
                            classNode.label + "." + child->label});
                    continue;
                }
                if (!occupiedNames.insert(child->label).second) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "event conflicts with another class member: " +
                            classNode.label + "." + child->label});
                    continue;
                }
                declareSymbol(SymbolKind::Event, child->label,
                              child->span, classNode.label);
            }
        }

        const std::string shortClassName =
            unqualifiedClassName(classNode.label);
        for (const auto& block : classNode.children) {
            if (block->kind != SyntaxKind::EnumerationBlock) {
                continue;
            }
            for (const auto& child : block->children) {
                if (child->kind != SyntaxKind::EnumMember ||
                    child->label.empty()) {
                    continue;
                }
                if (child->label == shortClassName) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "enumeration member cannot have the class name: " +
                            classNode.label + "." + child->label});
                    continue;
                }
                if (!occupiedNames.insert(child->label).second) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "enumeration member conflicts with another class "
                        "member: " + classNode.label + "." + child->label});
                    continue;
                }
                declareSymbol(SymbolKind::EnumerationMember, child->label,
                              child->span, classNode.label);
            }
        }
    }

    void predeclareClassScopes(const SyntaxNode& root) {
        for (const auto& child : root.children) {
            if (child->kind != SyntaxKind::ClassDef) {
                continue;
            }
            const int classScope = pushScope(ScopeKind::Class, child->label);
            classScopeByName_[child->label] = classScope;
            predeclareClassMembers(*child);
            popScope();
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
        case SyntaxKind::ImportStatement:
            return lowerGeneric(syntax, HirKind::Import);
        case SyntaxKind::ImportItem:
            return lowerGeneric(syntax, HirKind::Import);
        case SyntaxKind::PropertyDecl:
            return lowerDeclaration(syntax, HirKind::Property, SymbolKind::Property);
        case SyntaxKind::EventDecl:
            return lowerEvent(syntax);
        case SyntaxKind::EnumMember:
            return lowerEnumerationMember(syntax);
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
        case SyntaxKind::SuperclassCallExpr:
            return lowerSuperclassCall(syntax);
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
        for (const auto& child : syntax.children) {
            if (child->kind != SyntaxKind::SuperclassList) {
                continue;
            }
            for (const auto& superclass : child->children) {
                if (!superclass->label.empty()) {
                    node->superclasses.push_back(superclass->label);
                }
            }
        }
        classSuperclassesByName_[syntax.label] = node->superclasses;
        int classScope = -1;
        if (const auto existing = classScopeByName_.find(syntax.label);
            existing != classScopeByName_.end()) {
            classScope = existing->second;
            scopeStack_.push_back(classScope);
        } else {
            classScope = pushScope(ScopeKind::Class, syntax.label);
            classScopeByName_[syntax.label] = classScope;
            predeclareClassMembers(syntax);
        }
        classStack_.push_back(syntax.label);
        for (const auto& child : syntax.children) {
            if (child->kind != SyntaxKind::SuperclassList) {
                node->children.push_back(lower(*child));
            }
        }
        classStack_.pop_back();
        popScope();
        return node;
    }

    std::unique_ptr<HirNode> lowerFunction(const SyntaxNode& syntax) {
        const bool method = currentScope().kind == ScopeKind::Class;
        const std::string className = method && !classStack_.empty()
                                          ? classStack_.back()
                                          : std::string{};
        const bool constructor =
            method && syntax.label == unqualifiedClassName(className);
        const bool staticMethod =
            method && logicalAttributeEnabled(syntax.attributes, "Static");
        const std::string functionName =
            constructor ? className : syntax.label;
        declareSymbol(method ? SymbolKind::Method : SymbolKind::Function,
                      functionName, syntax.span);

        auto node = makeNode(HirKind::Function, syntax);
        node->label = functionName;
        if (method) {
            node->lexicalClassName = className;
        } else if (syntax.span.begin.sourceId != kInvalidSourceId) {
            const auto lexicalClass =
                lexicalClassBySource_.find(syntax.span.begin.sourceId);
            if (lexicalClass != lexicalClassBySource_.end()) {
                node->lexicalClassName = lexicalClass->second;
            }
        }
        pushScope(ScopeKind::Function, functionName);
        collectImportsForCurrentScope(syntax);

        const FunctionSignature signature = parseFunctionSignature(syntax);
        for (const auto& output : signature.outputs) {
            const std::string typeName =
                constructor ? className : std::string{};
            declareSymbol(SymbolKind::FunctionOutput, output, syntax.span,
                          typeName);
        }
        for (size_t i = 0; i < signature.parameters.size(); ++i) {
            const std::string typeName =
                method && !constructor && !staticMethod && i == 0
                    ? className
                    : std::string{};
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

        functionStack_.push_back(FunctionContext{
            className, functionName,
            signature.outputs.empty() ? std::string{}
                                      : signature.outputs.front(),
            constructor});
        lowerChildren(syntax, *node);
        functionStack_.pop_back();
        popScope();
        return node;
    }

    std::unique_ptr<HirNode> lowerDeclaration(const SyntaxNode& syntax, HirKind kind,
                                              SymbolKind symbolKind) {
        declareSymbol(symbolKind, syntax.label, syntax.span,
                      kind == HirKind::Property
                          ? syntax.property.className
                          : std::string{});
        return lowerGeneric(syntax, kind);
    }

    std::unique_ptr<HirNode> lowerEnumerationMember(
        const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::EnumerationMember, syntax);
        if (!classStack_.empty()) {
            if (const auto member =
                    resolveClassMember(classStack_.back(), syntax.label);
                member &&
                member->kind == BindingKind::EnumerationMember) {
                node->binding = *member;
            }
        }
        lowerChildren(syntax, *node);
        return node;
    }

    std::unique_ptr<HirNode> lowerEvent(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::Event, syntax);
        if (!classStack_.empty()) {
            if (const auto event =
                    resolveClassMember(classStack_.back(), syntax.label);
                event && event->kind == BindingKind::Event) {
                node->binding = *event;
            }
        }
        return node;
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
        const auto resolution = resolveName(syntax.label, syntax.span);
        node->binding = resolution.binding;
        if (!resolution.canonicalName.empty()) {
            node->label = resolution.canonicalName;
        }
        return node;
    }

    std::unique_ptr<HirNode> lowerMemberAccess(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::MemberAccess, syntax);
        lowerChildren(syntax, *node);

        if (node->binding.kind == BindingKind::Unresolved) {
            if (const auto dottedName = dottedReferenceName(*node)) {
                if (const auto imported =
                        resolveWildcardImport(*dottedName, syntax.span)) {
                    node->kind = HirKind::NameRef;
                    node->label = imported->canonicalName;
                    node->raw = syntax.raw;
                    node->binding = imported->binding;
                    node->children.clear();
                    return node;
                }
            }
        }

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

    std::unique_ptr<HirNode> lowerSuperclassCall(const SyntaxNode& syntax) {
        auto node = makeNode(HirKind::SuperclassCall, syntax);
        lowerChildren(syntax, *node);
        if (node->children.empty() || functionStack_.empty()) {
            return node;
        }

        const auto& function = functionStack_.back();
        const std::string& calleeOrObject = node->children.front()->label;
        if (function.constructor &&
            calleeOrObject == function.constructorOutput) {
            node->binding = resolveClass(syntax.label);
            return node;
        }

        if (const auto method =
                resolveDeclaredClassMethod(syntax.label, calleeOrObject)) {
            node->binding = *method;
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
        if (!classStack_.empty()) {
            node->lexicalClassName = classStack_.back();
        } else if (syntax.span.begin.sourceId != kInvalidSourceId) {
            const auto lexicalClass =
                lexicalClassBySource_.find(syntax.span.begin.sourceId);
            if (lexicalClass != lexicalClassBySource_.end()) {
                node->lexicalClassName = lexicalClass->second;
            }
        }
        if (syntax.label != "@()") {
            const auto resolution = resolveName(syntax.label, syntax.span);
            node->binding = resolution.binding;
            if (!resolution.canonicalName.empty()) {
                node->label = resolution.canonicalName;
            }
            return node;
        }

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
        case SyntaxKind::SuperclassCallExpr:
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
        node->attributes = syntax.attributes;
        node->property = syntax.property;
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

    BindingRef resolveVariableLike(const std::string& name) const {
        for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
            const auto& scope = result_.scopes[static_cast<size_t>(*it)];
            for (int symbolId : scope.symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.name != name ||
                    (symbol.kind != SymbolKind::Variable &&
                     symbol.kind != SymbolKind::FunctionParameter &&
                     symbol.kind != SymbolKind::FunctionOutput)) {
                    continue;
                }
                return BindingRef{bindingKindForSymbol(symbol.kind),
                                  symbol.id};
            }
        }
        return BindingRef{};
    }

    BindingRef resolveNonVariableLocal(const std::string& name) const {
        for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
            const auto& scope = result_.scopes[static_cast<size_t>(*it)];
            for (int symbolId : scope.symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.name == name &&
                    symbol.kind != SymbolKind::Variable &&
                    symbol.kind != SymbolKind::FunctionParameter &&
                    symbol.kind != SymbolKind::FunctionOutput) {
                    return BindingRef{bindingKindForSymbol(symbol.kind),
                                      symbol.id};
                }
            }
        }
        return BindingRef{};
    }

    BindingRef resolveSourceLocalFunction(const std::string& name,
                                          size_t sourceId) const {
        for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
            const auto& scope = result_.scopes[static_cast<size_t>(*it)];
            for (int symbolId : scope.symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.kind == SymbolKind::Function &&
                    symbol.name == name &&
                    symbol.span.begin.sourceId == sourceId) {
                    return BindingRef{BindingKind::Function, symbol.id};
                }
            }
        }
        return BindingRef{};
    }

    BindingRef resolveFunction(const std::string& name) const {
        for (const auto& symbol : result_.symbols) {
            if (symbol.kind == SymbolKind::Function &&
                symbol.name == name) {
                return BindingRef{BindingKind::Function, symbol.id};
            }
        }
        return BindingRef{};
    }

    BindingRef resolveQualifiedImportTarget(
        const std::string& target) const {
        if (const auto function = resolveFunction(target);
            function.kind != BindingKind::Unresolved) {
            return function;
        }
        if (const auto klass = resolveClass(target);
            klass.kind != BindingKind::Unresolved) {
            return klass;
        }

        const size_t dot = target.find_last_of('.');
        if (dot == std::string::npos) {
            return BindingRef{};
        }
        const auto member =
            resolveClassMember(target.substr(0, dot), target.substr(dot + 1));
        if (member && member->kind == BindingKind::EnumerationMember) {
            return *member;
        }
        return member && member->kind == BindingKind::Method &&
                       staticMethodSymbols_.contains(member->symbolId)
                   ? *member
                   : BindingRef{};
    }

    const ImportScope* currentImports() const {
        if (scopeStack_.empty()) {
            return nullptr;
        }
        const auto imports = importsByScope_.find(scopeStack_.back());
        return imports == importsByScope_.end() ? nullptr
                                                : &imports->second;
    }

    std::optional<NameResolution> resolveWildcardImport(
        const std::string& name, SourceSpan span) {
        const auto* imports = currentImports();
        if (!imports) {
            return std::nullopt;
        }

        std::vector<NameResolution> candidates;
        for (const auto& wildcard : imports->wildcardTargets) {
            const std::string target = wildcard + "." + name;
            const BindingRef binding = resolveQualifiedImportTarget(target);
            if (binding.kind == BindingKind::Unresolved) {
                continue;
            }
            const bool duplicate = std::any_of(
                candidates.begin(), candidates.end(),
                [&target](const NameResolution& candidate) {
                    return candidate.canonicalName == target;
                });
            if (!duplicate) {
                candidates.push_back(NameResolution{binding, target});
            }
        }

        if (candidates.size() > 1) {
            result_.diagnostics.push_back(Diagnostic{
                span, "ambiguous wildcard import for: " + name});
            return std::nullopt;
        }
        return candidates.empty()
                   ? std::nullopt
                   : std::optional<NameResolution>(candidates.front());
    }

    NameResolution resolveName(const std::string& name, SourceSpan span) {
        if (const auto variable = resolveVariableLike(name);
            variable.kind != BindingKind::Unresolved) {
            return NameResolution{variable, {}};
        }

        if (const auto* imports = currentImports()) {
            if (const auto explicitImport =
                    imports->explicitTargets.find(name);
                explicitImport != imports->explicitTargets.end()) {
                return NameResolution{
                    resolveQualifiedImportTarget(explicitImport->second),
                    explicitImport->second};
            }
        }

        if (span.begin.sourceId != kInvalidSourceId) {
            if (const auto sourceAliases =
                    sourceFunctionAliases_.find(span.begin.sourceId);
                sourceAliases != sourceFunctionAliases_.end()) {
                if (const auto alias = sourceAliases->second.find(name);
                    alias != sourceAliases->second.end()) {
                    const auto& symbol = result_.symbols[
                        static_cast<size_t>(alias->second.symbolId)];
                    return NameResolution{alias->second, symbol.name};
                }
            }

            if (const auto local =
                    resolveSourceLocalFunction(name, span.begin.sourceId);
                local.kind != BindingKind::Unresolved) {
                return NameResolution{local, {}};
            }
        }

        if (const auto wildcard = resolveWildcardImport(name, span)) {
            return *wildcard;
        }

        if (span.begin.sourceId != kInvalidSourceId) {
            if (const auto sourceAliases =
                    classPrivateFunctionAliases_.find(span.begin.sourceId);
                sourceAliases != classPrivateFunctionAliases_.end()) {
                if (const auto alias = sourceAliases->second.find(name);
                    alias != sourceAliases->second.end()) {
                    const auto& symbol = result_.symbols[
                        static_cast<size_t>(alias->second.symbolId)];
                    return NameResolution{alias->second, symbol.name};
                }
            }
        }

        if (const auto local = resolveNonVariableLocal(name);
            local.kind != BindingKind::Unresolved) {
            return NameResolution{local, {}};
        }

        if (span.begin.sourceId != kInvalidSourceId) {
            if (const auto sourceAliases =
                    externalFunctionAliases_.find(span.begin.sourceId);
                sourceAliases != externalFunctionAliases_.end()) {
                if (const auto alias = sourceAliases->second.find(name);
                    alias != sourceAliases->second.end()) {
                    const auto& symbol = result_.symbols[
                        static_cast<size_t>(alias->second.symbolId)];
                    return NameResolution{alias->second, symbol.name};
                }
            }
        }

        return NameResolution{resolveBuiltin(name), {}};
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
        std::unordered_set<std::string> visiting;
        return resolveClassMember(className, memberName, visiting);
    }

    std::optional<BindingRef> resolveDeclaredClassMethod(
        const std::string& className, const std::string& methodName) const {
        const auto scope = classScopeByName_.find(className);
        if (scope == classScopeByName_.end()) {
            return std::nullopt;
        }
        const auto& classScope =
            result_.scopes[static_cast<size_t>(scope->second)];
        for (int symbolId : classScope.symbols) {
            const auto& symbol =
                result_.symbols[static_cast<size_t>(symbolId)];
            if (symbol.kind == SymbolKind::Method &&
                symbol.name == methodName) {
                return BindingRef{BindingKind::Method, symbol.id};
            }
        }
        return std::nullopt;
    }

    std::optional<BindingRef> resolveClassMember(
        const std::string& className, const std::string& memberName,
        std::unordered_set<std::string>& visiting) const {
        if (!visiting.insert(className).second) {
            return std::nullopt;
        }

        const auto scope = classScopeByName_.find(className);
        if (scope != classScopeByName_.end()) {
            const auto& classScope =
                result_.scopes[static_cast<size_t>(scope->second)];
            for (int symbolId : classScope.symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.name == memberName &&
                    (symbol.kind == SymbolKind::Property ||
                     symbol.kind == SymbolKind::Method ||
                     symbol.kind == SymbolKind::Event ||
                     symbol.kind == SymbolKind::EnumerationMember)) {
                    visiting.erase(className);
                    return BindingRef{bindingKindForSymbol(symbol.kind),
                                      symbol.id};
                }
            }
        }

        std::optional<BindingRef> resolved;
        if (const auto superclasses = classSuperclassesByName_.find(className);
            superclasses != classSuperclassesByName_.end()) {
            for (const auto& superclass : superclasses->second) {
                auto candidate =
                    resolveClassMember(superclass, memberName, visiting);
                if (!candidate) {
                    continue;
                }
                if (resolved && resolved->symbolId != candidate->symbolId) {
                    const std::string resolvedClass =
                        declaringClassForMember(*resolved);
                    const std::string candidateClass =
                        declaringClassForMember(*candidate);
                    if (classDerivesFrom(candidateClass, resolvedClass)) {
                        resolved = candidate;
                        continue;
                    }
                    if (classDerivesFrom(resolvedClass, candidateClass)) {
                        continue;
                    }
                    visiting.erase(className);
                    return std::nullopt;
                }
                resolved = candidate;
            }
        }

        visiting.erase(className);
        return resolved;
    }

    std::string declaringClassForMember(BindingRef binding) const {
        if (binding.symbolId < 0) {
            return {};
        }
        const auto& symbol =
            result_.symbols[static_cast<size_t>(binding.symbolId)];
        if (symbol.scopeId < 0) {
            return {};
        }
        const auto& scope =
            result_.scopes[static_cast<size_t>(symbol.scopeId)];
        return scope.kind == ScopeKind::Class ? scope.label : std::string{};
    }

    bool classDerivesFrom(const std::string& className,
                          const std::string& possibleSuperclass) const {
        if (className.empty() || possibleSuperclass.empty()) {
            return false;
        }
        std::unordered_set<std::string> visiting;
        return classDerivesFrom(className, possibleSuperclass, visiting);
    }

    bool classDerivesFrom(
        const std::string& className, const std::string& possibleSuperclass,
        std::unordered_set<std::string>& visiting) const {
        if (className == possibleSuperclass) {
            return true;
        }
        if (!visiting.insert(className).second) {
            return false;
        }
        if (const auto superclasses = classSuperclassesByName_.find(className);
            superclasses != classSuperclassesByName_.end()) {
            for (const auto& superclass : superclasses->second) {
                if (superclass == possibleSuperclass ||
                    classDerivesFrom(superclass, possibleSuperclass,
                                     visiting)) {
                    visiting.erase(className);
                    return true;
                }
            }
        }
        visiting.erase(className);
        return false;
    }

    void resolveDeferredBindings(HirNode& node) {
        for (auto& child : node.children) {
            resolveDeferredBindings(*child);
        }

        if (node.kind == HirKind::NameRef &&
            node.binding.kind == BindingKind::Unresolved &&
            !node.label.empty()) {
            const BindingRef binding =
                resolveQualifiedImportTarget(node.label);
            if (binding.kind != BindingKind::Unresolved) {
                node.binding = binding;
                return;
            }
        }

        if (node.kind == HirKind::MemberAccess &&
            node.binding.kind == BindingKind::Unresolved &&
            !node.children.empty()) {
            if (const auto qualifiedName = dottedReferenceName(node)) {
                const auto functionBinding = resolveFunction(*qualifiedName);
                if (functionBinding.kind == BindingKind::Function) {
                    node.kind = HirKind::NameRef;
                    node.label = *qualifiedName;
                    node.raw = *qualifiedName;
                    node.binding = functionBinding;
                    node.children.clear();
                    return;
                }

                const auto classBinding = resolveClass(*qualifiedName);
                if (classBinding.kind == BindingKind::Class) {
                    node.kind = HirKind::NameRef;
                    node.label = *qualifiedName;
                    node.raw = *qualifiedName;
                    node.binding = classBinding;
                    node.children.clear();
                    return;
                }
            }

            const std::string className =
                classNameForBinding(node.children.front()->binding);
            if (!className.empty()) {
                if (const auto member =
                        resolveClassMember(className, node.label)) {
                    node.binding = *member;
                }
            }
        }

        if (node.kind == HirKind::SuperclassCall &&
            node.binding.kind == BindingKind::Unresolved &&
            !node.children.empty()) {
            if (const auto method = resolveDeclaredClassMethod(
                    node.label, node.children.front()->label)) {
                node.binding = *method;
            }
        }

        if (node.kind != HirKind::CallOrIndex ||
            node.binding.kind != BindingKind::Unresolved ||
            node.children.empty()) {
            return;
        }

        const BindingKind calleeKind = node.children.front()->binding.kind;
        if (calleeKind == BindingKind::Function ||
            calleeKind == BindingKind::Method ||
            calleeKind == BindingKind::Class ||
            calleeKind == BindingKind::Builtin) {
            node.binding = node.children.front()->binding;
        }
    }

    void validateImports() {
        for (const auto& import : registeredImports_) {
            if (resolveQualifiedImportTarget(import.target).kind !=
                BindingKind::Unresolved) {
                continue;
            }

            const size_t dot = import.target.find_last_of('.');
            if (dot != std::string::npos) {
                const auto member = resolveClassMember(
                    import.target.substr(0, dot),
                    import.target.substr(dot + 1));
                if (member && member->kind == BindingKind::Method &&
                    !staticMethodSymbols_.contains(member->symbolId)) {
                    result_.diagnostics.push_back(Diagnostic{
                        import.span,
                        "imported class method is not static: " +
                            import.target});
                    continue;
                }
            }

            result_.diagnostics.push_back(Diagnostic{
                import.span,
                "import target is not available: " + import.target});
        }
    }

    std::string constructorOutputForClass(
        const std::string& className) const {
        const auto classScope = classScopeByName_.find(className);
        if (classScope == classScopeByName_.end()) {
            return {};
        }
        for (const auto& scope : result_.scopes) {
            if (scope.kind != ScopeKind::Function ||
                scope.parentId != classScope->second ||
                scope.label != className) {
                continue;
            }
            for (int symbolId : scope.symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.kind == SymbolKind::FunctionOutput) {
                    return symbol.name;
                }
            }
        }
        return {};
    }

    bool containsNameReference(const HirNode& node,
                               const std::string& name,
                               bool skipFirstChild = false) const {
        if (node.kind == HirKind::NameRef && node.label == name) {
            return true;
        }
        const size_t begin = skipFirstChild && !node.children.empty() ? 1 : 0;
        for (size_t index = begin; index < node.children.size(); ++index) {
            if (containsNameReference(*node.children[index], name)) {
                return true;
            }
        }
        return false;
    }

    bool isConstructorSuperclassCall(
        const HirNode& node, const std::string& outputName) const {
        return node.kind == HirKind::SuperclassCall &&
               !outputName.empty() && !node.children.empty() &&
               node.children.front()->kind == HirKind::NameRef &&
               node.children.front()->label == outputName;
    }

    void addSemanticDiagnostic(const HirNode& node, std::string message) {
        result_.diagnostics.push_back(
            Diagnostic{node.span, std::move(message)});
    }

    void validateConstructorCall(
        HirNode& node, const HirNode& classNode,
        const std::string& outputName, int controlDepth,
        bool objectReferenced, bool returnSeen,
        std::unordered_set<std::string>& calledSuperclasses) {
        if (node.children.empty() ||
            node.children.front()->label != outputName) {
            addSemanticDiagnostic(
                node, "superclass constructor must use the constructor "
                      "output object");
        }

        if (std::find(classNode.superclasses.begin(),
                      classNode.superclasses.end(), node.label) ==
            classNode.superclasses.end() || node.label == "handle") {
            addSemanticDiagnostic(
                node, "superclass constructor is not a direct executable "
                      "superclass: " +
                          node.label);
        }
        if (!calledSuperclasses.insert(node.label).second) {
            addSemanticDiagnostic(
                node, "superclass constructor called more than once: " +
                          node.label);
        }
        if (controlDepth > 0) {
            addSemanticDiagnostic(
                node, "superclass constructor call cannot be conditional");
        }
        if (objectReferenced || returnSeen) {
            addSemanticDiagnostic(
                node, "superclass constructor call must precede all other "
                      "references to the constructed object");
        }
        if (containsNameReference(node, outputName, true)) {
            addSemanticDiagnostic(
                node, "superclass constructor arguments cannot reference "
                      "the constructed object");
        }
        if (node.binding.kind != BindingKind::Class) {
            addSemanticDiagnostic(
                node, "superclass constructor target is not available: " +
                          node.label);
        }
    }

    void validateConstructorSequence(
        HirNode& node, const HirNode& classNode,
        const std::string& outputName, int controlDepth,
        bool& objectReferenced, bool& returnSeen,
        std::unordered_set<std::string>& calledSuperclasses) {
        if (node.kind == HirKind::Function) {
            return;
        }

        if (isConstructorSuperclassCall(node, outputName)) {
            validateConstructorCall(node, classNode, outputName, controlDepth,
                                    objectReferenced, returnSeen,
                                    calledSuperclasses);
            return;
        }

        if (node.kind == HirKind::Assignment && node.children.size() >= 2 &&
            isConstructorSuperclassCall(*node.children[1], outputName)) {
            validateConstructorSequence(
                *node.children[1], classNode, outputName, controlDepth,
                objectReferenced, returnSeen, calledSuperclasses);
            for (size_t index = 2; index < node.children.size(); ++index) {
                validateConstructorSequence(
                    *node.children[index], classNode, outputName,
                    controlDepth, objectReferenced, returnSeen,
                    calledSuperclasses);
            }
            return;
        }

        if (node.kind == HirKind::NameRef && node.label == outputName) {
            objectReferenced = true;
        }
        if (node.kind == HirKind::Statement && node.label == "return") {
            returnSeen = true;
        }

        const int childControlDepth =
            controlDepth + (node.kind == HirKind::Control ? 1 : 0);
        for (auto& child : node.children) {
            validateConstructorSequence(
                *child, classNode, outputName, childControlDepth,
                objectReferenced, returnSeen, calledSuperclasses);
        }
    }

    void validateQualifiedMethodCalls(HirNode& node,
                                      const HirNode* classNode,
                                      const HirNode* functionNode,
                                      const std::string& constructorOutput) {
        if (node.kind == HirKind::Function && functionNode &&
            &node != functionNode) {
            return;
        }
        if (node.kind == HirKind::SuperclassCall && classNode &&
            functionNode &&
            !isConstructorSuperclassCall(node, constructorOutput)) {
            const std::string methodName =
                node.children.empty() ? std::string{}
                                      : node.children.front()->label;
            if (methodName.empty() || methodName != functionNode->label) {
                addSemanticDiagnostic(
                    node, "qualified superclass method call must name the "
                          "current method");
            }
            if (node.label == classNode->label ||
                !classDerivesFrom(classNode->label, node.label)) {
                addSemanticDiagnostic(
                    node, "qualified method target is not a superclass of " +
                              classNode->label + ": " + node.label);
            }
            if (node.children.size() < 2) {
                addSemanticDiagnostic(
                    node, "qualified superclass method call requires an "
                          "object argument");
            }
            if (node.binding.kind != BindingKind::Method) {
                addSemanticDiagnostic(
                    node, "superclass method is not available: " + node.label +
                              "." + methodName);
            }
        } else if (node.kind == HirKind::SuperclassCall &&
                   (!classNode || !functionNode)) {
            addSemanticDiagnostic(
                node, "superclass-qualified call is only valid in a class "
                      "method");
        }

        for (auto& child : node.children) {
            validateQualifiedMethodCalls(*child, classNode, functionNode,
                                         constructorOutput);
        }
    }

    void validateClassFunction(HirNode& functionNode,
                               const HirNode& classNode) {
        const bool constructor = functionNode.label == classNode.label;
        const std::string outputName =
            constructor ? constructorOutputForClass(classNode.label)
                        : std::string{};
        validateQualifiedMethodCalls(functionNode, &classNode, &functionNode,
                                     outputName);
        if (!constructor) {
            return;
        }
        if (outputName.empty()) {
            addSemanticDiagnostic(
                functionNode,
                "class constructor must declare an object output");
            return;
        }

        bool objectReferenced = false;
        bool returnSeen = false;
        std::unordered_set<std::string> calledSuperclasses;
        for (auto& child : functionNode.children) {
            validateConstructorSequence(
                *child, classNode, outputName, 0, objectReferenced,
                returnSeen, calledSuperclasses);
        }
    }

    void validateClassMembers(HirNode& node, const HirNode& classNode) {
        if (node.kind == HirKind::Function) {
            validateClassFunction(node, classNode);
            return;
        }
        if (node.kind == HirKind::Class && &node != &classNode) {
            return;
        }
        for (auto& child : node.children) {
            validateClassMembers(*child, classNode);
        }
    }

    void validateSuperclassCalls(HirNode& node) {
        if (node.kind == HirKind::Class) {
            validateClassMembers(node, node);
            return;
        }
        if (node.kind == HirKind::Function) {
            validateQualifiedMethodCalls(node, nullptr, &node, {});
            return;
        }
        for (auto& child : node.children) {
            validateSuperclassCalls(*child);
        }
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
    struct FunctionContext {
        std::string className;
        std::string name;
        std::string constructorOutput;
        bool constructor = false;
    };
    std::vector<int> scopeStack_;
    std::vector<std::string> classStack_;
    std::vector<FunctionContext> functionStack_;
    std::unordered_map<std::string, int> classScopeByName_;
    std::unordered_map<std::string, std::vector<std::string>>
        classSuperclassesByName_;
    std::unordered_map<std::string, int> builtinSymbols_;
    std::unordered_set<int> staticMethodSymbols_;
    std::unordered_map<int, ImportScope> importsByScope_;
    std::unordered_map<
        size_t, std::unordered_map<std::string, BindingRef>>
        sourceFunctionAliases_;
    std::unordered_map<
        size_t, std::unordered_map<std::string, BindingRef>>
        classPrivateFunctionAliases_;
    std::unordered_map<
        size_t, std::unordered_map<std::string, BindingRef>>
        externalFunctionAliases_;
    std::unordered_map<size_t, std::string> lexicalClassBySource_;
    std::vector<RegisteredImport> registeredImports_;
};

} // namespace

SemanticResult SemanticAnalyzer::analyze(
    const SyntaxNode& root, const std::vector<SourceUnit>& sources) {
    AnalyzerContext context;
    return context.analyze(root, sources);
}

const char* hirKindName(HirKind kind) {
    switch (kind) {
    case HirKind::Module:
        return "Module";
    case HirKind::Class:
        return "Class";
    case HirKind::Function:
        return "Function";
    case HirKind::Import:
        return "Import";
    case HirKind::Property:
        return "Property";
    case HirKind::Event:
        return "Event";
    case HirKind::EnumerationMember:
        return "EnumerationMember";
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
    case HirKind::SuperclassCall:
        return "SuperclassCall";
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
    case BindingKind::Event:
        return "Event";
    case BindingKind::EnumerationMember:
        return "EnumerationMember";
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
    case SymbolKind::Event:
        return "Event";
    case SymbolKind::EnumerationMember:
        return "EnumerationMember";
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
