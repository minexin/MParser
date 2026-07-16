#include "mparser/function_signature.h"

#include <algorithm>
#include <cctype>
#include <iterator>
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

std::string_view nameValueRoot(std::string_view name) {
    const size_t dot = name.find('.');
    return dot == std::string_view::npos ? std::string_view{}
                                         : name.substr(0, dot);
}

void setParameterKind(FunctionSignature& signature, std::string_view name,
                      FunctionParameterKind kind) {
    const auto found =
        std::find(signature.parameters.begin(), signature.parameters.end(), name);
    if (found == signature.parameters.end()) {
        return;
    }
    const size_t index =
        static_cast<size_t>(std::distance(signature.parameters.begin(), found));
    if (index < signature.parameterKinds.size()) {
        signature.parameterKinds[index] = kind;
    }
}

} // namespace

FunctionSignature parseFunctionSignature(std::string_view rawDeclaration,
                                         std::string_view sourceName) {
    FunctionSignature signature;
    std::string text = trim(rawDeclaration);
    if (const size_t local = sourceName.find_last_of('>');
        local != std::string_view::npos) {
        sourceName.remove_prefix(local + 1);
    } else if (text.find(sourceName) == std::string::npos) {
        if (const size_t qualified = sourceName.find_last_of('.');
            qualified != std::string_view::npos) {
            sourceName.remove_prefix(qualified + 1);
        }
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
            signature.parameterKinds.push_back(
                FunctionParameterKind::Positional);
        }
    }
    signature.requiredPositionalParameterCount = signature.parameters.size();

    return signature;
}

FunctionSignature parseFunctionSignature(const HirNode& functionNode) {
    FunctionSignature signature =
        parseFunctionSignature(functionNode.raw, functionNode.label);
    for (const auto& block : functionNode.children) {
        if (block->kind != HirKind::ArgumentBlock) {
            continue;
        }
        if (block->argumentBlock.kind != ArgumentBlockKind::Input &&
            block->argumentBlock.kind != ArgumentBlockKind::RepeatingInput) {
            continue;
        }
        for (const auto& declaration : block->children) {
            if (declaration->kind != HirKind::Argument) {
                continue;
            }
            if (const std::string_view root =
                    nameValueRoot(declaration->label);
                !root.empty()) {
                setParameterKind(signature, root,
                                 FunctionParameterKind::NameValue);
                continue;
            }
            if (block->argumentBlock.kind ==
                ArgumentBlockKind::RepeatingInput) {
                if (declaration->label == "varargin") {
                    signature.vararginRepeating = true;
                } else {
                    setParameterKind(signature, declaration->label,
                                     FunctionParameterKind::Repeating);
                }
            }
        }
    }

    signature.requiredPositionalParameterCount = 0;
    const size_t positionalCount = functionPositionalParameterCount(signature);
    for (size_t index = 0; index < positionalCount; ++index) {
        bool hasDefault = false;
        for (const auto& block : functionNode.children) {
            if (block->kind != HirKind::ArgumentBlock ||
                block->argumentBlock.kind != ArgumentBlockKind::Input) {
                continue;
            }
            const auto declaration = std::find_if(
                block->children.begin(), block->children.end(),
                [&](const auto& candidate) {
                    return candidate->kind == HirKind::Argument &&
                           candidate->label == signature.parameters[index];
                });
            if (declaration != block->children.end()) {
                hasDefault = (*declaration)->property.hasExplicitDefault;
                break;
            }
        }
        if (!hasDefault) {
            signature.requiredPositionalParameterCount = index + 1;
        }
    }
    return signature;
}

FunctionParameterKind functionParameterKind(const FunctionSignature& signature,
                                            size_t index) {
    return index < signature.parameterKinds.size()
               ? signature.parameterKinds[index]
               : FunctionParameterKind::Positional;
}

size_t functionPositionalParameterCount(const FunctionSignature& signature) {
    size_t count = 0;
    while (count < signature.parameters.size() &&
           functionParameterKind(signature, count) ==
               FunctionParameterKind::Positional) {
        ++count;
    }
    return count;
}

size_t functionRequiredPositionalParameterCount(
    const FunctionSignature& signature) {
    return std::min(signature.requiredPositionalParameterCount,
                    functionPositionalParameterCount(signature));
}

size_t functionRepeatingParameterCount(const FunctionSignature& signature) {
    return static_cast<size_t>(std::count(
        signature.parameterKinds.begin(), signature.parameterKinds.end(),
        FunctionParameterKind::Repeating));
}

bool functionHasNameValueParameters(const FunctionSignature& signature) {
    return std::find(signature.parameterKinds.begin(),
                     signature.parameterKinds.end(),
                     FunctionParameterKind::NameValue) !=
           signature.parameterKinds.end();
}

FunctionArgumentCountStatus functionArgumentCountStatus(
    const FunctionSignature& signature, size_t argumentCount) {
    if (functionHasNameValueParameters(signature)) {
        return argumentCount <
                       functionRequiredPositionalParameterCount(signature)
                   ? FunctionArgumentCountStatus::Mismatch
                   : FunctionArgumentCountStatus::Valid;
    }
    return functionPositionalArgumentCountStatus(signature, argumentCount);
}

FunctionArgumentCountStatus functionPositionalArgumentCountStatus(
    const FunctionSignature& signature, size_t argumentCount) {
    if (argumentCount <
        functionRequiredPositionalParameterCount(signature)) {
        return FunctionArgumentCountStatus::Mismatch;
    }

    const size_t positionalCount =
        functionPositionalParameterCount(signature);
    const size_t repeatingWidth =
        functionRepeatingParameterCount(signature);
    if (repeatingWidth != 0) {
        if (argumentCount < positionalCount) {
            return FunctionArgumentCountStatus::Valid;
        }
        return (argumentCount - positionalCount) % repeatingWidth == 0
                   ? FunctionArgumentCountStatus::Valid
                   : FunctionArgumentCountStatus::IncompleteRepeatingGroup;
    }
    if (!signature.hasVarargin && argumentCount > positionalCount) {
        return FunctionArgumentCountStatus::Mismatch;
    }
    return FunctionArgumentCountStatus::Valid;
}

} // namespace mparser
