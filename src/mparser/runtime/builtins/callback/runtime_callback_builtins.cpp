#include "mparser/runtime/builtins/callback/runtime_callback_builtins.h"

#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <iterator>
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

BuiltinResult exactOutputs(const BuiltinCall& call,
                           std::vector<RuntimeValue> outputs,
                           std::vector<Diagnostic> diagnostics = {}) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success({}, std::move(diagnostics));
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "callback builtin produced an unexpected output "
                       "count",
                       "MParser:CallbackContractViolation");
    }
    return BuiltinResult::success(std::move(outputs),
                                  std::move(diagnostics));
}

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    return runtimeShapeElementCount(value) == 1
               ? runtimeNumericTruthValue(value)
               : std::nullopt;
}

RuntimeValue cellFromLogicalOrder(std::vector<size_t> dimensions,
                                  std::vector<RuntimeValue> logical) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    std::vector<RuntimeValue> storage(logical.size());
    for (size_t index = 0; index < logical.size(); ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(
                                      *coordinates, dimensions)
                                : std::nullopt;
        if (!offset || *offset >= storage.size()) {
            return makeRuntimeCellValue({0, 0}, {});
        }
        storage[*offset] = std::move(logical[index]);
    }
    return makeRuntimeCellValue(std::move(dimensions),
                                std::move(storage));
}

std::optional<RuntimeValue> arrayfunInputElement(
    const RuntimeValue& input, size_t logicalIndex) {
    if (isRuntimeNumericValue(input)) {
        const auto element = runtimeNumericElementValue(input,
                                                        logicalIndex);
        return element
                   ? runtimeNumericValueFromElements(
                         {1, 1}, {*element}, input.numericClass)
                   : std::nullopt;
    }
    if (isRuntimeCharacterArray(input)) {
        const auto element = runtimeCharacterElement(input,
                                                     logicalIndex);
        return element
                   ? std::optional<RuntimeValue>(
                         makeRuntimeCharacterArray({1, 1},
                                                   {*element}))
                   : std::nullopt;
    }
    if (isRuntimeStringArray(input)) {
        const auto* element = runtimeStringElement(input,
                                                   logicalIndex);
        return element
                   ? std::optional<RuntimeValue>(
                         makeRuntimeStringArray({1, 1}, {*element}))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::MissingArray) {
        return logicalIndex < runtimeShapeElementCount(input)
                   ? std::optional<RuntimeValue>(
                         makeRuntimeMissingArrayValue({1, 1}))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::Cell) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            input, logicalIndex);
        return offset && *offset < input.cells.size()
                   ? std::optional<RuntimeValue>(makeRuntimeCellValue(
                         {1, 1}, {input.cells[*offset]}))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::Struct) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            input, logicalIndex);
        const auto* element = offset
                                  ? runtimeStructElement(input, *offset)
                                  : nullptr;
        return element
                   ? std::optional<RuntimeValue>(
                         makeRuntimeStructArrayValue(
                             runtimeStructFieldOrder(input), {*element},
                             {1, 1}))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::Object) {
        const auto* element = runtimeObjectLogicalElement(input,
                                                          logicalIndex);
        return element ? std::optional<RuntimeValue>(*element)
                       : std::nullopt;
    }
    if (runtimeShapeElementCount(input) == 1 && logicalIndex == 0) {
        return input;
    }
    return std::nullopt;
}

bool optionBegins(const std::vector<RuntimeValue>& arguments,
                  size_t cursor) {
    if (arguments[cursor].kind == RuntimeValueKind::NameValueArgument) {
        return true;
    }
    if (cursor + 1 >= arguments.size()) {
        return false;
    }
    const auto name = runtimeTextScalarUtf8(arguments[cursor]);
    if (!name) {
        return false;
    }
    const std::string lowered = asciiLower(*name);
    return lowered == "uniformoutput" || lowered == "errorhandler";
}

BuiltinResult arrayfunBuiltin(const BuiltinCall& call) {
    const RuntimeValue& callable = call.arguments.front();
    size_t cursor = 1;
    std::vector<const RuntimeValue*> inputs;
    while (cursor < call.arguments.size() &&
           !optionBegins(call.arguments, cursor)) {
        inputs.push_back(&call.arguments[cursor]);
        ++cursor;
    }
    if (inputs.empty()) {
        return failure(call,
                       "arrayfun requires at least one array input",
                       "MParser:InvalidArrayfunInput");
    }
    const auto dimensions = runtimeDimensions(*inputs.front());
    if (std::any_of(inputs.begin(), inputs.end(),
                    [&dimensions](const RuntimeValue* value) {
                        return runtimeDimensions(*value) != dimensions;
                    })) {
        return failure(call,
                       "arrayfun input arrays must have identical "
                       "dimensions",
                       "MParser:InvalidArrayfunShape");
    }

    bool uniformOutput = true;
    std::optional<RuntimeValue> errorHandler;
    while (cursor < call.arguments.size()) {
        std::string name;
        const RuntimeValue* value = nullptr;
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            name = call.arguments[cursor].text;
            if (call.arguments[cursor].cells.size() != 1) {
                return failure(call,
                               "arrayfun name-value argument is malformed",
                               "MParser:InvalidArrayfunOption");
            }
            value = &call.arguments[cursor].cells.front();
            ++cursor;
        } else {
            const auto rawName = runtimeTextScalarUtf8(
                call.arguments[cursor]);
            if (!rawName || cursor + 1 >= call.arguments.size()) {
                return failure(call,
                               "arrayfun options must be name-value pairs",
                               "MParser:InvalidArrayfunOption");
            }
            name = *rawName;
            value = &call.arguments[cursor + 1];
            cursor += 2;
        }
        name = asciiLower(std::move(name));
        if (name == "uniformoutput") {
            const auto raw = logicalScalar(*value);
            if (!raw) {
                return failure(call,
                               "UniformOutput must be a logical scalar",
                               "MParser:InvalidArrayfunOption");
            }
            uniformOutput = *raw;
        } else if (name == "errorhandler") {
            errorHandler = *value;
        } else {
            return failure(call, "unknown arrayfun option: " + name,
                           "MParser:InvalidArrayfunOption");
        }
    }
    if (!call.context || !call.context->dynamicInvoker) {
        return failure(call,
                       "arrayfun requires the runtime dynamic-call context",
                       "MParser:MissingBuiltinContext");
    }

    const size_t count = runtimeShapeElementCount(*inputs.front());
    std::vector<std::vector<RuntimeValue>> logicalOutputs(
        call.requestedOutputCount);
    for (auto& output : logicalOutputs) {
        output.reserve(count);
    }
    std::vector<Diagnostic> diagnostics;
    for (size_t index = 0; index < count; ++index) {
        if (call.context->executionControl &&
            (index & 255U) == 0U &&
            !call.context->executionControl->checkpoint()) {
            return failure(call,
                           "arrayfun was stopped by runtime execution "
                           "control",
                           "MParser:ExecutionStopped");
        }
        std::vector<RuntimeValue> arguments;
        arguments.reserve(inputs.size());
        for (const RuntimeValue* input : inputs) {
            const auto element = arrayfunInputElement(*input, index);
            if (!element) {
                return failure(call,
                               "arrayfun could not map an input element",
                               "MParser:InvalidArrayfunInput");
            }
            arguments.push_back(*element);
        }
        BuiltinResult invoked = call.context->dynamicInvoker(
            callable, arguments, call.requestedOutputCount, call.span);
        if (!invoked.succeeded && errorHandler) {
            std::string identifier = "MParser:ArrayfunCallbackFailed";
            std::string message = "arrayfun callback failed";
            if (!invoked.diagnostics.empty()) {
                identifier = invoked.diagnostics.front().identifier;
                message = invoked.diagnostics.front().message;
            }
            RuntimeValue errorInfo = makeRuntimeStructValue({
                {"identifier", makeRuntimeCharacterVectorUtf8(identifier)},
                {"index", makeRuntimeNumberValue(
                              static_cast<double>(index + 1))},
                {"message", makeRuntimeCharacterVectorUtf8(message)},
            });
            errorInfo.fieldOrder = {"identifier", "message", "index"};
            arguments.insert(arguments.begin(), std::move(errorInfo));
            invoked = call.context->dynamicInvoker(
                *errorHandler, arguments, call.requestedOutputCount,
                call.span);
        }
        if (!invoked.succeeded) {
            if (invoked.diagnostics.empty()) {
                return failure(call, "arrayfun callback failed",
                               "MParser:ArrayfunCallbackFailed");
            }
            return BuiltinResult{false, {},
                                 std::move(invoked.diagnostics)};
        }
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(
                               invoked.diagnostics.begin()),
                           std::make_move_iterator(
                               invoked.diagnostics.end()));
        if (invoked.outputs.size() != call.requestedOutputCount) {
            return failure(call,
                           "arrayfun callback returned an unexpected output "
                           "count",
                           "MParser:ArrayfunCallbackContract");
        }
        for (size_t output = 0; output < invoked.outputs.size(); ++output) {
            logicalOutputs[output].push_back(
                std::move(invoked.outputs[output]));
        }
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success({}, std::move(diagnostics));
    }

    std::vector<RuntimeValue> outputs;
    outputs.reserve(call.requestedOutputCount);
    for (auto& logical : logicalOutputs) {
        if (!uniformOutput) {
            outputs.push_back(cellFromLogicalOrder(dimensions,
                                                   std::move(logical)));
            continue;
        }
        if (logical.empty()) {
            auto empty = runtimeNumericValueFromLogicalOrder(
                dimensions, {}, RuntimeNumericClass::Double);
            if (!empty) {
                return failure(call,
                               "arrayfun empty output has an invalid shape",
                               "MParser:InvalidArrayfunOutput");
            }
            outputs.push_back(std::move(*empty));
            continue;
        }
        if (std::any_of(logical.begin(), logical.end(),
                        [](const RuntimeValue& value) {
                            return runtimeShapeElementCount(value) != 1;
                        })) {
            return failure(call,
                           "arrayfun UniformOutput requires scalar callback "
                           "outputs",
                           "MParser:NonScalarArrayfunOutput");
        }
        const RuntimeObjectArrayPolicy policy =
            call.context->objectArrayPolicy
                ? *call.context->objectArrayPolicy
                : RuntimeObjectArrayPolicy{};
        auto concatenated = runtimeArrayOperationBuiltin(
            "horzcat", logical, policy);
        if (!concatenated.succeeded) {
            return failure(call,
                           "arrayfun could not concatenate uniform outputs: " +
                               concatenated.error,
                           "MParser:NonUniformArrayfunOutput");
        }
        auto reshaped = runtimeReshapeValue(
            concatenated.value, dimensions, policy);
        if (!reshaped.succeeded) {
            return failure(call,
                           "arrayfun could not reshape uniform outputs: " +
                               reshaped.error,
                           "MParser:InvalidArrayfunOutput");
        }
        outputs.push_back(std::move(reshaped.value));
    }
    return exactOutputs(call, std::move(outputs),
                        std::move(diagnostics));
}

} // namespace

bool isRuntimeCallbackLibraryBuiltin(std::string_view name) {
    return name == "arrayfun";
}

BuiltinResult invokeRuntimeCallbackLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "arrayfun") {
        return arrayfunBuiltin(call);
    }
    return failure(call, "unsupported callback builtin",
                   "MParser:UnsupportedCallbackBuiltin");
}

} // namespace mparser
