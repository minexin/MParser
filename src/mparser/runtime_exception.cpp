#include "mparser/runtime_exception.h"

#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"

#include <algorithm>
#include <iomanip>
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

RuntimeValue stringValue(std::string value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::String;
    result.text = std::move(value);
    setRuntimeDimensions(result, {1, result.text.size()});
    return result;
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

RuntimeValue stackFrameValue(const RuntimeExceptionFrame& frame) {
    RuntimeValue result = makeRuntimeStructValue();
    runtimeSetStructField(result, "file", stringValue(frame.file));
    runtimeSetStructField(result, "name", stringValue(frame.name));
    runtimeSetStructField(result, "line",
                          numberValue(static_cast<double>(frame.line)));
    return result;
}

RuntimeValue exceptionValue(std::string identifier, std::string message) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeExceptionClassName);
    setRuntimeDimensions(result, {1, 1});
    result.fieldOrder = {"identifier", "message", "stack", "cause"};
    result.fields.emplace("identifier", stringValue(std::move(identifier)));
    result.fields.emplace("message", stringValue(std::move(message)));
    result.fields.emplace("stack", emptyStackValue());
    result.fields.emplace("cause", emptyCellValue());
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
        if (value.kind != RuntimeValueKind::String) {
            error = "%s exception message values must be text scalars";
            return std::nullopt;
        }
        output << value.text;
        return output.str();
    }
    if (specifier.conversion == 'c') {
        if (value.kind == RuntimeValueKind::String && !value.text.empty()) {
            output << value.text.front();
            return output.str();
        }
        if (value.kind == RuntimeValueKind::Number) {
            output << static_cast<char>(static_cast<int>(value.number));
            return output.str();
        }
        error = "%c exception message values must be text or numeric scalars";
        return std::nullopt;
    }
    if (value.kind != RuntimeValueKind::Number) {
        error = "numeric exception message specifiers require scalar numbers";
        return std::nullopt;
    }

    switch (specifier.conversion) {
    case 'd':
    case 'i':
        output << static_cast<long long>(value.number);
        break;
    case 'f':
        output << std::fixed << value.number;
        break;
    case 'e':
        output << std::scientific << value.number;
        break;
    case 'g':
        output << std::defaultfloat << value.number;
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
    if (message.kind != RuntimeValueKind::String) {
        return failure("exception message must be a character vector or "
                       "string scalar");
    }
    std::string error;
    auto formatted = formatMessage(message.text, replacements, error);
    if (!formatted) {
        return failure(std::move(error));
    }
    return success(exceptionValue(std::move(identifier),
                                  std::move(*formatted)));
}

std::string exceptionTextProperty(const RuntimeValue& exception,
                                  std::string_view name) {
    const auto field = exception.fields.find(std::string(name));
    if (field == exception.fields.end() ||
        field->second.kind != RuntimeValueKind::String) {
        return {};
    }
    return field->second.text;
}

void attachFrames(RuntimeValue& exception,
                  const std::vector<RuntimeExceptionFrame>& frames) {
    exception.cells.clear();
    exception.cells.reserve(frames.size());
    for (const auto& frame : frames) {
        exception.cells.push_back(stackFrameValue(frame));
    }

    RuntimeValue publicStack = emptyStackValue();
    if (!exception.cells.empty()) {
        publicStack = exception.cells.front();
    }
    exception.fields["stack"] = std::move(publicStack);
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
    if (arguments.size() < 2 ||
        arguments[0].kind != RuntimeValueKind::String) {
        return failure("MException expects an identifier, message, and "
                       "optional replacement values");
    }
    if (!isRuntimeExceptionIdentifier(arguments[0].text)) {
        return failure("invalid MException identifier: " +
                       arguments[0].text);
    }
    return constructException(
        arguments[0].text, arguments[1],
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
        if (!message || message->kind != RuntimeValueKind::String) {
            return failure("error structure requires a text message field");
        }
        std::string id;
        if (identifier) {
            if (identifier->kind != RuntimeValueKind::String ||
                (!identifier->text.empty() &&
                 !isRuntimeExceptionIdentifier(identifier->text))) {
                return failure("error structure contains an invalid "
                               "identifier field");
            }
            id = identifier->text;
        }
        return constructException(std::move(id), *message, {});
    }
    if (arguments.front().kind != RuntimeValueKind::String) {
        return failure("error message must be a character vector or string "
                       "scalar");
    }

    size_t messageIndex = 0;
    std::string identifier;
    if (arguments.size() >= 2 &&
        isRuntimeExceptionIdentifier(arguments.front().text) &&
        arguments[1].kind == RuntimeValueKind::String) {
        identifier = arguments.front().text;
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
    bool preserveExistingStack) {
    if (!isRuntimeException(exception)) {
        return failure("throw and rethrow require a scalar MException object");
    }
    if (preserveExistingStack && exception.cells.empty()) {
        return failure("rethrow requires a previously thrown MException");
    }

    RuntimeValue result = exception;
    if (!preserveExistingStack) {
        attachFrames(result, frames);
    }
    return success(std::move(result));
}

RuntimeValue runtimeExceptionFromDiagnostic(
    const Diagnostic& diagnostic,
    const std::vector<RuntimeExceptionFrame>& frames) {
    RuntimeValue result = exceptionValue(
        diagnostic.identifier.empty()
            ? std::string(kRuntimeErrorIdentifier)
            : diagnostic.identifier,
        diagnostic.message);
    attachFrames(result, frames);
    return result;
}

Diagnostic runtimeDiagnosticFromException(
    const RuntimeValue& exception, SourceSpan fallbackSpan) {
    return Diagnostic{
        fallbackSpan,
        exceptionTextProperty(exception, "message"),
        exceptionTextProperty(exception, "identifier")};
}

const RuntimeValue* runtimeExceptionProperty(
    const RuntimeValue& exception, std::string_view name) {
    if (!isRuntimeException(exception)) {
        return nullptr;
    }
    const auto field = exception.fields.find(std::string(name));
    return field == exception.fields.end() ? nullptr : &field->second;
}

size_t runtimeExceptionFrameCount(const RuntimeValue& exception) {
    return isRuntimeException(exception) ? exception.cells.size() : 0;
}

} // namespace mparser
