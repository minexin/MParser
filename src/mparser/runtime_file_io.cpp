#include "mparser/runtime_file_io.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

constexpr size_t kMaximumScanWidth = 1024U * 1024U;
constexpr size_t kMaximumScanOutputElements = 16U * 1024U * 1024U;

enum class ScanTokenKind {
    Whitespace,
    Literal,
    Conversion,
};

enum class ScanConversionKind {
    Character,
    String,
    SignedDecimal,
    SignedAutomatic,
    UnsignedDecimal,
    UnsignedOctal,
    UnsignedHexadecimal,
    Floating,
};

enum class ScanOutputKind {
    Character,
    Double,
    Signed64,
    Unsigned64,
};

struct ScanConversion {
    ScanConversionKind kind = ScanConversionKind::String;
    bool suppressed = false;
    bool longInteger = false;
    std::optional<size_t> width;
};

struct ScanToken {
    ScanTokenKind kind = ScanTokenKind::Literal;
    char literal = '\0';
    ScanConversion conversion;
};

struct ScanElement {
    bool character = false;
    char16_t codeUnit = 0;
    RuntimeNumericElementValue numeric;
};

struct ParsedFormat {
    bool succeeded = false;
    bool hasConversion = false;
    ScanOutputKind outputKind = ScanOutputKind::Character;
    std::vector<ScanToken> tokens;
    std::string error;
};

RuntimeFileScanResult failure(std::string message) {
    RuntimeFileScanResult result;
    result.error = std::move(message);
    return result;
}

bool inputWhitespace(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string decodeEscapes(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\' || index + 1 >= text.size()) {
            result.push_back(text[index]);
            continue;
        }
        switch (text[++index]) {
        case 'n':
            result.push_back('\n');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case 't':
            result.push_back('\t');
            break;
        case 'b':
            result.push_back('\b');
            break;
        case 'f':
            result.push_back('\f');
            break;
        case '\\':
            result.push_back('\\');
            break;
        default:
            result.push_back('\\');
            result.push_back(text[index]);
            break;
        }
    }
    return result;
}

std::optional<ScanConversionKind> conversionKind(char value) {
    switch (value) {
    case 'c':
        return ScanConversionKind::Character;
    case 's':
        return ScanConversionKind::String;
    case 'd':
        return ScanConversionKind::SignedDecimal;
    case 'i':
        return ScanConversionKind::SignedAutomatic;
    case 'u':
        return ScanConversionKind::UnsignedDecimal;
    case 'o':
        return ScanConversionKind::UnsignedOctal;
    case 'x':
    case 'X':
        return ScanConversionKind::UnsignedHexadecimal;
    case 'e':
    case 'E':
    case 'f':
    case 'g':
    case 'G':
        return ScanConversionKind::Floating;
    default:
        return std::nullopt;
    }
}

bool isCharacterConversion(ScanConversionKind kind) {
    return kind == ScanConversionKind::Character ||
           kind == ScanConversionKind::String;
}

ParsedFormat parseFormat(std::string_view encoded) {
    ParsedFormat result;
    bool hasCharacter = false;
    bool hasNumeric = false;
    bool signed64Only = true;
    bool unsigned64Only = true;
    const std::string format = decodeEscapes(encoded);
    for (size_t index = 0; index < format.size();) {
        if (inputWhitespace(format[index])) {
            while (index < format.size() && inputWhitespace(format[index])) {
                ++index;
            }
            result.tokens.push_back(
                ScanToken{ScanTokenKind::Whitespace});
            continue;
        }
        if (format[index] != '%') {
            ScanToken token;
            token.kind = ScanTokenKind::Literal;
            token.literal = format[index++];
            result.tokens.push_back(token);
            continue;
        }
        ++index;
        if (index < format.size() && format[index] == '%') {
            ScanToken token;
            token.kind = ScanTokenKind::Literal;
            token.literal = '%';
            result.tokens.push_back(token);
            ++index;
            continue;
        }

        ScanToken token;
        token.kind = ScanTokenKind::Conversion;
        if (index < format.size() && format[index] == '*') {
            token.conversion.suppressed = true;
            ++index;
        }
        size_t width = 0;
        bool hasWidth = false;
        while (index < format.size() &&
               std::isdigit(static_cast<unsigned char>(format[index]))) {
            hasWidth = true;
            const unsigned digit =
                static_cast<unsigned>(format[index++] - '0');
            if (width > (kMaximumScanWidth - digit) / 10U) {
                result.error = "fscanf field width exceeds the runtime limit";
                return result;
            }
            width = width * 10U + digit;
        }
        if (hasWidth) {
            if (width == 0 || width > kMaximumScanWidth) {
                result.error =
                    "fscanf field width must be between 1 and 1048576";
                return result;
            }
            token.conversion.width = width;
        }
        if (index < format.size() && format[index] == 'l') {
            token.conversion.longInteger = true;
            ++index;
        }
        if (index >= format.size()) {
            result.error = "fscanf format ends inside a conversion";
            return result;
        }
        const auto kind = conversionKind(format[index++]);
        if (!kind) {
            result.error = "fscanf format contains an unsupported conversion";
            return result;
        }
        token.conversion.kind = *kind;
        if (token.conversion.longInteger &&
            (*kind == ScanConversionKind::Character ||
             *kind == ScanConversionKind::String ||
             *kind == ScanConversionKind::Floating)) {
            result.error =
                "fscanf long modifier is supported only for integer conversions";
            return result;
        }
        result.hasConversion = true;
        if (isCharacterConversion(*kind)) {
            hasCharacter = true;
            signed64Only = false;
            unsigned64Only = false;
        } else {
            hasNumeric = true;
            const bool signedInteger =
                *kind == ScanConversionKind::SignedDecimal ||
                *kind == ScanConversionKind::SignedAutomatic;
            const bool unsignedInteger =
                *kind == ScanConversionKind::UnsignedDecimal ||
                *kind == ScanConversionKind::UnsignedOctal ||
                *kind == ScanConversionKind::UnsignedHexadecimal;
            signed64Only = signed64Only && signedInteger &&
                           token.conversion.longInteger;
            unsigned64Only = unsigned64Only && unsignedInteger &&
                             token.conversion.longInteger;
        }
        result.tokens.push_back(token);
    }
    if (!result.hasConversion) {
        result.error = "fscanf format must contain a conversion";
        return result;
    }
    if (hasCharacter && !hasNumeric) {
        result.outputKind = ScanOutputKind::Character;
    } else if (!hasCharacter && signed64Only) {
        result.outputKind = ScanOutputKind::Signed64;
    } else if (!hasCharacter && unsigned64Only) {
        result.outputKind = ScanOutputKind::Unsigned64;
    } else {
        result.outputKind = ScanOutputKind::Double;
    }
    result.succeeded = true;
    return result;
}

size_t utf8SequenceLength(unsigned char first) {
    if (first <= 0x7f) {
        return 1;
    }
    if ((first & 0xe0U) == 0xc0U) {
        return 2;
    }
    if ((first & 0xf0U) == 0xe0U) {
        return 3;
    }
    if ((first & 0xf8U) == 0xf0U) {
        return 4;
    }
    return 0;
}

std::optional<size_t> advanceUtf8(std::string_view input, size_t cursor) {
    if (cursor >= input.size()) {
        return std::nullopt;
    }
    const size_t length = utf8SequenceLength(
        static_cast<unsigned char>(input[cursor]));
    if (length == 0 || length > input.size() - cursor) {
        return std::nullopt;
    }
    for (size_t offset = 1; offset < length; ++offset) {
        if ((static_cast<unsigned char>(input[cursor + offset]) & 0xc0U) !=
            0x80U) {
            return std::nullopt;
        }
    }
    return cursor + length;
}

bool appendTextElements(std::string_view text,
                        std::vector<ScanElement>& output,
                        std::string& error) {
    try {
        const std::u16string decoded = runtimeUtf8ToUtf16(text);
        if (decoded.size() >
            kMaximumScanOutputElements - output.size()) {
            error = "fscanf output exceeds the runtime limit";
            return false;
        }
        for (const char16_t codeUnit : decoded) {
            ScanElement element;
            element.character = true;
            element.codeUnit = codeUnit;
            output.push_back(element);
        }
        return true;
    } catch (const std::exception&) {
        error = "fscanf input is not valid UTF-8 text";
        return false;
    }
}

bool scanText(const ScanConversion& conversion, std::string_view input,
              size_t& cursor, std::vector<ScanElement>& output,
              size_t& matchedCount, std::optional<size_t> maximumMatches,
              std::string& error) {
    const bool character =
        conversion.kind == ScanConversionKind::Character;
    if (!character) {
        while (cursor < input.size() && inputWhitespace(input[cursor])) {
            ++cursor;
        }
    }
    if (cursor >= input.size()) {
        return false;
    }

    size_t maximumCharacters = conversion.width.value_or(
        character ? 1U : std::numeric_limits<size_t>::max());
    if (character && !conversion.suppressed && maximumMatches) {
        maximumCharacters = std::min(
            maximumCharacters, *maximumMatches - matchedCount);
    }
    const size_t begin = cursor;
    size_t characters = 0;
    while (cursor < input.size() && characters < maximumCharacters) {
        if (!character && inputWhitespace(input[cursor])) {
            break;
        }
        const auto next = advanceUtf8(input, cursor);
        if (!next) {
            error = "fscanf input is not valid UTF-8 text";
            return false;
        }
        cursor = *next;
        ++characters;
    }
    if (cursor == begin) {
        return false;
    }
    if (!conversion.suppressed) {
        const size_t outputBefore = output.size();
        if (!appendTextElements(input.substr(begin, cursor - begin), output,
                                error)) {
            return false;
        }
        matchedCount += character ? output.size() - outputBefore : 1U;
    }
    return true;
}

bool scanNumber(const ScanConversion& conversion, std::string_view input,
                size_t& cursor, std::vector<ScanElement>& output,
                size_t& matchedCount, std::string& error) {
    while (cursor < input.size() && inputWhitespace(input[cursor])) {
        ++cursor;
    }
    if (cursor >= input.size()) {
        return false;
    }
    const size_t available = conversion.width
                                 ? std::min(*conversion.width,
                                            input.size() - cursor)
                                 : input.size() - cursor;
    std::string candidate(input.substr(cursor, available));
    char* end = nullptr;
    errno = 0;
    ScanElement element;
    const bool negative = !candidate.empty() && candidate.front() == '-';
    switch (conversion.kind) {
    case ScanConversionKind::SignedDecimal:
    case ScanConversionKind::SignedAutomatic: {
        const int base = conversion.kind == ScanConversionKind::SignedDecimal
                             ? 10
                             : 0;
        const std::int64_t value = std::strtoll(
            candidate.c_str(), &end, base);
        if (conversion.longInteger) {
            element.numeric.numericClass = RuntimeNumericClass::Int64;
            element.numeric.integerRealBits =
                std::bit_cast<std::uint64_t>(value);
            element.numeric.real = static_cast<double>(value);
        } else {
            const std::int64_t clamped = std::clamp(
                value,
                static_cast<std::int64_t>(
                    std::numeric_limits<std::int32_t>::min()),
                static_cast<std::int64_t>(
                    std::numeric_limits<std::int32_t>::max()));
            element.numeric.real = static_cast<double>(clamped);
        }
        break;
    }
    case ScanConversionKind::UnsignedDecimal:
    case ScanConversionKind::UnsignedOctal:
    case ScanConversionKind::UnsignedHexadecimal: {
        const int base = conversion.kind == ScanConversionKind::UnsignedDecimal
                             ? 10
                             : conversion.kind ==
                                       ScanConversionKind::UnsignedOctal
                                   ? 8
                                   : 16;
        const std::uint64_t value = std::strtoull(
            candidate.c_str(), &end, base);
        if (conversion.longInteger) {
            element.numeric.numericClass = RuntimeNumericClass::UInt64;
            element.numeric.integerRealBits = value;
            element.numeric.real = static_cast<double>(value);
        } else {
            const std::uint32_t converted = negative
                                                ? static_cast<std::uint32_t>(
                                                      value)
                                                : static_cast<std::uint32_t>(
                                                      std::min(
                                                          value,
                                                          static_cast<std::uint64_t>(
                                                              std::numeric_limits<std::uint32_t>::max())));
            element.numeric.real = static_cast<double>(converted);
        }
        break;
    }
    case ScanConversionKind::Floating:
        element.numeric.real = std::strtod(candidate.c_str(), &end);
        break;
    case ScanConversionKind::Character:
    case ScanConversionKind::String:
        return false;
    }
    if (end == candidate.c_str()) {
        return false;
    }
    cursor += static_cast<size_t>(end - candidate.c_str());
    if (!conversion.suppressed) {
        if (output.size() >= kMaximumScanOutputElements) {
            error = "fscanf output exceeds the runtime limit";
            return false;
        }
        output.push_back(std::move(element));
        ++matchedCount;
    }
    return true;
}

RuntimeValue outputValue(const std::vector<ScanElement>& elements,
                         ScanOutputKind outputKind,
                         const RuntimeFileScanSize& requestedSize) {
    const bool characterOnly = outputKind == ScanOutputKind::Character;
    if (requestedSize.matrixRequested && requestedSize.rows == 0) {
        return characterOnly
                   ? makeRuntimeCharacterArray({0, 0}, {})
                   : makeRuntimeMatrixValue(0, 0, {});
    }
    if (elements.empty()) {
        return characterOnly
                   ? makeRuntimeCharacterArray({0, 0}, {})
                   : makeRuntimeMatrixValue(0, 0, {});
    }

    size_t paddedCount = elements.size();
    if (requestedSize.scalarRequested && requestedSize.maximumMatches) {
        paddedCount = std::max(paddedCount,
                               *requestedSize.maximumMatches);
    }
    if (requestedSize.matrixRequested && requestedSize.columns) {
        const size_t requestedCount =
            requestedSize.rows * *requestedSize.columns;
        paddedCount = std::max(paddedCount, requestedCount);
    }

    size_t rows = characterOnly ? 1 : paddedCount;
    size_t columns = characterOnly ? paddedCount : 1;
    if (requestedSize.matrixRequested && requestedSize.rows != 0) {
        rows = requestedSize.rows;
        columns = (paddedCount + rows - 1U) / rows;
    }
    if (rows != 0 && columns >
                         std::numeric_limits<size_t>::max() / rows) {
        return makeRuntimeMatrixValue(0, 0, {});
    }
    paddedCount = rows * columns;
    if (characterOnly) {
        std::u16string text;
        text.reserve(paddedCount);
        for (const auto& element : elements) {
            text.push_back(element.codeUnit);
        }
        text.resize(paddedCount, u'\0');
        return makeRuntimeCharacterArray({rows, columns}, std::move(text));
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(paddedCount);
    for (const auto& element : elements) {
        if (element.character) {
            RuntimeNumericElementValue value;
            value.real = static_cast<double>(element.codeUnit);
            values.push_back(value);
        } else {
            values.push_back(element.numeric);
        }
    }
    values.resize(paddedCount);
    const RuntimeNumericClass numericClass =
        outputKind == ScanOutputKind::Signed64
            ? RuntimeNumericClass::Int64
            : outputKind == ScanOutputKind::Unsigned64
                  ? RuntimeNumericClass::UInt64
                  : RuntimeNumericClass::Double;
    if (auto result = runtimeNumericValueFromElements(
            {rows, columns}, std::move(values), numericClass)) {
        return std::move(*result);
    }
    return makeRuntimeMatrixValue(0, 0, {});
}

} // namespace

RuntimeFileScanResult runtimeScanFormattedText(
    std::string_view input, std::string_view format,
    const RuntimeFileScanSize& size) {
    if (size.scalarRequested && size.maximumMatches &&
        *size.maximumMatches > kMaximumScanOutputElements) {
        return failure("fscanf output exceeds the runtime limit");
    }
    if (size.matrixRequested) {
        if (size.rows > kMaximumScanOutputElements ||
            (size.columns &&
             (size.rows != 0 &&
              *size.columns >
                  kMaximumScanOutputElements / size.rows))) {
            return failure("fscanf output exceeds the runtime limit");
        }
    }
    const auto parsed = parseFormat(format);
    if (!parsed.succeeded) {
        return failure(parsed.error);
    }
    if (size.maximumMatches && *size.maximumMatches == 0) {
        RuntimeFileScanResult result;
        result.succeeded = true;
        result.value = outputValue({}, parsed.outputKind, size);
        return result;
    }

    std::vector<ScanElement> output;
    size_t cursor = 0;
    size_t matchedCount = 0;
    bool stop = false;
    std::string error;
    while (!stop && cursor <= input.size()) {
        const size_t cycleCursor = cursor;
        const size_t cycleCount = matchedCount;
        for (const auto& token : parsed.tokens) {
            if (token.kind == ScanTokenKind::Whitespace) {
                while (cursor < input.size() && inputWhitespace(input[cursor])) {
                    ++cursor;
                }
                continue;
            }
            if (token.kind == ScanTokenKind::Literal) {
                if (cursor >= input.size() ||
                    input[cursor] != token.literal) {
                    stop = true;
                    break;
                }
                ++cursor;
                continue;
            }
            if (!token.conversion.suppressed && size.maximumMatches &&
                matchedCount >= *size.maximumMatches) {
                stop = true;
                break;
            }
            const bool converted = isCharacterConversion(
                                       token.conversion.kind)
                                       ? scanText(token.conversion, input,
                                                  cursor, output,
                                                  matchedCount,
                                                  size.maximumMatches, error)
                                       : scanNumber(token.conversion, input,
                                                   cursor, output,
                                                   matchedCount, error);
            if (!error.empty()) {
                return failure(std::move(error));
            }
            if (!converted) {
                stop = true;
                break;
            }
        }
        if (cursor == cycleCursor && matchedCount == cycleCount) {
            break;
        }
    }

    RuntimeFileScanResult result;
    result.succeeded = true;
    result.value = outputValue(output, parsed.outputKind, size);
    result.matchedCount = matchedCount;
    result.consumedBytes = cursor;
    return result;
}

} // namespace mparser
