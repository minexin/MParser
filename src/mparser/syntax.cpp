#include "mparser/syntax.h"

#include "mparser/token.h"

namespace mparser {

const char* syntaxKindName(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::CompilationUnit:
        return "CompilationUnit";
    case SyntaxKind::ClassDef:
        return "ClassDef";
    case SyntaxKind::SuperclassList:
        return "SuperclassList";
    case SyntaxKind::Superclass:
        return "Superclass";
    case SyntaxKind::PropertiesBlock:
        return "PropertiesBlock";
    case SyntaxKind::PropertyDecl:
        return "PropertyDecl";
    case SyntaxKind::MethodsBlock:
        return "MethodsBlock";
    case SyntaxKind::MethodPrototype:
        return "MethodPrototype";
    case SyntaxKind::FunctionDef:
        return "FunctionDef";
    case SyntaxKind::EventsBlock:
        return "EventsBlock";
    case SyntaxKind::EventDecl:
        return "EventDecl";
    case SyntaxKind::EnumerationBlock:
        return "EnumerationBlock";
    case SyntaxKind::EnumMember:
        return "EnumMember";
    case SyntaxKind::ArgumentsBlock:
        return "ArgumentsBlock";
    case SyntaxKind::ArgumentDecl:
        return "ArgumentDecl";
    case SyntaxKind::ImportStatement:
        return "ImportStatement";
    case SyntaxKind::ImportItem:
        return "ImportItem";
    case SyntaxKind::IfBlock:
        return "IfBlock";
    case SyntaxKind::ForBlock:
        return "ForBlock";
    case SyntaxKind::ParforBlock:
        return "ParforBlock";
    case SyntaxKind::WhileBlock:
        return "WhileBlock";
    case SyntaxKind::SwitchBlock:
        return "SwitchBlock";
    case SyntaxKind::TryBlock:
        return "TryBlock";
    case SyntaxKind::SpmdBlock:
        return "SpmdBlock";
    case SyntaxKind::ControlHeader:
        return "ControlHeader";
    case SyntaxKind::ControlArm:
        return "ControlArm";
    case SyntaxKind::Statement:
        return "Statement";
    case SyntaxKind::AssignmentStatement:
        return "AssignmentStatement";
    case SyntaxKind::ExpressionStatement:
        return "ExpressionStatement";
    case SyntaxKind::OutputList:
        return "OutputList";
    case SyntaxKind::ParameterList:
        return "ParameterList";
    case SyntaxKind::IdentifierExpr:
        return "IdentifierExpr";
    case SyntaxKind::NumberLiteralExpr:
        return "NumberLiteralExpr";
    case SyntaxKind::StringLiteralExpr:
        return "StringLiteralExpr";
    case SyntaxKind::EndExpr:
        return "EndExpr";
    case SyntaxKind::ColonExpr:
        return "ColonExpr";
    case SyntaxKind::IgnoredOutputExpr:
        return "IgnoredOutputExpr";
    case SyntaxKind::UnaryExpr:
        return "UnaryExpr";
    case SyntaxKind::BinaryExpr:
        return "BinaryExpr";
    case SyntaxKind::PostfixExpr:
        return "PostfixExpr";
    case SyntaxKind::ParenthesizedExpr:
        return "ParenthesizedExpr";
    case SyntaxKind::MatrixExpr:
        return "MatrixExpr";
    case SyntaxKind::MatrixRow:
        return "MatrixRow";
    case SyntaxKind::CellExpr:
        return "CellExpr";
    case SyntaxKind::MemberAccessExpr:
        return "MemberAccessExpr";
    case SyntaxKind::CallOrIndexExpr:
        return "CallOrIndexExpr";
    case SyntaxKind::SuperclassCallExpr:
        return "SuperclassCallExpr";
    case SyntaxKind::BraceIndexExpr:
        return "BraceIndexExpr";
    case SyntaxKind::FunctionHandleExpr:
        return "FunctionHandleExpr";
    case SyntaxKind::MetaClassExpr:
        return "MetaClassExpr";
    case SyntaxKind::Error:
        return "Error";
    }
    return "Unknown";
}

SyntaxKind syntaxKindForControlKeyword(TokenKind kind) {
    switch (kind) {
    case TokenKind::KeywordFor:
        return SyntaxKind::ForBlock;
    case TokenKind::KeywordIf:
        return SyntaxKind::IfBlock;
    case TokenKind::KeywordParfor:
        return SyntaxKind::ParforBlock;
    case TokenKind::KeywordSpmd:
        return SyntaxKind::SpmdBlock;
    case TokenKind::KeywordSwitch:
        return SyntaxKind::SwitchBlock;
    case TokenKind::KeywordTry:
        return SyntaxKind::TryBlock;
    case TokenKind::KeywordWhile:
        return SyntaxKind::WhileBlock;
    default:
        return SyntaxKind::Statement;
    }
}

} // namespace mparser
