#pragma once

#include "mparser/diagnostic.h"
#include "mparser/syntax.h"
#include "mparser/token.h"

#include <initializer_list>
#include <memory>
#include <vector>

namespace mparser {

struct ParseResult {
    std::unique_ptr<SyntaxNode> root;
    std::vector<Diagnostic> diagnostics;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    ParseResult parse();

private:
    const Token& current() const;
    const Token& previous() const;
    const Token& peek(size_t offset) const;
    bool at(TokenKind kind) const;
    bool atAny(std::initializer_list<TokenKind> kinds) const;
    bool isAtEnd() const;
    const Token& advance();
    bool match(TokenKind kind);

    std::unique_ptr<SyntaxNode> parseCompilationUnit();
    std::unique_ptr<SyntaxNode> parseClassDef();
    std::unique_ptr<SyntaxNode> parsePropertiesBlock();
    std::unique_ptr<SyntaxNode> parseMethodsBlock();
    std::unique_ptr<SyntaxNode> parseEventsBlock();
    std::unique_ptr<SyntaxNode> parseEnumerationBlock();
    std::unique_ptr<SyntaxNode> parseFunction();
    std::unique_ptr<SyntaxNode> parseArgumentsBlock();
    std::unique_ptr<SyntaxNode> parseImportStatement();
    std::unique_ptr<SyntaxNode> parseWorkspaceDeclaration();
    std::unique_ptr<SyntaxNode> parseControlBlock();
    std::unique_ptr<SyntaxNode> parseStatement();

    void parseBodyUntilEnd(SyntaxNode& parent);
    bool isControlBlockStart(TokenKind kind) const;
    bool isControlArm(TokenKind kind) const;

    std::vector<AttributeSyntax> parseAttributeList();
    AttributeSyntax buildAttribute(const std::vector<Token>& tokens) const;
    std::vector<Token> collectUntilSeparator(bool commaIsSeparator = false);
    void consumeSeparator();
    void skipSeparators();
    void consumeExpectedEnd(const char* owner);

    std::string joinTokens(const std::vector<Token>& tokens) const;
    std::string firstIdentifier(const std::vector<Token>& tokens) const;
    std::string functionNameFromHeader(const std::vector<Token>& tokens) const;

    std::unique_ptr<SyntaxNode> makeNode(SyntaxKind kind, SourcePosition begin);
    void finishNode(SyntaxNode& node);
    void diagnosticAtCurrent(std::string message);

    std::vector<Token> tokens_;
    size_t cursor_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace mparser
