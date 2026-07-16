#include "mparser/parser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <utility>

namespace mparser {
namespace {

void updateDepth(TokenKind kind, int& parenDepth, int& bracketDepth,
                 int& braceDepth) {
    switch (kind) {
    case TokenKind::LParen:
        ++parenDepth;
        break;
    case TokenKind::RParen:
        if (parenDepth > 0) {
            --parenDepth;
        }
        break;
    case TokenKind::LBracket:
        ++bracketDepth;
        break;
    case TokenKind::RBracket:
        if (bracketDepth > 0) {
            --bracketDepth;
        }
        break;
    case TokenKind::LBrace:
        ++braceDepth;
        break;
    case TokenKind::RBrace:
        if (braceDepth > 0) {
            --braceDepth;
        }
        break;
    default:
        break;
    }
}

bool atTopLevel(int parenDepth, int bracketDepth, int braceDepth) {
    return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0;
}

std::string joinTokenTexts(const std::vector<Token>& tokens) {
    std::string text;
    for (const auto& token : tokens) {
        for (const auto& trivia : token.leadingTrivia) {
            text += trivia.text;
        }
        text += token.text;
    }
    return text;
}

std::string trimAsciiWhitespace(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\r\n\v\f");
    return text.substr(first, last - first + 1);
}

SourceSpan spanFromTokens(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
        return SourceSpan{};
    }
    return mergeSpans(tokens.front().span, tokens.back().span);
}

bool parseMetaClassReference(const std::vector<Token>& tokens, size_t& index,
                             std::string& name) {
    if (index >= tokens.size() || tokens[index].kind != TokenKind::Question) {
        return false;
    }
    ++index;
    if (index >= tokens.size() || tokens[index].kind != TokenKind::Identifier) {
        return false;
    }

    name = tokens[index].text;
    ++index;
    while (index < tokens.size() && tokens[index].kind == TokenKind::Dot) {
        ++index;
        if (index >= tokens.size() ||
            tokens[index].kind != TokenKind::Identifier) {
            return false;
        }
        name += "." + tokens[index].text;
        ++index;
    }
    return true;
}

std::optional<std::vector<std::string>>
parseMetaClassList(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
        return std::nullopt;
    }

    size_t index = 0;
    std::vector<std::string> names;
    if (tokens.front().kind == TokenKind::Question) {
        std::string name;
        if (!parseMetaClassReference(tokens, index, name) ||
            index != tokens.size()) {
            return std::nullopt;
        }
        names.push_back(std::move(name));
        return names;
    }

    if (tokens.front().kind != TokenKind::LBrace) {
        return std::nullopt;
    }
    ++index;
    if (index < tokens.size() && tokens[index].kind == TokenKind::RBrace) {
        ++index;
        return index == tokens.size()
                   ? std::optional<std::vector<std::string>>(std::move(names))
                   : std::nullopt;
    }

    while (index < tokens.size()) {
        std::string name;
        if (!parseMetaClassReference(tokens, index, name)) {
            return std::nullopt;
        }
        names.push_back(std::move(name));
        if (index >= tokens.size()) {
            return std::nullopt;
        }
        if (tokens[index].kind == TokenKind::RBrace) {
            ++index;
            return index == tokens.size()
                       ? std::optional<std::vector<std::string>>(
                             std::move(names))
                       : std::nullopt;
        }
        if (tokens[index].kind != TokenKind::Comma) {
            return std::nullopt;
        }
        ++index;
    }
    return std::nullopt;
}

std::unique_ptr<SyntaxNode> makeNodeFromSpan(SyntaxKind kind, SourceSpan span) {
    auto node = std::make_unique<SyntaxNode>(kind);
    node->span = span;
    return node;
}

int binaryPrecedence(TokenKind kind) {
    switch (kind) {
    case TokenKind::DoublePipe:
        return 1;
    case TokenKind::DoubleAmpersand:
        return 2;
    case TokenKind::Pipe:
        return 3;
    case TokenKind::Ampersand:
        return 4;
    case TokenKind::Less:
    case TokenKind::Greater:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual:
    case TokenKind::EqualEqual:
    case TokenKind::NotEqual:
        return 5;
    case TokenKind::Colon:
        return 6;
    case TokenKind::Plus:
    case TokenKind::Minus:
        return 7;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Backslash:
    case TokenKind::DotStar:
    case TokenKind::DotSlash:
    case TokenKind::DotBackslash:
        return 8;
    case TokenKind::Caret:
    case TokenKind::DotCaret:
        return 10;
    default:
        return 0;
    }
}

bool isRightAssociativeBinary(TokenKind kind) {
    return kind == TokenKind::Caret || kind == TokenKind::DotCaret;
}

bool isPrefixOperator(TokenKind kind) {
    return kind == TokenKind::Plus || kind == TokenKind::Minus ||
           kind == TokenKind::Tilde;
}

bool isTopLevelSeparator(TokenKind kind) {
    return kind == TokenKind::Comma || kind == TokenKind::Semicolon;
}

struct ExpressionParseResult {
    std::unique_ptr<SyntaxNode> root;
    bool consumedAll = false;
};

class ExpressionParser {
public:
    explicit ExpressionParser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    ExpressionParseResult parse() {
        auto root = parseExpression(0);
        if (!root && !tokens_.empty()) {
            root = makeError(tokens_.front().span, "unparsed expression");
        }
        return ExpressionParseResult{std::move(root), isAtEnd()};
    }

private:
    bool isAtEnd() const {
        return cursor_ >= tokens_.size();
    }

    bool at(TokenKind kind) const {
        return !isAtEnd() && tokens_[cursor_].kind == kind;
    }

    bool at(size_t offset, TokenKind kind) const {
        return cursor_ + offset < tokens_.size() &&
               tokens_[cursor_ + offset].kind == kind;
    }

    bool atAny(std::initializer_list<TokenKind> kinds) const {
        if (isAtEnd()) {
            return false;
        }
        return std::find(kinds.begin(), kinds.end(), tokens_[cursor_].kind) !=
               kinds.end();
    }

    const Token& current() const {
        return tokens_[cursor_];
    }

    const Token& advance() {
        return tokens_[cursor_++];
    }

    std::unique_ptr<SyntaxNode> parseExpression(int minimumPrecedence) {
        auto left = parsePrefix();
        if (!left) {
            return nullptr;
        }

        while (!isAtEnd()) {
            const int precedence = binaryPrecedence(current().kind);
            if (precedence == 0 || precedence < minimumPrecedence) {
                break;
            }

            const Token op = advance();
            const int nextMinimum =
                precedence + (isRightAssociativeBinary(op.kind) ? 0 : 1);
            auto right = parseExpression(nextMinimum);
            if (!right) {
                right = makeError(op.span, "expected right-hand expression");
            }

            SourceSpan span = mergeSpans(left->span, right->span);
            auto binary = makeNodeFromSpan(SyntaxKind::BinaryExpr, span);
            binary->label = op.text;
            binary->raw = op.text;
            binary->children.push_back(std::move(left));
            binary->children.push_back(std::move(right));
            left = std::move(binary);
        }

        return left;
    }

    std::unique_ptr<SyntaxNode> parsePrefix() {
        if (!isAtEnd() && isPrefixOperator(current().kind)) {
            const Token op = advance();
            if (op.kind == TokenKind::Tilde && isAtEnd()) {
                auto ignored =
                    makeNodeFromSpan(SyntaxKind::IgnoredOutputExpr, op.span);
                ignored->label = op.text;
                ignored->raw = op.text;
                return ignored;
            }

            auto operand = parseExpression(9);
            if (!operand) {
                operand = makeError(op.span, "expected unary operand");
            }

            auto unary =
                makeNodeFromSpan(SyntaxKind::UnaryExpr,
                                 mergeSpans(op.span, operand->span));
            unary->label = op.text;
            unary->raw = op.text;
            unary->children.push_back(std::move(operand));
            return unary;
        }

        auto primary = parsePrimary();
        if (!primary) {
            return nullptr;
        }
        return parsePostfix(std::move(primary));
    }

    std::unique_ptr<SyntaxNode> parsePrimary() {
        if (isAtEnd()) {
            return nullptr;
        }

        const Token token = advance();
        switch (token.kind) {
        case TokenKind::Identifier:
        case TokenKind::KeywordEnumeration:
        case TokenKind::KeywordEvents:
        case TokenKind::KeywordMethods:
        case TokenKind::KeywordProperties:
            return makeLeaf(SyntaxKind::IdentifierExpr, token);
        case TokenKind::Number:
            return makeLeaf(SyntaxKind::NumberLiteralExpr, token);
        case TokenKind::String:
            return makeLeaf(SyntaxKind::StringLiteralExpr, token);
        case TokenKind::KeywordEnd:
            return makeLeaf(SyntaxKind::EndExpr, token);
        case TokenKind::Colon:
            return makeLeaf(SyntaxKind::ColonExpr, token);
        case TokenKind::LParen:
            return parseParenthesized(token);
        case TokenKind::LBracket:
            return parseMatrixDelimited(token);
        case TokenKind::LBrace:
            return parseDelimited(SyntaxKind::CellExpr, token, TokenKind::RBrace);
        case TokenKind::At:
            return parseFunctionHandle(token);
        case TokenKind::Question:
            return parseMetaClass(token);
        default:
            return makeError(token.span, "unexpected token in expression");
        }
    }

    std::unique_ptr<SyntaxNode> parsePostfix(std::unique_ptr<SyntaxNode> left) {
        while (!isAtEnd()) {
            if (at(TokenKind::Apostrophe)) {
                const Token op = advance();
                auto postfix =
                    makeNodeFromSpan(SyntaxKind::PostfixExpr,
                                     mergeSpans(left->span, op.span));
                postfix->label = op.text;
                postfix->raw = op.text;
                postfix->children.push_back(std::move(left));
                left = std::move(postfix);
                continue;
            }

            if (at(TokenKind::LParen)) {
                const Token open = advance();
                left = parsePostfixDelimited(SyntaxKind::CallOrIndexExpr,
                                             std::move(left), open,
                                             TokenKind::RParen, "()");
                continue;
            }

            if (at(TokenKind::At)) {
                left = parseSuperclassCall(std::move(left));
                continue;
            }

            if (at(TokenKind::LBrace)) {
                const Token open = advance();
                left = parsePostfixDelimited(SyntaxKind::BraceIndexExpr,
                                             std::move(left), open,
                                             TokenKind::RBrace, "{}");
                continue;
            }

            if (at(TokenKind::Dot)) {
                left = parseMemberAccess(std::move(left));
                continue;
            }

            break;
        }

        return left;
    }

    std::unique_ptr<SyntaxNode> parseSuperclassCall(
        std::unique_ptr<SyntaxNode> calleeOrObject) {
        const Token atToken = advance();
        auto node = makeNodeFromSpan(
            SyntaxKind::SuperclassCallExpr,
            mergeSpans(calleeOrObject->span, atToken.span));
        node->raw = atToken.text;
        node->children.push_back(std::move(calleeOrObject));

        std::vector<Token> nameTokens;
        while (!isAtEnd() &&
               atAny({TokenKind::Identifier, TokenKind::Dot})) {
            nameTokens.push_back(advance());
        }
        if (nameTokens.empty()) {
            node->children.push_back(
                makeError(atToken.span, "expected superclass name after @"));
            return node;
        }

        node->label = trimAsciiWhitespace(joinTokenTexts(nameTokens));
        node->raw += node->label;
        node->span = mergeSpans(node->span, nameTokens.back().span);
        if (!at(TokenKind::LParen)) {
            node->children.push_back(makeError(
                nameTokens.back().span,
                "expected argument list after superclass name"));
            return node;
        }

        const Token open = advance();
        while (!isAtEnd() && !at(TokenKind::RParen)) {
            if (atAny({TokenKind::Comma, TokenKind::Semicolon})) {
                advance();
                continue;
            }

            auto argument = parseCallArgument(true);
            if (argument) {
                node->children.push_back(std::move(argument));
                continue;
            }

            const Token unexpected = advance();
            node->children.push_back(makeError(
                unexpected.span,
                "unexpected token in superclass argument list"));
        }

        if (at(TokenKind::RParen)) {
            const Token close = advance();
            node->span = mergeSpans(node->span, close.span);
        } else {
            node->span = mergeSpans(node->span, open.span);
        }
        return node;
    }

    std::unique_ptr<SyntaxNode> parseParenthesized(const Token& open) {
        auto node = makeNodeFromSpan(SyntaxKind::ParenthesizedExpr, open.span);
        if (!at(TokenKind::RParen)) {
            auto expression = parseExpression(0);
            if (expression) {
                node->children.push_back(std::move(expression));
            }
        }

        if (at(TokenKind::RParen)) {
            const Token close = advance();
            node->span = mergeSpans(open.span, close.span);
        } else if (!node->children.empty()) {
            node->span = mergeSpans(open.span, node->children.back()->span);
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> parseMatrixDelimited(const Token& open) {
        auto node = makeNodeFromSpan(SyntaxKind::MatrixExpr, open.span);
        auto row = makeNodeFromSpan(SyntaxKind::MatrixRow, open.span);

        while (!isAtEnd() && !at(TokenKind::RBracket)) {
            if (at(TokenKind::Comma)) {
                advance();
                continue;
            }

            if (at(TokenKind::Semicolon)) {
                const Token separator = advance();
                if (!row->children.empty()) {
                    row->span = mergeSpans(row->children.front()->span,
                                           row->children.back()->span);
                    node->children.push_back(std::move(row));
                }
                row = makeNodeFromSpan(SyntaxKind::MatrixRow, separator.span);
                continue;
            }

            auto expression = parseExpression(0);
            if (expression) {
                if (row->children.empty()) {
                    row->span = expression->span;
                } else {
                    row->span = mergeSpans(row->span, expression->span);
                }
                row->children.push_back(std::move(expression));
                continue;
            }

            const Token unexpected = advance();
            row->children.push_back(
                makeError(unexpected.span, "unexpected token in matrix literal"));
        }

        if (!row->children.empty()) {
            row->span = mergeSpans(row->children.front()->span,
                                   row->children.back()->span);
            node->children.push_back(std::move(row));
        }

        if (at(TokenKind::RBracket)) {
            const Token close = advance();
            node->span = mergeSpans(open.span, close.span);
        } else if (!node->children.empty()) {
            node->span = mergeSpans(open.span, node->children.back()->span);
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> parseDelimited(SyntaxKind kind, const Token& open,
                                               TokenKind closingKind) {
        auto node = makeNodeFromSpan(kind, open.span);

        while (!isAtEnd() && !at(closingKind)) {
            if (atAny({TokenKind::Comma, TokenKind::Semicolon})) {
                advance();
                continue;
            }

            auto expression = parseExpression(0);
            if (expression) {
                node->children.push_back(std::move(expression));
                continue;
            }

            const Token unexpected = advance();
            node->children.push_back(
                makeError(unexpected.span, "unexpected token in delimited list"));
        }

        if (at(closingKind)) {
            const Token close = advance();
            node->span = mergeSpans(open.span, close.span);
        } else if (!node->children.empty()) {
            node->span = mergeSpans(open.span, node->children.back()->span);
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> parsePostfixDelimited(
        SyntaxKind kind, std::unique_ptr<SyntaxNode> callee, const Token& open,
        TokenKind closingKind, const char* label) {
        auto node = makeNodeFromSpan(kind, mergeSpans(callee->span, open.span));
        node->label = label;
        node->raw = label;
        node->children.push_back(std::move(callee));

        while (!isAtEnd() && !at(closingKind)) {
            if (atAny({TokenKind::Comma, TokenKind::Semicolon})) {
                advance();
                continue;
            }

            auto argument = parseCallArgument(
                kind == SyntaxKind::CallOrIndexExpr);
            if (argument) {
                node->children.push_back(std::move(argument));
                continue;
            }

            const Token unexpected = advance();
            node->children.push_back(
                makeError(unexpected.span, "unexpected token in argument list"));
        }

        if (at(closingKind)) {
            const Token close = advance();
            node->span = mergeSpans(node->span, close.span);
        } else if (!node->children.empty()) {
            node->span = mergeSpans(node->span, node->children.back()->span);
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> parseCallArgument(bool allowNameValue) {
        if (!allowNameValue || !at(TokenKind::Identifier) ||
            !at(1, TokenKind::Equal)) {
            return parseExpression(0);
        }

        const Token name = advance();
        const Token equal = advance();
        auto value = parseExpression(0);
        if (!value) {
            value = makeError(equal.span,
                              "expected value after name-value argument");
        }

        auto node = makeNodeFromSpan(
            SyntaxKind::NameValueArgumentExpr,
            mergeSpans(name.span, value->span));
        node->label = name.text;
        node->raw = name.text + equal.text;
        node->children.push_back(std::move(value));
        return node;
    }

    std::unique_ptr<SyntaxNode> parseMemberAccess(
        std::unique_ptr<SyntaxNode> object) {
        const Token dot = advance();
        auto node = makeNodeFromSpan(SyntaxKind::MemberAccessExpr,
                                     mergeSpans(object->span, dot.span));
        node->raw = dot.text;
        node->children.push_back(std::move(object));

        if (at(TokenKind::Identifier)) {
            const Token member = advance();
            node->label = member.text;
            node->span = mergeSpans(node->span, member.span);
            return node;
        }

        if (at(TokenKind::LParen)) {
            const Token open = advance();
            auto dynamicName =
                parseDelimited(SyntaxKind::ParenthesizedExpr, open,
                               TokenKind::RParen);
            node->label = ".()";
            node->span = mergeSpans(node->span, dynamicName->span);
            node->children.push_back(std::move(dynamicName));
            return node;
        }

        node->children.push_back(makeError(dot.span, "expected member name"));
        return node;
    }

    std::unique_ptr<SyntaxNode> parseFunctionHandle(const Token& atToken) {
        auto node =
            makeNodeFromSpan(SyntaxKind::FunctionHandleExpr, atToken.span);
        node->label = "@";
        node->raw = atToken.text;

        if (at(TokenKind::LParen)) {
            auto parameters = parseParameterList();
            node->label = "@()";
            node->span = mergeSpans(node->span, parameters->span);
            node->children.push_back(std::move(parameters));

            if (!isAtEnd()) {
                auto body = parseExpression(0);
                if (body) {
                    node->span = mergeSpans(node->span, body->span);
                    node->children.push_back(std::move(body));
                }
            }
            return node;
        }

        std::vector<Token> nameTokens;
        while (!isAtEnd() && atAny({TokenKind::Identifier, TokenKind::Dot})) {
            nameTokens.push_back(advance());
        }

        if (!nameTokens.empty()) {
            const std::string name = trimAsciiWhitespace(joinTokenTexts(nameTokens));
            node->label = name;
            node->raw = atToken.text + name;
            node->span = mergeSpans(atToken.span, nameTokens.back().span);
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> parseParameterList() {
        const Token open = advance();
        std::vector<Token> parameters;
        SourceSpan span = open.span;
        int depth = 1;

        while (!isAtEnd() && depth > 0) {
            const Token token = advance();
            if (token.kind == TokenKind::LParen) {
                ++depth;
                parameters.push_back(token);
                continue;
            }

            if (token.kind == TokenKind::RParen) {
                --depth;
                span = mergeSpans(open.span, token.span);
                if (depth == 0) {
                    break;
                }
                parameters.push_back(token);
                continue;
            }

            parameters.push_back(token);
        }

        auto node = makeNodeFromSpan(SyntaxKind::ParameterList, span);
        node->raw = joinTokenTexts(parameters);
        return node;
    }

    std::unique_ptr<SyntaxNode> parseMetaClass(const Token& questionToken) {
        auto node = makeNodeFromSpan(SyntaxKind::MetaClassExpr, questionToken.span);
        std::vector<Token> nameTokens;

        while (!isAtEnd() && atAny({TokenKind::Identifier, TokenKind::Dot})) {
            nameTokens.push_back(advance());
        }

        if (!nameTokens.empty()) {
            const std::string name = trimAsciiWhitespace(joinTokenTexts(nameTokens));
            node->label = name;
            node->raw = questionToken.text + name;
            node->span = mergeSpans(questionToken.span, nameTokens.back().span);
        } else {
            node->children.push_back(
                makeError(questionToken.span, "expected class name after ?"));
        }

        return node;
    }

    std::unique_ptr<SyntaxNode> makeLeaf(SyntaxKind kind, const Token& token) const {
        auto node = makeNodeFromSpan(kind, token.span);
        node->label = token.text;
        node->raw = token.text;
        return node;
    }

    std::unique_ptr<SyntaxNode> makeError(SourceSpan span,
                                          std::string message) const {
        auto node = makeNodeFromSpan(SyntaxKind::Error, span);
        node->raw = std::move(message);
        return node;
    }

    std::vector<Token> tokens_;
    size_t cursor_ = 0;
};

ExpressionParseResult parseExpressionTokens(const std::vector<Token>& tokens) {
    ExpressionParser parser(tokens);
    return parser.parse();
}

size_t findTopLevelAssignment(const std::vector<Token>& tokens) {
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Equal &&
            atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            return i;
        }
        updateDepth(tokens[i].kind, parenDepth, bracketDepth, braceDepth);
    }

    return tokens.size();
}

std::vector<std::vector<Token>> splitTopLevel(
    const std::vector<Token>& tokens) {
    std::vector<std::vector<Token>> parts;
    std::vector<Token> currentPart;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (const auto& token : tokens) {
        if (isTopLevelSeparator(token.kind) &&
            atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            parts.push_back(std::move(currentPart));
            currentPart = {};
            continue;
        }

        updateDepth(token.kind, parenDepth, bracketDepth, braceDepth);
        currentPart.push_back(token);
    }

    parts.push_back(std::move(currentPart));
    return parts;
}

std::vector<std::vector<Token>> splitTopLevelCommas(
    const std::vector<Token>& tokens) {
    std::vector<std::vector<Token>> parts;
    std::vector<Token> currentPart;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (const auto& token : tokens) {
        if (token.kind == TokenKind::Comma &&
            atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            parts.push_back(std::move(currentPart));
            currentPart = {};
            continue;
        }

        updateDepth(token.kind, parenDepth, bracketDepth, braceDepth);
        currentPart.push_back(token);
    }

    parts.push_back(std::move(currentPart));
    return parts;
}

struct PropertyParseDiagnostic {
    SourceSpan span;
    std::string message;
};

struct PropertyDeclarationParseResult {
    std::string label;
    std::string nameValueSourceClass;
    SourceSpan nameValueSourceSpan;
    PropertySpec spec;
    std::vector<Token> defaultTokens;
    std::vector<PropertyParseDiagnostic> diagnostics;
};

std::vector<Token> removeEllipses(const std::vector<Token>& tokens) {
    std::vector<Token> result;
    result.reserve(tokens.size());
    for (const auto& token : tokens) {
        if (token.kind != TokenKind::Ellipsis) {
            result.push_back(token);
        }
    }
    return result;
}

size_t findMatchingDelimiter(const std::vector<Token>& tokens, size_t begin,
                             TokenKind open, TokenKind close) {
    int depth = 0;
    for (size_t index = begin; index < tokens.size(); ++index) {
        if (tokens[index].kind == open) {
            ++depth;
        } else if (tokens[index].kind == close) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return tokens.size();
}

std::pair<std::string, size_t> parseDottedName(
    const std::vector<Token>& tokens, size_t begin) {
    if (begin >= tokens.size() ||
        tokens[begin].kind != TokenKind::Identifier) {
        return {{}, begin};
    }

    size_t end = begin + 1;
    while (end + 1 < tokens.size() &&
           tokens[end].kind == TokenKind::Dot &&
           tokens[end + 1].kind == TokenKind::Identifier) {
        end += 2;
    }
    std::string name;
    for (size_t index = begin; index < end; ++index) {
        name += tokens[index].text;
    }
    return {std::move(name), end};
}

bool isPositiveIntegerDimension(const std::vector<Token>& tokens) {
    if (tokens.size() != 1 || tokens.front().kind != TokenKind::Number) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(tokens.front().text.c_str(), &end);
    return end != tokens.front().text.c_str() && *end == '\0' &&
           std::isfinite(value) && value > 0.0 && std::floor(value) == value;
}

PropertyValidatorSpec parsePropertyValidator(
    const std::vector<Token>& tokens,
    std::vector<PropertyParseDiagnostic>& diagnostics) {
    PropertyValidatorSpec validator;
    if (tokens.empty()) {
        diagnostics.push_back(
            {SourceSpan{}, "expected property validation function"});
        return validator;
    }

    validator.raw = trimAsciiWhitespace(joinTokenTexts(tokens));
    validator.span = spanFromTokens(tokens);
    const auto [name, nameEnd] = parseDottedName(tokens, 0);
    validator.name = name;
    if (name.empty()) {
        diagnostics.push_back(
            {validator.span, "expected property validation function name"});
        return validator;
    }
    if (nameEnd == tokens.size()) {
        return validator;
    }
    if (tokens[nameEnd].kind != TokenKind::LParen) {
        diagnostics.push_back(
            {validator.span, "unexpected tokens after property validator"});
        return validator;
    }

    const size_t close = findMatchingDelimiter(
        tokens, nameEnd, TokenKind::LParen, TokenKind::RParen);
    if (close == tokens.size() || close + 1 != tokens.size()) {
        diagnostics.push_back(
            {validator.span, "malformed property validator arguments"});
        return validator;
    }

    const std::vector<Token> arguments(
        tokens.begin() + static_cast<std::ptrdiff_t>(nameEnd + 1),
        tokens.begin() + static_cast<std::ptrdiff_t>(close));
    if (arguments.empty()) {
        return validator;
    }
    for (const auto& argument : splitTopLevelCommas(arguments)) {
        if (argument.empty()) {
            diagnostics.push_back(
                {validator.span, "empty property validator argument"});
            continue;
        }
        validator.arguments.push_back(
            trimAsciiWhitespace(joinTokenTexts(argument)));
    }
    return validator;
}

PropertyDeclarationParseResult parsePropertyDeclarationTokens(
    const std::vector<Token>& sourceTokens, bool allowDottedName = false) {
    PropertyDeclarationParseResult result;
    const std::vector<Token> tokens = removeEllipses(sourceTokens);
    if (tokens.empty()) {
        result.diagnostics.push_back(
            {spanFromTokens(sourceTokens), "expected property declaration"});
        return result;
    }
    if (tokens.front().kind != TokenKind::Identifier) {
        result.diagnostics.push_back(
            {tokens.front().span, "expected property name"});
        return result;
    }

    size_t cursor = 1;
    if (allowDottedName) {
        if (tokens.size() >= 3 &&
            tokens[1].kind == TokenKind::Dot &&
            tokens[2].kind == TokenKind::Question) {
            result.label = tokens.front().text;
            const auto [className, classEnd] =
                parseDottedName(tokens, 3);
            if (className.empty()) {
                result.diagnostics.push_back(
                    {tokens[2].span,
                     "expected class name after '.?' in argument declaration"});
                return result;
            }
            result.nameValueSourceClass = className;
            result.nameValueSourceSpan =
                mergeSpans(tokens[2].span, tokens[classEnd - 1].span);
            if (classEnd != tokens.size()) {
                result.diagnostics.push_back(
                    {tokens[classEnd].span,
                     "unexpected tokens after class-property argument source"});
            }
            return result;
        }
        auto [name, end] = parseDottedName(tokens, 0);
        result.label = std::move(name);
        cursor = end;
    } else {
        result.label = tokens.front().text;
    }

    if (cursor < tokens.size() &&
        tokens[cursor].kind == TokenKind::LParen) {
        const size_t close = findMatchingDelimiter(
            tokens, cursor, TokenKind::LParen, TokenKind::RParen);
        if (close == tokens.size()) {
            result.diagnostics.push_back(
                {tokens[cursor].span, "unterminated property size declaration"});
            return result;
        }

        const std::vector<Token> dimensions(
            tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 1),
            tokens.begin() + static_cast<std::ptrdiff_t>(close));
        if (dimensions.empty()) {
            result.diagnostics.push_back(
                {tokens[cursor].span, "property size declaration is empty"});
        }
        for (const auto& dimension : splitTopLevelCommas(dimensions)) {
            PropertyDimensionSpec spec;
            spec.text = trimAsciiWhitespace(joinTokenTexts(dimension));
            spec.span = spanFromTokens(dimension);
            if (dimension.empty() ||
                !(dimension.size() == 1 &&
                  dimension.front().kind == TokenKind::Colon) &&
                    !isPositiveIntegerDimension(dimension)) {
                result.diagnostics.push_back(
                    {spec.span,
                     "property dimensions must be positive integers or ':'"});
            }
            result.spec.dimensions.push_back(std::move(spec));
        }
        cursor = close + 1;
    }

    if (cursor < tokens.size() &&
        tokens[cursor].kind == TokenKind::Identifier) {
        const size_t classBegin = cursor;
        auto [className, classEnd] = parseDottedName(tokens, cursor);
        result.spec.className = std::move(className);
        result.spec.classSpan = mergeSpans(tokens[classBegin].span,
                                           tokens[classEnd - 1].span);
        cursor = classEnd;
    }

    if (cursor < tokens.size() &&
        tokens[cursor].kind == TokenKind::LBrace) {
        const size_t close = findMatchingDelimiter(
            tokens, cursor, TokenKind::LBrace, TokenKind::RBrace);
        if (close == tokens.size()) {
            result.diagnostics.push_back(
                {tokens[cursor].span,
                 "unterminated property validation declaration"});
            return result;
        }
        const std::vector<Token> validators(
            tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 1),
            tokens.begin() + static_cast<std::ptrdiff_t>(close));
        if (validators.empty()) {
            result.diagnostics.push_back(
                {tokens[cursor].span,
                 "property validation declaration is empty"});
        }
        for (const auto& validator : splitTopLevelCommas(validators)) {
            result.spec.validators.push_back(
                parsePropertyValidator(validator, result.diagnostics));
        }
        cursor = close + 1;
    }

    if (cursor < tokens.size() && tokens[cursor].kind == TokenKind::Equal) {
        result.spec.hasExplicitDefault = true;
        ++cursor;
        result.defaultTokens.assign(
            tokens.begin() + static_cast<std::ptrdiff_t>(cursor), tokens.end());
        if (result.defaultTokens.empty()) {
            result.diagnostics.push_back(
                {tokens[cursor - 1].span,
                 "expected property default expression after '='"});
        }
        cursor = tokens.size();
    }

    if (cursor < tokens.size()) {
        result.diagnostics.push_back(
            {tokens[cursor].span, "unexpected tokens in property declaration"});
    }
    return result;
}

bool isBracketedOutputList(const std::vector<Token>& tokens) {
    return tokens.size() >= 2 && tokens.front().kind == TokenKind::LBracket &&
           tokens.back().kind == TokenKind::RBracket;
}

std::unique_ptr<SyntaxNode> buildOutputList(const std::vector<Token>& tokens) {
    auto node = makeNodeFromSpan(SyntaxKind::OutputList, spanFromTokens(tokens));
    node->raw = joinTokenTexts(tokens);

    if (!isBracketedOutputList(tokens)) {
        return node;
    }

    std::vector<Token> inner(tokens.begin() + 1, tokens.end() - 1);
    for (const auto& part : splitTopLevel(inner)) {
        if (part.empty()) {
            continue;
        }

        auto parsed = parseExpressionTokens(part);
        if (parsed.root) {
            node->children.push_back(std::move(parsed.root));
        }
    }

    return node;
}

bool isAssignableSyntax(const SyntaxNode& node) {
    switch (node.kind) {
    case SyntaxKind::IdentifierExpr:
    case SyntaxKind::MemberAccessExpr:
    case SyntaxKind::CallOrIndexExpr:
    case SyntaxKind::BraceIndexExpr:
    case SyntaxKind::IgnoredOutputExpr:
        return true;
    case SyntaxKind::OutputList:
        return std::all_of(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<SyntaxNode>& child) {
                               return child && isAssignableSyntax(*child);
                           });
    default:
        return false;
    }
}

std::unique_ptr<SyntaxNode> makeRawStatement(const std::vector<Token>& tokens);

bool isControlStatementKeyword(const std::vector<Token>& tokens) {
    return tokens.size() == 1 &&
           (tokens.front().kind == TokenKind::KeywordBreak ||
            tokens.front().kind == TokenKind::KeywordContinue ||
            tokens.front().kind == TokenKind::KeywordReturn);
}

std::unique_ptr<SyntaxNode> buildStatementLikeNode(
    const std::vector<Token>& tokens, SyntaxKind expressionWrapperKind) {
    if (tokens.empty()) {
        return makeRawStatement(tokens);
    }
    if (isControlStatementKeyword(tokens)) {
        return makeRawStatement(tokens);
    }

    const size_t assignmentIndex = findTopLevelAssignment(tokens);
    if (assignmentIndex != tokens.size()) {
        std::vector<Token> leftTokens(tokens.begin(),
                                      tokens.begin() +
                                          static_cast<std::ptrdiff_t>(
                                              assignmentIndex));
        std::vector<Token> rightTokens(
            tokens.begin() + static_cast<std::ptrdiff_t>(assignmentIndex + 1),
            tokens.end());

        std::unique_ptr<SyntaxNode> left;
        bool leftConsumed = true;
        if (isBracketedOutputList(leftTokens)) {
            left = buildOutputList(leftTokens);
        } else {
            auto parsedLeft = parseExpressionTokens(leftTokens);
            leftConsumed = parsedLeft.consumedAll;
            left = std::move(parsedLeft.root);
        }

        auto parsedRight = parseExpressionTokens(rightTokens);
        if (left && parsedRight.root && leftConsumed && parsedRight.consumedAll &&
            isAssignableSyntax(*left)) {
            auto assignment =
                makeNodeFromSpan(SyntaxKind::AssignmentStatement,
                                 spanFromTokens(tokens));
            assignment->label = "=";
            assignment->raw = joinTokenTexts(tokens);
            assignment->children.push_back(std::move(left));
            assignment->children.push_back(std::move(parsedRight.root));
            return assignment;
        }

        return makeRawStatement(tokens);
    }

    auto parsedExpression = parseExpressionTokens(tokens);
    if (parsedExpression.root && parsedExpression.consumedAll) {
        auto expression =
            makeNodeFromSpan(expressionWrapperKind, spanFromTokens(tokens));
        expression->raw = joinTokenTexts(tokens);
        expression->children.push_back(std::move(parsedExpression.root));
        return expression;
    }

    return makeRawStatement(tokens);
}

std::unique_ptr<SyntaxNode> buildControlHeader(const std::vector<Token>& tokens) {
    auto header = makeNodeFromSpan(SyntaxKind::ControlHeader, spanFromTokens(tokens));
    header->raw = joinTokenTexts(tokens);

    if (!tokens.empty()) {
        header->children.push_back(
            buildStatementLikeNode(tokens, SyntaxKind::ExpressionStatement));
    }

    return header;
}

std::unique_ptr<SyntaxNode> makeRawStatement(const std::vector<Token>& tokens) {
    auto node = makeNodeFromSpan(SyntaxKind::Statement, spanFromTokens(tokens));
    node->raw = joinTokenTexts(tokens);
    if (isControlStatementKeyword(tokens)) {
        node->label = tokens.front().text;
        return node;
    }

    for (const auto& token : tokens) {
        if (token.kind == TokenKind::Identifier) {
            node->label = token.text;
            break;
        }
    }
    return node;
}

} // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

ParseResult Parser::parse() {
    auto root = parseCompilationUnit();
    return ParseResult{std::move(root), std::move(diagnostics_)};
}

const Token& Parser::current() const {
    return tokens_[std::min(cursor_, tokens_.size() - 1)];
}

const Token& Parser::previous() const {
    if (cursor_ == 0) {
        return tokens_[0];
    }
    return tokens_[cursor_ - 1];
}

const Token& Parser::peek(size_t offset) const {
    return tokens_[std::min(cursor_ + offset, tokens_.size() - 1)];
}

bool Parser::at(TokenKind kind) const {
    return current().kind == kind;
}

bool Parser::atAny(std::initializer_list<TokenKind> kinds) const {
    return std::find(kinds.begin(), kinds.end(), current().kind) != kinds.end();
}

bool Parser::isAtEnd() const {
    return at(TokenKind::EndOfFile);
}

const Token& Parser::advance() {
    if (!isAtEnd()) {
        ++cursor_;
    }
    return previous();
}

bool Parser::match(TokenKind kind) {
    if (!at(kind)) {
        return false;
    }
    advance();
    return true;
}

std::unique_ptr<SyntaxNode> Parser::parseCompilationUnit() {
    auto root = makeNode(SyntaxKind::CompilationUnit, current().span.begin);
    skipSeparators();

    while (!isAtEnd()) {
        if (at(TokenKind::KeywordClassdef)) {
            root->children.push_back(parseClassDef());
        } else if (at(TokenKind::KeywordFunction)) {
            root->children.push_back(parseFunction());
        } else {
            root->children.push_back(parseStatement());
        }
        skipSeparators();
    }

    finishNode(*root);
    return root;
}

std::unique_ptr<SyntaxNode> Parser::parseClassDef() {
    auto node = makeNode(SyntaxKind::ClassDef, current().span.begin);
    advance();
    node->attributes = parseAttributeList();

    if (at(TokenKind::Identifier)) {
        node->label = current().text;
        advance();
    } else {
        diagnosticAtCurrent("expected class name after classdef");
    }

    if (match(TokenKind::Less)) {
        auto supers = std::make_unique<SyntaxNode>(SyntaxKind::SuperclassList);
        supers->span.begin = previous().span.begin;
        std::vector<Token> currentSuperclass;

        while (!isAtEnd() && !atAny({TokenKind::Newline, TokenKind::Semicolon})) {
            if (atAny({TokenKind::Ampersand, TokenKind::Comma})) {
                if (!currentSuperclass.empty()) {
                    auto superclass =
                        std::make_unique<SyntaxNode>(SyntaxKind::Superclass);
                    superclass->raw = joinTokenTexts(currentSuperclass);
                    superclass->label = trimAsciiWhitespace(superclass->raw);
                    superclass->span = mergeSpans(currentSuperclass.front().span,
                                                  currentSuperclass.back().span);
                    supers->children.push_back(std::move(superclass));
                    currentSuperclass.clear();
                }
                advance();
                continue;
            }

            if (atAny({TokenKind::Identifier, TokenKind::Dot})) {
                currentSuperclass.push_back(advance());
                continue;
            }

            break;
        }

        if (!currentSuperclass.empty()) {
            auto superclass = std::make_unique<SyntaxNode>(SyntaxKind::Superclass);
            superclass->raw = joinTokenTexts(currentSuperclass);
            superclass->label = trimAsciiWhitespace(superclass->raw);
            superclass->span = mergeSpans(currentSuperclass.front().span,
                                          currentSuperclass.back().span);
            supers->children.push_back(std::move(superclass));
        }

        if (supers->children.empty()) {
            diagnosticAtCurrent("expected superclass name after <");
        }

        finishNode(*supers);
        node->children.push_back(std::move(supers));
    }

    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        if (at(TokenKind::KeywordProperties)) {
            node->children.push_back(parsePropertiesBlock());
        } else if (at(TokenKind::KeywordMethods)) {
            node->children.push_back(parseMethodsBlock());
        } else if (at(TokenKind::KeywordEvents)) {
            node->children.push_back(parseEventsBlock());
        } else if (at(TokenKind::KeywordEnumeration)) {
            node->children.push_back(parseEnumerationBlock());
        } else {
            node->children.push_back(parseStatement());
        }
    }

    consumeExpectedEnd("classdef");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parsePropertiesBlock() {
    auto node = makeNode(SyntaxKind::PropertiesBlock, current().span.begin);
    advance();
    node->attributes = parseAttributeList();
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        auto declaration = makeNode(SyntaxKind::PropertyDecl, current().span.begin);
        const auto tokens = collectUntilSeparator();
        declaration->raw = joinTokens(tokens);
        declaration->attributes = node->attributes;
        auto parsed = parsePropertyDeclarationTokens(tokens);
        declaration->label = std::move(parsed.label);
        declaration->property = std::move(parsed.spec);
        for (auto& diagnostic : parsed.diagnostics) {
            diagnostics_.push_back(
                Diagnostic{diagnostic.span, std::move(diagnostic.message)});
        }
        if (declaration->property.hasExplicitDefault &&
            !parsed.defaultTokens.empty()) {
            auto expression = parseExpressionTokens(parsed.defaultTokens);
            if (expression.root && expression.consumedAll) {
                declaration->children.push_back(std::move(expression.root));
            } else {
                diagnostics_.push_back(Diagnostic{
                    spanFromTokens(parsed.defaultTokens),
                    "unable to parse property default expression"});
                if (expression.root) {
                    declaration->children.push_back(std::move(expression.root));
                }
            }
        }
        consumeSeparator();
        finishNode(*declaration);
        node->children.push_back(std::move(declaration));
    }

    consumeExpectedEnd("properties block");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseMethodsBlock() {
    auto node = makeNode(SyntaxKind::MethodsBlock, current().span.begin);
    advance();
    node->attributes = parseAttributeList();
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        if (at(TokenKind::KeywordFunction)) {
            auto method = parseFunction();
            method->attributes = node->attributes;
            node->children.push_back(std::move(method));
            continue;
        }

        auto method = makeNode(SyntaxKind::MethodPrototype, current().span.begin);
        const auto tokens = collectUntilSeparator();
        method->raw = joinTokens(tokens);
        method->label = functionNameFromHeader(tokens);
        method->attributes = node->attributes;
        consumeSeparator();
        finishNode(*method);
        node->children.push_back(std::move(method));
    }

    consumeExpectedEnd("methods block");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseEventsBlock() {
    auto node = makeNode(SyntaxKind::EventsBlock, current().span.begin);
    advance();
    node->attributes = parseAttributeList();
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        const auto declarationTokens =
            removeEllipses(collectUntilSeparator());
        for (const auto& tokens : splitTopLevelCommas(declarationTokens)) {
            if (tokens.empty()) {
                diagnostics_.push_back(Diagnostic{
                    current().span, "expected event name after comma"});
                continue;
            }

            auto event = makeNode(SyntaxKind::EventDecl,
                                  tokens.front().span.begin);
            event->raw = joinTokens(tokens);
            event->attributes = node->attributes;
            event->span = mergeSpans(tokens.front().span,
                                     tokens.back().span);
            if (tokens.size() != 1 ||
                tokens.front().kind != TokenKind::Identifier) {
                diagnostics_.push_back(Diagnostic{
                    event->span,
                    "event declaration must contain one identifier"});
            } else {
                event->label = tokens.front().text;
            }
            node->children.push_back(std::move(event));
        }
        consumeSeparator();
    }

    consumeExpectedEnd("events block");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseEnumerationBlock() {
    auto node = makeNode(SyntaxKind::EnumerationBlock, current().span.begin);
    advance();
    node->attributes = parseAttributeList();
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        const auto declarationTokens =
            removeEllipses(collectUntilSeparator());
        for (const auto& tokens : splitTopLevelCommas(declarationTokens)) {
            if (tokens.empty()) {
                diagnostics_.push_back(Diagnostic{
                    current().span,
                    "expected enumeration member after comma"});
                continue;
            }

            auto member = makeNode(SyntaxKind::EnumMember,
                                   tokens.front().span.begin);
            member->raw = joinTokens(tokens);
            member->attributes = node->attributes;
            member->span = mergeSpans(tokens.front().span,
                                      tokens.back().span);
            if (tokens.front().kind != TokenKind::Identifier) {
                diagnostics_.push_back(Diagnostic{
                    member->span,
                    "enumeration member must begin with an identifier"});
                node->children.push_back(std::move(member));
                continue;
            }
            member->label = tokens.front().text;

            if (tokens.size() > 1) {
                const size_t close =
                    tokens[1].kind == TokenKind::LParen
                        ? findMatchingDelimiter(tokens, 1, TokenKind::LParen,
                                                TokenKind::RParen)
                        : tokens.size();
                if (close != tokens.size() - 1) {
                    diagnostics_.push_back(Diagnostic{
                        member->span,
                        "enumeration member suffix must be a constructor "
                        "argument list"});
                } else {
                    const std::vector<Token> arguments(
                        tokens.begin() + 2,
                        tokens.begin() + static_cast<std::ptrdiff_t>(close));
                    if (!arguments.empty()) {
                        for (const auto& argument :
                             splitTopLevelCommas(arguments)) {
                            if (argument.empty()) {
                                diagnostics_.push_back(Diagnostic{
                                    member->span,
                                    "enumeration constructor argument cannot "
                                    "be empty"});
                                continue;
                            }
                            auto expression = parseExpressionTokens(argument);
                            if (!expression.root ||
                                !expression.consumedAll) {
                                diagnostics_.push_back(Diagnostic{
                                    spanFromTokens(argument),
                                    "unable to parse enumeration constructor "
                                    "argument"});
                                continue;
                            }
                            member->children.push_back(
                                std::move(expression.root));
                        }
                    }
                }
            }

            node->children.push_back(std::move(member));
        }
        consumeSeparator();
    }

    consumeExpectedEnd("enumeration block");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseFunction() {
    auto node = makeNode(SyntaxKind::FunctionDef, current().span.begin);
    advance();

    const auto header = collectUntilSeparator();
    node->raw = joinTokens(header);
    node->label = functionNameFromHeader(header);
    consumeSeparator();

    parseBodyUntilEnd(*node);
    consumeExpectedEnd("function");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseArgumentsBlock() {
    auto node = makeNode(SyntaxKind::ArgumentsBlock, current().span.begin);
    advance();
    node->attributes = parseAttributeList();
    if (!node->attributes.empty()) {
        node->raw = "(";
        for (size_t index = 0; index < node->attributes.size(); ++index) {
            if (index != 0) {
                node->raw += ", ";
            }
            node->raw += node->attributes[index].raw;
        }
        node->raw += ")";
    }

    bool sawInput = false;
    bool sawOutput = false;
    bool sawRepeating = false;
    for (const auto& attribute : node->attributes) {
        bool* seen = nullptr;
        if (attribute.name == "Input") {
            seen = &sawInput;
        } else if (attribute.name == "Output") {
            seen = &sawOutput;
        } else if (attribute.name == "Repeating") {
            seen = &sawRepeating;
        } else {
            diagnostics_.push_back(Diagnostic{
                attribute.span,
                "unsupported arguments block attribute: " + attribute.raw});
            node->argumentBlock.valid = false;
            continue;
        }
        if (attribute.negated || !attribute.value.empty()) {
            diagnostics_.push_back(Diagnostic{
                attribute.span,
                "arguments block attributes do not accept values or negation: " +
                    attribute.raw});
            node->argumentBlock.valid = false;
        }
        if (*seen) {
            diagnostics_.push_back(Diagnostic{
                attribute.span,
                "duplicate arguments block attribute: " + attribute.name});
            node->argumentBlock.valid = false;
        }
        *seen = true;
    }
    if (sawInput && sawOutput) {
        diagnostics_.push_back(Diagnostic{
            node->span,
            "arguments block cannot be both Input and Output"});
        node->argumentBlock.valid = false;
    }
    node->argumentBlock.explicitInput = sawInput;
    node->argumentBlock.explicitOutput = sawOutput;
    node->argumentBlock.repeating = sawRepeating;
    if (sawOutput) {
        node->argumentBlock.kind =
            sawRepeating ? ArgumentBlockKind::RepeatingOutput
                         : ArgumentBlockKind::Output;
    } else {
        node->argumentBlock.kind =
            sawRepeating ? ArgumentBlockKind::RepeatingInput
                         : ArgumentBlockKind::Input;
    }

    const auto trailingHeader = collectUntilSeparator();
    if (!trailingHeader.empty()) {
        diagnostics_.push_back(Diagnostic{
            spanFromTokens(trailingHeader),
            "unexpected tokens in arguments block header: " +
                joinTokens(trailingHeader)});
        node->argumentBlock.valid = false;
        if (!node->raw.empty()) {
            node->raw += " ";
        }
        node->raw += joinTokens(trailingHeader);
    }
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }
        auto declaration =
            makeNode(SyntaxKind::ArgumentDecl, current().span.begin);
        const auto tokens = collectUntilSeparator();
        declaration->raw = joinTokens(tokens);
        auto parsed = parsePropertyDeclarationTokens(tokens, true);
        declaration->label = std::move(parsed.label);
        declaration->nameValueSourceClass =
            std::move(parsed.nameValueSourceClass);
        declaration->nameValueSourceSpan = parsed.nameValueSourceSpan;
        declaration->property = std::move(parsed.spec);
        for (auto& diagnostic : parsed.diagnostics) {
            std::string message = std::move(diagnostic.message);
            const std::string property = "property";
            if (const size_t at = message.find(property);
                at != std::string::npos) {
                message.replace(at, property.size(), "argument");
            }
            diagnostics_.push_back(
                Diagnostic{diagnostic.span, std::move(message)});
        }
        if (declaration->property.hasExplicitDefault &&
            !parsed.defaultTokens.empty()) {
            auto expression = parseExpressionTokens(parsed.defaultTokens);
            if (expression.root && expression.consumedAll) {
                declaration->children.push_back(std::move(expression.root));
            } else {
                diagnostics_.push_back(Diagnostic{
                    spanFromTokens(parsed.defaultTokens),
                    "unable to parse argument default expression"});
            }
        }
        consumeSeparator();
        finishNode(*declaration);
        node->children.push_back(std::move(declaration));
    }

    consumeExpectedEnd("arguments block");
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseControlBlock() {
    const TokenKind opener = current().kind;
    auto node = makeNode(syntaxKindForControlKeyword(opener), current().span.begin);
    node->label = current().text;
    advance();

    const auto header = collectUntilSeparator();
    node->raw = joinTokens(header);
    if (!header.empty()) {
        node->children.push_back(buildControlHeader(header));
    }
    consumeSeparator();

    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        if (isControlArm(current().kind)) {
            auto arm = makeNode(SyntaxKind::ControlArm, current().span.begin);
            arm->label = current().text;
            advance();
            const auto armHeader = collectUntilSeparator();
            arm->raw = joinTokens(armHeader);
            if (!armHeader.empty()) {
                arm->children.push_back(buildControlHeader(armHeader));
            }
            consumeSeparator();
            finishNode(*arm);
            node->children.push_back(std::move(arm));
            continue;
        }

        if (at(TokenKind::KeywordFunction)) {
            node->children.push_back(parseFunction());
        } else if (at(TokenKind::KeywordArguments)) {
            node->children.push_back(parseArgumentsBlock());
        } else if (isControlBlockStart(current().kind)) {
            node->children.push_back(parseControlBlock());
        } else {
            node->children.push_back(parseStatement());
        }
    }

    consumeExpectedEnd(tokenKindName(opener));
    finishNode(*node);
    return node;
}

std::unique_ptr<SyntaxNode> Parser::parseStatement() {
    if (at(TokenKind::KeywordArguments)) {
        return parseArgumentsBlock();
    }

    if (at(TokenKind::KeywordImport)) {
        return parseImportStatement();
    }

    if (isControlBlockStart(current().kind)) {
        return parseControlBlock();
    }

    const auto tokens = collectUntilSeparator();
    consumeSeparator();

    return buildStatementLikeNode(tokens, SyntaxKind::ExpressionStatement);
}

std::unique_ptr<SyntaxNode> Parser::parseImportStatement() {
    auto node = makeNode(SyntaxKind::ImportStatement, current().span.begin);
    advance();
    const auto tokens = collectUntilSeparator();
    node->raw = joinTokens(tokens);
    consumeSeparator();

    size_t index = 0;
    while (index < tokens.size()) {
        if (tokens[index].kind == TokenKind::Comma) {
            ++index;
            continue;
        }
        if (tokens[index].kind != TokenKind::Identifier) {
            diagnostics_.push_back(Diagnostic{
                tokens[index].span, "expected namespace name after import"});
            ++index;
            continue;
        }

        auto item = std::make_unique<SyntaxNode>(SyntaxKind::ImportItem);
        item->span = tokens[index].span;
        item->label = tokens[index].text;
        ++index;

        bool malformed = false;
        while (index < tokens.size() &&
               (tokens[index].kind == TokenKind::Dot ||
                tokens[index].kind == TokenKind::DotStar)) {
            if (tokens[index].kind == TokenKind::DotStar) {
                item->label += ".*";
                item->span.end = tokens[index].span.end;
                ++index;
                break;
            }
            const Token& dot = tokens[index++];
            if (index >= tokens.size() ||
                (tokens[index].kind != TokenKind::Identifier &&
                 tokens[index].kind != TokenKind::Star)) {
                diagnostics_.push_back(Diagnostic{
                    dot.span, "expected namespace member after '.' in import"});
                malformed = true;
                break;
            }
            item->label += "." + tokens[index].text;
            item->span.end = tokens[index].span.end;
            const bool wildcard = tokens[index].kind == TokenKind::Star;
            ++index;
            if (wildcard) {
                break;
            }
        }

        item->raw = item->label;
        if (!malformed) {
            node->children.push_back(std::move(item));
        }
    }

    finishNode(*node);
    return node;
}

void Parser::parseBodyUntilEnd(SyntaxNode& parent) {
    while (!isAtEnd() && !at(TokenKind::KeywordEnd)) {
        skipSeparators();
        if (at(TokenKind::KeywordEnd) || isAtEnd()) {
            break;
        }

        if (at(TokenKind::KeywordFunction)) {
            parent.children.push_back(parseFunction());
        } else if (at(TokenKind::KeywordArguments)) {
            parent.children.push_back(parseArgumentsBlock());
        } else if (isControlBlockStart(current().kind)) {
            parent.children.push_back(parseControlBlock());
        } else {
            parent.children.push_back(parseStatement());
        }
    }
}

bool Parser::isControlBlockStart(TokenKind kind) const {
    switch (kind) {
    case TokenKind::KeywordFor:
    case TokenKind::KeywordIf:
    case TokenKind::KeywordParfor:
    case TokenKind::KeywordSpmd:
    case TokenKind::KeywordSwitch:
    case TokenKind::KeywordTry:
    case TokenKind::KeywordWhile:
        return true;
    default:
        return false;
    }
}

bool Parser::isControlArm(TokenKind kind) const {
    switch (kind) {
    case TokenKind::KeywordCase:
    case TokenKind::KeywordCatch:
    case TokenKind::KeywordElse:
    case TokenKind::KeywordElseif:
    case TokenKind::KeywordOtherwise:
        return true;
    default:
        return false;
    }
}

std::vector<AttributeSyntax> Parser::parseAttributeList() {
    std::vector<AttributeSyntax> attributes;
    if (!match(TokenKind::LParen)) {
        return attributes;
    }

    std::vector<Token> currentAttribute;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    while (!isAtEnd()) {
        if (at(TokenKind::RParen) && atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            if (!currentAttribute.empty()) {
                attributes.push_back(buildAttribute(currentAttribute));
                currentAttribute.clear();
            }
            advance();
            return attributes;
        }

        if (at(TokenKind::Comma) && atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            if (!currentAttribute.empty()) {
                attributes.push_back(buildAttribute(currentAttribute));
                currentAttribute.clear();
            }
            advance();
            continue;
        }

        updateDepth(current().kind, parenDepth, bracketDepth, braceDepth);
        currentAttribute.push_back(advance());
    }

    diagnosticAtCurrent("unterminated attribute list");
    return attributes;
}

AttributeSyntax Parser::buildAttribute(const std::vector<Token>& tokens) const {
    AttributeSyntax attribute;
    if (tokens.empty()) {
        return attribute;
    }

    attribute.span = mergeSpans(tokens.front().span, tokens.back().span);
    attribute.raw = joinTokens(tokens);

    size_t index = 0;
    if (tokens[index].kind == TokenKind::Tilde) {
        attribute.negated = true;
        ++index;
    }

    if (index < tokens.size() && tokens[index].kind == TokenKind::Identifier) {
        attribute.name = tokens[index].text;
    }

    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Equal &&
            atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            std::vector<Token> valueTokens(
                tokens.begin() + static_cast<std::ptrdiff_t>(i + 1),
                tokens.end());
            attribute.value = joinTokens(valueTokens);
            if (auto classNames = parseMetaClassList(valueTokens)) {
                attribute.hasMetaClassList = true;
                attribute.metaClassNames = std::move(*classNames);
            }
            break;
        }
        updateDepth(tokens[i].kind, parenDepth, bracketDepth, braceDepth);
    }

    return attribute;
}

std::vector<Token> Parser::collectUntilSeparator() {
    std::vector<Token> tokens;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    while (!isAtEnd()) {
        if (atTopLevel(parenDepth, bracketDepth, braceDepth) &&
            atAny({TokenKind::Newline, TokenKind::Semicolon})) {
            break;
        }

        if (at(TokenKind::Ellipsis)) {
            advance();
            if (at(TokenKind::Newline)) {
                advance();
            }
            continue;
        }

        updateDepth(current().kind, parenDepth, bracketDepth, braceDepth);
        tokens.push_back(advance());
    }

    return tokens;
}

void Parser::consumeSeparator() {
    while (atAny({TokenKind::Newline, TokenKind::Semicolon, TokenKind::Comma})) {
        advance();
    }
}

void Parser::skipSeparators() {
    consumeSeparator();
}

void Parser::consumeExpectedEnd(const char* owner) {
    if (match(TokenKind::KeywordEnd)) {
        consumeSeparator();
        return;
    }

    std::ostringstream message;
    message << "expected end for " << owner;
    diagnosticAtCurrent(message.str());
}

std::string Parser::joinTokens(const std::vector<Token>& tokens) const {
    return joinTokenTexts(tokens);
}

std::string Parser::firstIdentifier(const std::vector<Token>& tokens) const {
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::Identifier) {
            return token.text;
        }
    }
    return {};
}

std::string Parser::functionNameFromHeader(const std::vector<Token>& tokens) const {
    size_t start = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Equal &&
            atTopLevel(parenDepth, bracketDepth, braceDepth)) {
            start = i + 1;
            break;
        }
        updateDepth(tokens[i].kind, parenDepth, bracketDepth, braceDepth);
    }

    for (size_t i = start; i < tokens.size(); ++i) {
        if (tokens[i].kind == TokenKind::Identifier) {
            std::string name = tokens[i].text;
            size_t cursor = i + 1;
            while (cursor + 1 < tokens.size() &&
                   tokens[cursor].kind == TokenKind::Dot &&
                   tokens[cursor + 1].kind == TokenKind::Identifier) {
                name += "." + tokens[cursor + 1].text;
                cursor += 2;
            }
            return name;
        }
    }

    return {};
}

std::unique_ptr<SyntaxNode> Parser::makeNode(SyntaxKind kind,
                                             SourcePosition begin) {
    auto node = std::make_unique<SyntaxNode>(kind);
    node->span.begin = begin;
    node->span.end = begin;
    return node;
}

void Parser::finishNode(SyntaxNode& node) {
    if (cursor_ == 0) {
        node.span.end = current().span.end;
        return;
    }
    node.span.end = previous().span.end;
}

void Parser::diagnosticAtCurrent(std::string message) {
    diagnostics_.push_back(Diagnostic{current().span, std::move(message)});
}

} // namespace mparser
