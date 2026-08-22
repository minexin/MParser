#include "mparser/runtime_output.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>

namespace mparser {
namespace {

constexpr size_t kMaximumFormatWidth = 1024 * 1024;
constexpr size_t kMaximumFormatPrecision = 4096;
constexpr size_t kMaximumFormattedOutputBytes = 16 * 1024 * 1024;

struct FormatAtom {
    std::optional<RuntimeNumericElementValue> numeric;
    std::optional<std::string> text;
};

struct FormatSpecifier {
    bool left = false;
    bool showSign = false;
    bool leadingSpace = false;
    bool alternate = false;
    bool zeroFill = false;
    size_t width = 0;
    std::optional<size_t> precision;
    char conversion = '\0';
};

RuntimeFormatResult failure(std::string message) {
    return RuntimeFormatResult{false, {}, std::move(message)};
}

std::string decodeFormatEscapes(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\' || index + 1 >= text.size()) {
            decoded.push_back(text[index]);
            continue;
        }
        const char escaped = text[++index];
        switch (escaped) {
        case 'n':
            decoded.push_back('\n');
            break;
        case 'r':
            decoded.push_back('\r');
            break;
        case 't':
            decoded.push_back('\t');
            break;
        case 'b':
            decoded.push_back('\b');
            break;
        case 'f':
            decoded.push_back('\f');
            break;
        case '\\':
            decoded.push_back('\\');
            break;
        default:
            decoded.push_back('\\');
            decoded.push_back(escaped);
            break;
        }
    }
    return decoded;
}

bool appendAtoms(const RuntimeValue& value,
                 std::vector<FormatAtom>& atoms) {
    if (isRuntimeNumericValue(value)) {
        const size_t count = runtimeShapeElementCount(value);
        for (size_t index = 0; index < count; ++index) {
            const auto element = runtimeNumericElementValue(value, index);
            if (!element) {
                return false;
            }
            atoms.push_back(FormatAtom{*element, std::nullopt});
        }
        return true;
    }

    if (const auto scalar = runtimeTextScalarUtf8(value)) {
        atoms.push_back(FormatAtom{std::nullopt, *scalar});
        return true;
    }
    if (isRuntimeStringArray(value)) {
        for (const auto& element : value.stringElements) {
            if (element.missing) {
                return false;
            }
            atoms.push_back(FormatAtom{
                std::nullopt, runtimeUtf16ToUtf8(element.value)});
        }
        return true;
    }
    return false;
}

bool parseUnsigned(std::string_view format, size_t& index,
                   size_t& value) {
    value = 0;
    bool sawDigit = false;
    while (index < format.size() &&
           std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
        sawDigit = true;
        const unsigned digit =
            static_cast<unsigned>(format[index] - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        ++index;
    }
    return sawDigit;
}

std::optional<FormatSpecifier> parseSpecifier(
    std::string_view format, size_t& index, std::string& error) {
    FormatSpecifier specifier;
    bool parsingFlags = true;
    while (parsingFlags && index < format.size()) {
        switch (format[index]) {
        case '-':
            specifier.left = true;
            break;
        case '+':
            specifier.showSign = true;
            break;
        case ' ':
            specifier.leadingSpace = true;
            break;
        case '#':
            specifier.alternate = true;
            break;
        case '0':
            specifier.zeroFill = true;
            break;
        default:
            parsingFlags = false;
            continue;
        }
        ++index;
    }

    if (index < format.size() && format[index] == '*') {
        error = "dynamic fprintf width is not supported";
        return std::nullopt;
    }
    if (index < format.size() &&
        std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
        if (!parseUnsigned(format, index, specifier.width)) {
            error = "fprintf width is too large";
            return std::nullopt;
        }
        if (specifier.width > kMaximumFormatWidth) {
            error = "fprintf width exceeds the runtime limit";
            return std::nullopt;
        }
    }
    if (index < format.size() && format[index] == '.') {
        ++index;
        if (index < format.size() && format[index] == '*') {
            error = "dynamic fprintf precision is not supported";
            return std::nullopt;
        }
        size_t precision = 0;
        if (index < format.size() &&
            std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
            if (!parseUnsigned(format, index, precision)) {
                error = "fprintf precision is too large";
                return std::nullopt;
            }
        }
        if (precision > kMaximumFormatPrecision) {
            error = "fprintf precision exceeds the runtime limit";
            return std::nullopt;
        }
        specifier.precision = precision;
    }
    if (index < format.size() &&
        (format[index] == 'h' || format[index] == 'l' ||
         format[index] == 'L' || format[index] == 'j' ||
         format[index] == 'z' || format[index] == 't')) {
        error = "fprintf length modifiers are not supported";
        return std::nullopt;
    }
    if (index >= format.size()) {
        error = "fprintf format ends after '%'";
        return std::nullopt;
    }
    specifier.conversion = format[index++];
    if (std::string_view("diufFeEgGsc").find(specifier.conversion) ==
        std::string_view::npos) {
        error = std::string("unsupported fprintf conversion: %") +
                specifier.conversion;
        return std::nullopt;
    }
    return specifier;
}

std::optional<std::int64_t> signedInteger(
    const RuntimeNumericElementValue& value) {
    if (value.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            return std::bit_cast<std::int64_t>(value.integerRealBits);
        }
        if (value.integerRealBits >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value.integerRealBits);
    }
    if (!std::isfinite(value.real) ||
        value.real < static_cast<double>(
                         std::numeric_limits<std::int64_t>::min()) ||
        value.real > static_cast<double>(
                         std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value.real);
}

std::optional<std::uint64_t> unsignedInteger(
    const RuntimeNumericElementValue& value) {
    if (value.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            const auto signedValue =
                std::bit_cast<std::int64_t>(value.integerRealBits);
            if (signedValue < 0) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(signedValue);
        }
        return value.integerRealBits;
    }
    if (!std::isfinite(value.real) || value.real < 0.0 ||
        static_cast<long double>(value.real) >
            static_cast<long double>(
                std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value.real);
}

void configureStream(std::ostringstream& stream,
                     const FormatSpecifier& specifier) {
    stream.imbue(std::locale::classic());
    if (specifier.showSign) {
        stream << std::showpos;
    }
    if (specifier.alternate) {
        stream << std::showpoint;
    }
    if (specifier.precision) {
        stream << std::setprecision(static_cast<int>(std::min<size_t>(
            *specifier.precision,
            static_cast<size_t>(std::numeric_limits<int>::max()))));
    }
}

size_t utf8SequenceLength(unsigned char first) {
    if (first < 0x80) {
        return 1;
    }
    if ((first & 0xe0) == 0xc0) {
        return 2;
    }
    if ((first & 0xf0) == 0xe0) {
        return 3;
    }
    if ((first & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

size_t utf8PrefixBytes(std::string_view text, size_t codePoints) {
    size_t offset = 0;
    size_t count = 0;
    while (offset < text.size() && count < codePoints) {
        offset += std::min(
            utf8SequenceLength(static_cast<unsigned char>(text[offset])),
            text.size() - offset);
        ++count;
    }
    return offset;
}

size_t utf8CodePointCount(std::string_view text) {
    size_t offset = 0;
    size_t count = 0;
    while (offset < text.size()) {
        offset += std::min(
            utf8SequenceLength(static_cast<unsigned char>(text[offset])),
            text.size() - offset);
        ++count;
    }
    return count;
}

std::optional<std::string> utf8FromCodePoint(std::uint64_t codePoint) {
    if (codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
        return std::nullopt;
    }
    std::u16string utf16;
    if (codePoint <= 0xffff) {
        utf16.push_back(static_cast<char16_t>(codePoint));
    } else {
        codePoint -= 0x10000;
        utf16.push_back(static_cast<char16_t>(
            0xd800 + (codePoint >> 10)));
        utf16.push_back(static_cast<char16_t>(
            0xdc00 + (codePoint & 0x3ff)));
    }
    return runtimeUtf16ToUtf8(utf16);
}

std::string applyWidth(std::string text,
                       const FormatSpecifier& specifier,
                       bool numeric,
                       size_t displayWidth) {
    if (specifier.width <= displayWidth) {
        return text;
    }
    const size_t padding = specifier.width - displayWidth;
    if (specifier.left) {
        text.append(padding, ' ');
        return text;
    }
    if (numeric && specifier.zeroFill) {
        const size_t prefix =
            !text.empty() &&
                    (text.front() == '+' || text.front() == '-' ||
                     text.front() == ' ')
                ? 1
                : 0;
        text.insert(prefix, padding, '0');
        return text;
    }
    text.insert(0, padding, ' ');
    return text;
}

RuntimeFormatResult formatAtom(const FormatAtom& atom,
                               const FormatSpecifier& specifier) {
    std::ostringstream stream;
    configureStream(stream, specifier);
    const char conversion = specifier.conversion;
    if (conversion == 's') {
        if (atom.text) {
            const size_t prefix = specifier.precision
                                      ? utf8PrefixBytes(
                                            *atom.text,
                                            *specifier.precision)
                                      : atom.text->size();
            const std::string text = atom.text->substr(0, prefix);
            return RuntimeFormatResult{
                true,
                applyWidth(text, specifier, false,
                           utf8CodePointCount(text)),
                {}};
        } else {
            return failure("fprintf %s requires text input");
        }
    }
    if (conversion == 'c') {
        std::string text;
        if (atom.text) {
            if (atom.text->empty()) {
                return failure("fprintf %c requires a nonempty text value");
            }
            text = atom.text->substr(
                0, utf8PrefixBytes(*atom.text, 1));
        } else if (atom.numeric) {
            const auto code = unsignedInteger(*atom.numeric);
            const auto encoded = code ? utf8FromCodePoint(*code)
                                      : std::nullopt;
            if (!encoded) {
                return failure(
                    "fprintf %c requires a valid Unicode scalar value");
            }
            text = *encoded;
        } else {
            return failure("fprintf %c requires text or numeric input");
        }
        return RuntimeFormatResult{
            true, applyWidth(text, specifier, false, 1), {}};
    }
    if (!atom.numeric || atom.numeric->complex) {
        return failure("numeric fprintf conversion requires a real numeric value");
    }
    if (conversion == 'd' || conversion == 'i') {
        const auto value = signedInteger(*atom.numeric);
        if (!value) {
            return failure("fprintf signed-integer conversion is out of range");
        }
        if (specifier.leadingSpace && !specifier.showSign && *value >= 0) {
            stream << ' ';
        }
        stream << *value;
    } else if (conversion == 'u') {
        const auto value = unsignedInteger(*atom.numeric);
        if (!value) {
            return failure("fprintf unsigned-integer conversion is out of range");
        }
        stream << *value;
    } else {
        if (specifier.leadingSpace && !specifier.showSign &&
            !std::signbit(atom.numeric->real)) {
            stream << ' ';
        }
        switch (conversion) {
        case 'f':
        case 'F':
            stream << std::fixed;
            break;
        case 'e':
        case 'E':
            stream << std::scientific;
            break;
        case 'g':
        case 'G':
            stream << std::defaultfloat;
            break;
        default:
            break;
        }
        if (conversion == 'F' || conversion == 'E' || conversion == 'G') {
            stream << std::uppercase;
        }
        stream << atom.numeric->real;
    }
    std::string text = stream.str();
    const size_t displayWidth = text.size();
    return RuntimeFormatResult{
        true,
        applyWidth(std::move(text), specifier, true, displayWidth),
        {}};
}

RuntimeFormatResult formatOnePass(
    std::string_view format, const std::vector<FormatAtom>& atoms,
    size_t& atomIndex, bool& consumedAtom) {
    std::string output;
    for (size_t index = 0; index < format.size();) {
        if (format[index] != '%') {
            if (output.size() == kMaximumFormattedOutputBytes) {
                return failure(
                    "formatted output exceeds the runtime limit");
            }
            output.push_back(format[index++]);
            continue;
        }
        ++index;
        if (index < format.size() && format[index] == '%') {
            if (output.size() == kMaximumFormattedOutputBytes) {
                return failure(
                    "formatted output exceeds the runtime limit");
            }
            output.push_back('%');
            ++index;
            continue;
        }
        std::string error;
        const auto specifier = parseSpecifier(format, index, error);
        if (!specifier) {
            return failure(std::move(error));
        }
        if (atomIndex >= atoms.size()) {
            return failure("fprintf format requires more data arguments");
        }
        auto formatted = formatAtom(atoms[atomIndex++], *specifier);
        if (!formatted.succeeded) {
            return formatted;
        }
        consumedAtom = true;
        if (formatted.text.size() >
            kMaximumFormattedOutputBytes - output.size()) {
            return failure(
                "formatted output exceeds the runtime limit");
        }
        output += formatted.text;
    }
    return RuntimeFormatResult{true, std::move(output), {}};
}

size_t numericClassBitWidth(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Logical:
    case RuntimeNumericClass::Int8:
    case RuntimeNumericClass::UInt8:
        return 8;
    case RuntimeNumericClass::Int16:
    case RuntimeNumericClass::UInt16:
        return 16;
    case RuntimeNumericClass::Single:
    case RuntimeNumericClass::Int32:
    case RuntimeNumericClass::UInt32:
        return 32;
    case RuntimeNumericClass::Double:
    case RuntimeNumericClass::Int64:
    case RuntimeNumericClass::UInt64:
        return 64;
    }
    return 64;
}

std::string exactIntegerText(const RuntimeNumericElementValue& element,
                             bool imaginary, bool magnitude) {
    const std::uint64_t bits = imaginary
                                   ? element.integerImaginaryBits
                                   : element.integerRealBits;
    if (element.numericClass == RuntimeNumericClass::Logical) {
        return bits == 0 ? "0" : "1";
    }
    if (!runtimeNumericClassIsSignedInteger(element.numericClass)) {
        return std::to_string(bits);
    }
    const std::int64_t value = std::bit_cast<std::int64_t>(bits);
    if (!magnitude || value >= 0) {
        return std::to_string(value);
    }
    return std::to_string(std::uint64_t{0} -
                          static_cast<std::uint64_t>(value));
}

std::string hexadecimalText(const RuntimeNumericElementValue& element,
                            bool imaginary) {
    std::uint64_t bits = 0;
    if (runtimeNumericClassIsInteger(element.numericClass) ||
        element.numericClass == RuntimeNumericClass::Logical) {
        bits = imaginary ? element.integerImaginaryBits
                         : element.integerRealBits;
    } else if (element.numericClass == RuntimeNumericClass::Single) {
        const float value = static_cast<float>(
            imaginary ? element.imaginary : element.real);
        bits = std::bit_cast<std::uint32_t>(value);
    } else {
        bits = std::bit_cast<std::uint64_t>(
            imaginary ? element.imaginary : element.real);
    }
    const size_t width = numericClassBitWidth(element.numericClass);
    if (width < 64) {
        bits &= (std::uint64_t{1} << width) - 1U;
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(static_cast<int>(width / 4)) << bits;
    return output.str();
}

std::string rationalText(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Inf" : "Inf";
    }
    if (value == 0.0) {
        return "0";
    }

    const bool negative = std::signbit(value);
    const long double target =
        std::fabs(static_cast<long double>(value));
    long double remaining = target;
    constexpr std::int64_t kMaximumDenominator = 1'000'000;
    std::int64_t numeratorPrevious = 0;
    std::int64_t numerator = 1;
    std::int64_t denominatorPrevious = 1;
    std::int64_t denominator = 0;
    for (size_t iteration = 0; iteration < 32; ++iteration) {
        const long double whole = std::floor(remaining);
        const long double candidateNumerator =
            whole * static_cast<long double>(numerator) +
            static_cast<long double>(numeratorPrevious);
        const long double candidateDenominator =
            whole * static_cast<long double>(denominator) +
            static_cast<long double>(denominatorPrevious);
        if (candidateNumerator >
                static_cast<long double>(
                    std::numeric_limits<std::int64_t>::max()) ||
            candidateDenominator >
                static_cast<long double>(kMaximumDenominator)) {
            break;
        }
        const auto nextNumerator =
            static_cast<std::int64_t>(candidateNumerator);
        const auto nextDenominator =
            static_cast<std::int64_t>(candidateDenominator);
        numeratorPrevious = numerator;
        numerator = nextNumerator;
        denominatorPrevious = denominator;
        denominator = nextDenominator;

        const long double approximation =
            static_cast<long double>(numerator) /
            static_cast<long double>(denominator);
        if (std::fabs(approximation - target) <=
            1.0e-6L * std::max(1.0L, target)) {
            break;
        }

        const long double fraction = remaining - whole;
        if (fraction <= std::numeric_limits<long double>::epsilon()) {
            break;
        }
        remaining = 1.0L / fraction;
    }
    if (denominator == 0) {
        std::ostringstream fallback;
        fallback << std::setprecision(15) << value;
        return fallback.str();
    }
    std::ostringstream output;
    if (negative) {
        output << '-';
    }
    output << numerator;
    if (denominator != 1) {
        output << '/' << denominator;
    }
    return output.str();
}

std::string engineeringText(double value, int significantDigits,
                            int shortFractionDigits) {
    if (!std::isfinite(value) || value == 0.0) {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::fixed << std::setprecision(shortFractionDigits)
               << value << "e+000";
        return output.str();
    }
    const int exponent = static_cast<int>(
        std::floor(std::log10(std::fabs(value)) / 3.0) * 3.0);
    const double mantissa = value / std::pow(10.0, exponent);
    const int digitsBeforeDecimal =
        std::max(1, static_cast<int>(
                        std::floor(std::log10(std::fabs(mantissa))) + 1.0));
    const int fractionDigits = significantDigits == 0
                                   ? shortFractionDigits
                                   : std::max(0, significantDigits -
                                                     digitsBeforeDecimal);
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(fractionDigits) << mantissa
           << 'e' << (exponent < 0 ? '-' : '+') << std::setfill('0')
           << std::setw(3) << std::abs(exponent);
    return output.str();
}

std::string floatingText(double value, RuntimeNumericClass numericClass,
                         RuntimeNumericDisplayFormat format,
                         bool magnitude = false,
                         bool forceFractional = false) {
    if (magnitude) {
        value = std::fabs(value);
    }
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Inf" : "Inf";
    }
    if (format == RuntimeNumericDisplayFormat::Rational) {
        return rationalText(value);
    }
    if (format == RuntimeNumericDisplayFormat::Plus) {
        return value > 0.0 ? "+" : value < 0.0 ? "-" : " ";
    }
    if (format == RuntimeNumericDisplayFormat::ShortEng) {
        return engineeringText(value, 0, 4);
    }
    if (format == RuntimeNumericDisplayFormat::LongEng) {
        return engineeringText(
            value,
            numericClass == RuntimeNumericClass::Single ? 7 : 15, 0);
    }

    const int longPrecision =
        numericClass == RuntimeNumericClass::Single ? 7 : 15;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    switch (format) {
    case RuntimeNumericDisplayFormat::Short:
        if (!forceFractional && std::trunc(value) == value &&
            std::fabs(value) < 1.0e9) {
            output << std::fixed << std::setprecision(0);
        } else {
            output << std::fixed << std::setprecision(4);
        }
        break;
    case RuntimeNumericDisplayFormat::Long:
        if (!forceFractional && std::trunc(value) == value &&
            std::fabs(value) < 1.0e9) {
            output << std::fixed << std::setprecision(0);
        } else {
            output << std::fixed << std::setprecision(longPrecision);
        }
        break;
    case RuntimeNumericDisplayFormat::ShortE:
        output << std::scientific << std::setprecision(4);
        break;
    case RuntimeNumericDisplayFormat::LongE:
        output << std::scientific << std::setprecision(longPrecision);
        break;
    case RuntimeNumericDisplayFormat::ShortG:
        output << std::defaultfloat << std::setprecision(5);
        break;
    case RuntimeNumericDisplayFormat::LongG:
        output << std::defaultfloat << std::setprecision(longPrecision);
        break;
    case RuntimeNumericDisplayFormat::Bank:
        output << std::fixed << std::setprecision(2);
        break;
    case RuntimeNumericDisplayFormat::ShortEng:
    case RuntimeNumericDisplayFormat::LongEng:
    case RuntimeNumericDisplayFormat::Plus:
    case RuntimeNumericDisplayFormat::Hex:
    case RuntimeNumericDisplayFormat::Rational:
        break;
    }
    output << value;
    return output.str();
}

std::string numericComponentText(
    const RuntimeNumericElementValue& element, bool imaginary,
    bool magnitude, RuntimeNumericDisplayFormat format,
    bool forceFractional = false) {
    if (format == RuntimeNumericDisplayFormat::Hex) {
        return hexadecimalText(element, imaginary);
    }
    if (runtimeNumericClassIsInteger(element.numericClass) ||
        element.numericClass == RuntimeNumericClass::Logical) {
        if (format == RuntimeNumericDisplayFormat::Plus) {
            const std::string exact =
                exactIntegerText(element, imaginary, false);
            return exact == "0" ? " "
                                : exact.front() == '-' ? "-" : "+";
        }
        return exactIntegerText(element, imaginary, magnitude);
    }
    return floatingText(imaginary ? element.imaginary : element.real,
                        element.numericClass, format, magnitude,
                        forceFractional);
}

std::string numericElementText(
    const RuntimeNumericElementValue& element,
    RuntimeNumericDisplayFormat format) {
    if (format == RuntimeNumericDisplayFormat::Plus) {
        return numericComponentText(element, false, false, format);
    }
    if (element.complex &&
        format == RuntimeNumericDisplayFormat::Bank) {
        return numericComponentText(element, false, false, format);
    }
    if (element.complex &&
        format == RuntimeNumericDisplayFormat::Hex) {
        return numericComponentText(element, false, false, format) +
               "   " +
               numericComponentText(element, true, false, format) + "i";
    }
    const bool forceFractional =
        element.complex &&
        (format == RuntimeNumericDisplayFormat::Short ||
         format == RuntimeNumericDisplayFormat::Long);
    std::string result =
        numericComponentText(element, false, false, format,
                             forceFractional);
    if (!element.complex) {
        return result;
    }
    bool negative = std::signbit(element.imaginary);
    if (runtimeNumericClassIsSignedInteger(element.numericClass)) {
        negative = std::bit_cast<std::int64_t>(
                       element.integerImaginaryBits) < 0;
    }
    result += negative ? " - " : " + ";
    result += numericComponentText(element, true, negative, format,
                                   forceFractional);
    result += 'i';
    return result;
}

std::string numericValueText(const RuntimeValue& value,
                             RuntimeDisplayFormat format) {
    const auto elementText = [&value, format](size_t logicalIndex) {
        const auto element = runtimeNumericElementValue(value, logicalIndex);
        return element ? numericElementText(*element, format.numeric)
                       : std::string("<invalid-numeric>");
    };
    if (value.kind == RuntimeValueKind::Number) {
        return elementText(0);
    }

    const auto dimensions = runtimeDimensions(value);
    if (runtimeDimensionCount(value) > 2) {
        std::ostringstream output;
        output << "array(";
        for (size_t index = 0; index < dimensions.size(); ++index) {
            if (index != 0) {
                output << 'x';
            }
            output << dimensions[index];
        }
        output << ")[";
        const size_t count = runtimeShapeElementCount(value);
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) {
                output << ' ';
            }
            output << elementText(index);
        }
        output << ']';
        return output.str();
    }

    const size_t rows = dimensions.empty() ? 0 : dimensions[0];
    const size_t columns = dimensions.size() < 2 ? 0 : dimensions[1];
    std::ostringstream output;
    output << '[';
    for (size_t row = 0; row < rows; ++row) {
        if (row != 0) {
            output << "; ";
        }
        for (size_t column = 0; column < columns; ++column) {
            if (column != 0) {
                output << ' ';
            }
            output << elementText(row + column * rows);
        }
    }
    output << ']';
    return output.str();
}

} // namespace

std::string_view runtimeNumericDisplayFormatName(
    RuntimeNumericDisplayFormat format) {
    switch (format) {
    case RuntimeNumericDisplayFormat::Short:
        return "short";
    case RuntimeNumericDisplayFormat::Long:
        return "long";
    case RuntimeNumericDisplayFormat::ShortE:
        return "shortE";
    case RuntimeNumericDisplayFormat::LongE:
        return "longE";
    case RuntimeNumericDisplayFormat::ShortG:
        return "shortG";
    case RuntimeNumericDisplayFormat::LongG:
        return "longG";
    case RuntimeNumericDisplayFormat::ShortEng:
        return "shortEng";
    case RuntimeNumericDisplayFormat::LongEng:
        return "longEng";
    case RuntimeNumericDisplayFormat::Plus:
        return "+";
    case RuntimeNumericDisplayFormat::Bank:
        return "bank";
    case RuntimeNumericDisplayFormat::Hex:
        return "hex";
    case RuntimeNumericDisplayFormat::Rational:
        return "rational";
    }
    return "short";
}

std::string_view runtimeLineSpacingName(RuntimeLineSpacing spacing) {
    return spacing == RuntimeLineSpacing::Compact ? "compact" : "loose";
}

std::string runtimeFormatConsoleValue(
    const RuntimeValue& value, RuntimeDisplayFormat format) {
    return isRuntimeNumericValue(value)
               ? numericValueText(value, format)
               : runtimeValueToString(value);
}

RuntimeFormatResult runtimeFormatDisplay(
    const RuntimeValue& value, RuntimeDisplayFormat format) {
    const std::string suffix =
        format.spacing == RuntimeLineSpacing::Loose ? "\n\n" : "\n";
    if (const auto text = runtimeTextScalarUtf8(value)) {
        return RuntimeFormatResult{true, *text + suffix, {}};
    }
    return RuntimeFormatResult{
        true, runtimeFormatConsoleValue(value, format) + suffix, {}};
}

RuntimeFormatResult runtimeFormatPrintf(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        return failure("fprintf/sprintf requires a format argument");
    }
    const auto formatText = runtimeTextScalarUtf8(arguments.front());
    if (!formatText) {
        return failure("fprintf/sprintf format must be a text scalar");
    }
    const std::string format = decodeFormatEscapes(*formatText);
    if (format.size() > kMaximumFormattedOutputBytes) {
        return failure("fprintf/sprintf format exceeds the runtime limit");
    }
    std::vector<FormatAtom> atoms;
    for (size_t index = 1; index < arguments.size(); ++index) {
        if (!appendAtoms(arguments[index], atoms)) {
            return failure(
                "fprintf/sprintf data must be numeric arrays or text values");
        }
    }

    std::string output;
    size_t atomIndex = 0;
    bool firstPass = true;
    while (firstPass || atomIndex < atoms.size()) {
        firstPass = false;
        bool consumedAtom = false;
        auto pass = formatOnePass(format, atoms, atomIndex, consumedAtom);
        if (!pass.succeeded) {
            return pass;
        }
        if (pass.text.size() >
            kMaximumFormattedOutputBytes - output.size()) {
            return failure(
                "formatted output exceeds the runtime limit");
        }
        output += pass.text;
        if (!consumedAtom) {
            if (!atoms.empty()) {
                return failure(
                    "fprintf format contains no conversion for supplied data");
            }
            break;
        }
    }
    return RuntimeFormatResult{true, std::move(output), {}};
}

std::string runtimeRenderConsole(
    const std::vector<RuntimeOutputEvent>& outputEvents,
    const std::vector<RuntimeExpressionResult>& expressionResults) {
    std::string output;
    size_t outputIndex = 0;
    size_t expressionIndex = 0;
    while (outputIndex < outputEvents.size() ||
           expressionIndex < expressionResults.size()) {
        const bool emitOutput =
            expressionIndex >= expressionResults.size() ||
            (outputIndex < outputEvents.size() &&
             outputEvents[outputIndex].sequence <=
                 expressionResults[expressionIndex].sequence);
        if (emitOutput) {
            output += outputEvents[outputIndex++].text;
            continue;
        }

        const auto& expression = expressionResults[expressionIndex++];
        if (expression.outputSuppressed) {
            continue;
        }
        output += "ans = ";
        output += expression.displayText.empty()
                      ? runtimeValueToString(expression.value)
                      : expression.displayText;
        output.push_back('\n');
        if (expression.lineSpacing == RuntimeLineSpacing::Loose) {
            output.push_back('\n');
        }
    }
    return output;
}

} // namespace mparser
