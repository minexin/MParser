#pragma once

#include "mparser/token.h"

#include <string_view>
#include <vector>

namespace mparser {

class Lexer {
public:
    explicit Lexer(std::string_view source,
                   size_t sourceId = kInvalidSourceId);

    std::vector<Token> lex();

private:
    bool isAtEnd() const;
    char peek(size_t offset = 0) const;
    char advance();
    bool match(char expected);

    SourcePosition position() const;
    SourceSpan spanFrom(SourcePosition begin) const;

    std::vector<Trivia> scanTrivia();
    Token scanToken(std::vector<Trivia> leadingTrivia);
    Token makeToken(TokenKind kind, SourcePosition begin, std::string text,
                    std::vector<Trivia> leadingTrivia) const;

    Token scanIdentifierOrKeyword(std::vector<Trivia> leadingTrivia);
    Token scanNumber(std::vector<Trivia> leadingTrivia);
    Token scanString(char quote, std::vector<Trivia> leadingTrivia);
    Token scanSingleQuoteOrTranspose(std::vector<Trivia> leadingTrivia);
    Token scanNewline(std::vector<Trivia> leadingTrivia);

    void updatePreviousSignificant(TokenKind kind);

    std::string_view source_;
    size_t sourceId_ = kInvalidSourceId;
    size_t offset_ = 0;
    int line_ = 1;
    int column_ = 1;
    bool previousCanEndExpression_ = false;
    size_t arrayDelimiterDepth_ = 0;
};

} // namespace mparser
