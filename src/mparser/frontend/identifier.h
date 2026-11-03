#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace mparser {

// The compatibility baseline is MATLAB R2024b, not the R2025a length change.
inline constexpr std::size_t kIdentifierMaximumLength = 63;
inline constexpr std::array<std::string_view, 20> kLanguageKeywords = {
    "break", "case", "catch", "classdef", "continue", "else", "elseif",
    "end", "for", "function", "global", "if", "otherwise", "parfor",
    "persistent", "return", "spmd", "switch", "try", "while"};

enum class VariableNameStatus {
    Valid,
    Empty,
    TooLong,
    InvalidStart,
    InvalidCharacter,
    ReservedKeyword
};

constexpr bool isAsciiLetter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

constexpr bool isLanguageKeyword(std::string_view name) noexcept {
    for (const auto keyword : kLanguageKeywords) {
        if (name == keyword) {
            return true;
        }
    }
    return false;
}

constexpr VariableNameStatus validateVariableName(std::string_view name) noexcept {
    if (name.empty()) {
        return VariableNameStatus::Empty;
    }
    if (name.size() > kIdentifierMaximumLength) {
        return VariableNameStatus::TooLong;
    }
    if (!isAsciiLetter(name.front())) {
        return VariableNameStatus::InvalidStart;
    }
    for (const char value : name) {
        if (!isAsciiLetter(value) && !(value >= '0' && value <= '9') &&
            value != '_') {
            return VariableNameStatus::InvalidCharacter;
        }
    }
    return isLanguageKeyword(name) ? VariableNameStatus::ReservedKeyword
                                   : VariableNameStatus::Valid;
}

} // namespace mparser
