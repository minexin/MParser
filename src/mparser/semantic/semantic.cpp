#include "mparser/semantic/semantic.h"

#include "mparser/semantic/argument_contract.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/semantic/function_signature.h"

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
    case SymbolKind::GlobalVariable:
        return BindingKind::GlobalVariable;
    case SymbolKind::PersistentVariable:
        return BindingKind::PersistentVariable;
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

bool isInputArgumentBlock(ArgumentBlockKind kind) {
    return kind == ArgumentBlockKind::Input ||
           kind == ArgumentBlockKind::RepeatingInput;
}

bool isOutputArgumentBlock(ArgumentBlockKind kind) {
    return kind == ArgumentBlockKind::Output ||
           kind == ArgumentBlockKind::RepeatingOutput;
}

std::string argumentNameRoot(std::string_view name) {
    const size_t dot = name.find('.');
    return std::string(name.substr(0, dot));
}

bool isNameValueArgument(std::string_view name) {
    const size_t dot = name.find('.');
    return dot != std::string_view::npos && dot != 0 && dot + 1 < name.size();
}

std::string nameValueField(std::string_view name) {
    const size_t dot = name.find('.');
    return dot == std::string_view::npos ? std::string{}
                                         : std::string(name.substr(dot + 1));
}

std::optional<std::string> dottedSyntaxReferenceName(
    const SyntaxNode& node) {
    if (node.kind == SyntaxKind::IdentifierExpr && !node.label.empty()) {
        return node.label;
    }
    if (node.kind != SyntaxKind::MemberAccessExpr || node.children.empty() ||
        !isIdentifierText(node.label)) {
        return std::nullopt;
    }
    auto prefix = dottedSyntaxReferenceName(*node.children.front());
    if (!prefix) {
        return std::nullopt;
    }
    return *prefix + "." + node.label;
}

bool containsNameValueReference(
    const SyntaxNode& node,
    const std::unordered_set<std::string>& nameValueRoots,
    std::string& reference) {
    if (const auto dotted = dottedSyntaxReferenceName(node);
        dotted && nameValueRoots.contains(argumentNameRoot(*dotted))) {
        reference = *dotted;
        return true;
    }
    for (const auto& child : node.children) {
        if (containsNameValueReference(*child, nameValueRoots, reference)) {
            return true;
        }
    }
    return false;
}

bool textReferencesNameValueRoot(
    std::string_view text,
    const std::unordered_set<std::string>& nameValueRoots,
    std::string& reference) {
    for (const auto& root : nameValueRoots) {
        if (text == root ||
            (text.size() > root.size() && text.starts_with(root) &&
             text[root.size()] == '.')) {
            reference = std::string(text);
            return true;
        }
    }
    return false;
}

bool containsForbiddenArgumentFunction(const SyntaxNode& node,
                                       std::string& name) {
    static const std::unordered_set<std::string> forbidden = {
        "assignin",  "builtin",   "clear",      "dbstack",
        "eval",      "evalc",     "evalin",     "exist",
        "feval",     "input",     "inputname",  "load",
        "nargin",    "narginchk", "nargoutchk", "save",
        "who",       "whos",
    };
    if (node.kind == SyntaxKind::IdentifierExpr &&
        forbidden.contains(node.label)) {
        name = node.label;
        return true;
    }
    for (const auto& child : node.children) {
        if (containsForbiddenArgumentFunction(*child, name)) {
            return true;
        }
    }
    return false;
}

class AnalyzerContext {
public:
    explicit AnalyzerContext(
        std::shared_ptr<const BuiltinRegistry> builtinRegistry,
        std::vector<std::string> externalFunctionNames,
        SemanticAnalysisOptions options)
        : builtinRegistry_(builtinRegistry
                               ? std::move(builtinRegistry)
                               : defaultBuiltinRegistry()),
          externalFunctionNames_(std::move(externalFunctionNames)),
          options_(options) {
        result_.builtinRegistry = builtinRegistry_;
    }

    SemanticResult analyze(const SyntaxNode& root,
                           const std::vector<SourceUnit>& sources) {
        result_.sources.reserve(sources.size());
        for (size_t sourceId = 0; sourceId < sources.size(); ++sourceId) {
            result_.sources.push_back(
                SemanticSourceInfo{sources[sourceId].name,
                                   sources[sourceId].namespaceName,
                                   sources[sourceId].functionBindings});
            if (!sources[sourceId].classPrivateFunctionOwner.empty()) {
                lexicalClassBySource_[sourceId] =
                    sources[sourceId].classPrivateFunctionOwner;
            }
        }
        result_.root = makeNode(HirKind::Module, root);
        pushScope(ScopeKind::Module, "module");
        predeclareModuleSymbols(root);
        predeclareWorkspaceDeclarations(
            root, options_.allowTopLevelPersistentDeclarations);
        predeclareExternalFunctions();
        registerExternalFunctionBindings(sources);
        predeclareClassScopes(root);
        collectImportsForCurrentScope(root);
        lowerChildren(root, *result_.root);
        popScope();
        const ArgumentContractCatalog argumentCatalog =
            buildArgumentContractCatalog(*result_.root);
        const auto argumentDiagnostics =
            validateClassPropertyArgumentContracts(*result_.root,
                                                   argumentCatalog);
        result_.diagnostics.insert(result_.diagnostics.end(),
                                   argumentDiagnostics.begin(),
                                   argumentDiagnostics.end());
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

    void predeclareExternalFunctions() {
        for (const auto& name : externalFunctionNames_) {
            if (!name.empty() &&
                resolveLocal(name).kind == BindingKind::Unresolved) {
                declareSymbol(SymbolKind::Function, name, SourceSpan{});
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

    void collectWorkspaceDeclarations(
        const SyntaxNode& owner,
        std::vector<const SyntaxNode*>& declarations) const {
        for (const auto& child : owner.children) {
            if (child->kind == SyntaxKind::FunctionDef ||
                child->kind == SyntaxKind::ClassDef) {
                continue;
            }
            if (child->kind == SyntaxKind::GlobalStatement ||
                child->kind == SyntaxKind::PersistentStatement) {
                declarations.push_back(child.get());
                continue;
            }
            collectWorkspaceDeclarations(*child, declarations);
        }
    }

    bool referencesNameBefore(
        const SyntaxNode& node, const std::string& name,
        const SourcePosition& declaration) const {
        if (node.kind == SyntaxKind::FunctionDef ||
            node.kind == SyntaxKind::ClassDef ||
            node.kind == SyntaxKind::GlobalStatement ||
            node.kind == SyntaxKind::PersistentStatement) {
            return false;
        }
        if (node.kind == SyntaxKind::IdentifierExpr &&
            node.label == name &&
            node.span.begin.sourceId == declaration.sourceId &&
            node.span.begin.offset < declaration.offset) {
            return true;
        }
        for (const auto& child : node.children) {
            if (referencesNameBefore(*child, name, declaration)) {
                return true;
            }
        }
        return false;
    }

    void predeclareWorkspaceDeclarations(
        const SyntaxNode& owner, bool persistentAllowed) {
        std::vector<const SyntaxNode*> declarations;
        collectWorkspaceDeclarations(owner, declarations);

        struct DeclarationInfo {
            SymbolKind kind = SymbolKind::GlobalVariable;
            const SyntaxNode* declaration = nullptr;
            SourceSpan nameSpan;
        };
        std::vector<std::pair<std::string, DeclarationInfo>> declared;
        std::unordered_map<std::string, size_t> declaredByName;
        for (const SyntaxNode* declaration : declarations) {
            const bool persistent =
                declaration->kind == SyntaxKind::PersistentStatement;
            if (persistent && !persistentAllowed) {
                result_.diagnostics.push_back(Diagnostic{
                    declaration->span,
                    "persistent declaration is only valid inside a function"});
                continue;
            }
            const SymbolKind kind =
                persistent ? SymbolKind::PersistentVariable
                           : SymbolKind::GlobalVariable;
            for (const auto& name : declaration->children) {
                const auto [existing, inserted] =
                    declaredByName.try_emplace(
                        name->label, declared.size());
                if (inserted) {
                    declared.emplace_back(
                        name->label,
                        DeclarationInfo{
                            kind, declaration, name->span});
                    continue;
                }
                if (declared[existing->second].second.kind != kind) {
                    result_.diagnostics.push_back(Diagnostic{
                        name->span,
                        "variable cannot be both global and persistent: " +
                            name->label});
                }
            }
        }

        for (const auto& [name, declaration] : declared) {
            bool incompatible = false;
            for (int symbolId : currentScope().symbols) {
                const auto& symbol =
                    result_.symbols[static_cast<size_t>(symbolId)];
                if (symbol.name != name) {
                    continue;
                }
                if (symbol.kind == declaration.kind) {
                    incompatible = true;
                    break;
                }
                if (symbol.kind == SymbolKind::Variable ||
                    symbol.kind == SymbolKind::FunctionParameter ||
                    symbol.kind == SymbolKind::FunctionOutput ||
                    symbol.kind == SymbolKind::GlobalVariable ||
                    symbol.kind == SymbolKind::PersistentVariable) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration.nameSpan,
                        "workspace declaration conflicts with existing " +
                            std::string(symbolKindName(symbol.kind)) +
                            ": " + name});
                    incompatible = true;
                    break;
                }
            }
            if (incompatible) {
                continue;
            }
            bool referencedBefore = false;
            if (declaration.declaration) {
                for (const auto& child : owner.children) {
                    if (referencesNameBefore(
                            *child, name,
                            declaration.declaration->span.begin)) {
                        referencedBefore = true;
                        break;
                    }
                }
            }
            if (referencedBefore) {
                result_.diagnostics.push_back(Diagnostic{
                    declaration.nameSpan,
                    "workspace variable must be declared before it is "
                    "referenced: " +
                        name});
            }
            declareSymbol(declaration.kind, name,
                          declaration.nameSpan);
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
        case SyntaxKind::ArgumentsBlock:
            return lowerGeneric(syntax, HirKind::ArgumentBlock);
        case SyntaxKind::ArgumentDecl:
            return lowerGeneric(syntax, HirKind::Argument);
        case SyntaxKind::ImportStatement:
            return lowerGeneric(syntax, HirKind::Import);
        case SyntaxKind::ImportItem:
            return lowerGeneric(syntax, HirKind::Import);
        case SyntaxKind::GlobalStatement:
            return lowerWorkspaceDeclaration(
                syntax, HirKind::GlobalDeclaration);
        case SyntaxKind::PersistentStatement:
            return lowerWorkspaceDeclaration(
                syntax, HirKind::PersistentDeclaration);
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
        case SyntaxKind::CellRow:
            return lowerGeneric(syntax, HirKind::CellRow);
        case SyntaxKind::MemberAccessExpr:
            return lowerMemberAccess(syntax);
        case SyntaxKind::NameValueArgumentExpr:
            return lowerGeneric(syntax, HirKind::NameValueArgument);
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
        const bool nestedFunction = currentScope().kind == ScopeKind::Function;
        const std::string lexicalFunctionName =
            nestedFunction && !functionStack_.empty()
                ? functionStack_.back().qualifiedName
                : std::string{};
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
        node->lexicalFunctionName = lexicalFunctionName;
        if (method) {
            node->lexicalClassName = className;
        } else if (nestedFunction && !functionStack_.empty()) {
            node->lexicalClassName = functionStack_.back().className;
        } else if (syntax.span.begin.sourceId != kInvalidSourceId) {
            const auto lexicalClass =
                lexicalClassBySource_.find(syntax.span.begin.sourceId);
            if (lexicalClass != lexicalClassBySource_.end()) {
                node->lexicalClassName = lexicalClass->second;
            }
        }
        const int functionScope =
            pushScope(ScopeKind::Function, functionName);
        node->semanticScopeId = functionScope;
        collectImportsForCurrentScope(syntax);

        const FunctionSignature signature =
            parseFunctionSignature(syntax.raw, syntax.label);
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

        predeclareWorkspaceDeclarations(syntax, true);
        validateArgumentBlocks(syntax, signature);
        if (nestedFunction &&
            std::any_of(syntax.children.begin(), syntax.children.end(),
                        [](const std::unique_ptr<SyntaxNode>& child) {
                            return child->kind == SyntaxKind::ArgumentsBlock;
                        })) {
            result_.diagnostics.push_back(Diagnostic{
                syntax.span,
                "argument validation is not allowed in nested functions"});
        }

        const std::string qualifiedName =
            !lexicalFunctionName.empty()
                ? lexicalFunctionName + ">" + functionName
                : method && !className.empty()
                      ? className + "." + functionName
                      : functionName;
        functionStack_.push_back(FunctionContext{
            className, functionName, qualifiedName, functionScope,
            signature.outputs.empty() ? std::string{}
                                      : signature.outputs.front(),
            constructor});
        lowerChildren(syntax, *node);
        functionStack_.pop_back();
        popScope();
        return node;
    }

    void validateArgumentBlocks(const SyntaxNode& function,
                                const FunctionSignature& signature) {
        std::unordered_set<std::string> declaredInputs;
        std::unordered_set<std::string> declaredOutputs;
        std::unordered_set<std::string> repeatingNames;
        std::unordered_set<std::string> nameValueRoots;
        std::unordered_map<std::string, const SyntaxNode*> nameValueFields;
        std::vector<const SyntaxNode*> nameValueDeclarations;
        std::unordered_map<std::string, const SyntaxNode*> positionalContracts;
        std::vector<std::string> repeatingOrder;
        const SyntaxNode* repeatingBlock = nullptr;
        const SyntaxNode* repeatingOutputBlock = nullptr;
        std::string repeatingOutputName;
        bool executableSeen = false;
        bool inputBlockSeen = false;
        bool outputBlockSeen = false;
        bool repeatingInputSeen = false;
        bool repeatingOutputSeen = false;
        bool nameValueSeen = false;
        bool vararginDeclared = false;
        bool varargoutDeclared = false;

        const auto isParameter = [&](std::string_view name) {
            return std::find(signature.parameters.begin(),
                             signature.parameters.end(), name) !=
                   signature.parameters.end();
        };
        const auto isOutput = [&](std::string_view name) {
            return std::find(signature.outputs.begin(), signature.outputs.end(),
                             name) != signature.outputs.end();
        };

        for (const auto& child : function.children) {
            if (child->kind != SyntaxKind::ArgumentsBlock ||
                child->argumentBlock.kind != ArgumentBlockKind::Input) {
                continue;
            }
            for (const auto& declaration : child->children) {
                if (declaration->kind != SyntaxKind::ArgumentDecl) {
                    continue;
                }
                if (!declaration->nameValueSourceClass.empty()) {
                    nameValueRoots.insert(declaration->label);
                    continue;
                }
                if (!isNameValueArgument(declaration->label)) {
                    continue;
                }
                nameValueRoots.insert(argumentNameRoot(declaration->label));
                nameValueDeclarations.push_back(declaration.get());
                const std::string field =
                    nameValueField(declaration->label);
                const auto [existing, inserted] =
                    nameValueFields.emplace(field, declaration.get());
                if (!inserted &&
                    existing->second->label != declaration->label) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "name-value argument field must be globally unique: " +
                            field});
                }
            }
        }
        for (const SyntaxNode* declaration : nameValueDeclarations) {
            const std::string field = nameValueField(declaration->label);
            const auto collision = std::find_if(
                signature.parameters.begin(), signature.parameters.end(),
                [&](const std::string& parameter) {
                    return parameter == field &&
                           !nameValueRoots.contains(parameter);
                });
            if (collision != signature.parameters.end()) {
                result_.diagnostics.push_back(Diagnostic{
                    declaration->span,
                    "name-value argument conflicts with a positional or repeating parameter: " +
                        field});
            }
        }

        for (const auto& child : function.children) {
            if (child->kind != SyntaxKind::ArgumentsBlock) {
                executableSeen = true;
                continue;
            }

            const ArgumentBlockKind blockKind = child->argumentBlock.kind;
            const bool inputBlock = isInputArgumentBlock(blockKind);
            const bool outputBlock = isOutputArgumentBlock(blockKind);
            inputBlockSeen = inputBlockSeen || inputBlock;
            if (executableSeen) {
                result_.diagnostics.push_back(Diagnostic{
                    child->span,
                    "arguments blocks must precede executable function code"});
            }
            if (child->children.empty()) {
                result_.diagnostics.push_back(Diagnostic{
                    child->span, "arguments block must declare an argument"});
            }
            if (inputBlock && outputBlockSeen) {
                result_.diagnostics.push_back(Diagnostic{
                    child->span,
                    "input arguments block cannot follow an output block"});
            }
            if (outputBlock) {
                outputBlockSeen = true;
            }
            if (blockKind == ArgumentBlockKind::Output &&
                repeatingOutputSeen) {
                result_.diagnostics.push_back(Diagnostic{
                    child->span,
                    "fixed output block cannot follow a repeating output block"});
            }
            if (blockKind == ArgumentBlockKind::RepeatingInput) {
                if (repeatingInputSeen) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "function can contain only one repeating input block"});
                }
                if (nameValueSeen) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "repeating input block must precede name-value arguments"});
                }
                repeatingInputSeen = true;
                repeatingBlock = child.get();
            }
            if (blockKind == ArgumentBlockKind::RepeatingOutput) {
                if (repeatingOutputSeen) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "function can contain only one repeating output block"});
                }
                repeatingOutputSeen = true;
                repeatingOutputBlock = child.get();
                if (child->children.size() != 1) {
                    result_.diagnostics.push_back(Diagnostic{
                        child->span,
                        "repeating output block must declare exactly one argument"});
                }
            }

            bool nameValueSeenInBlock = false;
            for (const auto& declaration : child->children) {
                if (declaration->kind != SyntaxKind::ArgumentDecl) {
                    continue;
                }

                const bool input = isInputArgumentBlock(blockKind);
                auto& declared = input ? declaredInputs : declaredOutputs;
                if (!declared.insert(declaration->label).second) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "duplicate arguments block declaration: " +
                            declaration->label});
                }

                if (declaration->property.hasExplicitDefault &&
                    !declaration->children.empty()) {
                    std::string forbidden;
                    if (containsForbiddenArgumentFunction(
                            *declaration->children.front(), forbidden)) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "function is not allowed in an arguments block: " +
                                forbidden});
                    }
                    std::string reference;
                    if (containsNameValueReference(
                            *declaration->children.front(), nameValueRoots,
                            reference)) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "arguments block default cannot reference a name-value structure: " +
                                reference});
                    }
                }
                for (const auto& validator :
                     declaration->property.validators) {
                    for (const auto& argument : validator.arguments) {
                        std::string reference;
                        if (textReferencesNameValueRoot(
                                argument, nameValueRoots, reference)) {
                            result_.diagnostics.push_back(Diagnostic{
                                validator.span,
                                "arguments block validator cannot reference a name-value structure: " +
                                    reference});
                            break;
                        }
                    }
                }

                if (blockKind == ArgumentBlockKind::Input) {
                    if (!declaration->nameValueSourceClass.empty()) {
                        const std::string& root = declaration->label;
                        if (!isParameter(root)) {
                            result_.diagnostics.push_back(Diagnostic{
                                declaration->span,
                                "name-value structure is not a function parameter: " +
                                    root});
                        }
                        nameValueRoots.insert(root);
                        nameValueSeen = true;
                        nameValueSeenInBlock = true;
                        continue;
                    }
                    if (isNameValueArgument(declaration->label)) {
                        const std::string root =
                            argumentNameRoot(declaration->label);
                        if (!isParameter(root)) {
                            result_.diagnostics.push_back(Diagnostic{
                                declaration->span,
                                "name-value structure is not a function parameter: " +
                                    root});
                        }
                        nameValueRoots.insert(root);
                        nameValueSeen = true;
                        nameValueSeenInBlock = true;
                        continue;
                    }
                    if (nameValueSeenInBlock || nameValueSeen) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "positional argument cannot follow name-value arguments: " +
                                declaration->label});
                    }
                    if (repeatingInputSeen) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "positional argument cannot follow repeating arguments: " +
                                declaration->label});
                    }
                    if (declaration->label == "varargin") {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "varargin must be declared in a Repeating arguments block"});
                    } else if (!isParameter(declaration->label)) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "arguments block name is not a function parameter: " +
                                declaration->label});
                    }
                    positionalContracts[declaration->label] = declaration.get();
                    continue;
                }

                if (blockKind == ArgumentBlockKind::RepeatingInput) {
                    if (!declaration->nameValueSourceClass.empty()) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "class-property name-value source cannot appear in a Repeating block: " +
                                declaration->label});
                        continue;
                    }
                    if (isNameValueArgument(declaration->label)) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "name-value argument cannot appear in a Repeating block: " +
                                declaration->label});
                    }
                    if (declaration->property.hasExplicitDefault) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "repeating input argument cannot define a default: " +
                                declaration->label});
                    }
                    if (declaration->label == "varargin") {
                        vararginDeclared = true;
                        if (!signature.hasVarargin) {
                            result_.diagnostics.push_back(Diagnostic{
                                declaration->span,
                                "arguments block name is not a function parameter: varargin"});
                        }
                        if (child->children.size() != 1) {
                            result_.diagnostics.push_back(Diagnostic{
                                declaration->span,
                                "varargin must be the only declaration in its Repeating block"});
                        }
                    } else {
                        if (!isParameter(declaration->label)) {
                            result_.diagnostics.push_back(Diagnostic{
                                declaration->span,
                                "arguments block name is not a function parameter: " +
                                    declaration->label});
                        }
                        repeatingNames.insert(declaration->label);
                        repeatingOrder.push_back(declaration->label);
                    }
                    continue;
                }

                if (!declaration->nameValueSourceClass.empty()) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "output arguments cannot use a class-property name-value source: " +
                            declaration->label});
                } else if (isNameValueArgument(declaration->label)) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "output arguments cannot use name-value field syntax: " +
                            declaration->label});
                }
                if (declaration->property.hasExplicitDefault) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "output argument cannot define a default: " +
                            declaration->label});
                }
                if (blockKind == ArgumentBlockKind::RepeatingOutput &&
                    repeatingOutputName.empty()) {
                    repeatingOutputName = declaration->label;
                }
                if (declaration->label != "varargout") {
                    const auto current =
                        std::find(signature.outputs.begin(),
                                  signature.outputs.end(),
                                  declaration->label);
                    if (current != signature.outputs.end()) {
                        for (const auto& validator :
                             declaration->property.validators) {
                            for (const auto& argument :
                                 validator.arguments) {
                                const auto earlier = std::find(
                                    signature.outputs.begin(), current,
                                    argument);
                                if (earlier == current) {
                                    continue;
                                }
                                result_.diagnostics.push_back(Diagnostic{
                                    validator.span,
                                    "output argument validator cannot reference an earlier output: " +
                                        argument});
                                break;
                            }
                        }
                    }
                }
                if (declaration->label == "varargout") {
                    varargoutDeclared = true;
                    if (!signature.hasVarargout) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "arguments block name is not a function output: varargout"});
                    }
                    if (blockKind == ArgumentBlockKind::RepeatingOutput &&
                        child->children.size() != 1) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "varargout must be the only declaration in its Repeating block"});
                    }
                    if (blockKind != ArgumentBlockKind::RepeatingOutput) {
                        result_.diagnostics.push_back(Diagnostic{
                            declaration->span,
                            "varargout must be declared in a Repeating output arguments block"});
                    }
                } else if (!isOutput(declaration->label)) {
                    result_.diagnostics.push_back(Diagnostic{
                        declaration->span,
                        "arguments block name is not a function output: " +
                            declaration->label});
                }
            }
        }

        if (signature.hasVarargin && inputBlockSeen &&
            !vararginDeclared) {
            result_.diagnostics.push_back(Diagnostic{
                repeatingBlock != nullptr ? repeatingBlock->span : function.span,
                "varargin must be declared as the only argument in its Repeating block"});
        }
        if (signature.hasVarargout && outputBlockSeen &&
            !varargoutDeclared) {
            result_.diagnostics.push_back(Diagnostic{
                function.span,
                "varargout must be declared in a Repeating output arguments block"});
        }
        if (!repeatingOutputName.empty() &&
            repeatingOutputName != "varargout" &&
            (signature.outputs.empty() ||
             signature.outputs.back() != repeatingOutputName)) {
            result_.diagnostics.push_back(Diagnostic{
                repeatingOutputBlock != nullptr ? repeatingOutputBlock->span
                                                : function.span,
                "repeating output argument must be last in the function signature: " +
                    repeatingOutputName});
        }

        int parameterPhase = 0;
        std::vector<std::string> signatureRepeatingOrder;
        for (const auto& parameter : signature.parameters) {
            int phase = 0;
            if (repeatingNames.contains(parameter)) {
                phase = 1;
                signatureRepeatingOrder.push_back(parameter);
            } else if (nameValueRoots.contains(parameter)) {
                phase = 2;
            }
            if (phase < parameterPhase) {
                result_.diagnostics.push_back(Diagnostic{
                    function.span,
                    "function parameters must be ordered as positional, repeating, then name-value: " +
                        parameter});
            }
            parameterPhase = std::max(parameterPhase, phase);
        }
        if (repeatingOrder != signatureRepeatingOrder) {
            result_.diagnostics.push_back(Diagnostic{
                repeatingBlock != nullptr ? repeatingBlock->span : function.span,
                "repeating arguments block order must match the function signature"});
        }

        bool optionalParameterSeen = false;
        for (const auto& parameter : signature.parameters) {
            if (repeatingNames.contains(parameter) ||
                nameValueRoots.contains(parameter)) {
                continue;
            }
            const auto declaration = positionalContracts.find(parameter);
            const bool optional =
                declaration != positionalContracts.end() &&
                declaration->second->property.hasExplicitDefault;
            if (optional) {
                optionalParameterSeen = true;
            } else if (optionalParameterSeen) {
                result_.diagnostics.push_back(Diagnostic{
                    declaration != positionalContracts.end()
                        ? declaration->second->span
                        : function.span,
                    "required argument follows an optional argument: " +
                        parameter});
            }
        }
    }

    std::unique_ptr<HirNode> lowerDeclaration(const SyntaxNode& syntax, HirKind kind,
                                              SymbolKind symbolKind) {
        declareSymbol(symbolKind, syntax.label, syntax.span,
                      kind == HirKind::Property
                          ? syntax.property.className
                          : std::string{});
        return lowerGeneric(syntax, kind);
    }

    std::unique_ptr<HirNode> lowerWorkspaceDeclaration(
        const SyntaxNode& syntax, HirKind kind) {
        auto node = makeNode(kind, syntax);
        lowerChildren(syntax, *node);
        return node;
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
        node->argumentBlock = syntax.argumentBlock;
        node->nameValueSourceClass = syntax.nameValueSourceClass;
        node->nameValueSourceSpan = syntax.nameValueSourceSpan;
        node->property = syntax.property;
        node->capturesExpressionResult = syntax.capturesExpressionResult;
        node->outputSuppressed = syntax.outputSuppressed;
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
                    if ((symbol.kind == SymbolKind::GlobalVariable ||
                         symbol.kind == SymbolKind::PersistentVariable) &&
                        scope.id != scopeStack_.back()) {
                        continue;
                    }
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
                     symbol.kind != SymbolKind::FunctionOutput &&
                     symbol.kind != SymbolKind::GlobalVariable &&
                     symbol.kind != SymbolKind::PersistentVariable)) {
                    continue;
                }
                if ((symbol.kind == SymbolKind::GlobalVariable ||
                     symbol.kind == SymbolKind::PersistentVariable) &&
                    scope.id != scopeStack_.back()) {
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
                    symbol.kind != SymbolKind::FunctionOutput &&
                    symbol.kind != SymbolKind::GlobalVariable &&
                    symbol.kind != SymbolKind::PersistentVariable) {
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
        if (!builtinRegistry_->contains(name)) {
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
                const auto builtinBinding =
                    resolveBuiltin(*qualifiedName);
                if (builtinBinding.kind == BindingKind::Builtin) {
                    node.kind = HirKind::NameRef;
                    node.label = *qualifiedName;
                    node.raw = *qualifiedName;
                    node.binding = builtinBinding;
                    node.children.clear();
                    return;
                }

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
        std::string qualifiedName;
        int scopeId = -1;
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
    std::shared_ptr<const BuiltinRegistry> builtinRegistry_;
    std::vector<std::string> externalFunctionNames_;
    SemanticAnalysisOptions options_;
};

bool isAnonymousCaptureBinding(BindingKind kind) {
    switch (kind) {
    case BindingKind::Unresolved:
    case BindingKind::LocalVariable:
    case BindingKind::FunctionParameter:
    case BindingKind::FunctionOutput:
    case BindingKind::GlobalVariable:
    case BindingKind::PersistentVariable:
        return true;
    case BindingKind::Function:
    case BindingKind::Method:
    case BindingKind::Property:
    case BindingKind::Event:
    case BindingKind::EnumerationMember:
    case BindingKind::Class:
    case BindingKind::Builtin:
        return false;
    }
    return false;
}

class AnonymousCaptureCollector {
public:
    std::vector<std::string> collect(const HirNode& functionHandle) {
        if (functionHandle.kind != HirKind::FunctionHandle ||
            functionHandle.label != "@()" ||
            functionHandle.children.size() < 2) {
            return {};
        }

        pushParameters(*functionHandle.children.front());
        collectNode(*functionHandle.children[1]);
        popParameters(*functionHandle.children.front());
        return std::move(captures_);
    }

private:
    void pushParameters(const HirNode& parameterList) {
        for (const auto& parameter : splitCommaList(parameterList.raw)) {
            if (!parameter.empty() && parameter != "~") {
                ++boundNames_[parameter];
            }
        }
    }

    void popParameters(const HirNode& parameterList) {
        for (const auto& parameter : splitCommaList(parameterList.raw)) {
            const auto found = boundNames_.find(parameter);
            if (found == boundNames_.end()) {
                continue;
            }
            if (found->second == 1) {
                boundNames_.erase(found);
            } else {
                --found->second;
            }
        }
    }

    void collectNode(const HirNode& node) {
        if (node.kind == HirKind::NameRef &&
            isAnonymousCaptureBinding(node.binding.kind) &&
            !boundNames_.contains(node.label) &&
            capturedNames_.insert(node.label).second) {
            captures_.push_back(node.label);
        }

        if (node.kind == HirKind::FunctionHandle && node.label == "@()") {
            if (node.children.size() < 2) {
                return;
            }
            pushParameters(*node.children.front());
            collectNode(*node.children[1]);
            popParameters(*node.children.front());
            return;
        }

        for (const auto& child : node.children) {
            collectNode(*child);
        }
    }

    std::unordered_map<std::string, size_t> boundNames_;
    std::unordered_set<std::string> capturedNames_;
    std::vector<std::string> captures_;
};

bool isNestedCaptureBinding(BindingKind kind) {
    return kind == BindingKind::LocalVariable ||
           kind == BindingKind::FunctionParameter ||
           kind == BindingKind::FunctionOutput;
}

class NestedFunctionCaptureCollector {
public:
    explicit NestedFunctionCaptureCollector(
        const SemanticResult& semantic)
        : semantic_(semantic) {}

    std::vector<std::string> collect(const HirNode& function) {
        if (function.kind != HirKind::Function ||
            function.lexicalFunctionName.empty() ||
            function.semanticScopeId < 0 ||
            static_cast<size_t>(function.semanticScopeId) >=
                semantic_.scopes.size()) {
            return {};
        }
        functionScopeId_ = function.semanticScopeId;
        for (const auto& child : function.children) {
            collectNode(*child);
        }
        return std::move(captures_);
    }

private:
    bool isAncestorFunctionScope(int scopeId) const {
        int current = semantic_.scopes[static_cast<size_t>(
            functionScopeId_)].parentId;
        while (current >= 0 &&
               static_cast<size_t>(current) < semantic_.scopes.size()) {
            const SemanticScope& scope =
                semantic_.scopes[static_cast<size_t>(current)];
            if (current == scopeId) {
                return scope.kind == ScopeKind::Function;
            }
            current = scope.parentId;
        }
        return false;
    }

    void collectNode(const HirNode& node) {
        if (node.kind == HirKind::NameRef &&
            node.binding.symbolId >= 0 &&
            static_cast<size_t>(node.binding.symbolId) <
                semantic_.symbols.size() &&
            isNestedCaptureBinding(node.binding.kind)) {
            const SemanticSymbol& symbol = semantic_.symbols[
                static_cast<size_t>(node.binding.symbolId)];
            if (isAncestorFunctionScope(symbol.scopeId) &&
                capturedNames_.insert(symbol.name).second) {
                captures_.push_back(symbol.name);
            }
        }
        for (const auto& child : node.children) {
            collectNode(*child);
        }
    }

    const SemanticResult& semantic_;
    int functionScopeId_ = -1;
    std::unordered_set<std::string> capturedNames_;
    std::vector<std::string> captures_;
};

} // namespace

bool isKnownBuiltinName(std::string_view name) {
    return defaultBuiltinRegistry()->contains(name);
}

std::vector<std::string> anonymousFunctionCaptureNames(
    const HirNode& functionHandle) {
    return AnonymousCaptureCollector{}.collect(functionHandle);
}

std::vector<std::string> nestedFunctionCaptureNames(
    const HirNode& function, const SemanticResult& semantic) {
    return NestedFunctionCaptureCollector(semantic).collect(function);
}

SemanticAnalyzer::SemanticAnalyzer()
    : builtinRegistry_(defaultBuiltinRegistry()) {}

SemanticAnalyzer::SemanticAnalyzer(
    std::shared_ptr<const BuiltinRegistry> builtinRegistry,
    std::vector<std::string> externalFunctionNames)
    : builtinRegistry_(builtinRegistry
                           ? std::move(builtinRegistry)
                           : defaultBuiltinRegistry()),
      externalFunctionNames_(std::move(externalFunctionNames)) {}

SemanticResult SemanticAnalyzer::analyze(
    const SyntaxNode& root, const std::vector<SourceUnit>& sources,
    const SemanticAnalysisOptions& options) {
    AnalyzerContext context(builtinRegistry_, externalFunctionNames_,
                            options);
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
    case HirKind::ArgumentBlock:
        return "ArgumentBlock";
    case HirKind::Argument:
        return "Argument";
    case HirKind::Import:
        return "Import";
    case HirKind::GlobalDeclaration:
        return "GlobalDeclaration";
    case HirKind::PersistentDeclaration:
        return "PersistentDeclaration";
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
    case HirKind::CellRow:
        return "CellRow";
    case HirKind::MemberAccess:
        return "MemberAccess";
    case HirKind::NameValueArgument:
        return "NameValueArgument";
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
    case BindingKind::GlobalVariable:
        return "GlobalVariable";
    case BindingKind::PersistentVariable:
        return "PersistentVariable";
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
    case SymbolKind::GlobalVariable:
        return "GlobalVariable";
    case SymbolKind::PersistentVariable:
        return "PersistentVariable";
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
