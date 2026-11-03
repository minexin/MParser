#pragma once

#include "mparser/frontend/source.h"

#include <string>
#include <vector>

namespace mparser {

enum class TokenKind {
    EndOfFile,
    Newline,
    Identifier,
    Number,
    String,
    SystemCommand,
    KeywordArguments,
    KeywordBreak,
    KeywordCase,
    KeywordCatch,
    KeywordClassdef,
    KeywordContinue,
    KeywordElse,
    KeywordElseif,
    KeywordEnd,
    KeywordEnumeration,
    KeywordEvents,
    KeywordFor,
    KeywordFunction,
    KeywordGlobal,
    KeywordIf,
    KeywordImport,
    KeywordMethods,
    KeywordOtherwise,
    KeywordParfor,
    KeywordPersistent,
    KeywordProperties,
    KeywordReturn,
    KeywordSpmd,
    KeywordSwitch,
    KeywordTry,
    KeywordWhile,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Semicolon,
    Colon,
    Dot,
    DotDot,
    Ellipsis,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    EqualEqual,
    NotEqual,
    Plus,
    Minus,
    Star,
    Slash,
    Backslash,
    Caret,
    DotStar,
    DotSlash,
    DotBackslash,
    DotCaret,
    DotApostrophe,
    Ampersand,
    Pipe,
    DoubleAmpersand,
    DoublePipe,
    Tilde,
    Apostrophe,
    At,
    Question,
    Unknown,
};

constexpr bool isIdentifierNameToken(TokenKind kind) noexcept {
    return kind == TokenKind::Identifier || kind == TokenKind::KeywordArguments ||
           kind == TokenKind::KeywordImport || kind == TokenKind::KeywordProperties ||
           kind == TokenKind::KeywordMethods || kind == TokenKind::KeywordEvents ||
           kind == TokenKind::KeywordEnumeration;
}

enum class TriviaKind {
    Whitespace,
    LineComment,
    BlockComment,
};

struct Trivia {
    TriviaKind kind;
    std::string text;
    SourceSpan span;
};

struct Token {
    TokenKind kind;
    std::string text;
    SourceSpan span;
    std::vector<Trivia> leadingTrivia;
};

const char* tokenKindName(TokenKind kind);
const char* triviaKindName(TriviaKind kind);

} // namespace mparser
