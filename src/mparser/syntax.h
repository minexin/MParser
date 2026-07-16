#pragma once

#include "mparser/property_spec.h"
#include "mparser/source.h"
#include "mparser/token.h"

#include <memory>
#include <string>
#include <vector>

namespace mparser {

enum class SyntaxKind {
    CompilationUnit,
    ClassDef,
    SuperclassList,
    Superclass,
    PropertiesBlock,
    PropertyDecl,
    MethodsBlock,
    MethodPrototype,
    FunctionDef,
    EventsBlock,
    EventDecl,
    EnumerationBlock,
    EnumMember,
    ArgumentsBlock,
    ArgumentDecl,
    ImportStatement,
    ImportItem,
    IfBlock,
    ForBlock,
    ParforBlock,
    WhileBlock,
    SwitchBlock,
    TryBlock,
    SpmdBlock,
    ControlHeader,
    ControlArm,
    Statement,
    AssignmentStatement,
    ExpressionStatement,
    OutputList,
    ParameterList,
    IdentifierExpr,
    NumberLiteralExpr,
    StringLiteralExpr,
    EndExpr,
    ColonExpr,
    IgnoredOutputExpr,
    UnaryExpr,
    BinaryExpr,
    PostfixExpr,
    ParenthesizedExpr,
    MatrixExpr,
    MatrixRow,
    CellExpr,
    MemberAccessExpr,
    CallOrIndexExpr,
    SuperclassCallExpr,
    BraceIndexExpr,
    FunctionHandleExpr,
    MetaClassExpr,
    Error,
};

struct AttributeSyntax {
    std::string name;
    std::string value;
    std::string raw;
    bool negated = false;
    bool hasMetaClassList = false;
    std::vector<std::string> metaClassNames;
    SourceSpan span;
};

struct SyntaxNode {
    explicit SyntaxNode(SyntaxKind nodeKind) : kind(nodeKind) {}

    SyntaxKind kind;
    std::string label;
    std::string raw;
    SourceSpan span;
    std::vector<AttributeSyntax> attributes;
    PropertySpec property;
    std::vector<std::unique_ptr<SyntaxNode>> children;
};

const char* syntaxKindName(SyntaxKind kind);
SyntaxKind syntaxKindForControlKeyword(TokenKind kind);

} // namespace mparser
