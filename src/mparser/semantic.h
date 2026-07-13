#pragma once

#include "mparser/diagnostic.h"
#include "mparser/source.h"
#include "mparser/syntax.h"

#include <memory>
#include <string>
#include <vector>

namespace mparser {

enum class HirKind {
    Module,
    Class,
    Function,
    Property,
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
    CallOrIndex,
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
    Function,
    Method,
    Property,
    Class,
    Builtin,
};

enum class SymbolKind {
    Variable,
    FunctionParameter,
    FunctionOutput,
    Function,
    Method,
    Property,
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
    SourceSpan span;
    BindingRef binding;
    std::vector<AttributeSyntax> attributes;
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

struct SemanticResult {
    std::unique_ptr<HirNode> root;
    std::vector<SemanticScope> scopes;
    std::vector<SemanticSymbol> symbols;
    std::vector<Diagnostic> diagnostics;
};

class SemanticAnalyzer {
public:
    SemanticResult analyze(const SyntaxNode& root);
};

const char* hirKindName(HirKind kind);
const char* bindingKindName(BindingKind kind);
const char* symbolKindName(SymbolKind kind);
const char* scopeKindName(ScopeKind kind);

} // namespace mparser
