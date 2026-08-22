#include "mparser/runtime_system_builtins.h"

#include "mparser/filesystem_utf8.h"
#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_file_io.h"
#include "mparser/runtime_metadata.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_output.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_system.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_warning.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef MPARSER_VERSION
#define MPARSER_VERSION "0.0.0"
#endif

namespace mparser {
namespace {

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier =
                          "MParser:InvalidSystemBuiltinCall") {
    return BuiltinResult::failure(
        call.span, std::move(message), std::move(identifier));
}

BuiltinResult selectedOutputs(
    const BuiltinCall& call, std::vector<RuntimeValue> outputs) {
    if (call.requestedOutputCount > outputs.size()) {
        return failure(call, "system builtin could not produce the requested "
                             "number of outputs");
    }
    outputs.resize(call.requestedOutputCount);
    return BuiltinResult::success(std::move(outputs));
}

RuntimeSystemContext* systemContext(const BuiltinCall& call) {
    return call.context ? call.context->systemContext : nullptr;
}

std::optional<std::string> textArgument(const RuntimeValue& value) {
    return runtimeTextScalarUtf8(value);
}

std::optional<int> fileIdentifier(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) || value.numericComplex ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto numeric = runtimeNumericElement(value, 0);
    if (!numeric || !std::isfinite(*numeric) ||
        std::trunc(*numeric) != *numeric || *numeric < 0.0 ||
        *numeric > static_cast<double>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*numeric);
}

std::optional<std::int64_t> fileOffset(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) || value.numericComplex ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto numeric = runtimeNumericElement(value, 0);
    constexpr double kSigned64Limit = 9223372036854775808.0;
    if (!numeric || !std::isfinite(*numeric) ||
        std::trunc(*numeric) != *numeric || *numeric < -kSigned64Limit ||
        *numeric >= kSigned64Limit) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*numeric);
}

std::optional<RuntimeFileSeekOrigin> fileSeekOrigin(
    const RuntimeValue& value) {
    if (const auto text = textArgument(value)) {
        if (*text == "bof") {
            return RuntimeFileSeekOrigin::Beginning;
        }
        if (*text == "cof") {
            return RuntimeFileSeekOrigin::Current;
        }
        if (*text == "eof") {
            return RuntimeFileSeekOrigin::End;
        }
        return std::nullopt;
    }
    const auto numeric = fileOffset(value);
    if (!numeric || *numeric < -1 || *numeric > 1) {
        return std::nullopt;
    }
    switch (*numeric) {
    case -1:
        return RuntimeFileSeekOrigin::Beginning;
    case 0:
        return RuntimeFileSeekOrigin::Current;
    case 1:
        return RuntimeFileSeekOrigin::End;
    default:
        return std::nullopt;
    }
}

BuiltinResult emit(const BuiltinCall& call, RuntimeOutputKind kind,
                   std::string text) {
    if (!call.context || !call.context->outputSink) {
        return failure(call, "system builtin requires an output sink",
                       "MParser:MissingBuiltinContext");
    }
    RuntimeOutputEvent event;
    event.kind = kind;
    event.text = std::move(text);
    event.span = call.span;
    if (!(*call.context->outputSink)(event)) {
        return failure(call, "host output sink rejected system output",
                       "MParser:OutputSinkRejected");
    }
    return BuiltinResult::success();
}

char folded(char value) {
#ifdef _WIN32
    return static_cast<char>(std::tolower(
        static_cast<unsigned char>(value)));
#else
    return value;
#endif
}

bool wildcardMatch(std::string_view pattern, std::string_view value) {
    std::vector<bool> previous(value.size() + 1, false);
    std::vector<bool> current(value.size() + 1, false);
    previous[0] = true;
    for (const char token : pattern) {
        std::fill(current.begin(), current.end(), false);
        if (token == '*') {
            current[0] = previous[0];
            for (size_t index = 1; index <= value.size(); ++index) {
                current[index] = previous[index] || current[index - 1];
            }
        } else {
            for (size_t index = 1; index <= value.size(); ++index) {
                current[index] = previous[index - 1] &&
                    (token == '?' || folded(token) == folded(value[index - 1]));
            }
        }
        previous.swap(current);
    }
    return previous[value.size()];
}

bool nameSelected(std::string_view name,
                  const std::vector<std::string>& patterns) {
    return patterns.empty() ||
           std::any_of(patterns.begin(), patterns.end(),
                       [name](const std::string& pattern) {
                           return wildcardMatch(pattern, name);
                       });
}

std::optional<std::vector<std::string>> workspacePatterns(
    const BuiltinCall& call) {
    std::vector<std::string> result;
    result.reserve(call.arguments.size());
    for (const RuntimeValue& argument : call.arguments) {
        const auto pattern = textArgument(argument);
        if (!pattern) {
            return std::nullopt;
        }
        result.push_back(*pattern);
    }
    return result;
}

RuntimeValue workspaceNamesValue(
    const RuntimeWorkspace& workspace,
    const std::vector<std::string>& patterns) {
    std::vector<RuntimeValue> names;
    for (const auto& [name, value] : workspace) {
        (void)value;
        if (nameSelected(name, patterns)) {
            names.push_back(makeRuntimeCharacterVectorUtf8(name));
        }
    }
    const size_t count = names.size();
    return makeRuntimeCellValue({count, 1}, std::move(names));
}

RuntimeValue workspaceDetailsValue(
    const RuntimeWorkspace& workspace,
    const std::vector<std::string>& patterns) {
    const std::vector<std::string> fields = {
        "name", "size", "bytes", "class", "global",
        "sparse", "complex", "nesting", "persistent"};
    std::vector<RuntimeStructElement> elements;
    for (const auto& [name, value] : workspace) {
        if (!nameSelected(name, patterns)) {
            continue;
        }
        std::vector<double> dimensions;
        for (const size_t dimension : runtimeDimensions(value)) {
            dimensions.push_back(static_cast<double>(dimension));
        }
        const auto bytes = runtimeValueArrayBytes(value);
        RuntimeStructElement element{
            {"name", makeRuntimeCharacterVectorUtf8(name)},
            {"size", makeRuntimeVectorValue(std::move(dimensions))},
            {"bytes", makeRuntimeNumberValue(
                          static_cast<double>(bytes.value_or(0)))},
            {"class", makeRuntimeCharacterVectorUtf8(
                          runtimeValueClassName(value))},
            {"global", makeRuntimeLogicalValue(false)},
            {"sparse", makeRuntimeLogicalValue(false)},
            {"complex", makeRuntimeLogicalValue(value.numericComplex)},
            {"nesting", makeRuntimeStructValue()},
            {"persistent", makeRuntimeLogicalValue(false)},
        };
        elements.push_back(std::move(element));
    }
    const size_t count = elements.size();
    return makeRuntimeStructArrayValue(
        fields, std::move(elements), {count, 1});
}

std::string shapeText(const RuntimeValue& value) {
    const auto dimensions = runtimeDimensions(value);
    std::ostringstream output;
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (index != 0) {
            output << 'x';
        }
        output << dimensions[index];
    }
    return output.str();
}

BuiltinResult workspaceQuery(std::string_view name,
                             const BuiltinCall& call) {
    if (!call.context || !call.context->workspace ||
        !call.context->workspace->variables) {
        return failure(call, std::string(name) + " requires a workspace",
                       "MParser:MissingBuiltinContext");
    }
    const RuntimeWorkspace& workspace =
        *call.context->workspace->variables;
    const auto patterns = workspacePatterns(call);
    if (!patterns) {
        return failure(call, std::string(name) +
                                 " patterns must be text scalars");
    }
    if (call.requestedOutputCount == 1) {
        return BuiltinResult::success({
            name == "who"
                ? workspaceNamesValue(workspace, *patterns)
                : workspaceDetailsValue(workspace, *patterns)});
    }

    std::ostringstream output;
    if (name == "who") {
        for (const auto& [variableName, value] : workspace) {
            (void)value;
            if (nameSelected(variableName, *patterns)) {
                output << variableName << '\n';
            }
        }
    } else {
        output << "  Name                 Size          Bytes  Class\n";
        for (const auto& [variableName, value] : workspace) {
            if (!nameSelected(variableName, *patterns)) {
                continue;
            }
            output << "  " << std::left << std::setw(20) << variableName
                   << std::setw(14) << shapeText(value)
                   << std::setw(7)
                   << runtimeValueArrayBytes(value).value_or(0) << "  "
                   << runtimeValueClassName(value) << '\n';
        }
    }
    return emit(call, RuntimeOutputKind::StandardOutput, output.str());
}

BuiltinResult clearBuiltin(const BuiltinCall& call) {
    if (!call.context || !call.context->workspace ||
        !call.context->workspace->variables ||
        !call.context->workspace->clearVariables ||
        !call.context->workspace->eraseVariable) {
        return failure(call, "clear requires a mutable workspace",
                       "MParser:MissingBuiltinContext");
    }

    BuiltinWorkspaceAccess& workspace = *call.context->workspace;
    if (call.arguments.empty()) {
        workspace.clearVariables();
        return BuiltinResult::success();
    }

    std::vector<std::string> selectors;
    selectors.reserve(call.arguments.size());
    for (const RuntimeValue& argument : call.arguments) {
        const auto selector = textArgument(argument);
        if (!selector) {
            return failure(call, "clear selectors must be text scalars");
        }
        selectors.push_back(*selector);
    }

    if (selectors.front() == "-regexp") {
        if (selectors.size() == 1) {
            return failure(call, "clear -regexp requires an expression");
        }
        std::vector<std::regex> expressions;
        try {
            for (size_t index = 1; index < selectors.size(); ++index) {
                expressions.emplace_back(selectors[index]);
            }
        } catch (const std::regex_error&) {
            return failure(call,
                           "clear received an invalid regular expression",
                           "MParser:InvalidRegularExpression");
        }

        std::vector<std::string> names;
        for (const auto& [name, value] : *workspace.variables) {
            (void)value;
            if (std::any_of(
                    expressions.begin(), expressions.end(),
                    [&name](const std::regex& expression) {
                        return std::regex_search(name, expression);
                    })) {
                names.push_back(name);
            }
        }
        for (const std::string& name : names) {
            workspace.eraseVariable(name);
        }
        return BuiltinResult::success();
    }

    if (selectors.size() == 1 &&
        !workspace.variables->contains(selectors.front())) {
        if (selectors.front() == "all" ||
            selectors.front() == "variables") {
            workspace.clearVariables();
            return BuiltinResult::success();
        }
        if (selectors.front() == "classes" ||
            selectors.front() == "functions" ||
            selectors.front() == "global" ||
            selectors.front() == "import" ||
            selectors.front() == "java" ||
            selectors.front() == "mex") {
            return failure(
                call, "clear item type is not supported yet: " +
                          selectors.front(),
                "MParser:UnsupportedClearTarget");
        }
    }

    for (const std::string& selector : selectors) {
        workspace.eraseVariable(selector);
    }
    return BuiltinResult::success();
}

std::optional<BuiltinWorkspaceScope> workspaceScope(
    const RuntimeValue& value) {
    const auto name = textArgument(value);
    if (!name) {
        return std::nullopt;
    }
    if (*name == "base") {
        return BuiltinWorkspaceScope::Base;
    }
    if (*name == "caller") {
        return BuiltinWorkspaceScope::Caller;
    }
    return std::nullopt;
}

bool validWorkspaceVariableName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto start = [](char character) {
        const unsigned char value =
            static_cast<unsigned char>(character);
        return std::isalpha(value) != 0 || character == '_';
    };
    const auto part = [](char character) {
        const unsigned char value =
            static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_';
    };
    if (!start(name.front()) ||
        !std::all_of(name.begin() + 1, name.end(), part)) {
        return false;
    }
    static constexpr std::array<std::string_view, 26> keywords = {
        "arguments", "break", "case", "catch", "classdef",
        "continue", "else", "elseif", "end", "enumeration",
        "events", "for", "function", "global", "if", "import",
        "methods", "otherwise", "parfor", "persistent", "properties",
        "return", "spmd", "switch", "try", "while"};
    return std::find(keywords.begin(), keywords.end(), name) ==
           keywords.end();
}

BuiltinResult assigninBuiltin(const BuiltinCall& call) {
    if (!call.context || !call.context->workspace ||
        !call.context->workspace->resolveVariables) {
        return failure(call, "assignin requires workspace resolution",
                       "MParser:MissingBuiltinContext");
    }
    const auto scope = workspaceScope(call.arguments[0]);
    if (!scope) {
        return failure(call,
                       "assignin workspace must be 'base' or 'caller'",
                       "MParser:InvalidWorkspaceScope");
    }
    const auto name = textArgument(call.arguments[1]);
    if (!name || !validWorkspaceVariableName(*name)) {
        return failure(call,
                       "assignin variable name must be a valid identifier",
                       "MParser:InvalidWorkspaceVariableName");
    }
    RuntimeWorkspace* workspace =
        call.context->workspace->resolveVariables(*scope);
    if (!workspace) {
        return failure(call, "assignin could not resolve the workspace",
                       "MParser:MissingBuiltinContext");
    }
    (*workspace)[*name] = call.arguments[2];
    return BuiltinResult::success();
}

BuiltinSourceEvaluationResult evaluateSource(
    const BuiltinCall& call, BuiltinWorkspaceScope scope,
    std::string source, size_t requestedOutputCount,
    bool captureOutput) {
    if (!call.context || !call.context->sourceEvaluator) {
        BuiltinSourceEvaluationResult result;
        result.diagnostics.push_back(Diagnostic{
            call.span, "dynamic source evaluation context is unavailable",
            "MParser:MissingBuiltinContext"});
        return result;
    }
    return call.context->sourceEvaluator(
        BuiltinSourceEvaluationRequest{
            std::move(source), scope, requestedOutputCount,
            captureOutput, call.span});
}

BuiltinResult dynamicEvaluationBuiltin(
    std::string_view name, const BuiltinCall& call) {
    const bool evalc = name == "evalc";
    BuiltinWorkspaceScope scope = BuiltinWorkspaceScope::Current;
    size_t sourceIndex = 0;
    if (name == "evalin") {
        const auto selected = workspaceScope(call.arguments[0]);
        if (!selected) {
            return failure(call,
                           "evalin workspace must be 'base' or 'caller'",
                           "MParser:InvalidWorkspaceScope");
        }
        scope = *selected;
        sourceIndex = 1;
    }

    const auto source = textArgument(call.arguments[sourceIndex]);
    if (!source) {
        return failure(call,
                       std::string(name) +
                           " source must be a character vector or string "
                           "scalar",
                       "MParser:InvalidDynamicSource");
    }
    const size_t expressionOutputCount =
        evalc && call.requestedOutputCount > 0
            ? call.requestedOutputCount - 1
            : evalc ? 0 : call.requestedOutputCount;
    auto evaluated = evaluateSource(
        call, scope, *source, expressionOutputCount, evalc);

    const bool hasCatchExpression =
        call.arguments.size() > sourceIndex + 1;
    if (!evaluated.succeeded && hasCatchExpression) {
        const auto catchSource =
            textArgument(call.arguments[sourceIndex + 1]);
        if (!catchSource) {
            return failure(call,
                           std::string(name) +
                               " catch source must be a character vector or "
                               "string scalar",
                           "MParser:InvalidDynamicSource");
        }
        std::vector<Diagnostic> warnings;
        for (auto& diagnostic : evaluated.diagnostics) {
            if (!isErrorDiagnostic(diagnostic)) {
                warnings.push_back(std::move(diagnostic));
            }
        }
        std::string captured = std::move(evaluated.capturedOutput);
        evaluated = evaluateSource(
            call, scope, *catchSource, expressionOutputCount, evalc);
        if (evalc) {
            evaluated.capturedOutput =
                std::move(captured) + evaluated.capturedOutput;
        }
        warnings.insert(
            warnings.end(),
            std::make_move_iterator(evaluated.diagnostics.begin()),
            std::make_move_iterator(evaluated.diagnostics.end()));
        evaluated.diagnostics = std::move(warnings);
        evaluated.succeeded =
            !hasErrorDiagnostics(evaluated.diagnostics);
    }

    if (!evaluated.succeeded) {
        return BuiltinResult{
            false, {}, std::move(evaluated.diagnostics)};
    }

    std::vector<RuntimeValue> outputs;
    if (evalc && call.requestedOutputCount > 0) {
        outputs.push_back(makeRuntimeCharacterVectorUtf8(
            evaluated.capturedOutput));
    }
    outputs.insert(
        outputs.end(),
        std::make_move_iterator(evaluated.outputs.begin()),
        std::make_move_iterator(evaluated.outputs.end()));
    return BuiltinResult::success(
        std::move(outputs), std::move(evaluated.diagnostics));
}

struct FullfileArgument {
    bool stringValue = false;
    std::vector<size_t> dimensions{1, 1};
    std::vector<std::string> elements;
};

constexpr char nativeFileSeparator() {
    return std::filesystem::path::preferred_separator;
}

bool isFileSeparator(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    return value == '/';
#endif
}

std::string normalizeFileSeparators(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (!isFileSeparator(character)) {
            result.push_back(character);
            continue;
        }
        const char separator = nativeFileSeparator();
        if (result.empty() || result.back() != separator ||
            (result.size() == 1 && result.front() == separator)) {
            result.push_back(separator);
        }
    }
    return result;
}

std::string joinFullfileComponents(
    const std::vector<std::string_view>& components) {
    std::string result;
    for (const std::string_view rawComponent : components) {
        std::string component = normalizeFileSeparators(rawComponent);
        if (component.empty()) {
            continue;
        }
        if (result.empty()) {
            result = std::move(component);
            continue;
        }

        size_t firstContent = 0;
        while (firstContent < component.size() &&
               component[firstContent] == nativeFileSeparator()) {
            ++firstContent;
        }
        if (firstContent == component.size()) {
            if (result.back() != nativeFileSeparator()) {
                result.push_back(nativeFileSeparator());
            }
            continue;
        }
        if (result.back() != nativeFileSeparator()) {
            result.push_back(nativeFileSeparator());
        }
        result.append(component, firstContent, std::string::npos);
    }
    return result;
}

std::optional<FullfileArgument> fullfileArgument(
    const RuntimeValue& value) {
    FullfileArgument result;
    if (isRuntimeStringArray(value)) {
        result.stringValue = true;
        result.dimensions = runtimeDimensions(value);
        const size_t count = runtimeShapeElementCount(value);
        result.elements.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const RuntimeStringElement* element =
                runtimeStringElement(value, index);
            if (!element) {
                return std::nullopt;
            }
            result.elements.push_back(
                element->missing
                    ? std::string{}
                    : runtimeUtf16ToUtf8(element->value));
        }
        return result;
    }
    const auto text = textArgument(value);
    if (!text) {
        return std::nullopt;
    }
    result.elements.push_back(*text);
    return result;
}

BuiltinResult fullfileBuiltin(const BuiltinCall& call) {
    std::vector<FullfileArgument> arguments;
    arguments.reserve(call.arguments.size());
    std::vector<size_t> outputDimensions{1, 1};
    size_t outputCount = 1;
    bool stringOutput = false;
    for (const RuntimeValue& value : call.arguments) {
        auto argument = fullfileArgument(value);
        if (!argument) {
            return failure(
                call,
                "fullfile inputs must be character vectors or string arrays");
        }
        stringOutput = stringOutput || argument->stringValue;
        if (argument->elements.size() != 1) {
            if (outputCount == 1) {
                outputCount = argument->elements.size();
                outputDimensions = argument->dimensions;
            } else if (argument->elements.size() != outputCount ||
                       argument->dimensions != outputDimensions) {
                return failure(
                    call,
                    "fullfile string-array inputs must have compatible shapes");
            }
        }
        arguments.push_back(std::move(*argument));
    }

    std::vector<RuntimeStringElement> output;
    output.reserve(outputCount);
    for (size_t index = 0; index < outputCount; ++index) {
        std::vector<std::string_view> components;
        components.reserve(arguments.size());
        for (const auto& argument : arguments) {
            const auto& element = argument.elements[
                argument.elements.size() == 1 ? 0 : index];
            if (element.find('\0') != std::string::npos) {
                return failure(call,
                               "fullfile input contains a null byte");
            }
            components.push_back(element);
        }
        output.push_back(RuntimeStringElement{
            runtimeUtf8ToUtf16(joinFullfileComponents(components)), false});
    }
    if (!stringOutput && outputCount == 1) {
        return BuiltinResult::success({
            makeRuntimeCharacterVector(std::move(output.front().value))});
    }
    return BuiltinResult::success({makeRuntimeStringArray(
        std::move(outputDimensions), std::move(output))});
}

enum class PathTextContainer {
    Character,
    String,
    Cell,
};

struct PathTextInput {
    PathTextContainer container = PathTextContainer::Character;
    std::vector<size_t> dimensions{1, 1};
    std::vector<std::string> elements;
    std::vector<bool> missing;
};

RuntimeSystemResult<PathTextInput> pathTextInput(
    const RuntimeValue& value) {
    PathTextInput result;
    if (isRuntimeCharacterVector(value)) {
        result.elements.push_back(
            runtimeTextScalarUtf8(value).value_or(std::string{}));
        result.missing.push_back(false);
        return RuntimeSystemResult<PathTextInput>::success(
            std::move(result));
    }
    if (isRuntimeStringArray(value)) {
        result.container = PathTextContainer::String;
        result.dimensions = runtimeDimensions(value);
        const size_t count = runtimeShapeElementCount(value);
        result.elements.reserve(count);
        result.missing.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const RuntimeStringElement* element =
                runtimeStringElement(value, index);
            if (!element) {
                return RuntimeSystemResult<PathTextInput>::failure(
                    "string path input is malformed");
            }
            result.elements.push_back(
                element->missing
                    ? std::string{}
                    : runtimeUtf16ToUtf8(element->value));
            result.missing.push_back(element->missing);
        }
        return RuntimeSystemResult<PathTextInput>::success(
            std::move(result));
    }
    if (value.kind == RuntimeValueKind::Cell) {
        result.container = PathTextContainer::Cell;
        result.dimensions = runtimeDimensions(value);
        const size_t count = runtimeShapeElementCount(value);
        if (value.cells.size() != count) {
            return RuntimeSystemResult<PathTextInput>::failure(
                "cell path input is malformed");
        }
        result.elements.reserve(count);
        result.missing.assign(count, false);
        for (const RuntimeValue& element : value.cells) {
            if (!isRuntimeCharacterVector(element)) {
                return RuntimeSystemResult<PathTextInput>::failure(
                    "cell path input must contain character vectors");
            }
            result.elements.push_back(
                runtimeTextScalarUtf8(element).value_or(std::string{}));
        }
        return RuntimeSystemResult<PathTextInput>::success(
            std::move(result));
    }
    return RuntimeSystemResult<PathTextInput>::failure(
        "path input must be a character vector, string array, or cell "
        "array of character vectors");
}

RuntimeValue pathTextOutput(const PathTextInput& input,
                            std::vector<std::string> elements) {
    if (input.container == PathTextContainer::Character) {
        return makeRuntimeCharacterVectorUtf8(
            elements.empty() ? std::string_view{} : elements.front());
    }
    if (input.container == PathTextContainer::String) {
        std::vector<RuntimeStringElement> strings;
        strings.reserve(elements.size());
        for (const auto& element : elements) {
            strings.push_back(
                {runtimeUtf8ToUtf16(element), false});
        }
        return makeRuntimeStringArray(input.dimensions,
                                      std::move(strings));
    }
    std::vector<RuntimeValue> cells;
    cells.reserve(elements.size());
    for (const auto& element : elements) {
        cells.push_back(makeRuntimeCharacterVectorUtf8(element));
    }
    return makeRuntimeCellValue(input.dimensions, std::move(cells));
}

RuntimeValue logicalPathResults(const PathTextInput& input,
                                std::vector<double> values) {
    const auto dimensions =
        input.container == PathTextContainer::Character
            ? std::vector<size_t>{1, 1}
            : input.dimensions;
    auto result = runtimeNumericValueFromLogicalOrder(
        dimensions, std::move(values), RuntimeNumericClass::Logical);
    return result ? std::move(*result)
                  : makeRuntimeMatrixValue(0, 0, {},
                                           RuntimeNumericClass::Logical);
}

BuiltinResult pathExistenceBuiltin(std::string_view name,
                                   const BuiltinCall& call) {
    auto input = pathTextInput(call.arguments.front());
    if (!input.succeeded) {
        return failure(call, std::move(input.error),
                       "MParser:InvalidPathInput");
    }
    std::vector<double> values;
    values.reserve(input.value.elements.size());
    RuntimeSystemContext* context = systemContext(call);
    for (size_t index = 0; index < input.value.elements.size(); ++index) {
        if (input.value.missing[index]) {
            values.push_back(0.0);
            continue;
        }
        const auto& text = input.value.elements[index];
        if (text.find('\0') != std::string::npos) {
            return failure(call, "path input contains a null byte",
                           "MParser:InvalidPathInput");
        }
        const auto result =
            name == "isfile"
                ? context->regularFileExists(pathFromUtf8(text))
                : context->directoryExists(pathFromUtf8(text));
        if (!result.succeeded) {
            return failure(call, std::move(result.error),
                           "MParser:SystemOperationFailed");
        }
        values.push_back(result.value ? 1.0 : 0.0);
    }
    return selectedOutputs(call, {
        logicalPathResults(input.value, std::move(values))});
}

struct FileParts {
    std::string path;
    std::string name;
    std::string extension;
};

FileParts splitFileParts(std::string_view value) {
    size_t separator = std::string_view::npos;
    for (size_t index = 0; index < value.size(); ++index) {
        if (isFileSeparator(value[index])) {
            separator = index;
        }
    }

    FileParts result;
    std::string_view leaf = value;
    if (separator != std::string_view::npos) {
        leaf = value.substr(separator + 1);
        size_t pathLength = separator;
        if (separator == 0 ||
            (separator == 1 && isFileSeparator(value[0]))) {
            pathLength = separator + 1;
        }
#ifdef _WIN32
        if (separator == 2 && value.size() >= 3 && value[1] == ':') {
            pathLength = 3;
        }
#endif
        result.path.assign(value.substr(0, pathLength));
    }

    const size_t dot = leaf.rfind('.');
    if (dot == std::string_view::npos) {
        result.name.assign(leaf);
    } else {
        result.name.assign(leaf.substr(0, dot));
        result.extension.assign(leaf.substr(dot));
    }
    return result;
}

BuiltinResult filepartsBuiltin(const BuiltinCall& call) {
    auto input = pathTextInput(call.arguments.front());
    if (!input.succeeded) {
        return failure(call, std::move(input.error),
                       "MParser:InvalidFileName");
    }
    std::vector<std::string> paths;
    std::vector<std::string> names;
    std::vector<std::string> extensions;
    paths.reserve(input.value.elements.size());
    names.reserve(input.value.elements.size());
    extensions.reserve(input.value.elements.size());
    for (size_t index = 0; index < input.value.elements.size(); ++index) {
        if (input.value.missing[index]) {
            return failure(call,
                           "fileparts does not accept missing strings",
                           "MParser:MissingFileName");
        }
        if (input.value.elements[index].find('\0') != std::string::npos) {
            return failure(call, "file name contains a null byte",
                           "MParser:InvalidFileName");
        }
        auto parts = splitFileParts(input.value.elements[index]);
        paths.push_back(std::move(parts.path));
        names.push_back(std::move(parts.name));
        extensions.push_back(std::move(parts.extension));
    }
    std::vector<RuntimeValue> outputs;
    outputs.push_back(pathTextOutput(input.value, std::move(paths)));
    outputs.push_back(pathTextOutput(input.value, std::move(names)));
    outputs.push_back(pathTextOutput(input.value, std::move(extensions)));
    outputs.resize(call.requestedOutputCount);
    return BuiltinResult::success(std::move(outputs));
}

RuntimeSystemResult<std::filesystem::path> readableFilePath(
    RuntimeSystemContext& context, const std::filesystem::path& requested) {
    auto exists = context.regularFileExists(requested);
    if (!exists.succeeded) {
        return RuntimeSystemResult<std::filesystem::path>::failure(
            std::move(exists.error));
    }
    if (exists.value) {
        return RuntimeSystemResult<std::filesystem::path>::success(
            requested);
    }
    if (requested.is_absolute() || requested.has_parent_path() ||
        !context.hasCapability(RuntimeSystemCapability::SearchPaths)) {
        return RuntimeSystemResult<std::filesystem::path>::failure(
            "file does not exist: " + pathToNativeUtf8(requested));
    }
    const auto paths = context.searchPaths();
    if (!paths.succeeded) {
        return RuntimeSystemResult<std::filesystem::path>::failure(
            std::move(paths.error));
    }
    for (const auto& path : paths.value) {
        const auto candidate = path / requested;
        exists = context.regularFileExists(candidate);
        if (!exists.succeeded) {
            return RuntimeSystemResult<std::filesystem::path>::failure(
                std::move(exists.error));
        }
        if (exists.value) {
            return RuntimeSystemResult<std::filesystem::path>::success(
                candidate);
        }
    }
    return RuntimeSystemResult<std::filesystem::path>::failure(
        "file does not exist: " + pathToNativeUtf8(requested));
}

BuiltinResult filereadBuiltin(const BuiltinCall& call) {
    const auto fileName = textArgument(call.arguments.front());
    if (!fileName || fileName->find('\0') != std::string::npos) {
        return failure(call,
                       "fileread file name must be a text scalar without "
                       "null bytes",
                       "MParser:InvalidFileName");
    }
    RuntimeSystemContext* context = systemContext(call);
    auto path = readableFilePath(*context, pathFromUtf8(*fileName));
    if (!path.succeeded) {
        const bool missing = path.error.starts_with("file does not exist");
        return failure(call, std::move(path.error),
                       missing ? "MParser:FileNotFound"
                               : "MParser:SystemOperationFailed");
    }
    RuntimeFileOpenOptions options;
    options.readable = true;
    options.binary = false;
    options.permission = "rt";
    options.machineFormat = std::string(runtimeFileByteOrderName(
        runtimeNativeFileByteOrder()));
    options.encoding = "UTF-8";
    auto opened = context->openFile(path.value, options);
    if (!opened.succeeded) {
        return failure(call, std::move(opened.error),
                       "MParser:FileReadFailed");
    }
    auto contents = context->readFileRemaining(opened.value);
    const auto closed = context->closeFile(opened.value);
    if (!contents.succeeded) {
        return failure(call, std::move(contents.error),
                       "MParser:FileReadFailed");
    }
    if (!closed.succeeded) {
        return failure(call, std::move(closed.error),
                       "MParser:FileReadFailed");
    }
    if (contents.value.starts_with("\xef\xbb\xbf")) {
        contents.value.erase(0, 3);
    }
    return selectedOutputs(call, {
        makeRuntimeCharacterVectorUtf8(contents.value)});
}

BuiltinResult tempnameBuiltin(const BuiltinCall& call) {
    std::optional<std::filesystem::path> directory;
    if (!call.arguments.empty()) {
        const auto text = textArgument(call.arguments.front());
        if (!text || text->find('\0') != std::string::npos) {
            return failure(call,
                           "tempname directory must be a text scalar without "
                           "null bytes",
                           "MParser:InvalidDirectoryName");
        }
        directory = pathFromUtf8(*text);
    }
    auto path = systemContext(call)->temporaryName(directory);
    return path.succeeded
               ? selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
                     pathToNativeUtf8(path.value))})
               : failure(call, std::move(path.error),
                         "MParser:SystemOperationFailed");
}

BuiltinResult filesystemStatusResult(
    const BuiltinCall& call, bool succeeded, std::string message,
    std::string identifier, bool warning = false) {
    if (call.requestedOutputCount == 0) {
        if (!succeeded) {
            return failure(call, std::move(message),
                           std::move(identifier));
        }
        if (warning) {
            if (call.context && call.context->warningContext) {
                auto result = call.context->warningContext->warning(
                    {makeRuntimeCharacterVectorUtf8(identifier),
                     makeRuntimeCharacterVectorUtf8(message)},
                    0);
                if (!result.succeeded) {
                    return failure(call, std::move(result.error),
                                   "MParser:InvalidWarning");
                }
                if (!result.emitted) {
                    return BuiltinResult::success();
                }
                return BuiltinResult::success(
                    {}, {Diagnostic{call.span,
                                    std::move(result.emitted->message),
                                    std::move(result.emitted->identifier),
                                    DiagnosticSeverity::Warning}});
            }
            return BuiltinResult::success(
                {}, {Diagnostic{call.span, std::move(message),
                                std::move(identifier),
                                DiagnosticSeverity::Warning}});
        }
        return BuiltinResult::success();
    }
    std::vector<RuntimeValue> outputs = {
        makeRuntimeLogicalValue(succeeded),
        makeRuntimeCharacterVectorUtf8(message),
        makeRuntimeCharacterVectorUtf8(identifier),
    };
    outputs.resize(call.requestedOutputCount);
    return BuiltinResult::success(std::move(outputs));
}

BuiltinResult mkdirBuiltin(const BuiltinCall& call) {
    const auto first = textArgument(call.arguments.front());
    if (!first || first->empty() || first->find('\0') != std::string::npos) {
        return failure(call, "mkdir folder name must be a nonempty text scalar",
                       "MParser:InvalidDirectoryName");
    }
    std::filesystem::path path = pathFromUtf8(*first);
    if (call.arguments.size() == 2) {
        const auto second = textArgument(call.arguments[1]);
        if (!second || second->empty() ||
            second->find('\0') != std::string::npos) {
            return failure(call,
                           "mkdir child name must be a nonempty text scalar",
                           "MParser:InvalidDirectoryName");
        }
        const auto child = pathFromUtf8(*second);
        if (child.is_absolute()) {
            return failure(call,
                           "mkdir child name must be relative to its parent",
                           "MParser:InvalidDirectoryName");
        }
        path /= child;
    }
    auto created = systemContext(call)->createDirectories(path);
    if (!created.succeeded) {
        return filesystemStatusResult(
            call, false, std::move(created.error),
            "MATLAB:MKDIR:OSError");
    }
    if (!created.value) {
        return filesystemStatusResult(
            call, true, "Directory already exists.",
            "MATLAB:MKDIR:DirectoryExists", true);
    }
    return filesystemStatusResult(call, true, {}, {});
}

BuiltinResult rmdirBuiltin(const BuiltinCall& call) {
    const auto folder = textArgument(call.arguments.front());
    if (!folder || folder->empty() ||
        folder->find('\0') != std::string::npos) {
        return failure(call, "rmdir folder name must be a nonempty text scalar",
                       "MParser:InvalidDirectoryName");
    }
    bool recursive = false;
    if (call.arguments.size() == 2) {
        const auto option = textArgument(call.arguments[1]);
        if (!option || *option != "s") {
            return failure(call, "rmdir option must be 's'",
                           "MParser:InvalidDirectoryOption");
        }
        recursive = true;
    }
    auto removed = systemContext(call)->removeDirectory(
        pathFromUtf8(*folder), recursive);
    if (!removed.succeeded) {
        const bool missing =
            removed.error.find("not a directory") != std::string::npos;
        return filesystemStatusResult(
            call, false, std::move(removed.error),
            missing ? "MATLAB:RMDIR:NotADirectory"
                    : "MATLAB:RMDIR:DirectoryNotRemoved");
    }
    return filesystemStatusResult(call, true, {}, {});
}

bool pathHasWildcard(const std::filesystem::path& path) {
    const auto text = pathToUtf8(path);
    return text.find('*') != std::string::npos ||
           text.find('?') != std::string::npos;
}

RuntimeSystemResult<std::vector<std::filesystem::path>> expandPathPattern(
    RuntimeSystemContext& context, const std::filesystem::path& source) {
    if (!pathHasWildcard(source)) {
        return RuntimeSystemResult<
            std::vector<std::filesystem::path>>::success({source});
    }
    const auto parent = source.parent_path();
    if (pathHasWildcard(parent)) {
        return RuntimeSystemResult<
            std::vector<std::filesystem::path>>::failure(
                "wildcards in parent directory components are unsupported");
    }
    const auto directory = parent.empty()
                               ? std::filesystem::path(".")
                               : parent;
    auto listing = context.listDirectory(directory);
    if (!listing.succeeded) {
        return RuntimeSystemResult<
            std::vector<std::filesystem::path>>::failure(
                std::move(listing.error));
    }
    const std::string pattern = pathToUtf8(source.filename());
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : listing.value) {
        if (wildcardMatch(pattern, entry.name)) {
            paths.push_back(parent.empty()
                                ? pathFromUtf8(entry.name)
                                : parent / pathFromUtf8(entry.name));
        }
    }
    if (paths.empty()) {
        return RuntimeSystemResult<
            std::vector<std::filesystem::path>>::failure(
                "no files match source pattern: " +
                pathToNativeUtf8(source));
    }
    return RuntimeSystemResult<
        std::vector<std::filesystem::path>>::success(std::move(paths));
}

BuiltinResult copyMoveBuiltin(std::string_view name,
                              const BuiltinCall& call) {
    const auto sourceText = textArgument(call.arguments.front());
    if (!sourceText || sourceText->empty() ||
        sourceText->find('\0') != std::string::npos) {
        return failure(call,
                       std::string(name) +
                           " source must be a nonempty text scalar",
                       "MParser:InvalidFileName");
    }
    std::string destinationText = ".";
    if (call.arguments.size() >= 2) {
        const auto destination = textArgument(call.arguments[1]);
        if (!destination || destination->empty() ||
            destination->find('\0') != std::string::npos) {
            return failure(call,
                           std::string(name) +
                               " destination must be a nonempty text scalar",
                           "MParser:InvalidFileName");
        }
        destinationText = *destination;
    }
    bool force = false;
    if (call.arguments.size() == 3) {
        const auto option = textArgument(call.arguments[2]);
        if (!option || *option != "f") {
            return failure(call,
                           std::string(name) + " option must be 'f'",
                           "MParser:InvalidFileOption");
        }
        force = true;
    }

    RuntimeSystemContext* context = systemContext(call);
    const auto source = pathFromUtf8(*sourceText);
    const auto destination = pathFromUtf8(destinationText);
    if (pathHasWildcard(destination)) {
        return failure(call,
                       std::string(name) +
                           " destination cannot contain wildcards",
                       "MParser:InvalidFileName");
    }
    auto sources = expandPathPattern(*context, source);
    if (!sources.succeeded) {
        return filesystemStatusResult(
            call, false, std::move(sources.error),
            name == "copyfile" ? "MATLAB:COPYFILE:FileNotFound"
                               : "MATLAB:MOVEFILE:FileNotFound");
    }

    const bool wildcard = pathHasWildcard(source);
    if (wildcard) {
        auto destinationDirectory = context->directoryExists(destination);
        if (!destinationDirectory.succeeded) {
            return filesystemStatusResult(
                call, false, std::move(destinationDirectory.error),
                "MParser:SystemOperationFailed");
        }
        if (!destinationDirectory.value) {
            auto created = context->createDirectories(destination);
            if (!created.succeeded) {
                return filesystemStatusResult(
                    call, false, std::move(created.error),
                    "MParser:SystemOperationFailed");
            }
        }
    }

    for (const auto& expandedSource : sources.value) {
        RuntimeSystemStatus status;
        if (name == "copyfile") {
            const auto sourceDirectory =
                context->directoryExists(expandedSource);
            if (!sourceDirectory.succeeded) {
                return filesystemStatusResult(
                    call, false, std::move(sourceDirectory.error),
                    "MParser:SystemOperationFailed");
            }
            const auto target = wildcard && sourceDirectory.value
                                    ? destination /
                                          expandedSource.filename()
                                    : destination;
            status = context->copyPath(expandedSource, target, force);
        } else {
            status = context->movePath(
                expandedSource, destination, force);
        }
        if (!status.succeeded) {
            return filesystemStatusResult(
                call, false, std::move(status.error),
                name == "copyfile" ? "MATLAB:COPYFILE:OSError"
                                   : "MATLAB:MOVEFILE:OSError");
        }
    }
    return filesystemStatusResult(call, true, {}, {});
}

std::optional<RuntimeFileOpenOptions> fileOpenOptions(
    std::string_view text, std::string& error) {
    if (text.empty() ||
        (text.front() != 'r' && text.front() != 'w' &&
         text.front() != 'a')) {
        error = "fopen permission must begin with r, w, or a";
        return std::nullopt;
    }
    bool update = false;
    bool binary = true;
    bool binarySpecified = false;
    bool textSpecified = false;
    for (size_t index = 1; index < text.size(); ++index) {
        switch (text[index]) {
        case '+':
            if (update) {
                error = "fopen permission contains duplicate + modifiers";
                return std::nullopt;
            }
            update = true;
            break;
        case 'b':
            if (binarySpecified || textSpecified) {
                error = "fopen permission contains incompatible mode modifiers";
                return std::nullopt;
            }
            binarySpecified = true;
            binary = true;
            break;
        case 't':
            if (binarySpecified || textSpecified) {
                error = "fopen permission contains incompatible mode modifiers";
                return std::nullopt;
            }
            textSpecified = true;
            binary = false;
            break;
        default:
            error = "fopen permission contains an unsupported modifier";
            return std::nullopt;
        }
    }

    RuntimeFileOpenOptions options;
    options.binary = binary;
    switch (text.front()) {
    case 'r':
        options.readable = true;
        options.writable = update;
        break;
    case 'w':
        options.readable = update;
        options.writable = true;
        options.truncate = true;
        break;
    case 'a':
        options.readable = update;
        options.writable = true;
        options.append = true;
        break;
    default:
        break;
    }
    options.permission.assign(1, text.front());
    if (update) {
        options.permission.push_back('+');
    }
    options.permission.push_back(binary ? 'b' : 't');
    options.machineFormat = std::string(runtimeFileByteOrderName(
        runtimeNativeFileByteOrder()));
    return options;
}

RuntimeValue fileIdentifierVector(const std::vector<int>& identifiers) {
    std::vector<double> values;
    values.reserve(identifiers.size());
    for (const int identifier : identifiers) {
        values.push_back(static_cast<double>(identifier));
    }
    if (auto result = runtimeNumericValueFromLogicalOrder(
            {1, identifiers.size()}, std::move(values),
            RuntimeNumericClass::Double)) {
        return std::move(*result);
    }
    return makeRuntimeMatrixValue(0, 0, {});
}

BuiltinResult fopenBuiltin(const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    if (const auto identifier = fileIdentifier(call.arguments.front())) {
        if (call.arguments.size() != 1) {
            return failure(call,
                           "fopen file-identifier query accepts one input",
                           "MParser:InvalidFileQuery");
        }
        const auto info = context->openFileInfo(*identifier);
        if (!info.succeeded) {
            return failure(call, std::move(info.error),
                           "MParser:InvalidFileIdentifier");
        }
        return selectedOutputs(call, {
            makeRuntimeCharacterVectorUtf8(
                pathToNativeUtf8(info.value.path)),
            makeRuntimeCharacterVectorUtf8(info.value.options.permission),
            makeRuntimeCharacterVectorUtf8(
                info.value.options.machineFormat),
            makeRuntimeCharacterVectorUtf8(info.value.options.encoding),
        });
    }

    const auto pathText = textArgument(call.arguments.front());
    if (!pathText) {
        return failure(call,
                       "fopen filename must be a text scalar or a valid fid");
    }
    if (*pathText == "all" && call.arguments.size() == 1) {
        const auto identifiers = context->openFileIdentifiers();
        return identifiers.succeeded
                   ? selectedOutputs(
                         call, {fileIdentifierVector(identifiers.value)})
                   : failure(call, identifiers.error,
                             "MParser:SystemOperationFailed");
    }
    const std::string permission =
        call.arguments.size() == 1
            ? "r"
            : textArgument(call.arguments[1]).value_or(std::string{});
    if (call.arguments.size() >= 2 && permission.empty() &&
        !textArgument(call.arguments[1])) {
        return failure(call, "fopen permission must be a text scalar");
    }
    std::string modeError;
    auto options = fileOpenOptions(permission, modeError);
    if (!options) {
        return failure(call, std::move(modeError),
                       "MParser:InvalidFilePermission");
    }
    if (call.arguments.size() >= 3) {
        const auto machineFormat = textArgument(call.arguments[2]);
        const auto byteOrder = machineFormat
                                   ? runtimeFileByteOrderFromName(
                                         *machineFormat)
                                   : std::nullopt;
        if (!byteOrder) {
            return failure(call,
                           "fopen machine format must be native, ieee-le, "
                           "or ieee-be",
                           "MParser:InvalidMachineFormat");
        }
        options->machineFormat =
            std::string(runtimeFileByteOrderName(*byteOrder));
    }
    if (call.arguments.size() >= 4) {
        const auto encoding = textArgument(call.arguments[3]);
        if (!encoding ||
            (*encoding != "UTF-8" && *encoding != "utf-8" &&
             *encoding != "UTF8" && *encoding != "utf8")) {
            return failure(call,
                           "fopen currently supports only UTF-8 encoding",
                           "MParser:UnsupportedFileEncoding");
        }
        options->encoding = "UTF-8";
    }

    RuntimeSystemResult<int> opened;
    try {
        opened = context->openFile(pathFromUtf8(*pathText), *options);
    } catch (const std::exception& error) {
        opened = RuntimeSystemResult<int>::failure(error.what());
    }
    if (!opened.succeeded) {
        return selectedOutputs(call, {
            makeRuntimeNumberValue(-1.0),
            makeRuntimeCharacterVectorUtf8(opened.error),
        });
    }
    return selectedOutputs(call, {
        makeRuntimeNumberValue(static_cast<double>(opened.value)),
        makeRuntimeCharacterVectorUtf8(""),
    });
}

BuiltinResult fcloseBuiltin(const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    if (textArgument(call.arguments.front()) ==
        std::optional<std::string>{"all"}) {
        const auto status = context->closeAllFiles();
        return status.succeeded
                   ? selectedOutputs(call, {makeRuntimeNumberValue(0.0)})
                   : failure(call, std::move(status.error),
                             "MParser:SystemOperationFailed");
    }
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "fclose requires an open file identifier or all",
                       "MParser:InvalidFileIdentifier");
    }
    const auto status = context->closeFile(*identifier);
    return status.succeeded
               ? selectedOutputs(call, {makeRuntimeNumberValue(0.0)})
               : failure(call, std::move(status.error),
                         "MParser:InvalidFileIdentifier");
}

BuiltinResult fseekBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments[0]);
    const auto offset = fileOffset(call.arguments[1]);
    const auto origin = fileSeekOrigin(call.arguments[2]);
    if (!identifier || *identifier < 3) {
        return failure(call, "fseek requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    if (!offset || !origin) {
        return failure(
            call,
            "fseek requires an integer byte offset and bof/cof/eof origin",
            "MParser:InvalidFilePosition");
    }
    const auto status = systemContext(call)->seekFile(
        *identifier, *offset, *origin);
    return selectedOutputs(
        call, {makeRuntimeNumberValue(status.succeeded ? 0.0 : -1.0)});
}

BuiltinResult ftellBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "ftell requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    const auto position = systemContext(call)->filePosition(*identifier);
    return BuiltinResult::success({makeRuntimeNumberValue(
        position.succeeded ? static_cast<double>(position.value) : -1.0)});
}

BuiltinResult frewindBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "frewind requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    const auto status = systemContext(call)->seekFile(
        *identifier, 0, RuntimeFileSeekOrigin::Beginning);
    return status.succeeded
               ? BuiltinResult::success()
               : failure(call, std::move(status.error),
                         "MParser:SystemOperationFailed");
}

std::optional<size_t> nonnegativeFileSize(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) || value.numericComplex ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    return element
               ? runtimeNumericElementAsNonnegativeSize(*element)
               : std::nullopt;
}

BuiltinResult fileLineBuiltin(std::string_view name,
                              const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, std::string(name) +
                                 " requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    std::optional<size_t> maximumCharacters;
    if (call.arguments.size() == 2) {
        maximumCharacters = nonnegativeFileSize(call.arguments[1]);
        if (!maximumCharacters || *maximumCharacters == 0) {
            return failure(call,
                           "fgets character limit must be a positive integer",
                           "MParser:InvalidFileReadSize");
        }
    }

    RuntimeSystemContext* context = systemContext(call);
    auto input = context->readFileRemaining(*identifier);
    if (!input.succeeded) {
        return failure(call, std::move(input.error),
                       "MParser:SystemOperationFailed");
    }
    const bool keepTerminator = name == "fgets";
    auto line = runtimeReadFileLine(
        input.value, keepTerminator, maximumCharacters);
    const auto restored = context->restoreUnreadFileData(
        *identifier, input.value.substr(line.consumedBytes));
    if (!restored.succeeded) {
        return failure(call, std::move(restored.error),
                       "MParser:SystemOperationFailed");
    }

    RuntimeValue value = line.hasValue
                             ? makeRuntimeCharacterVectorUtf8(line.text)
                             : makeRuntimeNumberValue(-1.0);
    if (name == "fgetl") {
        return BuiltinResult::success({std::move(value)});
    }
    const double terminator = line.terminator.empty()
                                  ? 0.0
                                  : static_cast<double>(
                                        static_cast<unsigned char>(
                                            line.terminator.back()));
    return selectedOutputs(call, {
        std::move(value), makeRuntimeNumberValue(terminator)});
}

BuiltinResult feofBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "feof requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    const auto result = systemContext(call)->fileEndOfFile(*identifier);
    return result.succeeded
               ? BuiltinResult::success({makeRuntimeNumberValue(
                     result.value ? 1.0 : 0.0)})
               : failure(call, std::move(result.error),
                         "MParser:InvalidFileIdentifier");
}

BuiltinResult ferrorBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "ferror requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    bool clear = false;
    if (call.arguments.size() == 2) {
        const auto operation = textArgument(call.arguments[1]);
        if (!operation || *operation != "clear") {
            return failure(call,
                           "ferror second input must be clear",
                           "MParser:InvalidFileErrorOperation");
        }
        clear = true;
    }
    const auto result = systemContext(call)->fileError(*identifier, clear);
    return result.succeeded
               ? selectedOutputs(call, {
                     makeRuntimeCharacterVectorUtf8(result.value.message),
                     makeRuntimeNumberValue(
                         static_cast<double>(result.value.number)),
                 })
               : failure(call, std::move(result.error),
                         "MParser:InvalidFileIdentifier");
}

std::optional<RuntimeBinaryReadSize> binaryReadSize(
    const RuntimeValue& value, std::string& error) {
    if (!isRuntimeNumericValue(value) || value.numericComplex) {
        error = "fread size must be a real numeric scalar or two-element vector";
        return std::nullopt;
    }
    const size_t count = runtimeShapeElementCount(value);
    if (count != 1 && count != 2) {
        error = "fread size must contain one or two elements";
        return std::nullopt;
    }
    std::vector<std::optional<size_t>> dimensions;
    dimensions.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto numeric = runtimeNumericElement(value, index);
        if (!numeric || *numeric < 0.0 || std::isnan(*numeric) ||
            (std::isfinite(*numeric) && std::trunc(*numeric) != *numeric) ||
            (std::isfinite(*numeric) &&
             *numeric > static_cast<double>(
                            std::numeric_limits<size_t>::max()))) {
            error = "fread size elements must be nonnegative integers or Inf";
            return std::nullopt;
        }
        dimensions.push_back(
            std::isinf(*numeric)
                ? std::nullopt
                : std::optional<size_t>(static_cast<size_t>(*numeric)));
    }

    RuntimeBinaryReadSize result;
    if (count == 1) {
        result.scalarRequested = dimensions.front().has_value();
        result.maximumValues = dimensions.front();
        return result;
    }
    if (!dimensions.front()) {
        error = "fread row count must be finite";
        return std::nullopt;
    }
    result.matrixRequested = true;
    result.rows = *dimensions.front();
    result.columns = dimensions[1];
    if (result.rows == 0) {
        result.maximumValues = 0;
    } else if (result.columns) {
        if (*result.columns >
            std::numeric_limits<size_t>::max() / result.rows) {
            error = "fread size is too large";
            return std::nullopt;
        }
        result.maximumValues = result.rows * *result.columns;
    }
    return result;
}

std::optional<RuntimeFileByteOrder> fileByteOrder(
    RuntimeSystemContext& context, int identifier,
    const std::optional<std::string>& overrideName,
    std::string& error) {
    if (overrideName) {
        const auto result = runtimeFileByteOrderFromName(*overrideName);
        if (!result) {
            error = "file machine format must be native, ieee-le, or ieee-be";
        }
        return result;
    }
    const auto info = context.openFileInfo(identifier);
    if (!info.succeeded) {
        error = info.error;
        return std::nullopt;
    }
    const auto result = runtimeFileByteOrderFromName(
        info.value.options.machineFormat);
    if (!result) {
        error = "open file has an invalid machine format";
    }
    return result;
}

BuiltinResult freadBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "fread requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }

    RuntimeBinaryReadSize size;
    std::string precisionText = "uint8=>double";
    size_t skipBytes = 0;
    std::optional<std::string> machineFormat;
    size_t index = 1;
    if (index < call.arguments.size() &&
        !textArgument(call.arguments[index])) {
        std::string sizeError;
        const auto parsed = binaryReadSize(
            call.arguments[index++], sizeError);
        if (!parsed) {
            return failure(call, std::move(sizeError),
                           "MParser:InvalidFileReadSize");
        }
        size = *parsed;
    }
    if (index < call.arguments.size()) {
        if (const auto precision = textArgument(call.arguments[index])) {
            precisionText = *precision;
            ++index;
        }
    }
    if (index < call.arguments.size()) {
        if (const auto format = textArgument(call.arguments[index])) {
            machineFormat = *format;
            ++index;
        } else {
            const auto skip = nonnegativeFileSize(call.arguments[index]);
            if (!skip) {
                return failure(call,
                               "fread skip must be a nonnegative integer",
                               "MParser:InvalidFileReadSkip");
            }
            skipBytes = *skip;
            ++index;
        }
    }
    if (index < call.arguments.size()) {
        machineFormat = textArgument(call.arguments[index++]);
        if (!machineFormat) {
            return failure(call,
                           "fread machine format must be a text scalar",
                           "MParser:InvalidMachineFormat");
        }
    }
    if (index != call.arguments.size()) {
        return failure(call, "fread received too many inputs");
    }

    const auto precision = runtimeParseBinaryPrecision(
        precisionText, true);
    if (!precision.succeeded) {
        return failure(call, precision.error,
                       "MParser:InvalidBinaryPrecision");
    }
    RuntimeSystemContext* context = systemContext(call);
    std::string orderError;
    const auto order = fileByteOrder(
        *context, *identifier, machineFormat, orderError);
    if (!order) {
        return failure(call, std::move(orderError),
                       "MParser:InvalidMachineFormat");
    }
    auto input = context->readFileRemaining(*identifier);
    if (!input.succeeded) {
        return failure(call, std::move(input.error),
                       "MParser:SystemOperationFailed");
    }
    auto decoded = runtimeDecodeBinaryData(
        input.value, precision.precision, size, skipBytes, *order);
    if (!decoded.succeeded) {
        (void)context->restoreUnreadFileData(
            *identifier, std::move(input.value));
        return failure(call, std::move(decoded.error),
                       "MParser:InvalidBinaryInput");
    }
    const auto restored = context->restoreUnreadFileData(
        *identifier, input.value.substr(decoded.consumedBytes));
    if (!restored.succeeded) {
        return failure(call, std::move(restored.error),
                       "MParser:SystemOperationFailed");
    }
    return selectedOutputs(call, {
        std::move(decoded.value),
        makeRuntimeNumberValue(static_cast<double>(decoded.valueCount)),
    });
}

BuiltinResult fwriteBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments.front());
    if (!identifier || *identifier < 3) {
        return failure(call, "fwrite requires an open file identifier",
                       "MParser:InvalidFileIdentifier");
    }
    std::string precisionText = "uint8";
    size_t skipBytes = 0;
    std::optional<std::string> machineFormat;
    size_t index = 2;
    if (index < call.arguments.size()) {
        const auto precision = textArgument(call.arguments[index]);
        if (!precision) {
            return failure(call,
                           "fwrite precision must be a text scalar",
                           "MParser:InvalidBinaryPrecision");
        }
        precisionText = *precision;
        ++index;
    }
    if (index < call.arguments.size()) {
        if (const auto format = textArgument(call.arguments[index])) {
            machineFormat = *format;
            ++index;
        } else {
            const auto skip = nonnegativeFileSize(call.arguments[index]);
            if (!skip) {
                return failure(call,
                               "fwrite skip must be a nonnegative integer",
                               "MParser:InvalidFileWriteSkip");
            }
            skipBytes = *skip;
            ++index;
        }
    }
    if (index < call.arguments.size()) {
        machineFormat = textArgument(call.arguments[index++]);
        if (!machineFormat) {
            return failure(call,
                           "fwrite machine format must be a text scalar",
                           "MParser:InvalidMachineFormat");
        }
    }
    if (index != call.arguments.size()) {
        return failure(call, "fwrite received too many inputs");
    }

    const auto precision = runtimeParseBinaryPrecision(
        precisionText, false);
    if (!precision.succeeded) {
        return failure(call, precision.error,
                       "MParser:InvalidBinaryPrecision");
    }
    RuntimeSystemContext* context = systemContext(call);
    std::string orderError;
    const auto order = fileByteOrder(
        *context, *identifier, machineFormat, orderError);
    if (!order) {
        return failure(call, std::move(orderError),
                       "MParser:InvalidMachineFormat");
    }
    auto encoded = runtimeEncodeBinaryData(
        call.arguments[1], precision.precision, *order);
    if (!encoded.succeeded) {
        return failure(call, std::move(encoded.error),
                       "MParser:InvalidBinaryOutput");
    }
    const auto written = context->writeFileBlocks(
        *identifier, encoded.bytes, encoded.blockBytes, skipBytes);
    if (!written.succeeded) {
        return failure(call, std::move(written.error),
                       "MParser:SystemOperationFailed");
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({makeRuntimeNumberValue(
                     static_cast<double>(encoded.valueCount))});
}

std::optional<RuntimeFileScanSize> fileScanSize(
    const RuntimeValue& value, std::string& error) {
    if (!isRuntimeNumericValue(value) || value.numericComplex) {
        error = "fscanf size must be a real numeric scalar or two-element vector";
        return std::nullopt;
    }
    const size_t count = runtimeShapeElementCount(value);
    if (count != 1 && count != 2) {
        error = "fscanf size must contain one or two elements";
        return std::nullopt;
    }
    std::vector<std::optional<size_t>> dimensions;
    dimensions.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto numeric = runtimeNumericElement(value, index);
        if (!numeric || *numeric < 0.0 || std::isnan(*numeric) ||
            (std::isfinite(*numeric) && std::trunc(*numeric) != *numeric) ||
            (std::isfinite(*numeric) &&
             *numeric > static_cast<double>(
                            std::numeric_limits<size_t>::max()))) {
            error = "fscanf size elements must be nonnegative integers or Inf";
            return std::nullopt;
        }
        dimensions.push_back(
            std::isinf(*numeric)
                ? std::nullopt
                : std::optional<size_t>(static_cast<size_t>(*numeric)));
    }

    RuntimeFileScanSize result;
    if (count == 1) {
        result.scalarRequested = dimensions.front().has_value();
        result.maximumMatches = dimensions.front();
        return result;
    }
    if (!dimensions.front()) {
        error = "fscanf row count must be finite";
        return std::nullopt;
    }
    result.matrixRequested = true;
    result.rows = *dimensions.front();
    result.columns = dimensions[1];
    if (result.rows == 0) {
        result.maximumMatches = 0;
        return result;
    }
    if (result.columns) {
        if (result.rows != 0 &&
            *result.columns >
                std::numeric_limits<size_t>::max() / result.rows) {
            error = "fscanf size is too large";
            return std::nullopt;
        }
        result.maximumMatches = result.rows * *result.columns;
    }
    return result;
}

BuiltinResult fscanfBuiltin(const BuiltinCall& call) {
    const auto identifier = fileIdentifier(call.arguments[0]);
    const auto format = textArgument(call.arguments[1]);
    if (!identifier || *identifier < 3 || !format) {
        return failure(call,
                       "fscanf requires an open fid and a text format",
                       "MParser:InvalidFormattedInput");
    }
    RuntimeFileScanSize size;
    if (call.arguments.size() == 3) {
        std::string sizeError;
        const auto parsed = fileScanSize(call.arguments[2], sizeError);
        if (!parsed) {
            return failure(call, std::move(sizeError),
                           "MParser:InvalidFormattedInput");
        }
        size = *parsed;
    }

    RuntimeSystemContext* context = systemContext(call);
    auto input = context->readFileRemaining(*identifier);
    if (!input.succeeded) {
        return failure(call, std::move(input.error),
                       "MParser:SystemOperationFailed");
    }
    auto scanned = runtimeScanFormattedText(input.value, *format, size);
    if (!scanned.succeeded) {
        (void)context->restoreUnreadFileData(
            *identifier, std::move(input.value));
        return failure(call, std::move(scanned.error),
                       "MParser:InvalidFormattedInput");
    }
    const auto restored = context->restoreUnreadFileData(
        *identifier, input.value.substr(scanned.consumedBytes));
    if (!restored.succeeded) {
        return failure(call, std::move(restored.error),
                       "MParser:SystemOperationFailed");
    }
    return selectedOutputs(call, {
        std::move(scanned.value),
        makeRuntimeNumberValue(static_cast<double>(scanned.matchedCount)),
    });
}

BuiltinResult fprintfBuiltin(const BuiltinCall& call) {
    size_t formatIndex = 0;
    std::optional<int> identifier;
    if (call.arguments.size() >= 2) {
        identifier = fileIdentifier(call.arguments.front());
        if (identifier) {
            formatIndex = 1;
        }
    }
    std::vector<RuntimeValue> formatArguments(
        call.arguments.begin() + static_cast<std::ptrdiff_t>(formatIndex),
        call.arguments.end());
    const auto formatted = runtimeFormatPrintf(formatArguments);
    if (!formatted.succeeded) {
        return failure(call, formatted.error,
                       "MParser:InvalidFormattedOutput");
    }

    if (!identifier || *identifier == 1 || *identifier == 2) {
        const auto emitted = emit(call, RuntimeOutputKind::StandardOutput,
                                  formatted.text);
        if (!emitted.succeeded) {
            return emitted;
        }
    } else {
        if (*identifier < 3 || !systemContext(call)) {
            return failure(call, "fprintf received an invalid file identifier",
                           "MParser:InvalidFileIdentifier");
        }
        const auto written =
            systemContext(call)->writeFile(*identifier, formatted.text);
        if (!written.succeeded) {
            return failure(call, std::move(written.error),
                           "MParser:SystemOperationFailed");
        }
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({makeRuntimeNumberValue(
                     static_cast<double>(formatted.text.size()))});
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::optional<RuntimeNumericDisplayFormat> numericDisplayFormat(
    std::string_view style) {
    const std::string normalized = lowercase(std::string(style));
    if (normalized == "short") {
        return RuntimeNumericDisplayFormat::Short;
    }
    if (normalized == "long") {
        return RuntimeNumericDisplayFormat::Long;
    }
    if (normalized == "shorte" || normalized == "short e") {
        return RuntimeNumericDisplayFormat::ShortE;
    }
    if (normalized == "longe" || normalized == "long e") {
        return RuntimeNumericDisplayFormat::LongE;
    }
    if (normalized == "shortg" || normalized == "short g") {
        return RuntimeNumericDisplayFormat::ShortG;
    }
    if (normalized == "longg" || normalized == "long g") {
        return RuntimeNumericDisplayFormat::LongG;
    }
    if (normalized == "shorteng" || normalized == "short eng") {
        return RuntimeNumericDisplayFormat::ShortEng;
    }
    if (normalized == "longeng" || normalized == "long eng") {
        return RuntimeNumericDisplayFormat::LongEng;
    }
    if (normalized == "+") {
        return RuntimeNumericDisplayFormat::Plus;
    }
    if (normalized == "bank") {
        return RuntimeNumericDisplayFormat::Bank;
    }
    if (normalized == "hex") {
        return RuntimeNumericDisplayFormat::Hex;
    }
    if (normalized == "rational" || normalized == "rat") {
        return RuntimeNumericDisplayFormat::Rational;
    }
    return std::nullopt;
}

RuntimeValue displayFormatValue(const RuntimeDisplayFormat& format) {
    RuntimeValue result = makeRuntimeStructValue({
        {"NumericFormat", makeRuntimeCharacterVectorUtf8(
                              runtimeNumericDisplayFormatName(
                                  format.numeric))},
        {"LineSpacing", makeRuntimeCharacterVectorUtf8(
                            runtimeLineSpacingName(format.spacing))},
    });
    result.fieldOrder = {"NumericFormat", "LineSpacing"};
    return result;
}

std::optional<RuntimeDisplayFormat> parseDisplayFormatValue(
    const RuntimeValue& value) {
    if (!isRuntimeScalarStruct(value)) {
        return std::nullopt;
    }
    const RuntimeValue* numeric = runtimeStructField(value, "NumericFormat");
    const RuntimeValue* spacing = runtimeStructField(value, "LineSpacing");
    if (!numeric || !spacing) {
        return std::nullopt;
    }
    const auto numericText = textArgument(*numeric);
    const auto spacingText = textArgument(*spacing);
    const auto numericFormat =
        numericText ? numericDisplayFormat(*numericText) : std::nullopt;
    if (!numericFormat || !spacingText ||
        (*spacingText != "loose" && *spacingText != "compact")) {
        return std::nullopt;
    }
    return RuntimeDisplayFormat{
        *numericFormat,
        *spacingText == "compact" ? RuntimeLineSpacing::Compact
                                   : RuntimeLineSpacing::Loose};
}

BuiltinResult formatBuiltin(const BuiltinCall& call) {
    if (!call.context || !call.context->displayFormat ||
        !call.context->displayFormat->current ||
        !call.context->displayFormat->replace) {
        return failure(call, "format requires session display state",
                       "MParser:MissingBuiltinContext");
    }
    RuntimeDisplayFormat previous =
        call.context->displayFormat->current();
    RuntimeDisplayFormat next = previous;
    bool replace = false;

    if (call.arguments.empty()) {
        if (call.requestedOutputCount == 0) {
            next = {};
            replace = true;
        }
    } else if (call.arguments.size() == 1 &&
               isRuntimeScalarStruct(call.arguments.front())) {
        const auto parsed = parseDisplayFormatValue(call.arguments.front());
        if (!parsed) {
            return failure(call,
                           "format options structure is invalid");
        }
        next = *parsed;
        replace = true;
    } else {
        std::vector<std::string> styles;
        styles.reserve(call.arguments.size());
        for (const RuntimeValue& argument : call.arguments) {
            const auto style = textArgument(argument);
            if (!style) {
                return failure(call,
                               "format style must be a text scalar or "
                               "format options structure");
            }
            styles.push_back(*style);
        }
        std::string combined = styles.front();
        if (styles.size() == 2) {
            combined += " " + styles[1];
        }
        const std::string normalized = lowercase(combined);
        if (normalized == "default") {
            next = {};
        } else if (normalized == "compact") {
            next.spacing = RuntimeLineSpacing::Compact;
        } else if (normalized == "loose") {
            next.spacing = RuntimeLineSpacing::Loose;
        } else if (const auto numeric = numericDisplayFormat(combined)) {
            next.numeric = *numeric;
        } else {
            return failure(call, "unsupported format style: " + combined,
                           "MParser:InvalidDisplayFormat");
        }
        replace = true;
    }

    if (replace) {
        previous = call.context->displayFormat->replace(next);
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({displayFormatValue(previous)});
}

constexpr char pathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::string pathListText(
    const std::vector<std::filesystem::path>& paths) {
    std::string result;
    for (const auto& path : paths) {
        if (!result.empty()) {
            result.push_back(pathListSeparator());
        }
        result += pathToNativeUtf8(path);
    }
    return result;
}

BuiltinResult separatorBuiltin(std::string_view name,
                               const BuiltinCall& call) {
    const char value = name == "filesep"
                           ? nativeFileSeparator()
                           : pathListSeparator();
    return selectedOutputs(
        call, {makeRuntimeCharacterVectorUtf8(std::string(1, value))});
}

std::vector<std::filesystem::path> splitPathList(std::string_view text) {
    std::vector<std::filesystem::path> result;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(pathListSeparator(), begin);
        const std::string_view part = text.substr(
            begin, end == std::string_view::npos
                       ? std::string_view::npos
                       : end - begin);
        if (!part.empty()) {
            result.push_back(pathFromUtf8(part));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

BuiltinResult pathBuiltin(std::string_view name,
                          const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    const auto oldPaths = context->searchPaths();
    if (!oldPaths.succeeded) {
        return failure(call, std::move(oldPaths.error),
                       "MParser:SystemCapabilityDenied");
    }
    const RuntimeValue oldValue = makeRuntimeCharacterVectorUtf8(
        pathListText(oldPaths.value));

    RuntimeSystemStatus status = RuntimeSystemStatus::success();
    if (name == "path" && !call.arguments.empty()) {
        const auto value = textArgument(call.arguments.front());
        if (!value) {
            return failure(call, "path value must be a text scalar");
        }
        status = context->setSearchPaths(splitPathList(*value));
    } else if (name == "addpath") {
        bool prepend = true;
        std::vector<std::filesystem::path> additions;
        for (const RuntimeValue& argument : call.arguments) {
            const auto value = textArgument(argument);
            if (!value) {
                return failure(call,
                               "addpath arguments must be text scalars");
            }
            if (*value == "-begin") {
                prepend = true;
            } else if (*value == "-end") {
                prepend = false;
            } else {
                additions.push_back(pathFromUtf8(*value));
            }
        }
        status = context->addSearchPaths(additions, prepend);
    } else if (name == "rmpath") {
        std::vector<std::filesystem::path> removals;
        for (const RuntimeValue& argument : call.arguments) {
            const auto value = textArgument(argument);
            if (!value) {
                return failure(call,
                               "rmpath arguments must be text scalars");
            }
            removals.push_back(pathFromUtf8(*value));
        }
        status = context->removeSearchPaths(removals);
    }
    if (!status.succeeded) {
        return failure(call, std::move(status.error),
                       "MParser:SystemOperationFailed");
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({oldValue});
}

BuiltinResult directoryBuiltin(std::string_view name,
                               const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    if (name == "tempdir") {
        const auto result = context->temporaryDirectory();
        if (!result.succeeded) {
            return failure(call, std::move(result.error),
                           "MParser:SystemOperationFailed");
        }
        std::string path = pathToNativeUtf8(result.value);
        if (!path.ends_with(nativeFileSeparator())) {
            path.push_back(nativeFileSeparator());
        }
        return selectedOutputs(
            call, {makeRuntimeCharacterVectorUtf8(std::move(path))});
    }

    const auto oldDirectory = context->currentDirectory();
    if (!oldDirectory.succeeded) {
        return failure(call, std::move(oldDirectory.error),
                       "MParser:SystemCapabilityDenied");
    }
    if (name == "cd" && !call.arguments.empty()) {
        const auto value = textArgument(call.arguments.front());
        if (!value) {
            return failure(call, "cd target must be a text scalar");
        }
        const auto status = context->changeCurrentDirectory(
            pathFromUtf8(*value));
        if (!status.succeeded) {
            return failure(call, status.error,
                           "MParser:SystemOperationFailed");
        }
    }
    return selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
        pathToNativeUtf8(oldDirectory.value))});
}

BuiltinResult environmentBuiltin(const BuiltinCall& call) {
    const auto name = textArgument(call.arguments.front());
    if (!name) {
        return failure(call, "getenv name must be a text scalar");
    }
    const auto result = systemContext(call)->environment(*name);
    return result.succeeded
               ? selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
                     result.value.value_or(""))})
               : failure(call, std::move(result.error),
                         "MParser:SystemOperationFailed");
}

std::filesystem::path functionFileName(std::string_view name) {
    auto result = pathFromUtf8(name);
    if (!result.has_extension()) {
        result += ".m";
    }
    return result;
}

std::vector<std::filesystem::path> searchRoots(
    RuntimeSystemContext& context) {
    std::vector<std::filesystem::path> roots;
    const auto current = context.currentDirectory();
    if (current.succeeded) {
        roots.push_back(current.value);
    }
    const auto paths = context.searchPaths();
    if (paths.succeeded) {
        for (const auto& path : paths.value) {
            if (std::find(roots.begin(), roots.end(), path) == roots.end()) {
                roots.push_back(path);
            }
        }
    }
    return roots;
}

std::vector<std::filesystem::path> fileCandidates(
    std::string_view name) {
    const std::filesystem::path requested = pathFromUtf8(name);
    if (requested.has_extension()) {
        return {requested};
    }
    return {
        requested,
        pathFromUtf8(std::string(name) + ".mexw64"),
        pathFromUtf8(std::string(name) + ".mexa64"),
        pathFromUtf8(std::string(name) + ".mexmaci64"),
        pathFromUtf8(std::string(name) + ".mexmaca64"),
        pathFromUtf8(std::string(name) + ".p"),
        pathFromUtf8(std::string(name) + ".slx"),
        pathFromUtf8(std::string(name) + ".mdl"),
        pathFromUtf8(std::string(name) + ".m"),
        pathFromUtf8(std::string(name) + ".mlx"),
        pathFromUtf8(std::string(name) + ".mlapp"),
    };
}

double fileTypeCode(const std::filesystem::path& path) {
    std::string extension = pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (extension.starts_with(".mex")) {
        return 3.0;
    }
    if (extension == ".slx" || extension == ".mdl") {
        return 4.0;
    }
    if (extension == ".p") {
        return 6.0;
    }
    return 2.0;
}

struct ExistingPath {
    double code = 0.0;
    std::filesystem::path path;
};

std::optional<ExistingPath> findExistingPath(
    RuntimeSystemContext& context, std::string_view name,
    bool directoriesOnly) {
    const auto candidates = fileCandidates(name);
    const auto inspect = [&context, directoriesOnly](
                             const std::filesystem::path& candidate)
        -> std::optional<ExistingPath> {
        const auto directory = context.directoryExists(candidate);
        if (directory.succeeded && directory.value) {
            return ExistingPath{7.0, candidate.lexically_normal()};
        }
        if (directoriesOnly) {
            return std::nullopt;
        }
        const auto file = context.regularFileExists(candidate);
        if (file.succeeded && file.value) {
            return ExistingPath{
                fileTypeCode(candidate), candidate.lexically_normal()};
        }
        return std::nullopt;
    };

    if (pathFromUtf8(name).is_absolute()) {
        for (const auto& candidate : candidates) {
            if (const auto existing = inspect(candidate)) {
                return existing;
            }
        }
        return std::nullopt;
    }

    const auto roots = searchRoots(context);
    for (const auto& root : roots) {
        for (const auto& relative : candidates) {
            if (const auto existing = inspect(root / relative)) {
                return existing;
            }
        }
    }
    return std::nullopt;
}

bool workspaceFunctionExists(const BuiltinCall& call,
                             std::string_view name) {
    return call.context && call.context->workspace &&
           call.context->workspace->functionExists &&
           call.context->workspace->functionExists(name);
}

bool workspaceClassExists(const BuiltinCall& call,
                          std::string_view name) {
    return call.context && call.context->workspace &&
           call.context->workspace->classExists &&
           call.context->workspace->classExists(name);
}

BuiltinResult existBuiltin(const BuiltinCall& call) {
    const auto name = textArgument(call.arguments.front());
    if (!name) {
        return failure(call, "exist name must be a text scalar");
    }
    std::optional<std::string> searchType;
    if (call.arguments.size() == 2) {
        searchType = textArgument(call.arguments[1]);
        if (!searchType) {
            return failure(call,
                           "exist search type must be a text scalar");
        }
        *searchType = lowercase(std::move(*searchType));
        if (*searchType != "var" && *searchType != "builtin" &&
            *searchType != "class" && *searchType != "dir" &&
            *searchType != "file") {
            return failure(call, "unsupported exist search type: " +
                                     *searchType,
                           "MParser:InvalidExistSearchType");
        }
    }

    const bool variable = call.context && call.context->workspace &&
                          call.context->workspace->variables &&
                          call.context->workspace->variables->contains(*name);
    const bool builtin = call.context && call.context->registry &&
                         call.context->registry->contains(*name);
    const bool klass = workspaceClassExists(call, *name);
    const bool function = workspaceFunctionExists(call, *name);
    const auto result = [](double code) {
        return BuiltinResult::success({makeRuntimeNumberValue(code)});
    };

    if (searchType == "var") {
        return result(variable ? 1.0 : 0.0);
    }
    if (searchType == "builtin") {
        return result(builtin ? 5.0 : 0.0);
    }
    if (searchType == "class") {
        return result(klass ? 8.0 : 0.0);
    }

    RuntimeSystemContext* context = systemContext(call);
    if (searchType == "dir") {
        const auto path = context
                              ? findExistingPath(*context, *name, true)
                              : std::nullopt;
        return result(path && path->code == 7.0 ? 7.0 : 0.0);
    }
    if (searchType == "file") {
        const auto path = context
                              ? findExistingPath(*context, *name, false)
                              : std::nullopt;
        return result(path ? path->code : function ? 2.0 : 0.0);
    }

    if (variable) {
        return result(1.0);
    }
    if (builtin) {
        return result(5.0);
    }
    if (klass) {
        return result(8.0);
    }
    const auto path = context
                          ? findExistingPath(*context, *name, false)
                          : std::nullopt;
    if (path) {
        return result(path->code);
    }
    return result(function ? 2.0 : 0.0);
}

BuiltinResult whichBuiltin(const BuiltinCall& call) {
    const auto name = textArgument(call.arguments.front());
    if (!name) {
        return failure(call, "which name must be a text scalar");
    }
    if (call.context && call.context->workspace &&
        call.context->workspace->variables &&
        call.context->workspace->variables->contains(*name)) {
        return selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
            "variable: " + *name)});
    }

    RuntimeSystemContext* context = systemContext(call);
    const auto current = context->currentDirectory();
    const auto paths = context->searchPaths();
    if (current.succeeded && paths.succeeded) {
        std::vector<std::filesystem::path> roots{current.value};
        roots.insert(roots.end(), paths.value.begin(), paths.value.end());
        const auto relative = functionFileName(*name);
        for (const auto& root : roots) {
            const auto candidate = relative.is_absolute()
                                       ? relative
                                       : root / relative;
            const auto exists = context->regularFileExists(candidate);
            if (!exists.succeeded) {
                return failure(call, std::move(exists.error),
                               "MParser:SystemOperationFailed");
            }
            if (exists.value) {
                return selectedOutputs(call, {
                    makeRuntimeCharacterVectorUtf8(
                        pathToNativeUtf8(
                            candidate.lexically_normal()))});
            }
        }
    }
    if (call.context && call.context->registry &&
        call.context->registry->contains(*name)) {
        return selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
            "built-in (MParser): " + *name)});
    }
    return selectedOutputs(call, {makeRuntimeCharacterVectorUtf8("")});
}

RuntimeValue directoryEntriesValue(
    const std::vector<RuntimeDirectoryEntry>& entries) {
    const std::vector<std::string> fields = {
        "name", "folder", "date", "bytes", "isdir", "datenum"};
    std::vector<RuntimeStructElement> values;
    values.reserve(entries.size());
    for (const auto& entry : entries) {
        values.push_back({
            {"name", makeRuntimeCharacterVectorUtf8(entry.name)},
            {"folder", makeRuntimeCharacterVectorUtf8(entry.folder)},
            {"date", makeRuntimeCharacterVectorUtf8(entry.date)},
            {"bytes", makeRuntimeNumberValue(
                          static_cast<double>(entry.bytes))},
            {"isdir", makeRuntimeLogicalValue(entry.directory)},
            {"datenum", makeRuntimeNumberValue(0.0)},
        });
    }
    const size_t count = values.size();
    return makeRuntimeStructArrayValue(
        fields, std::move(values), {count, 1});
}

BuiltinResult dirBuiltin(const BuiltinCall& call) {
    std::filesystem::path query;
    if (!call.arguments.empty()) {
        const auto value = textArgument(call.arguments.front());
        if (!value) {
            return failure(call, "dir target must be a text scalar");
        }
        query = pathFromUtf8(*value);
    }

    RuntimeSystemContext* context = systemContext(call);
    std::filesystem::path directory = query;
    std::string pattern = "*";
    if (!query.empty()) {
        const auto isDirectory = context->directoryExists(query);
        if (!isDirectory.succeeded) {
            return failure(call, std::move(isDirectory.error),
                           "MParser:SystemOperationFailed");
        }
        if (!isDirectory.value) {
            directory = query.parent_path();
            pattern = pathToUtf8(query.filename());
            if (directory.empty()) {
                directory = ".";
            }
        }
    }
    const auto listing = context->listDirectory(directory);
    if (!listing.succeeded) {
        return failure(call, std::move(listing.error),
                       "MParser:SystemOperationFailed");
    }
    std::vector<RuntimeDirectoryEntry> selected;
    for (const auto& entry : listing.value) {
        if (wildcardMatch(pattern, entry.name)) {
            selected.push_back(entry);
        }
    }
    if (call.requestedOutputCount == 1) {
        return BuiltinResult::success({
            directoryEntriesValue(selected)});
    }
    std::ostringstream output;
    for (const auto& entry : selected) {
        output << entry.name;
        if (entry.directory) {
            output << '/';
        }
        output << '\n';
    }
    return emit(call, RuntimeOutputKind::StandardOutput, output.str());
}

std::string dateText(const RuntimeCalendarTime& value) {
    static constexpr std::array<std::string_view, 12> months = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (value.month < 1 || value.month > 12) {
        return {};
    }
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << value.day << '-'
           << months[static_cast<size_t>(value.month - 1)] << '-'
           << std::setw(4) << value.year;
    return output.str();
}

BuiltinResult clockBuiltin(std::string_view name,
                           const BuiltinCall& call) {
    const auto calendar = systemContext(call)->localCalendarTime();
    if (!calendar.succeeded) {
        return failure(call, std::move(calendar.error),
                       "MParser:SystemOperationFailed");
    }
    if (name == "date") {
        return selectedOutputs(call, {
            makeRuntimeCharacterVectorUtf8(dateText(calendar.value))});
    }
    return selectedOutputs(call, {makeRuntimeVectorValue({
        static_cast<double>(calendar.value.year),
        static_cast<double>(calendar.value.month),
        static_cast<double>(calendar.value.day),
        static_cast<double>(calendar.value.hour),
        static_cast<double>(calendar.value.minute),
        calendar.value.second})});
}

std::string computerIdentifier() {
#if defined(_WIN32) && defined(_M_ARM64)
    return "PCWINARM64";
#elif defined(_WIN32)
    return "PCWIN64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "MACA64";
#elif defined(__APPLE__)
    return "MACI64";
#elif defined(__aarch64__)
    return "GLNXARM64";
#else
    return "GLNXA64";
#endif
}

struct RandomArraySpec {
    bool succeeded = false;
    std::vector<size_t> dimensions;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    std::string error;
};

std::optional<size_t> dimensionValue(const RuntimeValue& value,
                                     size_t index) {
    if (!isRuntimeNumericValue(value) || value.numericComplex) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, index);
    return element ? runtimeNumericElementAsNonnegativeSize(*element)
                   : std::nullopt;
}

RandomArraySpec randomArraySpec(
    const std::vector<RuntimeValue>& arguments, size_t begin,
    bool allowIntegerClass) {
    RandomArraySpec result;
    result.numericClass = RuntimeNumericClass::Double;
    size_t end = arguments.size();

    if (end >= begin + 2) {
        const auto like = textArgument(arguments[end - 2]);
        if (like && *like == "like") {
            const RuntimeValue& prototype = arguments[end - 1];
            if (!isRuntimeNumericValue(prototype) || prototype.numericComplex ||
                (!allowIntegerClass &&
                 !runtimeNumericClassIsFloating(prototype.numericClass)) ||
                prototype.numericClass == RuntimeNumericClass::Logical) {
                result.error =
                    "random like prototype has an unsupported numeric class";
                return result;
            }
            result.numericClass = prototype.numericClass;
            end -= 2;
        }
    }
    if (end > begin) {
        const auto className = textArgument(arguments[end - 1]);
        if (className) {
            const auto numericClass = runtimeNumericClassFromName(*className);
            if (!numericClass ||
                (!allowIntegerClass &&
                 !runtimeNumericClassIsFloating(*numericClass)) ||
                *numericClass == RuntimeNumericClass::Logical) {
                result.error = "unsupported random output class: " +
                               *className;
                return result;
            }
            result.numericClass = *numericClass;
            --end;
        }
    }

    if (begin == end) {
        result.dimensions = {1, 1};
    } else if (end - begin == 1) {
        const RuntimeValue& dimensions = arguments[begin];
        if (!isRuntimeNumericValue(dimensions) || dimensions.numericComplex) {
            result.error = "random dimensions must be real numeric values";
            return result;
        }
        const size_t count = runtimeShapeElementCount(dimensions);
        if (count == 0) {
            result.error = "random size vector cannot be empty";
            return result;
        }
        if (count == 1) {
            const auto dimension = dimensionValue(dimensions, 0);
            if (!dimension) {
                result.error =
                    "random dimension must be a nonnegative integer";
                return result;
            }
            result.dimensions = {*dimension, *dimension};
        } else {
            result.dimensions.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                const auto dimension = dimensionValue(dimensions, index);
                if (!dimension) {
                    result.error =
                        "random dimensions must be nonnegative integers";
                    return result;
                }
                result.dimensions.push_back(*dimension);
            }
        }
    } else {
        result.dimensions.reserve(end - begin);
        for (size_t index = begin; index < end; ++index) {
            if (runtimeShapeElementCount(arguments[index]) != 1) {
                result.error =
                    "each random dimension argument must be scalar";
                return result;
            }
            const auto dimension = dimensionValue(arguments[index], 0);
            if (!dimension) {
                result.error =
                    "random dimensions must be nonnegative integers";
                return result;
            }
            result.dimensions.push_back(*dimension);
        }
    }
    result.dimensions = normalizeRuntimeDimensions(
        std::move(result.dimensions));
    if (!checkedRuntimeDimensionProduct(result.dimensions)) {
        result.error = "random dimensions are too large";
        return result;
    }
    result.succeeded = true;
    return result;
}

BuiltinResult randomFloatingBuiltin(std::string_view name,
                                    const BuiltinCall& call) {
    auto spec = randomArraySpec(call.arguments, 0, false);
    if (!spec.succeeded) {
        return failure(call, std::move(spec.error));
    }
    const size_t count =
        *checkedRuntimeDimensionProduct(spec.dimensions);
    auto generated = name == "rand"
        ? systemContext(call)->randomUniform(count)
        : systemContext(call)->randomNormal(count);
    if (!generated.succeeded) {
        return failure(call, std::move(generated.error),
                       "MParser:SystemOperationFailed");
    }
    auto value = runtimeNumericValueFromLogicalOrder(
        std::move(spec.dimensions), std::move(generated.value),
        spec.numericClass);
    if (!value) {
        return failure(call, "random result could not be represented");
    }
    return selectedOutputs(call, {std::move(*value)});
}

std::optional<double> integerBound(const RuntimeValue& value,
                                   size_t index) {
    if (!isRuntimeNumericValue(value) || value.numericComplex) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElement(value, index);
    if (!element || !std::isfinite(*element) ||
        std::trunc(*element) != *element) {
        return std::nullopt;
    }
    return element;
}

BuiltinResult randiBuiltin(const BuiltinCall& call) {
    const RuntimeValue& bounds = call.arguments.front();
    const size_t boundCount = runtimeShapeElementCount(bounds);
    if (boundCount != 1 && boundCount != 2) {
        return failure(call,
                       "randi bounds must be a scalar or two-element vector");
    }
    const auto first = integerBound(bounds, 0);
    const auto second = boundCount == 1
        ? first
        : integerBound(bounds, 1);
    if (!first || !second) {
        return failure(call, "randi bounds must be finite integers");
    }
    const double minimum = boundCount == 1 ? 1.0 : *first;
    const double maximum = *second;
    if (minimum > maximum) {
        return failure(call, "randi lower bound exceeds its upper bound");
    }
    const double span = maximum - minimum + 1.0;
    if (!std::isfinite(span) || span <= 0.0 ||
        span > 9007199254740992.0) {
        return failure(call, "randi range exceeds exact integer precision");
    }

    auto spec = randomArraySpec(call.arguments, 1, true);
    if (!spec.succeeded) {
        return failure(call, std::move(spec.error));
    }
    const auto convertedMinimum = runtimeCoerceNumericElement(
        minimum, spec.numericClass);
    const auto convertedMaximum = runtimeCoerceNumericElement(
        maximum, spec.numericClass);
    if (!convertedMinimum || !convertedMaximum ||
        *convertedMinimum != minimum || *convertedMaximum != maximum) {
        return failure(call,
                       "randi bounds do not fit the requested numeric class");
    }

    const size_t count =
        *checkedRuntimeDimensionProduct(spec.dimensions);
    auto generated = systemContext(call)->randomUniform(count);
    if (!generated.succeeded) {
        return failure(call, std::move(generated.error),
                       "MParser:SystemOperationFailed");
    }
    for (double& value : generated.value) {
        value = minimum + std::floor(value * span);
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        std::move(spec.dimensions), std::move(generated.value),
        spec.numericClass);
    if (!result) {
        return failure(call, "randi result could not be represented");
    }
    return selectedOutputs(call, {std::move(*result)});
}

BuiltinResult randpermBuiltin(const BuiltinCall& call) {
    if (runtimeShapeElementCount(call.arguments[0]) != 1 ||
        (call.arguments.size() == 2 &&
         runtimeShapeElementCount(call.arguments[1]) != 1)) {
        return failure(call,
                       "randperm n and optional k must be numeric scalars");
    }
    const auto rawCount = integerBound(call.arguments[0], 0);
    const auto rawSelection = call.arguments.size() == 2
        ? integerBound(call.arguments[1], 0)
        : rawCount;
    if (!rawCount || !rawSelection || *rawCount < 0.0 ||
        *rawSelection < 0.0 || *rawSelection > *rawCount ||
        *rawCount > 9007199254740992.0 ||
        static_cast<long double>(*rawCount) >
            static_cast<long double>(
                std::numeric_limits<size_t>::max())) {
        return failure(
            call,
            "randperm expects n and optional k as exact nonnegative "
            "integers with k no greater than n");
    }
    const size_t count = static_cast<size_t>(*rawCount);
    const size_t selection = static_cast<size_t>(*rawSelection);
    if (call.context && call.context->executionControl) {
        if (!call.context->executionControl->checkpoint() ||
            selection > std::numeric_limits<size_t>::max() /
                            sizeof(double) ||
            !call.context->executionControl->observeArrayBytes(
                selection * sizeof(double))) {
            return failure(
                call,
                "randperm execution was stopped by runtime control",
                "MParser:ExecutionStopped");
        }
    }
    auto generated = systemContext(call)->randomUniform(selection);
    if (!generated.succeeded) {
        return failure(call, std::move(generated.error),
                       "MParser:SystemOperationFailed");
    }

    std::unordered_map<size_t, size_t> substitutions;
    if (selection <= substitutions.max_size() / 2U) {
        substitutions.reserve(selection * 2U);
    }
    std::vector<double> values;
    values.reserve(selection);
    const auto valueAt = [&substitutions](size_t position) {
        const auto found = substitutions.find(position);
        return found == substitutions.end() ? position : found->second;
    };
    for (size_t index = 0; index < selection; ++index) {
        if (call.context && call.context->executionControl &&
            index % 16384U == 0 &&
            !call.context->executionControl->checkpoint()) {
            return failure(
                call,
                "randperm execution was stopped by runtime control",
                "MParser:ExecutionStopped");
        }
        const size_t remaining = count - index;
        const double unit = std::clamp(
            generated.value[index], 0.0,
            std::nextafter(1.0, 0.0));
        const size_t selected = index + static_cast<size_t>(
            std::floor(unit * static_cast<double>(remaining)));
        const size_t selectedValue = valueAt(selected);
        const size_t currentValue = valueAt(index);
        substitutions[selected] = currentValue;
        values.push_back(static_cast<double>(selectedValue + 1U));
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        {1, selection}, std::move(values), RuntimeNumericClass::Double);
    if (!result) {
        return failure(call, "randperm result could not be represented");
    }
    return selectedOutputs(call, {std::move(*result)});
}

std::optional<RuntimeValue> randomStateWords(std::string_view state) {
    std::istringstream input{std::string(state)};
    std::vector<RuntimeNumericElementValue> words;
    std::uint64_t word = 0;
    while (input >> word) {
        RuntimeNumericElementValue element;
        element.numericClass = RuntimeNumericClass::UInt64;
        element.real = static_cast<double>(word);
        element.integerRealBits = word;
        words.push_back(element);
    }
    input >> std::ws;
    if (!input.eof() || words.empty()) {
        return std::nullopt;
    }
    const size_t count = words.size();
    return runtimeNumericValueFromElements(
        {count, 1}, std::move(words), RuntimeNumericClass::UInt64);
}

RuntimeValue randomStateValue(const RuntimeRandomState& state) {
    auto words = randomStateWords(state.engineState);
    if (!words) {
        return makeRuntimeStructValue();
    }
    RuntimeStructElement fields{
        {"Type", makeRuntimeCharacterVectorUtf8("twister")},
        {"Seed", makeRuntimeNumberValue(
                     static_cast<double>(state.seed),
                     RuntimeNumericClass::UInt64)},
        {"State", std::move(*words)},
        {"MParserHasSpareNormal",
         makeRuntimeLogicalValue(state.hasSpareNormal)},
        {"MParserSpareNormal",
         makeRuntimeNumberValue(state.spareNormal)},
    };
    return makeRuntimeStructArrayValue(
        {"Type", "Seed", "State", "MParserHasSpareNormal",
         "MParserSpareNormal"},
        {std::move(fields)}, {1, 1});
}

std::optional<std::uint64_t> randomSeedValue(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) || value.numericComplex ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(element->numericClass)) {
        if (element->real < 0.0 ||
            element->real > 4294967295.0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(element->real);
    }
    if (!std::isfinite(element->real) || element->real < 0.0 ||
        std::trunc(element->real) != element->real ||
        element->real > 4294967295.0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(element->real);
}

std::optional<RuntimeRandomState> parseRandomState(
    const RuntimeValue& value) {
    if (!isRuntimeScalarStruct(value)) {
        return std::nullopt;
    }
    const RuntimeValue* type = runtimeStructField(value, "Type");
    const RuntimeValue* seed = runtimeStructField(value, "Seed");
    const RuntimeValue* words = runtimeStructField(value, "State");
    if (!type || textArgument(*type) != std::optional<std::string>("twister") ||
        !seed || !words || !isRuntimeNumericValue(*words) ||
        words->numericComplex) {
        return std::nullopt;
    }
    const auto parsedSeed = randomSeedValue(*seed);
    if (!parsedSeed) {
        return std::nullopt;
    }
    std::ostringstream encoded;
    const size_t count = runtimeShapeElementCount(*words);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(*words, index);
        if (!element || !runtimeNumericClassIsInteger(element->numericClass) ||
            (runtimeNumericClassIsSignedInteger(element->numericClass) &&
             element->real < 0.0)) {
            return std::nullopt;
        }
        if (index != 0) {
            encoded << ' ';
        }
        encoded << element->integerRealBits;
    }
    RuntimeRandomState result;
    result.seed = *parsedSeed;
    result.engineState = encoded.str();
    if (const RuntimeValue* hasSpare =
            runtimeStructField(value, "MParserHasSpareNormal")) {
        const auto logical = runtimeNumericElement(*hasSpare, 0);
        if (!logical) {
            return std::nullopt;
        }
        result.hasSpareNormal = *logical != 0.0;
    }
    if (const RuntimeValue* spare =
            runtimeStructField(value, "MParserSpareNormal")) {
        const auto numeric = runtimeNumericElement(*spare, 0);
        if (!numeric || !std::isfinite(*numeric)) {
            return std::nullopt;
        }
        result.spareNormal = *numeric;
    }
    return result;
}

BuiltinResult rngBuiltin(const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    const auto previous = context->randomState();
    if (!previous.succeeded) {
        return failure(call, std::move(previous.error),
                       "MParser:SystemOperationFailed");
    }
    if (call.arguments.empty()) {
        return selectedOutputs(call, {randomStateValue(previous.value)});
    }
    if (call.arguments.size() == 2) {
        const auto algorithm = textArgument(call.arguments[1]);
        if (!algorithm || *algorithm != "twister") {
            return failure(call,
                           "rng supports only the twister algorithm name");
        }
    }

    RuntimeSystemStatus status;
    if (const auto seed = randomSeedValue(call.arguments.front())) {
        status = context->reseedRandom(*seed);
    } else if (const auto command = textArgument(call.arguments.front())) {
        if (*command == "default") {
            status = context->reseedRandom(0U);
        } else if (*command == "shuffle") {
            const auto time = context->localCalendarTime();
            if (!time.succeeded) {
                return failure(call, std::move(time.error),
                               "MParser:SystemOperationFailed");
            }
            const auto& value = time.value;
            const std::uint64_t shuffledSeed =
                static_cast<std::uint64_t>(value.year) * 10000000000ULL +
                static_cast<std::uint64_t>(value.month) * 100000000ULL +
                static_cast<std::uint64_t>(value.day) * 1000000ULL +
                static_cast<std::uint64_t>(value.hour) * 10000ULL +
                static_cast<std::uint64_t>(value.minute) * 100ULL +
                static_cast<std::uint64_t>(value.second);
            status = context->reseedRandom(shuffledSeed);
        } else {
            return failure(call,
                           "rng option must be default, shuffle, or a seed");
        }
    } else if (const auto state = parseRandomState(call.arguments.front())) {
        status = context->restoreRandomState(*state);
    } else {
        return failure(call, "rng state argument is invalid");
    }
    if (!status.succeeded) {
        return failure(call, std::move(status.error),
                       "MParser:SystemOperationFailed");
    }
    return selectedOutputs(call, {randomStateValue(previous.value)});
}

BuiltinResult identityBuiltin(std::string_view name,
                              const BuiltinCall& call) {
    if (name == "version") {
        return selectedOutputs(call, {makeRuntimeCharacterVectorUtf8(
            "MParser " MPARSER_VERSION)});
    }
    const double maximumSize = sizeof(void*) >= 8
        ? 281474976710655.0
        : 2147483647.0;
    const char endian = std::endian::native == std::endian::little
                            ? 'L'
                            : 'B';
    return selectedOutputs(call, {
        makeRuntimeCharacterVectorUtf8(computerIdentifier()),
        makeRuntimeNumberValue(maximumSize),
        makeRuntimeCharacterVectorUtf8(std::string(1, endian)),
    });
}

BuiltinResult pauseBuiltin(const BuiltinCall& call) {
    RuntimeSystemContext* context = systemContext(call);
    if (call.arguments.empty()) {
        if (!context->pauseEnabled()) {
            return BuiltinResult::success();
        }
        return failure(call,
                       "interactive pause without a duration is unavailable",
                       "MParser:InteractiveOperationUnsupported");
    }
    if (const auto command = textArgument(call.arguments.front())) {
        if (*command == "on" || *command == "off") {
            const bool previous = context->pauseEnabled();
            context->setPauseEnabled(*command == "on");
            return call.requestedOutputCount == 0
                       ? BuiltinResult::success()
                       : BuiltinResult::success({
                             makeRuntimeCharacterVectorUtf8(
                                 previous ? "on" : "off")});
        }
        if (*command == "query") {
            return selectedOutputs(call, {
                makeRuntimeCharacterVectorUtf8(
                    context->pauseEnabled() ? "on" : "off")});
        }
        return failure(call, "pause text option must be on, off, or query");
    }
    if (!isRuntimeNumericValue(call.arguments.front()) ||
        runtimeShapeElementCount(call.arguments.front()) != 1 ||
        call.arguments.front().numericComplex) {
        return failure(call,
                       "pause duration must be a real numeric scalar and has "
                       "no output");
    }
    const auto seconds = runtimeNumericElement(call.arguments.front(), 0);
    if (!seconds || !std::isfinite(*seconds) || *seconds < 0.0 ||
        *seconds > static_cast<double>(
            std::chrono::nanoseconds::max().count()) / 1.0e9) {
        return failure(call, "pause duration is outside the supported range");
    }
    if (!context->pauseEnabled()) {
        return selectedOutputs(call, {makeRuntimeNumberValue(0.0)});
    }
    const auto duration = std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::duration<double>(*seconds));
    const auto status = context->sleepFor(
        duration,
        call.context ? call.context->executionControl : nullptr);
    return status.succeeded
               ? selectedOutputs(call, {makeRuntimeNumberValue(0.0)})
               : failure(call, std::move(status.error),
                         "MParser:SystemOperationFailed");
}

BuiltinResult systemBuiltin(const BuiltinCall& call) {
    const auto command = textArgument(call.arguments.front());
    if (!command) {
        return failure(call, "system command must be a text scalar");
    }
    const auto result = systemContext(call)->executeProcess(*command);
    if (!result.succeeded) {
        return failure(call, std::move(result.error),
                       "MParser:SystemOperationFailed");
    }
    if (call.requestedOutputCount == 0) {
        return emit(call, RuntimeOutputKind::StandardOutput,
                    result.value.output);
    }
    return selectedOutputs(call, {
        makeRuntimeNumberValue(static_cast<double>(result.value.status)),
        makeRuntimeCharacterVectorUtf8(result.value.output),
    });
}

} // namespace

bool isRuntimeSystemBuiltin(std::string_view name) {
    static constexpr std::array<std::string_view, 54> names = {
        "addpath", "assignin", "cd", "clear", "clock", "computer",
        "copyfile", "date", "dir", "eval", "evalc", "evalin", "exist",
        "fclose", "feof", "ferror", "fgetl", "fgets", "fileparts",
        "fileread", "filesep", "fopen", "format", "fprintf", "fread",
        "frewind", "fscanf", "fseek", "ftell", "fullfile", "fwrite",
        "getenv", "isfile", "isfolder", "mkdir", "movefile", "path",
        "pathsep", "pause", "pwd", "rand", "randi", "randn",
        "randperm", "rmdir", "rmpath", "rng", "system", "tempdir",
        "tempname", "version", "which", "who", "whos"};
    return std::find(names.begin(), names.end(), name) != names.end();
}

BuiltinResult invokeRuntimeSystemBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "clear") {
        return clearBuiltin(call);
    }
    if (name == "assignin") {
        return assigninBuiltin(call);
    }
    if (name == "eval" || name == "evalc" || name == "evalin") {
        return dynamicEvaluationBuiltin(name, call);
    }
    if (name == "exist") {
        return existBuiltin(call);
    }
    if (name == "format") {
        return formatBuiltin(call);
    }
    if (name == "fullfile") {
        return fullfileBuiltin(call);
    }
    if (name == "fileparts") {
        return filepartsBuiltin(call);
    }
    if (name == "isfile" || name == "isfolder") {
        return pathExistenceBuiltin(name, call);
    }
    if (name == "fileread") {
        return filereadBuiltin(call);
    }
    if (name == "tempname") {
        return tempnameBuiltin(call);
    }
    if (name == "mkdir") {
        return mkdirBuiltin(call);
    }
    if (name == "rmdir") {
        return rmdirBuiltin(call);
    }
    if (name == "copyfile" || name == "movefile") {
        return copyMoveBuiltin(name, call);
    }
    if (name == "filesep" || name == "pathsep") {
        return separatorBuiltin(name, call);
    }
    if (name == "fopen") {
        return fopenBuiltin(call);
    }
    if (name == "fclose") {
        return fcloseBuiltin(call);
    }
    if (name == "feof") {
        return feofBuiltin(call);
    }
    if (name == "ferror") {
        return ferrorBuiltin(call);
    }
    if (name == "fgetl" || name == "fgets") {
        return fileLineBuiltin(name, call);
    }
    if (name == "fseek") {
        return fseekBuiltin(call);
    }
    if (name == "ftell") {
        return ftellBuiltin(call);
    }
    if (name == "frewind") {
        return frewindBuiltin(call);
    }
    if (name == "fscanf") {
        return fscanfBuiltin(call);
    }
    if (name == "fread") {
        return freadBuiltin(call);
    }
    if (name == "fprintf") {
        return fprintfBuiltin(call);
    }
    if (name == "fwrite") {
        return fwriteBuiltin(call);
    }
    if (name == "who" || name == "whos") {
        return workspaceQuery(name, call);
    }
    if (name == "path" || name == "addpath" || name == "rmpath") {
        return pathBuiltin(name, call);
    }
    if (name == "pwd" || name == "cd" || name == "tempdir") {
        return directoryBuiltin(name, call);
    }
    if (name == "getenv") {
        return environmentBuiltin(call);
    }
    if (name == "which") {
        return whichBuiltin(call);
    }
    if (name == "dir") {
        return dirBuiltin(call);
    }
    if (name == "date" || name == "clock") {
        return clockBuiltin(name, call);
    }
    if (name == "computer" || name == "version") {
        return identityBuiltin(name, call);
    }
    if (name == "pause") {
        return pauseBuiltin(call);
    }
    if (name == "rand" || name == "randn") {
        return randomFloatingBuiltin(name, call);
    }
    if (name == "randi") {
        return randiBuiltin(call);
    }
    if (name == "randperm") {
        return randpermBuiltin(call);
    }
    if (name == "rng") {
        return rngBuiltin(call);
    }
    if (name == "system") {
        return systemBuiltin(call);
    }
    return failure(call, "unsupported runtime system builtin: " +
                             std::string(name),
                   "MParser:UnsupportedBuiltin");
}

} // namespace mparser
