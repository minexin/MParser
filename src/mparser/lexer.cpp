#include "mparser/lexer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace mparser {
namespace {

bool isIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentifierPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isHorizontalWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

const std::unordered_map<std::string, TokenKind>& keywordTable() {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"arguments", TokenKind::KeywordArguments},
        {"break", TokenKind::KeywordBreak},
        {"case", TokenKind::KeywordCase},
        {"catch", TokenKind::KeywordCatch},
        {"classdef", TokenKind::KeywordClassdef},
        {"continue", TokenKind::KeywordContinue},
        {"else", TokenKind::KeywordElse},
        {"elseif", TokenKind::KeywordElseif},
        {"end", TokenKind::KeywordEnd},
        {"enumeration", TokenKind::KeywordEnumeration},
        {"events", TokenKind::KeywordEvents},
        {"for", TokenKind::KeywordFor},
        {"function", TokenKind::KeywordFunction},
        {"global", TokenKind::KeywordGlobal},
        {"if", TokenKind::KeywordIf},
        {"import", TokenKind::KeywordImport},
        {"methods", TokenKind::KeywordMethods},
        {"otherwise", TokenKind::KeywordOtherwise},
        {"parfor", TokenKind::KeywordParfor},
        {"persistent", TokenKind::KeywordPersistent},
        {"properties", TokenKind::KeywordProperties},
        {"return", TokenKind::KeywordReturn},
        {"spmd", TokenKind::KeywordSpmd},
        {"switch", TokenKind::KeywordSwitch},
        {"try", TokenKind::KeywordTry},
        {"while", TokenKind::KeywordWhile},
    };
    return keywords;
}

bool canEndExpression(TokenKind kind) {
    switch (kind) {
    case TokenKind::Identifier:
    case TokenKind::Number:
    case TokenKind::String:
    case TokenKind::RParen:
    case TokenKind::RBracket:
    case TokenKind::RBrace:
    case TokenKind::DotApostrophe:
    case TokenKind::Apostrophe:
    case TokenKind::KeywordEnd:
        return true;
    default:
        return false;
    }
}

} // namespace

Lexer::Lexer(std::string_view source, size_t sourceId)
    : source_(source), sourceId_(sourceId) {}

std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;

    while (true) {
        auto leadingTrivia = scanTrivia();
        Token token = isAtEnd() ? makeToken(TokenKind::EndOfFile, position(), "",
                                            std::move(leadingTrivia))
                                : scanToken(std::move(leadingTrivia));
        const TokenKind kind = token.kind;
        tokens.push_back(std::move(token));
        updatePreviousSignificant(kind);
        if (kind == TokenKind::EndOfFile) {
            break;
        }
    }

    return tokens;
}

bool Lexer::isAtEnd() const {
    return offset_ >= source_.size();
}

char Lexer::peek(size_t offset) const {
    const size_t index = offset_ + offset;
    if (index >= source_.size()) {
        return '\0';
    }
    return source_[index];
}

char Lexer::advance() {
    const char c = source_[offset_++];
    ++column_;
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || peek() != expected) {
        return false;
    }
    advance();
    return true;
}

SourcePosition Lexer::position() const {
    return SourcePosition{offset_, line_, column_, sourceId_};
}

SourceSpan Lexer::spanFrom(SourcePosition begin) const {
    return SourceSpan{begin, position()};
}

std::vector<Trivia> Lexer::scanTrivia() {
    std::vector<Trivia> trivia;

    while (!isAtEnd()) {
        const SourcePosition begin = position();

        if (isHorizontalWhitespace(peek())) {
            std::string text;
            while (!isAtEnd() && isHorizontalWhitespace(peek())) {
                text.push_back(advance());
            }
            trivia.push_back(Trivia{TriviaKind::Whitespace, std::move(text),
                                    spanFrom(begin)});
            continue;
        }

        if (peek() == '%' && peek(1) == '{') {
            std::string text;
            text.push_back(advance());
            text.push_back(advance());
            while (!isAtEnd()) {
                if (peek() == '%' && peek(1) == '}') {
                    text.push_back(advance());
                    text.push_back(advance());
                    break;
                }

                if (peek() == '\r' || peek() == '\n') {
                    const auto newlineBegin = position();
                    if (peek() == '\r') {
                        text.push_back(advance());
                        if (peek() == '\n') {
                            text.push_back(advance());
                        }
                    } else {
                        text.push_back(advance());
                    }
                    (void)newlineBegin;
                    ++line_;
                    column_ = 1;
                    continue;
                }

                text.push_back(advance());
            }
            trivia.push_back(Trivia{TriviaKind::BlockComment, std::move(text),
                                    spanFrom(begin)});
            continue;
        }

        if (peek() == '%') {
            std::string text;
            while (!isAtEnd() && peek() != '\r' && peek() != '\n') {
                text.push_back(advance());
            }
            trivia.push_back(Trivia{TriviaKind::LineComment, std::move(text),
                                    spanFrom(begin)});
            continue;
        }

        break;
    }

    return trivia;
}

Token Lexer::scanToken(std::vector<Trivia> leadingTrivia) {
    if (peek() == '\r' || peek() == '\n') {
        return scanNewline(std::move(leadingTrivia));
    }

    if (isIdentifierStart(peek())) {
        return scanIdentifierOrKeyword(std::move(leadingTrivia));
    }

    if (std::isdigit(static_cast<unsigned char>(peek())) != 0 ||
        (peek() == '.' &&
         std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
        return scanNumber(std::move(leadingTrivia));
    }

    if (peek() == '"') {
        return scanString('"', std::move(leadingTrivia));
    }

    if (peek() == '\'') {
        return scanSingleQuoteOrTranspose(std::move(leadingTrivia));
    }

    const SourcePosition begin = position();

    switch (advance()) {
    case '(':
        return makeToken(TokenKind::LParen, begin, "(", std::move(leadingTrivia));
    case ')':
        return makeToken(TokenKind::RParen, begin, ")", std::move(leadingTrivia));
    case '[':
        return makeToken(TokenKind::LBracket, begin, "[", std::move(leadingTrivia));
    case ']':
        return makeToken(TokenKind::RBracket, begin, "]", std::move(leadingTrivia));
    case '{':
        return makeToken(TokenKind::LBrace, begin, "{", std::move(leadingTrivia));
    case '}':
        return makeToken(TokenKind::RBrace, begin, "}", std::move(leadingTrivia));
    case ',':
        return makeToken(TokenKind::Comma, begin, ",", std::move(leadingTrivia));
    case ';':
        return makeToken(TokenKind::Semicolon, begin, ";", std::move(leadingTrivia));
    case ':':
        return makeToken(TokenKind::Colon, begin, ":", std::move(leadingTrivia));
    case '+':
        return makeToken(TokenKind::Plus, begin, "+", std::move(leadingTrivia));
    case '-':
        return makeToken(TokenKind::Minus, begin, "-", std::move(leadingTrivia));
    case '*':
        return makeToken(TokenKind::Star, begin, "*", std::move(leadingTrivia));
    case '/':
        return makeToken(TokenKind::Slash, begin, "/", std::move(leadingTrivia));
    case '\\':
        return makeToken(TokenKind::Backslash, begin, "\\", std::move(leadingTrivia));
    case '^':
        return makeToken(TokenKind::Caret, begin, "^", std::move(leadingTrivia));
    case '@':
        return makeToken(TokenKind::At, begin, "@", std::move(leadingTrivia));
    case '?':
        return makeToken(TokenKind::Question, begin, "?", std::move(leadingTrivia));
    case '~':
        if (match('=')) {
            return makeToken(TokenKind::NotEqual, begin, "~=", std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Tilde, begin, "~", std::move(leadingTrivia));
    case '=':
        if (match('=')) {
            return makeToken(TokenKind::EqualEqual, begin, "==", std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Equal, begin, "=", std::move(leadingTrivia));
    case '<':
        if (match('=')) {
            return makeToken(TokenKind::LessEqual, begin, "<=", std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Less, begin, "<", std::move(leadingTrivia));
    case '>':
        if (match('=')) {
            return makeToken(TokenKind::GreaterEqual, begin, ">=", std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Greater, begin, ">", std::move(leadingTrivia));
    case '&':
        if (match('&')) {
            return makeToken(TokenKind::DoubleAmpersand, begin, "&&",
                             std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Ampersand, begin, "&", std::move(leadingTrivia));
    case '|':
        if (match('|')) {
            return makeToken(TokenKind::DoublePipe, begin, "||",
                             std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Pipe, begin, "|", std::move(leadingTrivia));
    case '.':
        if (match('.')) {
            if (match('.')) {
                return makeToken(TokenKind::Ellipsis, begin, "...",
                                 std::move(leadingTrivia));
            }
            return makeToken(TokenKind::DotDot, begin, "..", std::move(leadingTrivia));
        }
        if (match('*')) {
            return makeToken(TokenKind::DotStar, begin, ".*", std::move(leadingTrivia));
        }
        if (match('/')) {
            return makeToken(TokenKind::DotSlash, begin, "./", std::move(leadingTrivia));
        }
        if (match('\\')) {
            return makeToken(TokenKind::DotBackslash, begin, ".\\",
                             std::move(leadingTrivia));
        }
        if (match('^')) {
            return makeToken(TokenKind::DotCaret, begin, ".^", std::move(leadingTrivia));
        }
        if (match('\'')) {
            return makeToken(TokenKind::DotApostrophe, begin, ".'",
                             std::move(leadingTrivia));
        }
        return makeToken(TokenKind::Dot, begin, ".", std::move(leadingTrivia));
    default:
        return makeToken(TokenKind::Unknown, begin,
                         std::string(source_.substr(begin.offset, 1)),
                         std::move(leadingTrivia));
    }
}

Token Lexer::makeToken(TokenKind kind, SourcePosition begin, std::string text,
                       std::vector<Trivia> leadingTrivia) const {
    return Token{kind, std::move(text), SourceSpan{begin, position()},
                 std::move(leadingTrivia)};
}

Token Lexer::scanIdentifierOrKeyword(std::vector<Trivia> leadingTrivia) {
    const SourcePosition begin = position();
    std::string text;
    while (!isAtEnd() && isIdentifierPart(peek())) {
        text.push_back(advance());
    }

    const auto keyword = keywordTable().find(text);
    const TokenKind kind =
        keyword == keywordTable().end() ? TokenKind::Identifier : keyword->second;
    return makeToken(kind, begin, std::move(text), std::move(leadingTrivia));
}

Token Lexer::scanNumber(std::vector<Trivia> leadingTrivia) {
    const SourcePosition begin = position();
    std::string text;

    if (peek() == '0' &&
        (peek(1) == 'x' || peek(1) == 'X' ||
         peek(1) == 'b' || peek(1) == 'B')) {
        text.push_back(advance());
        text.push_back(advance());
        while (!isAtEnd() &&
               (std::isalnum(static_cast<unsigned char>(peek())) != 0 ||
                peek() == '_')) {
            text.push_back(advance());
        }
        return makeToken(TokenKind::Number, begin, std::move(text),
                         std::move(leadingTrivia));
    }

    if (peek() == '.') {
        text.push_back(advance());
    }

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        text.push_back(advance());
    }

    if (!isAtEnd() && peek() == '.' && peek(1) != '.') {
        text.push_back(advance());
        while (!isAtEnd() &&
               std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            text.push_back(advance());
        }
    }

    if (!isAtEnd() && (peek() == 'e' || peek() == 'E')) {
        text.push_back(advance());
        if (!isAtEnd() && (peek() == '+' || peek() == '-')) {
            text.push_back(advance());
        }
        while (!isAtEnd() &&
               std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            text.push_back(advance());
        }
    }

    if (!isAtEnd() && (peek() == 'i' || peek() == 'j')) {
        text.push_back(advance());
    }

    return makeToken(TokenKind::Number, begin, std::move(text),
                     std::move(leadingTrivia));
}

Token Lexer::scanString(char quote, std::vector<Trivia> leadingTrivia) {
    const SourcePosition begin = position();
    std::string text;
    text.push_back(advance());

    while (!isAtEnd()) {
        const char c = advance();
        text.push_back(c);
        if (c == quote) {
            if (peek() == quote) {
                text.push_back(advance());
                continue;
            }
            break;
        }
        if (c == '\r' || c == '\n') {
            ++line_;
            column_ = 1;
            if (c == '\r' && peek() == '\n') {
                text.push_back(advance());
            }
        }
    }

    return makeToken(TokenKind::String, begin, std::move(text),
                     std::move(leadingTrivia));
}

Token Lexer::scanSingleQuoteOrTranspose(std::vector<Trivia> leadingTrivia) {
    const bool separatedArrayElement =
        arrayDelimiterDepth_ != 0 &&
        std::any_of(leadingTrivia.begin(), leadingTrivia.end(),
                    [](const Trivia& trivia) {
                        return trivia.kind == TriviaKind::Whitespace;
                    });
    if (previousCanEndExpression_ && !separatedArrayElement) {
        const SourcePosition begin = position();
        advance();
        return makeToken(TokenKind::Apostrophe, begin, "'",
                         std::move(leadingTrivia));
    }
    return scanString('\'', std::move(leadingTrivia));
}

Token Lexer::scanNewline(std::vector<Trivia> leadingTrivia) {
    const SourcePosition begin = position();
    std::string text;

    if (peek() == '\r') {
        text.push_back(advance());
        if (peek() == '\n') {
            text.push_back(advance());
        }
    } else {
        text.push_back(advance());
    }

    ++line_;
    column_ = 1;

    return Token{TokenKind::Newline, std::move(text), SourceSpan{begin, position()},
                 std::move(leadingTrivia)};
}

void Lexer::updatePreviousSignificant(TokenKind kind) {
    if (kind == TokenKind::LBracket || kind == TokenKind::LBrace) {
        ++arrayDelimiterDepth_;
    } else if ((kind == TokenKind::RBracket ||
                kind == TokenKind::RBrace) &&
               arrayDelimiterDepth_ != 0) {
        --arrayDelimiterDepth_;
    }

    switch (kind) {
    case TokenKind::Newline:
    case TokenKind::Semicolon:
    case TokenKind::Comma:
        previousCanEndExpression_ = false;
        break;
    case TokenKind::EndOfFile:
        break;
    default:
        previousCanEndExpression_ = canEndExpression(kind);
        break;
    }
}

} // namespace mparser
