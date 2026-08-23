#include "mparser/runtime/core/session/runtime_exception.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <utility>

namespace mparser {
namespace {

constexpr bool isAsciiLetter(char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
}

constexpr bool isAsciiDigit(char character) {
    return character >= '0' && character <= '9';
}

constexpr bool isAsciiIdentifierCharacter(char character) {
    return isAsciiLetter(character) || isAsciiDigit(character) ||
           character == '_';
}

RuntimeValue characterValue(std::string_view value) {
    return makeRuntimeCharacterVectorUtf8(value);
}

RuntimeValue numberValue(double value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue emptyCellValue() {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    setRuntimeDimensions(result, {0, 1});
    return result;
}

RuntimeValue emptyStackValue() {
    return makeRuntimeStructArrayValue(
        {"file", "name", "line"}, {}, {0, 1});
}

RuntimeStructElement stackFrameElement(
    const RuntimeExceptionFrame& frame) {
    RuntimeStructElement result;
    result.emplace("file", characterValue(frame.file));
    result.emplace("name", characterValue(frame.name));
    result.emplace("line", numberValue(static_cast<double>(frame.line)));
    return result;
}

RuntimeValue exceptionValue(std::string identifier, std::string message) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeExceptionClassName);
    setRuntimeDimensions(result, {1, 1});
    result.fieldOrder = {"identifier", "message", "stack", "cause",
                         "Correction"};
    result.fields.emplace("identifier", characterValue(identifier));
    result.fields.emplace("message", characterValue(message));
    result.fields.emplace("stack", emptyStackValue());
    result.fields.emplace("cause", emptyCellValue());
    result.fields.emplace("Correction", RuntimeValue{});
    return result;
}

RuntimeExceptionOperationResult failure(std::string error) {
    return RuntimeExceptionOperationResult{false, {}, std::move(error)};
}

RuntimeExceptionOperationResult success(RuntimeValue value) {
    return RuntimeExceptionOperationResult{true, std::move(value), {}};
}

struct FormatSpecifier {
    size_t end = 0;
    std::optional<int> width;
    std::optional<int> precision;
    char conversion = '\0';
};

std::optional<FormatSpecifier> parseFormatSpecifier(
    std::string_view format, size_t percent, std::string& error) {
    size_t cursor = percent + 1;
    if (cursor >= format.size()) {
        error = "exception message ends with an incomplete format specifier";
        return std::nullopt;
    }

    if (format[cursor] == '%') {
        return FormatSpecifier{cursor, {}, {}, '%'};
    }

    FormatSpecifier result;
    if (isAsciiDigit(format[cursor])) {
        int width = 0;
        while (cursor < format.size() &&
               isAsciiDigit(format[cursor])) {
            width = width * 10 + (format[cursor] - '0');
            ++cursor;
        }
        result.width = width;
    }
    if (cursor < format.size() && format[cursor] == '.') {
        ++cursor;
        if (cursor >= format.size() ||
            !isAsciiDigit(format[cursor])) {
            error = "exception message precision requires digits";
            return std::nullopt;
        }
        int precision = 0;
        while (cursor < format.size() &&
               isAsciiDigit(format[cursor])) {
            precision = precision * 10 + (format[cursor] - '0');
            ++cursor;
        }
        result.precision = precision;
    }
    if (cursor >= format.size()) {
        error = "exception message ends with an incomplete format specifier";
        return std::nullopt;
    }

    result.conversion = format[cursor];
    result.end = cursor;
    constexpr std::string_view supported = "sdifgec";
    if (supported.find(result.conversion) == std::string_view::npos) {
        error = "unsupported exception message format specifier: %";
        error.push_back(result.conversion);
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> formatValue(
    const RuntimeValue& value, const FormatSpecifier& specifier,
    std::string& error) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    if (specifier.width) {
        output << std::setw(*specifier.width);
    }
    if (specifier.precision) {
        output << std::setprecision(*specifier.precision);
    }

    if (specifier.conversion == 's') {
        const auto text = runtimeTextScalarUtf8(value);
        if (!text) {
            error = "%s exception message values must be text scalars";
            return std::nullopt;
        }
        output << *text;
        return output.str();
    }
    if (specifier.conversion == 'c') {
        const auto text = runtimeTextScalarCodeUnits(value);
        if (text && !text->empty()) {
            size_t length = 1;
            if ((*text)[0] >= 0xd800 && (*text)[0] <= 0xdbff &&
                text->size() >= 2 && (*text)[1] >= 0xdc00 &&
                (*text)[1] <= 0xdfff) {
                length = 2;
            }
            output << runtimeUtf16ToUtf8(
                std::u16string_view(text->data(), length));
            return output.str();
        }
        const auto numeric = value.kind == RuntimeValueKind::Number
                                 ? runtimeNumericElementValue(value, 0)
                                 : std::nullopt;
        if (numeric) {
            output << static_cast<char>(
                static_cast<int>(numeric->real));
            return output.str();
        }
        error = "%c exception message values must be text or numeric scalars";
        return std::nullopt;
    }
    const auto numeric = value.kind == RuntimeValueKind::Number
                             ? runtimeNumericElementValue(value, 0)
                             : std::nullopt;
    if (!numeric) {
        error = "numeric exception message specifiers require scalar numbers";
        return std::nullopt;
    }

    switch (specifier.conversion) {
    case 'd':
    case 'i':
        if (runtimeNumericClassIsInteger(numeric->numericClass)) {
            if (runtimeNumericClassIsSignedInteger(
                    numeric->numericClass)) {
                output << std::bit_cast<std::int64_t>(
                    numeric->integerRealBits);
            } else {
                output << numeric->integerRealBits;
            }
        } else {
            output << static_cast<long long>(numeric->real);
        }
        break;
    case 'f':
        output << std::fixed << numeric->real;
        break;
    case 'e':
        output << std::scientific << numeric->real;
        break;
    case 'g':
        output << std::defaultfloat << numeric->real;
        break;
    default:
        return std::nullopt;
    }
    return output.str();
}

std::optional<std::string> formatMessage(
    std::string_view format,
    const std::vector<RuntimeValue>& replacements,
    std::string& error) {
    if (replacements.empty()) {
        return std::string(format);
    }

    std::string result;
    size_t replacement = 0;
    for (size_t cursor = 0; cursor < format.size();) {
        if (format[cursor] != '%') {
            result.push_back(format[cursor++]);
            continue;
        }

        const auto specifier = parseFormatSpecifier(format, cursor, error);
        if (!specifier) {
            return std::nullopt;
        }
        cursor = specifier->end + 1;
        if (specifier->conversion == '%') {
            result.push_back('%');
            continue;
        }
        if (replacement >= replacements.size()) {
            error = "exception message has too few replacement values";
            return std::nullopt;
        }
        const auto formatted = formatValue(
            replacements[replacement++], *specifier, error);
        if (!formatted) {
            return std::nullopt;
        }
        result += *formatted;
    }
    if (replacement != replacements.size()) {
        error = "exception message has unused replacement values";
        return std::nullopt;
    }
    return result;
}

RuntimeExceptionOperationResult constructException(
    std::string identifier, const RuntimeValue& message,
    const std::vector<RuntimeValue>& replacements) {
    const auto messageText = runtimeTextScalarUtf8(message);
    if (!messageText) {
        return failure("exception message must be a character vector or "
                       "string scalar");
    }
    std::string error;
    auto formatted = formatMessage(*messageText, replacements, error);
    if (!formatted) {
        return failure(std::move(error));
    }
    return success(exceptionValue(std::move(identifier),
                                  std::move(*formatted)));
}

std::string exceptionTextProperty(const RuntimeValue& exception,
                                  std::string_view name) {
    const auto field = exception.fields.find(std::string(name));
    if (field == exception.fields.end()) {
        return {};
    }
    return runtimeTextScalarUtf8(field->second).value_or(std::string{});
}

std::optional<std::vector<RuntimeExceptionFrame>> stackFramesFromValue(
    const RuntimeValue& stack, std::string& error) {
    if (stack.kind != RuntimeValueKind::Struct) {
        error = "exception stack must be a structure array";
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(stack);
    if (dimensions.size() != 2 || dimensions[1] != 1) {
        error = "exception stack must be an N-by-1 structure array";
        return std::nullopt;
    }

    std::vector<RuntimeExceptionFrame> frames;
    frames.reserve(runtimeStructElementCount(stack));
    for (size_t index = 0; index < runtimeStructElementCount(stack); ++index) {
        const RuntimeValue* file = runtimeStructField(stack, "file", index);
        const RuntimeValue* name = runtimeStructField(stack, "name", index);
        const RuntimeValue* line = runtimeStructField(stack, "line", index);
        const auto fileText = file
            ? runtimeTextScalarUtf8(*file) : std::nullopt;
        const auto nameText = name
            ? runtimeTextScalarUtf8(*name) : std::nullopt;
        if (!fileText || !nameText || !line ||
            line->kind != RuntimeValueKind::Number ||
            !std::isfinite(line->number) || line->number < 1.0 ||
            std::floor(line->number) != line->number ||
            line->number > static_cast<double>(
                               std::numeric_limits<int>::max())) {
            error = "exception stack entries require text file/name fields "
                    "and a positive integer line field";
            return std::nullopt;
        }
        frames.push_back(RuntimeExceptionFrame{
            *fileText, *nameText, static_cast<int>(line->number)});
    }
    return frames;
}

void attachFrames(RuntimeValue& exception,
                  const std::vector<RuntimeExceptionFrame>& frames) {
    std::vector<RuntimeStructElement> publicFrames;
    publicFrames.reserve(frames.size());
    for (const auto& frame : frames) {
        publicFrames.push_back(stackFrameElement(frame));
    }
    exception.fields["stack"] = makeRuntimeStructArrayValue(
        {"file", "name", "line"}, std::move(publicFrames),
        {frames.size(), 1});
}

} // namespace

bool isRuntimeException(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           value.className == kRuntimeExceptionClassName;
}

bool isRuntimeExceptionIdentifier(std::string_view identifier) {
    if (identifier.empty()) {
        return false;
    }

    size_t fieldBegin = 0;
    size_t fieldCount = 0;
    while (fieldBegin < identifier.size()) {
        const size_t fieldEnd = identifier.find(':', fieldBegin);
        const std::string_view field = identifier.substr(
            fieldBegin, fieldEnd == std::string_view::npos
                            ? std::string_view::npos
                            : fieldEnd - fieldBegin);
        if (field.empty() || !isAsciiLetter(field.front()) ||
            !std::all_of(field.begin() + 1, field.end(),
                         isAsciiIdentifierCharacter)) {
            return false;
        }
        ++fieldCount;
        if (fieldEnd == std::string_view::npos) {
            break;
        }
        fieldBegin = fieldEnd + 1;
    }
    return fieldCount >= 2;
}

RuntimeExceptionOperationResult runtimeConstructMException(
    const std::vector<RuntimeValue>& arguments) {
    const auto identifier = arguments.empty()
        ? std::nullopt : runtimeTextScalarUtf8(arguments[0]);
    if (arguments.size() < 2 || !identifier) {
        return failure("MException expects an identifier, message, and "
                       "optional replacement values");
    }
    if (!isRuntimeExceptionIdentifier(*identifier)) {
        return failure("invalid MException identifier: " + *identifier);
    }
    return constructException(
        *identifier, arguments[1],
        std::vector<RuntimeValue>(arguments.begin() + 2, arguments.end()));
}

RuntimeExceptionOperationResult runtimeCreateErrorException(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        return failure("error expects a message or error structure");
    }
    if (arguments.size() == 1 &&
        arguments.front().kind == RuntimeValueKind::Struct) {
        if (!isRuntimeScalarStruct(arguments.front())) {
            return failure("error structure must be scalar");
        }
        const RuntimeValue* message =
            runtimeStructField(arguments.front(), "message");
        const RuntimeValue* identifier =
            runtimeStructField(arguments.front(), "identifier");
        const RuntimeValue* stack =
            runtimeStructField(arguments.front(), "stack");
        const RuntimeValue* correction =
            runtimeStructField(arguments.front(), "correction");
        if (!message || !runtimeTextScalarUtf8(*message)) {
            return failure("error structure requires a text message field");
        }
        if (correction && correction->kind != RuntimeValueKind::Missing) {
            return failure("error correction objects are outside the "
                           "supported exception subset");
        }
        std::string id;
        if (identifier) {
            const auto identifierText = runtimeTextScalarUtf8(*identifier);
            if (!identifierText ||
                (!identifierText->empty() &&
                 !isRuntimeExceptionIdentifier(*identifierText))) {
                return failure("error structure contains an invalid "
                               "identifier field");
            }
            id = *identifierText;
        }
        auto result = constructException(std::move(id), *message, {});
        if (!result.succeeded || !stack) {
            return result;
        }
        std::string stackError;
        const auto frames = stackFramesFromValue(*stack, stackError);
        if (!frames) {
            return failure(std::move(stackError));
        }
        attachFrames(result.value, *frames);
        return result;
    }
    const auto firstText = runtimeTextScalarUtf8(arguments.front());
    if (!firstText) {
        return failure("error message must be a character vector or string "
                       "scalar");
    }

    size_t messageIndex = 0;
    std::string identifier;
    if (arguments.size() >= 2 &&
        isRuntimeExceptionIdentifier(*firstText) &&
        runtimeTextScalarUtf8(arguments[1])) {
        identifier = *firstText;
        messageIndex = 1;
    }
    return constructException(
        std::move(identifier), arguments[messageIndex],
        std::vector<RuntimeValue>(arguments.begin() + messageIndex + 1,
                                  arguments.end()));
}

RuntimeExceptionOperationResult runtimePrepareExceptionForThrow(
    const RuntimeValue& exception,
    const std::vector<RuntimeExceptionFrame>& frames,
    RuntimeExceptionStackPolicy policy) {
    if (!isRuntimeException(exception)) {
        return failure("throw, rethrow, and throwAsCaller require a scalar "
                       "MException object");
    }
    if (policy == RuntimeExceptionStackPolicy::Preserve &&
        runtimeExceptionFrameCount(exception) == 0) {
        return failure("rethrow requires a previously thrown MException");
    }

    RuntimeValue result = exception;
    if (policy != RuntimeExceptionStackPolicy::Preserve) {
        const size_t first =
            policy == RuntimeExceptionStackPolicy::AsCaller &&
                    !frames.empty()
                ? 1
                : 0;
        attachFrames(result, std::vector<RuntimeExceptionFrame>(
                                 frames.begin() +
                                     static_cast<std::ptrdiff_t>(first),
                                 frames.end()));
    }
    return success(std::move(result));
}

namespace {

std::vector<RuntimeValue> exceptionCauseValues(
    const RuntimeValue& exception) {
    const auto found = exception.fields.find("cause");
    if (found == exception.fields.end() ||
        found->second.kind != RuntimeValueKind::Cell) {
        return {};
    }
    return found->second.cells;
}

DiagnosticCause diagnosticCauseFromException(
    const RuntimeValue& exception, size_t depth) {
    DiagnosticCause result;
    result.identifier = exceptionTextProperty(exception, "identifier");
    result.message = exceptionTextProperty(exception, "message");
    result.stack = runtimeExceptionFrames(exception);
    if (depth >= 32) {
        return result;
    }
    for (const auto& cause : exceptionCauseValues(exception)) {
        if (isRuntimeException(cause)) {
            result.causes.push_back(
                diagnosticCauseFromException(cause, depth + 1));
        }
    }
    return result;
}

RuntimeValue exceptionFromDiagnosticCause(
    const DiagnosticCause& cause, size_t depth) {
    RuntimeValue result = exceptionValue(cause.identifier, cause.message);
    attachFrames(result, cause.stack);
    if (depth >= 32) {
        return result;
    }
    RuntimeValue& causes = result.fields["cause"];
    for (const auto& nested : cause.causes) {
        causes.cells.push_back(
            exceptionFromDiagnosticCause(nested, depth + 1));
    }
    setRuntimeDimensions(causes, {causes.cells.size(), 1});
    return result;
}

void appendExceptionReport(std::ostringstream& output,
                           const RuntimeValue& exception,
                           std::string_view indentation,
                           size_t depth) {
    const std::string identifier =
        exceptionTextProperty(exception, "identifier");
    if (!identifier.empty()) {
        output << indentation << identifier << ": ";
    } else {
        output << indentation;
    }
    output << exceptionTextProperty(exception, "message");

    for (const auto& frame : runtimeExceptionFrames(exception)) {
        output << '\n' << indentation << "  at " << frame.name;
        if (!frame.file.empty()) {
            output << " (" << frame.file << ':' << frame.line << ')';
        } else {
            output << " (line " << frame.line << ')';
        }
    }

    if (depth >= 32) {
        output << '\n' << indentation << "Caused by: <depth limit>";
        return;
    }
    for (const auto& cause : exceptionCauseValues(exception)) {
        if (!isRuntimeException(cause)) {
            continue;
        }
        output << '\n' << indentation << "Caused by:" << '\n';
        appendExceptionReport(output, cause,
                              std::string(indentation) + "  ", depth + 1);
    }
}

} // namespace

RuntimeExceptionOperationResult runtimeAddExceptionCause(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 2 ||
        !isRuntimeException(arguments[0]) ||
        !isRuntimeException(arguments[1])) {
        return failure("addCause expects a base MException and one cause "
                       "MException");
    }
    RuntimeValue result = arguments[0];
    RuntimeValue& causes = result.fields["cause"];
    if (causes.kind != RuntimeValueKind::Cell) {
        causes = emptyCellValue();
    }
    causes.cells.push_back(arguments[1]);
    setRuntimeDimensions(causes, {causes.cells.size(), 1});
    return success(std::move(result));
}

RuntimeExceptionOperationResult runtimeGetExceptionReport(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty() || arguments.size() > 4 ||
        !isRuntimeException(arguments.front())) {
        return failure("getReport expects an MException and optional report "
                       "format arguments");
    }

    std::string type = "extended";
    if (arguments.size() >= 2) {
        const auto typeText = runtimeTextScalarUtf8(arguments[1]);
        if (!typeText ||
            (*typeText != "basic" && *typeText != "extended")) {
            return failure("getReport type must be basic or extended");
        }
        type = *typeText;
    }
    if (arguments.size() == 3) {
        return failure("getReport hyperlinks requires a name and value");
    }
    if (arguments.size() == 4) {
        const auto name = runtimeTextScalarUtf8(arguments[2]);
        const auto value = runtimeTextScalarUtf8(arguments[3]);
        if (!name || *name != "hyperlinks" || !value ||
            (*value != "default" && *value != "on" &&
             *value != "off")) {
            return failure("getReport hyperlinks must be default, on, or "
                           "off");
        }
    }

    if (type == "basic") {
        return success(characterValue(
            exceptionTextProperty(arguments.front(), "message")));
    }
    std::ostringstream report;
    appendExceptionReport(report, arguments.front(), {}, 0);
    return success(characterValue(report.str()));
}

RuntimeValue runtimeExceptionFromDiagnostic(
    const Diagnostic& diagnostic,
    const std::vector<RuntimeExceptionFrame>& frames) {
    RuntimeValue result = exceptionValue(
        diagnostic.identifier.empty()
            ? std::string(kRuntimeErrorIdentifier)
            : diagnostic.identifier,
        diagnostic.message);
    attachFrames(result, frames.empty() ? diagnostic.stack : frames);
    RuntimeValue& causes = result.fields["cause"];
    for (const auto& cause : diagnostic.causes) {
        causes.cells.push_back(exceptionFromDiagnosticCause(cause, 0));
    }
    setRuntimeDimensions(causes, {causes.cells.size(), 1});
    return result;
}

Diagnostic runtimeDiagnosticFromException(
    const RuntimeValue& exception, SourceSpan fallbackSpan) {
    Diagnostic result{
        fallbackSpan,
        exceptionTextProperty(exception, "message"),
        exceptionTextProperty(exception, "identifier")};
    result.stack = runtimeExceptionFrames(exception);
    for (const auto& cause : exceptionCauseValues(exception)) {
        if (isRuntimeException(cause)) {
            result.causes.push_back(diagnosticCauseFromException(cause, 0));
        }
    }
    return result;
}

const RuntimeValue* runtimeExceptionProperty(
    const RuntimeValue& exception, std::string_view name) {
    if (!isRuntimeException(exception)) {
        return nullptr;
    }
    const auto field = exception.fields.find(std::string(name));
    return field == exception.fields.end() ? nullptr : &field->second;
}

std::vector<RuntimeExceptionFrame> runtimeExceptionFrames(
    const RuntimeValue& exception) {
    if (!isRuntimeException(exception)) {
        return {};
    }
    const auto stack = exception.fields.find("stack");
    if (stack == exception.fields.end()) {
        return {};
    }
    std::string error;
    const auto frames = stackFramesFromValue(stack->second, error);
    return frames.value_or(std::vector<RuntimeExceptionFrame>{});
}

size_t runtimeExceptionFrameCount(const RuntimeValue& exception) {
    return runtimeExceptionFrames(exception).size();
}

size_t runtimeExceptionCauseCount(const RuntimeValue& exception) {
    return exceptionCauseValues(exception).size();
}

} // namespace mparser
