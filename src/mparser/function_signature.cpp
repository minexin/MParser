#include "mparser/function_signature.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace mparser {
namespace {

std::string trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> splitCommaList(std::string_view text) {
    std::vector<std::string> parts;
    size_t begin = 0;
    for (size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == ',') {
            parts.push_back(trim(text.substr(begin, index - begin)));
            begin = index + 1;
        }
    }
    return parts;
}

bool isIdentifierText(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(text.front());
    if (std::isalpha(first) == 0 && text.front() != '_') {
        return false;
    }

    return std::all_of(text.begin() + 1, text.end(), [](char c) {
        const unsigned char value = static_cast<unsigned char>(c);
        return std::isalnum(value) != 0 || c == '_';
    });
}

} // namespace

FunctionSignature parseFunctionSignature(const HirNode& functionNode) {
    FunctionSignature signature;
    std::string text = trim(functionNode.raw);
    std::string_view sourceName = functionNode.label;
    if (const size_t local = sourceName.find_last_of('>');
        local != std::string_view::npos) {
        sourceName.remove_prefix(local + 1);
    } else if (const size_t dot = sourceName.find_last_of('.');
        dot != std::string_view::npos) {
        sourceName.remove_prefix(dot + 1);
    }

    const auto equal = text.find('=');
    std::string declaration = text;
    if (equal != std::string::npos) {
        std::string outputs = trim(text.substr(0, equal));
        declaration = trim(text.substr(equal + 1));

        if (outputs.size() >= 2 && outputs.front() == '[' &&
            outputs.back() == ']') {
            outputs = outputs.substr(1, outputs.size() - 2);
        }

        for (const auto& part : splitCommaList(outputs)) {
            if (part == "varargout") {
                signature.hasVarargout = true;
            } else if (isIdentifierText(part)) {
                signature.outputs.push_back(part);
            }
        }
    }

    const auto namePosition = declaration.find(sourceName);
    if (namePosition == std::string::npos) {
        return signature;
    }

    const auto open =
        declaration.find('(', namePosition + sourceName.size());
    if (open == std::string::npos) {
        return signature;
    }

    const auto close = declaration.find_last_of(')');
    if (close == std::string::npos || close <= open) {
        return signature;
    }

    const std::string parameters =
        declaration.substr(open + 1, close - open - 1);
    for (const auto& part : splitCommaList(parameters)) {
        if (part == "varargin") {
            signature.hasVarargin = true;
        } else if (isIdentifierText(part) && part != "~") {
            signature.parameters.push_back(part);
        }
    }

    return signature;
}

} // namespace mparser
