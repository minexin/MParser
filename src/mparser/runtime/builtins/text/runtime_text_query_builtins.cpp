#include "mparser/runtime/builtins/text/runtime_text_query_builtins.h"

#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <optional>
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

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

char16_t asciiFold(char16_t value) {
    return value >= u'A' && value <= u'Z'
               ? static_cast<char16_t>(value - u'A' + u'a')
               : value;
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    return runtimeShapeElementCount(value) == 1
               ? runtimeNumericTruthValue(value)
               : std::nullopt;
}

bool characterTextScalar(const RuntimeValue& value) {
    if (!isRuntimeCharacterArray(value)) {
        return false;
    }
    const auto dimensions = runtimeDimensions(value);
    return dimensions.size() == 2 &&
           (dimensions[0] == 1 || runtimeShapeElementCount(value) == 0);
}

bool cellTextScalars(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Cell) {
        return false;
    }
    for (size_t index = 0; index < runtimeShapeElementCount(value);
         ++index) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            value, index);
        if (!offset || *offset >= value.cells.size() ||
            !characterTextScalar(value.cells[*offset])) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::u16string>> textSequence(
    const RuntimeValue& value, bool pattern, std::string& error) {
    std::vector<std::u16string> result;
    if (characterTextScalar(value)) {
        const auto text = runtimeTextScalarCodeUnits(value);
        if (!text) {
            error = "character text input must be a row vector";
            return std::nullopt;
        }
        result.push_back(*text);
        return result;
    }
    if (isRuntimeStringArray(value)) {
        result.reserve(runtimeShapeElementCount(value));
        for (size_t index = 0; index < runtimeShapeElementCount(value);
             ++index) {
            const auto* element = runtimeStringElement(value, index);
            if (!element) {
                error = "string input could not be mapped";
                return std::nullopt;
            }
            if (element->missing) {
                if (pattern) {
                    error = "text patterns cannot contain missing strings";
                    return std::nullopt;
                }
                result.emplace_back();
                continue;
            }
            result.push_back(element->value);
        }
        return result;
    }
    if (cellTextScalars(value)) {
        result.reserve(runtimeShapeElementCount(value));
        for (size_t index = 0; index < runtimeShapeElementCount(value);
             ++index) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                value, index);
            const auto text = offset && *offset < value.cells.size()
                                  ? runtimeTextScalarCodeUnits(
                                        value.cells[*offset])
                                  : std::nullopt;
            if (!text) {
                error = "Cell text input contains a non-text element";
                return std::nullopt;
            }
            result.push_back(*text);
        }
        return result;
    }
    error = pattern
                ? "pattern must be a character vector, string array, or "
                  "Cell array of character vectors"
                : "text input must be a character vector, string array, "
                  "or Cell array of character vectors";
    return std::nullopt;
}

std::optional<bool> parseIgnoreCase(const BuiltinCall& call,
                                    std::string& error) {
    bool ignoreCase = false;
    size_t cursor = 2;
    while (cursor < call.arguments.size()) {
        std::string name;
        const RuntimeValue* value = nullptr;
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            name = call.arguments[cursor].text;
            if (call.arguments[cursor].cells.size() != 1) {
                error = "text query name-value argument is malformed";
                return std::nullopt;
            }
            value = &call.arguments[cursor].cells.front();
            ++cursor;
        } else {
            const auto rawName = runtimeTextScalarUtf8(
                call.arguments[cursor]);
            if (!rawName || cursor + 1 >= call.arguments.size()) {
                error = "text query options must be name-value pairs";
                return std::nullopt;
            }
            name = *rawName;
            value = &call.arguments[cursor + 1];
            cursor += 2;
        }
        if (asciiLower(std::move(name)) != "ignorecase") {
            error = "unsupported text query option";
            return std::nullopt;
        }
        const auto logical = logicalScalar(*value);
        if (!logical) {
            error = "IgnoreCase must be a logical scalar";
            return std::nullopt;
        }
        ignoreCase = *logical;
    }
    return ignoreCase;
}

bool codeUnitsEqual(char16_t left, char16_t right, bool ignoreCase) {
    return ignoreCase ? asciiFold(left) == asciiFold(right)
                      : left == right;
}

bool matchesText(std::string_view operation,
                 std::u16string_view text,
                 std::u16string_view pattern,
                 bool ignoreCase) {
    if (pattern.empty()) {
        return true;
    }
    if (pattern.size() > text.size()) {
        return false;
    }
    const auto equal = [ignoreCase](char16_t left, char16_t right) {
        return codeUnitsEqual(left, right, ignoreCase);
    };
    if (operation == "startsWith") {
        return std::equal(pattern.begin(), pattern.end(), text.begin(),
                          equal);
    }
    if (operation == "endsWith") {
        return std::equal(pattern.begin(), pattern.end(),
                          text.end() -
                              static_cast<std::ptrdiff_t>(pattern.size()),
                          equal);
    }
    return std::search(text.begin(), text.end(), pattern.begin(),
                       pattern.end(), equal) != text.end();
}

BuiltinResult textQueryBuiltin(std::string_view name,
                               const BuiltinCall& call) {
    std::string error;
    const auto ignoreCase = parseIgnoreCase(call, error);
    if (!ignoreCase) {
        return failure(call, std::move(error),
                       "MParser:InvalidTextQueryOption");
    }
    auto source = textSequence(call.arguments[0], false, error);
    if (!source) {
        return failure(call, std::move(error),
                       "MParser:InvalidTextQueryInput");
    }
    auto patterns = textSequence(call.arguments[1], true, error);
    if (!patterns) {
        return failure(call, std::move(error),
                       "MParser:InvalidTextQueryPattern");
    }

    const RuntimeValue& input = call.arguments[0];
    std::vector<size_t> dimensions =
        characterTextScalar(input) ? std::vector<size_t>{1, 1}
                                   : runtimeDimensions(input);
    std::vector<double> results(source->size(), 0.0);
    for (size_t index = 0; index < source->size(); ++index) {
        if (call.context && call.context->executionControl &&
            (index & 255U) == 0U &&
            !call.context->executionControl->checkpoint()) {
            return failure(call,
                           std::string(name) +
                               " was stopped by runtime execution control",
                           "MParser:ExecutionStopped");
        }
        bool missingSource = false;
        if (isRuntimeStringArray(input)) {
            const auto* element = runtimeStringElement(input, index);
            missingSource = element && element->missing;
        }
        if (missingSource) {
            continue;
        }
        results[index] = std::any_of(
            patterns->begin(), patterns->end(),
            [&](const std::u16string& pattern) {
                return matchesText(name, (*source)[index], pattern,
                                   *ignoreCase);
            })
                             ? 1.0
                             : 0.0;
    }
    auto output = runtimeNumericValueFromLogicalOrder(
        dimensions, std::move(results), RuntimeNumericClass::Logical);
    if (!output) {
        return failure(call, "text query output shape is invalid",
                       "MParser:InvalidTextQueryShape");
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(*output)});
}

} // namespace

bool isRuntimeTextQueryBuiltin(std::string_view name) {
    return name == "contains" || name == "endsWith" ||
           name == "startsWith";
}

BuiltinResult invokeRuntimeTextQueryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (isRuntimeTextQueryBuiltin(name)) {
        return textQueryBuiltin(name, call);
    }
    return failure(call, "unsupported text query builtin",
                   "MParser:UnsupportedTextQueryBuiltin");
}

} // namespace mparser
