#include "mparser/runtime_file_io.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
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
            ScanToken token;
            token.kind = ScanTokenKind::Whitespace;
            result.tokens.push_back(std::move(token));
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

namespace {

constexpr size_t kMaximumBinaryOutputElements = 16U * 1024U * 1024U;

std::string lowercaseAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

std::string_view trimAscii(std::string_view text) {
    while (!text.empty() && inputWhitespace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && inputWhitespace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<RuntimeNumericClass> binaryNumericClass(
    std::string_view name) {
    const std::string normalized = lowercaseAscii(trimAscii(name));
    if (normalized == "uint8" || normalized == "uchar" ||
        normalized == "unsigned char") {
        return RuntimeNumericClass::UInt8;
    }
    if (normalized == "int8" || normalized == "schar" ||
        normalized == "signed char" || normalized == "integer*1") {
        return RuntimeNumericClass::Int8;
    }
    if (normalized == "uint16" || normalized == "ushort" ||
        normalized == "unsigned short") {
        return RuntimeNumericClass::UInt16;
    }
    if (normalized == "int16" || normalized == "short" ||
        normalized == "integer*2") {
        return RuntimeNumericClass::Int16;
    }
    if (normalized == "uint32" || normalized == "uint" ||
        normalized == "ulong" || normalized == "unsigned int" ||
        normalized == "unsigned long") {
        return RuntimeNumericClass::UInt32;
    }
    if (normalized == "int32" || normalized == "int" ||
        normalized == "long" || normalized == "integer*4") {
        return RuntimeNumericClass::Int32;
    }
    if (normalized == "uint64") {
        return RuntimeNumericClass::UInt64;
    }
    if (normalized == "int64" || normalized == "integer*8") {
        return RuntimeNumericClass::Int64;
    }
    if (normalized == "single" || normalized == "float" ||
        normalized == "float32" || normalized == "real*4") {
        return RuntimeNumericClass::Single;
    }
    if (normalized == "double" || normalized == "float64" ||
        normalized == "real*8") {
        return RuntimeNumericClass::Double;
    }
    if (normalized == "logical") {
        return RuntimeNumericClass::Logical;
    }
    return std::nullopt;
}

size_t binaryElementBytes(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Logical:
    case RuntimeNumericClass::Int8:
    case RuntimeNumericClass::UInt8:
        return 1;
    case RuntimeNumericClass::Int16:
    case RuntimeNumericClass::UInt16:
        return 2;
    case RuntimeNumericClass::Single:
    case RuntimeNumericClass::Int32:
    case RuntimeNumericClass::UInt32:
        return 4;
    case RuntimeNumericClass::Double:
    case RuntimeNumericClass::Int64:
    case RuntimeNumericClass::UInt64:
        return 8;
    }
    return 0;
}

size_t nextUtf8CodePointBytes(std::string_view text, size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    size_t count = 1;
    if ((first & 0xE0U) == 0xC0U) {
        count = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
        count = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
        count = 4;
    }
    if (count > text.size() - offset) {
        return 1;
    }
    for (size_t index = 1; index < count; ++index) {
        if ((static_cast<unsigned char>(text[offset + index]) & 0xC0U) !=
            0x80U) {
            return 1;
        }
    }
    return count;
}

std::uint64_t readUnsigned(std::string_view input, size_t offset,
                           size_t width, RuntimeFileByteOrder order) {
    std::uint64_t result = 0;
    for (size_t index = 0; index < width; ++index) {
        const size_t source = order == RuntimeFileByteOrder::LittleEndian
                                  ? offset + index
                                  : offset + width - index - 1U;
        result |= static_cast<std::uint64_t>(
                      static_cast<unsigned char>(input[source]))
                  << (index * 8U);
    }
    return result;
}

RuntimeNumericElementValue decodeBinaryElement(
    std::string_view input, size_t offset,
    RuntimeNumericClass numericClass, RuntimeFileByteOrder order) {
    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    const size_t width = binaryElementBytes(numericClass);
    const std::uint64_t bits = readUnsigned(input, offset, width, order);
    switch (numericClass) {
    case RuntimeNumericClass::Logical:
        result.real = bits == 0 ? 0.0 : 1.0;
        break;
    case RuntimeNumericClass::Int8:
        result.integerRealBits = bits;
        result.real = static_cast<double>(std::bit_cast<std::int8_t>(
            static_cast<std::uint8_t>(bits)));
        break;
    case RuntimeNumericClass::UInt8:
        result.integerRealBits = bits;
        result.real = static_cast<double>(static_cast<std::uint8_t>(bits));
        break;
    case RuntimeNumericClass::Int16:
        result.integerRealBits = bits;
        result.real = static_cast<double>(std::bit_cast<std::int16_t>(
            static_cast<std::uint16_t>(bits)));
        break;
    case RuntimeNumericClass::UInt16:
        result.integerRealBits = bits;
        result.real = static_cast<double>(static_cast<std::uint16_t>(bits));
        break;
    case RuntimeNumericClass::Int32:
        result.integerRealBits = bits;
        result.real = static_cast<double>(std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(bits)));
        break;
    case RuntimeNumericClass::UInt32:
        result.integerRealBits = bits;
        result.real = static_cast<double>(static_cast<std::uint32_t>(bits));
        break;
    case RuntimeNumericClass::Int64: {
        result.integerRealBits = bits;
        const auto value = std::bit_cast<std::int64_t>(bits);
        result.real = static_cast<double>(value);
        break;
    }
    case RuntimeNumericClass::UInt64:
        result.integerRealBits = bits;
        result.real = static_cast<double>(bits);
        break;
    case RuntimeNumericClass::Single:
        result.real = static_cast<double>(std::bit_cast<float>(
            static_cast<std::uint32_t>(bits)));
        break;
    case RuntimeNumericClass::Double:
        result.real = std::bit_cast<double>(bits);
        break;
    }
    return result;
}

void appendUnsigned(std::string& output, std::uint64_t bits,
                    size_t width, RuntimeFileByteOrder order) {
    for (size_t index = 0; index < width; ++index) {
        const size_t shiftIndex =
            order == RuntimeFileByteOrder::LittleEndian
                ? index
                : width - index - 1U;
        output.push_back(static_cast<char>(
            (bits >> (shiftIndex * 8U)) & 0xFFU));
    }
}

void appendBinaryElement(std::string& output,
                         const RuntimeNumericElementValue& element,
                         RuntimeNumericClass numericClass,
                         RuntimeFileByteOrder order) {
    std::uint64_t bits = 0;
    switch (numericClass) {
    case RuntimeNumericClass::Logical:
        bits = element.real == 0.0 ? 0U : 1U;
        break;
    case RuntimeNumericClass::Int8:
    case RuntimeNumericClass::UInt8:
    case RuntimeNumericClass::Int16:
    case RuntimeNumericClass::UInt16:
    case RuntimeNumericClass::Int32:
    case RuntimeNumericClass::UInt32:
    case RuntimeNumericClass::Int64:
    case RuntimeNumericClass::UInt64:
        bits = element.integerRealBits;
        break;
    case RuntimeNumericClass::Single:
        bits = std::bit_cast<std::uint32_t>(
            static_cast<float>(element.real));
        break;
    case RuntimeNumericClass::Double:
        bits = std::bit_cast<std::uint64_t>(element.real);
        break;
    }
    appendUnsigned(output, bits, binaryElementBytes(numericClass), order);
}

RuntimeBinaryReadResult binaryReadFailure(std::string message) {
    RuntimeBinaryReadResult result;
    result.error = std::move(message);
    return result;
}

RuntimeBinaryWriteResult binaryWriteFailure(std::string message) {
    RuntimeBinaryWriteResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

RuntimeFileByteOrder runtimeNativeFileByteOrder() {
    return std::endian::native == std::endian::big
               ? RuntimeFileByteOrder::BigEndian
               : RuntimeFileByteOrder::LittleEndian;
}

std::optional<RuntimeFileByteOrder>
runtimeFileByteOrderFromName(std::string_view name) {
    const std::string normalized = lowercaseAscii(trimAscii(name));
    if (normalized == "n" || normalized == "native") {
        return runtimeNativeFileByteOrder();
    }
    if (normalized == "l" || normalized == "a" ||
        normalized == "ieee-le" || normalized == "ieee-le.l64") {
        return RuntimeFileByteOrder::LittleEndian;
    }
    if (normalized == "b" || normalized == "s" ||
        normalized == "ieee-be" || normalized == "ieee-be.l64") {
        return RuntimeFileByteOrder::BigEndian;
    }
    return std::nullopt;
}

std::string_view runtimeFileByteOrderName(RuntimeFileByteOrder order) {
    return order == RuntimeFileByteOrder::LittleEndian
               ? "ieee-le"
               : "ieee-be";
}

RuntimeFileLineResult runtimeReadFileLine(
    std::string_view input, bool keepTerminator,
    std::optional<size_t> maximumCharacters) {
    RuntimeFileLineResult result;
    if (input.empty()) {
        return result;
    }
    result.hasValue = true;
    size_t cursor = 0;
    size_t characterCount = 0;
    while (cursor < input.size() &&
           (!maximumCharacters || characterCount < *maximumCharacters)) {
        if (input[cursor] == '\r' || input[cursor] == '\n') {
            const size_t terminatorStart = cursor;
            const char first = input[cursor++];
            if (cursor < input.size() && input[cursor] != first &&
                (input[cursor] == '\r' || input[cursor] == '\n')) {
                ++cursor;
            }
            result.terminator.assign(
                input.substr(terminatorStart, cursor - terminatorStart));
            if (keepTerminator) {
                result.text.append(result.terminator);
            }
            result.consumedBytes = cursor;
            return result;
        }
        const size_t count = nextUtf8CodePointBytes(input, cursor);
        result.text.append(input.substr(cursor, count));
        cursor += count;
        ++characterCount;
    }
    result.consumedBytes = cursor;
    return result;
}

RuntimeBinaryPrecisionResult runtimeParseBinaryPrecision(
    std::string_view text, bool allowOutputClass) {
    RuntimeBinaryPrecisionResult result;
    text = trimAscii(text);
    if (text.empty()) {
        result.error = "binary precision must not be empty";
        return result;
    }

    bool outputMatchesSource = false;
    if (text.front() == '*') {
        outputMatchesSource = true;
        text.remove_prefix(1);
    }
    const size_t conversion = text.find("=>");
    std::string_view source = conversion == std::string_view::npos
                                  ? text
                                  : text.substr(0, conversion);
    std::string_view output = conversion == std::string_view::npos
                                  ? std::string_view{}
                                  : text.substr(conversion + 2U);
    if (conversion != std::string_view::npos && !allowOutputClass) {
        result.error = "fwrite precision cannot specify an output class";
        return result;
    }
    if (outputMatchesSource && conversion != std::string_view::npos) {
        result.error = "binary precision cannot combine *source with =>output";
        return result;
    }

    const size_t repeatSeparator = source.find('*');
    if (repeatSeparator != std::string_view::npos && repeatSeparator != 0 &&
        std::all_of(source.begin(), source.begin() +
                                      static_cast<std::ptrdiff_t>(
                                          repeatSeparator),
                    [](unsigned char character) {
                        return std::isdigit(character) != 0;
                    })) {
        size_t repeat = 0;
        for (size_t index = 0; index < repeatSeparator; ++index) {
            const size_t digit = static_cast<size_t>(source[index] - '0');
            if (repeat > (kMaximumBinaryOutputElements - digit) / 10U) {
                result.error = "binary precision repeat exceeds the runtime limit";
                return result;
            }
            repeat = repeat * 10U + digit;
        }
        if (repeat == 0) {
            result.error = "binary precision repeat must be positive";
            return result;
        }
        result.precision.valuesPerBlock = repeat;
        source.remove_prefix(repeatSeparator + 1U);
    }

    const auto sourceClass = binaryNumericClass(source);
    if (!sourceClass) {
        result.error = "binary precision contains an unsupported source class";
        return result;
    }
    result.precision.sourceClass = *sourceClass;
    if (outputMatchesSource) {
        result.precision.outputClass = *sourceClass;
    } else if (!output.empty()) {
        const auto outputClass = binaryNumericClass(output);
        if (!outputClass) {
            result.error = "binary precision contains an unsupported output class";
            return result;
        }
        result.precision.outputClass = *outputClass;
    }
    result.succeeded = true;
    return result;
}

RuntimeBinaryReadResult runtimeDecodeBinaryData(
    std::string_view input,
    const RuntimeBinaryPrecision& precision,
    const RuntimeBinaryReadSize& size, size_t skipBytes,
    RuntimeFileByteOrder byteOrder) {
    if (precision.valuesPerBlock == 0) {
        return binaryReadFailure("binary precision repeat must be positive");
    }
    if (size.maximumValues &&
        *size.maximumValues > kMaximumBinaryOutputElements) {
        return binaryReadFailure("fread output exceeds the runtime limit");
    }
    const size_t width = binaryElementBytes(precision.sourceClass);
    std::vector<RuntimeNumericElementValue> values;
    values.reserve(size.maximumValues
                       ? *size.maximumValues
                       : std::min(input.size() / width,
                                  kMaximumBinaryOutputElements));
    size_t cursor = 0;
    bool exhausted = false;
    while (!exhausted &&
           (!size.maximumValues || values.size() < *size.maximumValues)) {
        size_t blockValues = 0;
        while (blockValues < precision.valuesPerBlock &&
               (!size.maximumValues ||
                values.size() < *size.maximumValues)) {
            if (cursor > input.size() || width > input.size() - cursor) {
                cursor = input.size();
                exhausted = true;
                break;
            }
            values.push_back(decodeBinaryElement(
                input, cursor, precision.sourceClass, byteOrder));
            cursor += width;
            ++blockValues;
            if (values.size() > kMaximumBinaryOutputElements) {
                return binaryReadFailure(
                    "fread output exceeds the runtime limit");
            }
        }
        if (blockValues == precision.valuesPerBlock && skipBytes != 0) {
            cursor = skipBytes > input.size() - cursor
                         ? input.size()
                         : cursor + skipBytes;
        }
        if (blockValues == 0 || cursor >= input.size()) {
            exhausted = true;
        }
    }

    const size_t readCount = values.size();
    std::vector<size_t> dimensions{readCount, 1};
    size_t outputCount = readCount;
    if (size.scalarRequested && size.maximumValues) {
        outputCount = *size.maximumValues;
        dimensions = {outputCount, 1};
    } else if (size.matrixRequested) {
        if (size.columns) {
            if (size.rows != 0 &&
                *size.columns >
                    kMaximumBinaryOutputElements / size.rows) {
                return binaryReadFailure(
                    "fread output exceeds the runtime limit");
            }
            outputCount = size.rows * *size.columns;
            dimensions = {size.rows, *size.columns};
        } else if (size.rows == 0) {
            outputCount = 0;
            dimensions = {0, 0};
        } else {
            const size_t columns =
                (readCount + size.rows - 1U) / size.rows;
            outputCount = size.rows * columns;
            dimensions = {size.rows, columns};
        }
    }
    if (outputCount > kMaximumBinaryOutputElements) {
        return binaryReadFailure("fread output exceeds the runtime limit");
    }
    values.resize(outputCount);
    auto value = runtimeNumericValueFromElements(
        std::move(dimensions), std::move(values), precision.outputClass);
    if (!value) {
        return binaryReadFailure("fread could not construct its output array");
    }

    RuntimeBinaryReadResult result;
    result.succeeded = true;
    result.value = std::move(*value);
    result.valueCount = readCount;
    result.consumedBytes = cursor;
    return result;
}

RuntimeBinaryWriteResult runtimeEncodeBinaryData(
    const RuntimeValue& value,
    const RuntimeBinaryPrecision& precision,
    RuntimeFileByteOrder byteOrder) {
    if (!isRuntimeNumericValue(value)) {
        return binaryWriteFailure("fwrite data must be a numeric array");
    }
    if (value.numericComplex) {
        return binaryWriteFailure(
            "fwrite complex data is not supported by this binary I/O slice");
    }
    if (precision.valuesPerBlock == 0) {
        return binaryWriteFailure("binary precision repeat must be positive");
    }
    const size_t count = runtimeShapeElementCount(value);
    const size_t width = binaryElementBytes(precision.sourceClass);
    if (count > kMaximumBinaryOutputElements ||
        (width != 0 && count >
                           std::numeric_limits<size_t>::max() / width)) {
        return binaryWriteFailure("fwrite input exceeds the runtime limit");
    }

    RuntimeBinaryWriteResult result;
    result.bytes.reserve(count * width);
    for (size_t index = 0; index < count; ++index) {
        const auto source = runtimeNumericElementValue(value, index);
        const auto converted = source
                                   ? runtimeConvertNumericElementValue(
                                         *source, precision.sourceClass)
                                   : std::nullopt;
        if (!converted || converted->complex) {
            return binaryWriteFailure(
                "fwrite could not convert an input element to its precision");
        }
        appendBinaryElement(result.bytes, *converted,
                            precision.sourceClass, byteOrder);
    }
    if (precision.valuesPerBlock >
        std::numeric_limits<size_t>::max() / width) {
        return binaryWriteFailure(
            "fwrite block size exceeds the supported range");
    }
    result.succeeded = true;
    result.valueCount = count;
    result.blockBytes = precision.valuesPerBlock * width;
    return result;
}

} // namespace mparser
