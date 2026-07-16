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

enum class ArgumentBlockKind {
    Input,
    RepeatingInput,
    Output,
    RepeatingOutput,
};

struct ArgumentBlockSpec {
    ArgumentBlockKind kind = ArgumentBlockKind::Input;
    bool explicitInput = false;
    bool explicitOutput = false;
    bool repeating = false;
    bool valid = true;
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
    ArgumentBlockSpec argumentBlock;
    PropertySpec property;
    std::vector<std::unique_ptr<SyntaxNode>> children;
};

const char* syntaxKindName(SyntaxKind kind);
const char* argumentBlockKindName(ArgumentBlockKind kind);
SyntaxKind syntaxKindForControlKeyword(TokenKind kind);

} // namespace mparser
