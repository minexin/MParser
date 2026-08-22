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
                std::endian::native == std::endian::little
                    ? "ieee-le"
                    : "ieee-be"),
            makeRuntimeCharacterVectorUtf8("UTF-8"),
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
    if (call.arguments.size() == 2 && permission.empty() &&
        !textArgument(call.arguments[1])) {
        return failure(call, "fopen permission must be a text scalar");
    }
    std::string modeError;
    const auto options = fileOpenOptions(permission, modeError);
    if (!options) {
        return failure(call, std::move(modeError),
                       "MParser:InvalidFilePermission");
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
    static constexpr std::array<std::string_view, 39> names = {
        "addpath", "assignin", "cd", "clear", "clock", "computer",
        "date", "dir", "eval", "evalc", "evalin", "exist", "fclose",
        "filesep", "fopen", "format", "fprintf", "frewind", "fscanf",
        "fseek", "ftell", "fullfile", "getenv", "path", "pathsep",
        "pause", "pwd", "rand", "randi", "randn", "randperm", "rmpath",
        "rng", "system", "tempdir", "version", "which", "who", "whos"};
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
    if (name == "filesep" || name == "pathsep") {
        return separatorBuiltin(name, call);
    }
    if (name == "fopen") {
        return fopenBuiltin(call);
    }
    if (name == "fclose") {
        return fcloseBuiltin(call);
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
    if (name == "fprintf") {
        return fprintfBuiltin(call);
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
