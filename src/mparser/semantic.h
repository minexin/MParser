#pragma once

#include "mparser/diagnostic.h"
#include "mparser/source.h"
#include "mparser/syntax.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

class BuiltinRegistry;

enum class HirKind {
    Module,
    Class,
    Function,
    ArgumentBlock,
    Argument,
    Import,
    GlobalDeclaration,
    PersistentDeclaration,
    Property,
    Event,
    EnumerationMember,
    MethodPrototype,
    Control,
    ControlHeader,
    ControlArm,
    Statement,
    Assignment,
    OutputList,
    ParameterList,
    NameRef,
    Literal,
    Unary,
    Binary,
    Postfix,
    Matrix,
    MatrixRow,
    Cell,
    MemberAccess,
    NameValueArgument,
    CallOrIndex,
    SuperclassCall,
    BraceIndex,
    FunctionHandle,
    MetaClass,
    Unknown,
};

enum class BindingKind {
    Unresolved,
    LocalVariable,
    FunctionParameter,
    FunctionOutput,
    GlobalVariable,
    PersistentVariable,
    Function,
    Method,
    Property,
    Event,
    EnumerationMember,
    Class,
    Builtin,
};

enum class SymbolKind {
    Variable,
    FunctionParameter,
    FunctionOutput,
    GlobalVariable,
    PersistentVariable,
    Function,
    Method,
    Property,
    Event,
    EnumerationMember,
    Class,
    Builtin,
};

enum class ScopeKind {
    Module,
    Class,
    Function,
};

struct BindingRef {
    BindingKind kind = BindingKind::Unresolved;
    int symbolId = -1;
};

struct HirNode {
    explicit HirNode(HirKind nodeKind) : kind(nodeKind) {}

    HirKind kind;
    std::string label;
    std::string raw;
    std::string lexicalClassName;
    SourceSpan span;
    BindingRef binding;
    std::vector<AttributeSyntax> attributes;
    ArgumentBlockSpec argumentBlock;
    std::string nameValueSourceClass;
    SourceSpan nameValueSourceSpan;
    std::vector<std::string> superclasses;
    PropertySpec property;
    bool capturesExpressionResult = false;
    bool outputSuppressed = false;
    std::vector<std::unique_ptr<HirNode>> children;
};

struct SemanticSymbol {
    int id = -1;
    SymbolKind kind = SymbolKind::Variable;
    std::string name;
    std::string typeName;
    int scopeId = -1;
    SourceSpan span;
};

struct SemanticScope {
    int id = -1;
    int parentId = -1;
    ScopeKind kind = ScopeKind::Module;
    std::string label;
    std::vector<int> symbols;
};

struct SemanticSourceInfo {
    std::string name;
    std::string namespaceName;
};

struct SemanticResult {
    std::unique_ptr<HirNode> root;
    std::vector<SemanticScope> scopes;
    std::vector<SemanticSymbol> symbols;
    std::vector<SemanticSourceInfo> sources;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<const BuiltinRegistry> builtinRegistry;
};

class SemanticAnalyzer {
public:
    SemanticAnalyzer();
    explicit SemanticAnalyzer(
        std::shared_ptr<const BuiltinRegistry> builtinRegistry);

    SemanticResult analyze(
        const SyntaxNode& root,
        const std::vector<SourceUnit>& sources = {});

private:
    std::shared_ptr<const BuiltinRegistry> builtinRegistry_;
};

const char* hirKindName(HirKind kind);
const char* bindingKindName(BindingKind kind);
const char* symbolKindName(SymbolKind kind);
const char* scopeKindName(ScopeKind kind);
bool isKnownBuiltinName(std::string_view name);
std::vector<std::string> anonymousFunctionCaptureNames(
    const HirNode& functionHandle);

} // namespace mparser
