#include "mparser/runtime_text_builtins.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_output.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier) {
    return BuiltinResult::failure(call.span, std::move(message),
                                  std::move(identifier));
}

BuiltinResult returnOutputs(const BuiltinCall& call,
                            std::vector<RuntimeValue> outputs) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "text builtin produced an unexpected output count",
                       "MParser:TextContractViolation");
    }
    return BuiltinResult::success(std::move(outputs));
}

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

char16_t asciiCase(char16_t value, bool upper) {
    if (upper && value >= u'a' && value <= u'z') {
        return static_cast<char16_t>(value - u'a' + u'A');
    }
    if (!upper && value >= u'A' && value <= u'Z') {
        return static_cast<char16_t>(value - u'A' + u'a');
    }
    return value;
}

bool isTrimWhitespace(char16_t value) {
    return value == u' ' || value == u'\t' || value == u'\n' ||
           value == u'\r' || value == u'\v' || value == u'\f';
}

std::u16string trimCodeUnits(std::u16string_view text) {
    size_t begin = 0;
    while (begin < text.size() && isTrimWhitespace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && isTrimWhitespace(text[end - 1])) {
        --end;
    }
    return std::u16string(text.substr(begin, end - begin));
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    const auto numeric = runtimeNumericTruthValue(value);
    return numeric && runtimeShapeElementCount(value) == 1
               ? numeric
               : std::nullopt;
}

std::optional<size_t> positiveIntegerScalar(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    const auto converted = runtimeNumericElementAsNonnegativeSize(*element);
    return converted && *converted != 0 ? converted : std::nullopt;
}

BuiltinResult caseBuiltin(const BuiltinCall& call, bool upper) {
    const RuntimeValue& input = call.arguments.front();
    RuntimeValue output = input;
    if (isRuntimeCharacterArray(output)) {
        std::transform(output.characterElements.begin(),
                       output.characterElements.end(),
                       output.characterElements.begin(),
                       [upper](char16_t value) {
                           return asciiCase(value, upper);
                       });
    } else if (isRuntimeStringArray(output)) {
        for (auto& element : output.stringElements) {
            if (element.missing) {
                continue;
            }
            std::transform(element.value.begin(), element.value.end(),
                           element.value.begin(),
                           [upper](char16_t value) {
                               return asciiCase(value, upper);
                           });
        }
    } else {
        return failure(call, std::string(upper ? "upper" : "lower") +
                                 " expects a character or string array",
                       "MParser:InvalidTextInput");
    }
    return returnOutputs(call, {std::move(output)});
}

BuiltinResult strtrimBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (isRuntimeStringArray(input)) {
        RuntimeValue output = input;
        for (auto& element : output.stringElements) {
            if (!element.missing) {
                element.value = trimCodeUnits(element.value);
            }
        }
        return returnOutputs(call, {std::move(output)});
    }
    if (!isRuntimeCharacterArray(input)) {
        return failure(call,
                       "strtrim expects a character or string array",
                       "MParser:InvalidTextInput");
    }
    const auto dimensions = runtimeDimensions(input);
    if (dimensions.size() != 2) {
        return failure(call,
                       "strtrim character input must be two-dimensional",
                       "MParser:InvalidTextShape");
    }
    if (dimensions[0] == 0 || dimensions[1] == 0) {
        return returnOutputs(call, {input});
    }

    std::vector<std::u16string> rows;
    rows.reserve(dimensions[0]);
    size_t width = 0;
    for (size_t row = 0; row < dimensions[0]; ++row) {
        const std::u16string_view source(
            input.characterElements.data() + row * dimensions[1],
            dimensions[1]);
        rows.push_back(trimCodeUnits(source));
        width = std::max(width, rows.back().size());
    }
    std::u16string storage(dimensions[0] * width, u' ');
    for (size_t row = 0; row < rows.size(); ++row) {
        std::copy(rows[row].begin(), rows[row].end(),
                  storage.begin() + static_cast<std::ptrdiff_t>(row * width));
    }
    return returnOutputs(
        call, {makeRuntimeCharacterArray({dimensions[0], width},
                                         std::move(storage))});
}

std::string floatingText(double value, size_t precision) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Inf" : "Inf";
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::defaultfloat
           << std::setprecision(static_cast<int>(std::min<size_t>(
                  precision,
                  static_cast<size_t>(std::numeric_limits<int>::max()))))
           << value;
    return output.str();
}

std::string exactIntegerText(const RuntimeNumericElementValue& value,
                             bool imaginary = false) {
    const std::uint64_t bits = imaginary ? value.integerImaginaryBits
                                         : value.integerRealBits;
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        return std::to_string(std::bit_cast<std::int64_t>(bits));
    }
    return std::to_string(bits);
}

std::string defaultNumericText(const RuntimeNumericElementValue& value,
                               size_t precision) {
    const auto component = [&](bool imaginary, bool magnitude) {
        if (runtimeNumericClassIsInteger(value.numericClass) ||
            value.numericClass == RuntimeNumericClass::Logical) {
            std::string result = exactIntegerText(value, imaginary);
            if (magnitude && !result.empty() && result.front() == '-') {
                result.erase(result.begin());
            }
            return result;
        }
        double numeric = imaginary ? value.imaginary : value.real;
        if (magnitude) {
            numeric = std::fabs(numeric);
        }
        return floatingText(numeric, precision);
    };

    std::string result = component(false, false);
    if (!value.complex) {
        return result;
    }
    bool negative = std::signbit(value.imaginary);
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        negative = std::bit_cast<std::int64_t>(
                       value.integerImaginaryBits) < 0;
    }
    result += negative ? "-" : "+";
    result += component(true, true);
    result += "i";
    return result;
}

std::optional<std::string> formattedNumericText(
    const RuntimeNumericElementValue& element,
    const std::optional<std::string>& format, size_t precision,
    std::string& error) {
    if (!format) {
        return defaultNumericText(element, precision);
    }
    if (element.complex) {
        error = "num2str format specifications for complex values are not "
                "implemented";
        return std::nullopt;
    }
    auto scalar = runtimeNumericValueFromElements(
        {1, 1}, {element}, element.numericClass);
    if (!scalar) {
        error = "num2str could not construct a numeric scalar";
        return std::nullopt;
    }
    const auto formatted = runtimeFormatPrintf(
        {makeRuntimeCharacterVectorUtf8(*format), *scalar});
    if (!formatted.succeeded) {
        error = formatted.error;
        return std::nullopt;
    }
    return formatted.text;
}

BuiltinResult num2strBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input)) {
        return failure(call, "num2str expects a numeric array",
                       "MParser:InvalidNumericTextConversion");
    }
    const auto dimensions = runtimeDimensions(input);
    if (dimensions.size() != 2) {
        return failure(call,
                       "num2str currently requires a two-dimensional array",
                       "MParser:InvalidNumericTextShape");
    }

    size_t precision = 5;
    std::optional<std::string> format;
    if (call.arguments.size() == 2) {
        if (const auto raw = positiveIntegerScalar(call.arguments[1])) {
            precision = *raw;
        } else if (const auto text =
                       runtimeTextScalarUtf8(call.arguments[1])) {
            format = *text;
        } else {
            return failure(call,
                           "num2str precision must be a positive integer or "
                           "a text format specification",
                           "MParser:InvalidNumericTextFormat");
        }
    }

    const size_t rowCount = dimensions[0];
    const size_t columnCount = dimensions[1];
    if (rowCount == 0 || columnCount == 0) {
        return returnOutputs(
            call, {makeRuntimeCharacterArray({0, 0}, std::u16string{})});
    }
    std::vector<std::vector<std::string>> fields(
        rowCount, std::vector<std::string>(columnCount));
    std::vector<size_t> widths(columnCount, 0);
    for (size_t row = 0; row < rowCount; ++row) {
        for (size_t column = 0; column < columnCount; ++column) {
            const size_t logicalIndex = row + rowCount * column;
            const auto element = runtimeNumericElementValue(
                input, logicalIndex);
            if (!element) {
                return failure(call,
                               "num2str could not map a numeric element",
                               "MParser:InvalidNumericTextConversion");
            }
            std::string error;
            auto text = formattedNumericText(
                *element, format, precision, error);
            if (!text) {
                return failure(call, std::move(error),
                               "MParser:InvalidNumericTextFormat");
            }
            fields[row][column] = std::move(*text);
            widths[column] = std::max(widths[column],
                                      fields[row][column].size());
        }
    }

    std::vector<std::string> rows(rowCount);
    size_t outputWidth = 0;
    for (size_t row = 0; row < rowCount; ++row) {
        for (size_t column = 0; column < columnCount; ++column) {
            if (!format) {
                rows[row].append(widths[column] -
                                     fields[row][column].size(),
                                 ' ');
            }
            rows[row] += fields[row][column];
            if (!format && column + 1 != columnCount) {
                rows[row] += "    ";
            }
        }
        outputWidth = std::max(outputWidth, rows[row].size());
    }
    std::u16string storage(rowCount * outputWidth, u' ');
    for (size_t row = 0; row < rowCount; ++row) {
        const auto converted = runtimeUtf8ToUtf16(rows[row]);
        if (converted.size() > outputWidth) {
            return failure(call,
                           "num2str produced an invalid UTF-16 row width",
                           "MParser:InvalidNumericTextConversion");
        }
        std::copy(converted.begin(), converted.end(),
                  storage.begin() + static_cast<std::ptrdiff_t>(
                                        row * outputWidth));
    }
    return returnOutputs(
        call, {makeRuntimeCharacterArray({rowCount, outputWidth},
                                         std::move(storage))});
}

std::string decodeDelimiterEscapes(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '\\' || index + 1 == input.size()) {
            output.push_back(input[index]);
            continue;
        }
        const char escaped = input[++index];
        switch (escaped) {
        case '0': output.push_back('\0'); break;
        case 'a': output.push_back('\a'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'v': output.push_back('\v'); break;
        case '\\': output.push_back('\\'); break;
        default:
            output.push_back('\\');
            output.push_back(escaped);
            break;
        }
    }
    return output;
}

std::optional<std::vector<std::string>> textList(
    const RuntimeValue& value) {
    if (const auto scalar = runtimeTextScalarUtf8(value)) {
        return std::vector<std::string>{*scalar};
    }
    std::vector<std::string> result;
    if (isRuntimeStringArray(value)) {
        result.reserve(runtimeShapeElementCount(value));
        for (size_t index = 0; index < runtimeShapeElementCount(value);
             ++index) {
            const auto* element = runtimeStringElement(value, index);
            if (!element || element->missing) {
                return std::nullopt;
            }
            result.push_back(runtimeUtf16ToUtf8(element->value));
        }
        return result;
    }
    if (value.kind != RuntimeValueKind::Cell) {
        return std::nullopt;
    }
    result.reserve(value.cells.size());
    for (size_t index = 0; index < runtimeShapeElementCount(value);
         ++index) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            value, index);
        if (!offset || *offset >= value.cells.size()) {
            return std::nullopt;
        }
        const auto text = runtimeTextScalarUtf8(value.cells[*offset]);
        if (!text) {
            return std::nullopt;
        }
        result.push_back(*text);
    }
    return result;
}

RuntimeValue textSequence(bool stringOutput,
                          const std::vector<std::string>& values) {
    if (stringOutput) {
        std::vector<RuntimeStringElement> elements;
        elements.reserve(values.size());
        for (const auto& value : values) {
            elements.push_back(
                RuntimeStringElement{runtimeUtf8ToUtf16(value), false});
        }
        return makeRuntimeStringArray({1, values.size()},
                                      std::move(elements));
    }
    std::vector<RuntimeValue> cells;
    cells.reserve(values.size());
    for (const auto& value : values) {
        cells.push_back(makeRuntimeCharacterVectorUtf8(value));
    }
    return makeRuntimeCellValue({1, values.size()}, std::move(cells));
}

struct SplitMatch {
    size_t position = 0;
    size_t length = 0;
    std::string text;
};

std::optional<SplitMatch> nextSimpleDelimiter(
    std::string_view input, size_t start,
    const std::vector<std::string>& delimiters) {
    std::optional<SplitMatch> best;
    for (const auto& delimiter : delimiters) {
        if (delimiter.empty()) {
            continue;
        }
        const size_t position = input.find(delimiter, start);
        if (position == std::string_view::npos) {
            continue;
        }
        if (!best || position < best->position) {
            best = SplitMatch{position, delimiter.size(), delimiter};
        }
    }
    return best;
}

std::optional<SplitMatch> nextRegexDelimiter(
    std::string_view input, size_t start,
    const std::vector<std::regex>& delimiters) {
    std::optional<SplitMatch> best;
    const std::string remaining(input.substr(start));
    for (const auto& delimiter : delimiters) {
        std::smatch match;
        if (!std::regex_search(remaining, match, delimiter)) {
            continue;
        }
        const size_t position =
            start + static_cast<size_t>(match.position());
        const size_t length = static_cast<size_t>(match.length());
        if (!best || position < best->position) {
            best = SplitMatch{position, length, match.str()};
        }
    }
    return best;
}

BuiltinResult strsplitBuiltin(const BuiltinCall& call) {
    const auto inputText = runtimeTextScalarUtf8(call.arguments.front());
    if (!inputText) {
        return failure(call,
                       "strsplit input must be a character vector or "
                       "string scalar",
                       "MParser:InvalidTextInput");
    }
    const bool stringOutput =
        isRuntimeStringScalar(call.arguments.front());
    size_t cursor = 1;
    std::vector<std::string> delimiters;
    bool whitespaceDefault = false;
    if (cursor < call.arguments.size() &&
        call.arguments[cursor].kind !=
            RuntimeValueKind::NameValueArgument) {
        const auto list = textList(call.arguments[cursor]);
        if (!list) {
            return failure(call,
                           "strsplit delimiter must be text or a text "
                           "array",
                           "MParser:InvalidTextDelimiter");
        }
        delimiters = *list;
        ++cursor;
    }
    if (delimiters.empty()) {
        whitespaceDefault = true;
        delimiters = {"[ \\f\\n\\r\\t\\v]+"};
    }

    bool collapse = true;
    bool regexMode = whitespaceDefault;
    while (cursor < call.arguments.size()) {
        std::string name;
        const RuntimeValue* value = nullptr;
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            name = call.arguments[cursor].text;
            if (call.arguments[cursor].cells.size() != 1) {
                return failure(call,
                               "strsplit name-value argument is malformed",
                               "MParser:InvalidTextOption");
            }
            value = &call.arguments[cursor].cells.front();
            ++cursor;
        } else {
            const auto rawName =
                runtimeTextScalarUtf8(call.arguments[cursor]);
            if (!rawName || cursor + 1 >= call.arguments.size()) {
                return failure(call,
                               "strsplit options must be name-value pairs",
                               "MParser:InvalidTextOption");
            }
            name = *rawName;
            value = &call.arguments[cursor + 1];
            cursor += 2;
        }
        name = asciiLower(std::move(name));
        if (name == "collapsedelimiters") {
            const auto raw = logicalScalar(*value);
            if (!raw) {
                return failure(call,
                               "CollapseDelimiters must be a logical scalar",
                               "MParser:InvalidTextOption");
            }
            collapse = *raw;
        } else if (name == "delimitertype") {
            const auto raw = runtimeTextScalarUtf8(*value);
            if (!raw) {
                return failure(call,
                               "DelimiterType must be Simple or "
                               "RegularExpression",
                               "MParser:InvalidTextOption");
            }
            const std::string mode = asciiLower(*raw);
            if (mode == "simple") {
                regexMode = false;
            } else if (mode == "regularexpression") {
                regexMode = true;
            } else {
                return failure(call,
                               "DelimiterType must be Simple or "
                               "RegularExpression",
                               "MParser:InvalidTextOption");
            }
        } else {
            return failure(call, "unknown strsplit option: " + name,
                           "MParser:InvalidTextOption");
        }
    }

    std::vector<std::regex> regexDelimiters;
    try {
        if (regexMode) {
            regexDelimiters.reserve(delimiters.size());
            for (const auto& delimiter : delimiters) {
                regexDelimiters.emplace_back(delimiter,
                                             std::regex::ECMAScript);
            }
        } else {
            for (auto& delimiter : delimiters) {
                delimiter = decodeDelimiterEscapes(delimiter);
            }
        }
    } catch (const std::regex_error& error) {
        return failure(call,
                       "invalid strsplit regular expression: " +
                           std::string(error.what()),
                       "MParser:InvalidRegularExpression");
    }

    std::vector<std::string> parts;
    std::vector<std::string> matches;
    size_t position = 0;
    while (position <= inputText->size()) {
        const auto next = regexMode
                              ? nextRegexDelimiter(*inputText, position,
                                                   regexDelimiters)
                              : nextSimpleDelimiter(*inputText, position,
                                                    delimiters);
        if (!next) {
            parts.push_back(inputText->substr(position));
            break;
        }
        if (next->length == 0) {
            return failure(call,
                           "strsplit delimiter must not match empty text",
                           "MParser:InvalidTextDelimiter");
        }
        parts.push_back(inputText->substr(position,
                                          next->position - position));
        std::string consumed = next->text;
        position = next->position + next->length;
        if (collapse) {
            while (position < inputText->size()) {
                const auto adjacent = regexMode
                    ? nextRegexDelimiter(*inputText, position,
                                         regexDelimiters)
                    : nextSimpleDelimiter(*inputText, position,
                                          delimiters);
                if (!adjacent || adjacent->position != position ||
                    adjacent->length == 0) {
                    break;
                }
                consumed += adjacent->text;
                position += adjacent->length;
            }
        }
        matches.push_back(std::move(consumed));
    }
    std::vector<RuntimeValue> outputs;
    outputs.push_back(textSequence(stringOutput, parts));
    if (call.requestedOutputCount > 1) {
        outputs.push_back(textSequence(stringOutput, matches));
    }
    return returnOutputs(call, std::move(outputs));
}

struct RegexMatch {
    size_t bytePosition = 0;
    size_t byteLength = 0;
    std::string text;
    std::vector<std::string> captures;
};

size_t utf16PrefixLength(std::string_view text, size_t byteCount) {
    return runtimeUtf8ToUtf16(text.substr(0, byteCount)).size();
}

RuntimeValue numericRow(const std::vector<double>& values) {
    return runtimeNumericValueFromLogicalOrder(
               {1, values.size()}, values, RuntimeNumericClass::Double)
        .value_or(RuntimeValue{});
}

std::vector<double> substringPositions(std::u16string_view input,
                                       std::u16string_view pattern) {
    std::vector<double> positions;
    if (pattern.empty() || pattern.size() > input.size()) {
        return positions;
    }
    size_t cursor = 0;
    while (cursor + pattern.size() <= input.size()) {
        const size_t found = input.find(pattern, cursor);
        if (found == std::u16string_view::npos) {
            break;
        }
        positions.push_back(static_cast<double>(found + 1));
        cursor = found + 1;
    }
    return positions;
}

std::u16string replaceSubstrings(std::u16string_view input,
                                 std::u16string_view pattern,
                                 std::u16string_view replacement) {
    if (pattern.empty() || pattern.size() > input.size()) {
        return std::u16string(input);
    }
    std::u16string output;
    size_t copiedThrough = 0;
    size_t cursor = 0;
    while (cursor + pattern.size() <= input.size()) {
        const size_t found = input.find(pattern, cursor);
        if (found == std::u16string_view::npos) {
            break;
        }
        if (found > copiedThrough) {
            output.append(input.substr(copiedThrough,
                                       found - copiedThrough));
        }
        output.append(replacement);
        copiedThrough = std::max(copiedThrough,
                                 found + pattern.size());
        cursor = found + 1;
    }
    output.append(input.substr(copiedThrough));
    return output;
}

BuiltinResult strfindBuiltin(const BuiltinCall& call) {
    const auto pattern = runtimeTextScalarCodeUnits(call.arguments[1]);
    if (!pattern) {
        return failure(call, "strfind pattern must be a text scalar",
                       "MParser:InvalidTextPattern");
    }
    const RuntimeValue& input = call.arguments.front();
    if (const auto scalar = runtimeTextScalarCodeUnits(input)) {
        return returnOutputs(
            call, {numericRow(substringPositions(*scalar, *pattern))});
    }

    std::vector<RuntimeValue> cells;
    const auto dimensions = runtimeDimensions(input);
    if (isRuntimeStringArray(input)) {
        cells.reserve(input.stringElements.size());
        for (const auto& element : input.stringElements) {
            cells.push_back(numericRow(
                element.missing
                    ? std::vector<double>{}
                    : substringPositions(element.value, *pattern)));
        }
    } else if (input.kind == RuntimeValueKind::Cell) {
        cells.reserve(input.cells.size());
        for (const RuntimeValue& element : input.cells) {
            const auto text = runtimeTextScalarCodeUnits(element);
            if (!text) {
                return failure(
                    call,
                    "strfind Cell inputs must contain text scalars",
                    "MParser:InvalidTextInput");
            }
            cells.push_back(numericRow(
                substringPositions(*text, *pattern)));
        }
    } else {
        return failure(
            call,
            "strfind input must be a text scalar, string array, or Cell "
            "text array",
            "MParser:InvalidTextInput");
    }
    return returnOutputs(
        call, {makeRuntimeCellValue(dimensions, std::move(cells))});
}

BuiltinResult strrepBuiltin(const BuiltinCall& call) {
    const auto pattern = runtimeTextScalarCodeUnits(call.arguments[1]);
    const auto replacement = runtimeTextScalarCodeUnits(call.arguments[2]);
    if (!pattern || !replacement) {
        return failure(
            call,
            "strrep pattern and replacement must be text scalars",
            "MParser:InvalidTextPattern");
    }
    const RuntimeValue& input = call.arguments.front();
    if (isRuntimeCharacterVector(input)) {
        const auto text = runtimeTextScalarCodeUnits(input);
        return text
                   ? returnOutputs(
                         call,
                         {makeRuntimeCharacterVector(
                             replaceSubstrings(*text, *pattern,
                                               *replacement))})
                   : failure(call, "strrep could not read character input",
                             "MParser:InvalidTextInput");
    }
    if (isRuntimeStringArray(input)) {
        RuntimeValue output = input;
        for (auto& element : output.stringElements) {
            if (!element.missing) {
                element.value = replaceSubstrings(
                    element.value, *pattern, *replacement);
            }
        }
        return returnOutputs(call, {std::move(output)});
    }
    if (input.kind == RuntimeValueKind::Cell) {
        RuntimeValue output = input;
        for (RuntimeValue& element : output.cells) {
            const auto text = runtimeTextScalarCodeUnits(element);
            if (!text) {
                return failure(
                    call,
                    "strrep Cell inputs must contain text scalars",
                    "MParser:InvalidTextInput");
            }
            const auto replaced = replaceSubstrings(
                *text, *pattern, *replacement);
            element = isRuntimeStringScalar(element)
                          ? makeRuntimeStringScalar(replaced)
                          : makeRuntimeCharacterVector(replaced);
        }
        return returnOutputs(call, {std::move(output)});
    }
    return failure(
        call,
        "strrep input must be a character vector, string array, or Cell "
        "text array",
        "MParser:InvalidTextInput");
}

BuiltinResult regexpBuiltin(const BuiltinCall& call) {
    const auto input = runtimeTextScalarUtf8(call.arguments[0]);
    const auto pattern = runtimeTextScalarUtf8(call.arguments[1]);
    if (!input || !pattern) {
        return failure(call,
                       "regexp input and expression must be text scalars",
                       "MParser:InvalidRegularExpressionInput");
    }
    bool once = false;
    bool ignoreCase = false;
    std::vector<std::string> keys;
    for (size_t index = 2; index < call.arguments.size(); ++index) {
        const auto option = runtimeTextScalarUtf8(call.arguments[index]);
        if (!option) {
            return failure(call, "regexp options must be text scalars",
                           "MParser:InvalidRegularExpressionOption");
        }
        const std::string lower = asciiLower(*option);
        if (lower == "once") {
            once = true;
        } else if (lower == "ignorecase") {
            ignoreCase = true;
        } else if (lower == "start" || lower == "end" ||
                   lower == "match" || lower == "split" ||
                   lower == "tokens") {
            keys.push_back(lower);
        } else {
            return failure(call,
                           "regexp option is not implemented: " + *option,
                           "MParser:UnsupportedRegularExpressionOption");
        }
    }
    const std::vector<std::string> defaults = {
        "start", "end", "tokens", "match", "split"};
    if (keys.empty()) {
        if (call.requestedOutputCount > defaults.size()) {
            return failure(call,
                           "regexp requested too many default outputs",
                           "MParser:InvalidRegularExpressionOutput");
        }
        keys.assign(defaults.begin(),
                    defaults.begin() + static_cast<std::ptrdiff_t>(
                        call.requestedOutputCount));
    } else if (call.requestedOutputCount != 0 &&
               keys.size() != call.requestedOutputCount) {
        return failure(call,
                       "regexp output keywords must match the number of "
                       "requested outputs",
                       "MParser:InvalidRegularExpressionOutput");
    }

    std::vector<RegexMatch> matches;
    try {
        auto flags = std::regex::ECMAScript;
        if (ignoreCase) {
            flags |= std::regex::icase;
        }
        const std::regex expression(*pattern, flags);
        for (std::sregex_iterator iterator(input->begin(), input->end(),
                                           expression), end;
             iterator != end; ++iterator) {
            RegexMatch match;
            match.bytePosition =
                static_cast<size_t>(iterator->position());
            match.byteLength = static_cast<size_t>(iterator->length());
            match.text = iterator->str();
            for (size_t group = 1; group < iterator->size(); ++group) {
                match.captures.push_back((*iterator)[group].matched
                                             ? (*iterator)[group].str()
                                             : std::string{});
            }
            matches.push_back(std::move(match));
            if (once) {
                break;
            }
        }
    } catch (const std::regex_error& error) {
        return failure(call,
                       "invalid regular expression: " +
                           std::string(error.what()),
                       "MParser:InvalidRegularExpression");
    }

    const bool stringOutput = isRuntimeStringScalar(call.arguments[0]);
    std::vector<RuntimeValue> outputs;
    outputs.reserve(keys.size());
    for (const auto& key : keys) {
        if (key == "start" || key == "end") {
            std::vector<double> indices;
            indices.reserve(matches.size());
            for (const auto& match : matches) {
                const size_t start = utf16PrefixLength(
                    *input, match.bytePosition) + 1;
                const size_t finish = utf16PrefixLength(
                    *input, match.bytePosition + match.byteLength);
                indices.push_back(static_cast<double>(
                    key == "start" ? start : finish));
            }
            if (once && indices.size() == 1) {
                outputs.push_back(makeRuntimeNumberValue(indices.front()));
            } else {
                outputs.push_back(numericRow(indices));
            }
            continue;
        }
        if (key == "match") {
            std::vector<std::string> values;
            for (const auto& match : matches) {
                values.push_back(match.text);
            }
            if (once && values.size() == 1) {
                outputs.push_back(stringOutput
                    ? makeRuntimeStringScalarUtf8(values.front())
                    : makeRuntimeCharacterVectorUtf8(values.front()));
            } else {
                outputs.push_back(textSequence(stringOutput, values));
            }
            continue;
        }
        if (key == "split") {
            std::vector<std::string> values;
            size_t position = 0;
            for (const auto& match : matches) {
                values.push_back(input->substr(
                    position, match.bytePosition - position));
                position = match.bytePosition + match.byteLength;
            }
            values.push_back(input->substr(position));
            outputs.push_back(textSequence(stringOutput, values));
            continue;
        }
        std::vector<RuntimeValue> tokenMatches;
        tokenMatches.reserve(matches.size());
        for (const auto& match : matches) {
            tokenMatches.push_back(
                textSequence(stringOutput, match.captures));
        }
        if (once && tokenMatches.size() == 1) {
            outputs.push_back(std::move(tokenMatches.front()));
        } else {
            const size_t tokenMatchCount = tokenMatches.size();
            outputs.push_back(makeRuntimeCellValue(
                {1, tokenMatchCount}, std::move(tokenMatches)));
        }
    }
    return returnOutputs(call, std::move(outputs));
}

} // namespace

bool isRuntimeTextLibraryBuiltin(std::string_view name) {
    return name == "lower" || name == "num2str" ||
           name == "regexp" || name == "strsplit" ||
           name == "strfind" || name == "strrep" ||
           name == "strtrim" || name == "upper";
}

BuiltinResult invokeRuntimeTextLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "upper" || name == "lower") {
        return caseBuiltin(call, name == "upper");
    }
    if (name == "strtrim") {
        return strtrimBuiltin(call);
    }
    if (name == "num2str") {
        return num2strBuiltin(call);
    }
    if (name == "strsplit") {
        return strsplitBuiltin(call);
    }
    if (name == "strfind") {
        return strfindBuiltin(call);
    }
    if (name == "strrep") {
        return strrepBuiltin(call);
    }
    if (name == "regexp") {
        return regexpBuiltin(call);
    }
    return failure(call, "unknown text library builtin",
                   "MParser:UnknownBuiltin");
}

} // namespace mparser
