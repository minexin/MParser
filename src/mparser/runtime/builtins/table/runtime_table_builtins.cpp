#include "mparser/runtime/builtins/table/runtime_table_builtins.h"

#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

namespace mparser {
namespace {

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinResult failure(const BuiltinCall& call, std::string message) {
    return BuiltinResult::failure(
        call.span, std::move(message), "MParser:InvalidTableCall");
}

BuiltinResult oneOutput(const BuiltinCall& call,
                        RuntimeTableOperationResult result) {
    if (!result.succeeded) {
        return failure(call, std::move(result.error));
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(result.value)});
}

BuiltinResult scalarOutput(const BuiltinCall& call, RuntimeValue value) {
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(value)});
}

RuntimeValue oneBasedRange(size_t count) {
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        values.push_back(static_cast<double>(index + 1));
    }
    return makeRuntimeVectorValue(std::move(values));
}

struct TableOptions {
    std::vector<std::string> variableNames;
    std::vector<std::string> rowNames;
    std::vector<std::string> dimensionNames;
    bool hasVariableNames = false;
    bool hasRowNames = false;
    bool hasDimensionNames = false;
};

bool isOptionName(std::string_view name) {
    return name == "VariableNames" || name == "RowNames" ||
           name == "DimensionNames";
}

bool storeOption(TableOptions& options, std::string_view name,
                 const RuntimeValue& value, std::string& error) {
    auto names = runtimeTableNames(value, name);
    if (!names.succeeded) {
        error = std::move(names.error);
        return false;
    }
    if (name == "VariableNames") {
        if (options.hasVariableNames) {
            error = "VariableNames was supplied more than once";
            return false;
        }
        options.hasVariableNames = true;
        options.variableNames = std::move(names.names);
        return true;
    }
    if (name == "RowNames") {
        if (options.hasRowNames) {
            error = "RowNames was supplied more than once";
            return false;
        }
        options.hasRowNames = true;
        options.rowNames = std::move(names.names);
        return true;
    }
    if (name == "DimensionNames") {
        if (options.hasDimensionNames) {
            error = "DimensionNames was supplied more than once";
            return false;
        }
        options.hasDimensionNames = true;
        options.dimensionNames = std::move(names.names);
        return true;
    }
    error = "unsupported table option: " + std::string(name);
    return false;
}

bool consumeOption(const std::vector<RuntimeValue>& arguments,
                   size_t& index, TableOptions& options,
                   bool requireOption, std::string& error) {
    const RuntimeValue& argument = arguments[index];
    if (argument.kind == RuntimeValueKind::NameValueArgument) {
        if (argument.cells.size() != 1 || !isOptionName(argument.text)) {
            error = "unsupported table name-value argument: " +
                    argument.text;
            return false;
        }
        if (!storeOption(options, argument.text,
                         argument.cells.front(), error)) {
            return false;
        }
        ++index;
        return true;
    }

    const auto name = runtimeTextScalarUtf8(argument);
    if (!name || !isOptionName(*name)) {
        if (requireOption) {
            error = "table options must use a supported name-value pair";
            return false;
        }
        return false;
    }
    if (index + 1 >= arguments.size()) {
        error = "table option is missing a value: " + *name;
        return false;
    }
    if (!storeOption(options, *name, arguments[index + 1], error)) {
        return false;
    }
    index += 2;
    return true;
}

struct TableConstructorArguments {
    bool succeeded = false;
    std::vector<RuntimeValue> variables;
    TableOptions options;
    std::string error;
};

TableConstructorArguments parseTableConstructor(
    const std::vector<RuntimeValue>& arguments) {
    TableConstructorArguments result;
    size_t index = 0;
    while (index < arguments.size()) {
        std::string error;
        const size_t before = index;
        if (consumeOption(arguments, index, result.options, false, error)) {
            continue;
        }
        if (!error.empty()) {
            result.error = std::move(error);
            return result;
        }
        index = before;
        result.variables.push_back(arguments[index++]);
    }
    result.succeeded = true;
    return result;
}

struct ConversionArguments {
    bool succeeded = false;
    RuntimeValue value;
    TableOptions options;
    std::string error;
};

ConversionArguments parseConversionArguments(
    const std::vector<RuntimeValue>& arguments) {
    ConversionArguments result;
    if (arguments.empty()) {
        result.error = "table conversion requires an input value";
        return result;
    }
    result.value = arguments.front();
    size_t index = 1;
    while (index < arguments.size()) {
        std::string error;
        if (!consumeOption(arguments, index, result.options, true, error)) {
            result.error = std::move(error);
            return result;
        }
    }
    result.succeeded = true;
    return result;
}

BuiltinResult tableToArray(const BuiltinCall& call) {
    if (call.arguments.size() != 1 ||
        !isRuntimeTableValue(call.arguments.front())) {
        return failure(call, "table2array expects one table input");
    }
    const RuntimeTableStorage* storage =
        runtimeTableStorage(call.arguments.front());
    auto contents = runtimeTableContents(
        call.arguments.front(),
        {oneBasedRange(storage->rowCount),
         oneBasedRange(storage->variables.size())});
    if (!contents.succeeded) {
        return failure(call, std::move(contents.error));
    }

    RuntimeValue value;
    if (contents.values.empty()) {
        value = makeRuntimeMatrixValue(storage->rowCount, 0, {});
    } else if (contents.values.size() == 1) {
        value = std::move(contents.values.front());
    } else {
        const RuntimeObjectArrayPolicy policy =
            call.context && call.context->objectArrayPolicy
                ? *call.context->objectArrayPolicy
                : RuntimeObjectArrayPolicy{};
        auto concatenated = runtimeArrayOperationBuiltin(
            "horzcat", contents.values, policy);
        if (!concatenated.succeeded) {
            return failure(
                call, "table variables cannot form a homogeneous array: " +
                          concatenated.error);
        }
        value = std::move(concatenated.value);
    }
    return scalarOutput(call, std::move(value));
}

} // namespace

bool isRuntimeTableBuiltin(std::string_view name) {
    return matches(name, {"table", "height", "width", "istable",
                          "array2table", "table2array",
                          "struct2table", "table2struct"});
}

BuiltinResult invokeRuntimeTableBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "table") {
        auto parsed = parseTableConstructor(call.arguments);
        if (!parsed.succeeded) {
            return failure(call, std::move(parsed.error));
        }
        if (parsed.options.hasVariableNames &&
            parsed.options.variableNames.empty() &&
            !parsed.variables.empty()) {
            return failure(
                call,
                "explicit VariableNames cannot be empty for a nonempty table");
        }
        if (parsed.options.hasDimensionNames &&
            parsed.options.dimensionNames.size() != 2) {
            return failure(
                call,
                "DimensionNames must contain exactly two names");
        }
        return oneOutput(
            call, runtimeMakeTable(
                      std::move(parsed.variables),
                      std::move(parsed.options.variableNames),
                      std::move(parsed.options.rowNames),
                      std::move(parsed.options.dimensionNames)));
    }
    if (name == "height" || name == "width") {
        if (call.arguments.size() != 1) {
            return failure(call, std::string(name) + " expects one input");
        }
        const size_t dimension = name == "height" ? 0 : 1;
        return scalarOutput(
            call, makeRuntimeNumberValue(static_cast<double>(
                      runtimeDimension(call.arguments.front(), dimension))));
    }
    if (name == "istable") {
        if (call.arguments.size() != 1) {
            return failure(call, "istable expects one input");
        }
        return scalarOutput(
            call, makeRuntimeLogicalValue(
                      isRuntimeTableValue(call.arguments.front())));
    }
    if (name == "table2array") {
        return tableToArray(call);
    }
    if (name == "table2struct") {
        if (call.arguments.size() != 1) {
            return failure(call, "table2struct expects one input");
        }
        return oneOutput(call,
                         runtimeTableToStruct(call.arguments.front()));
    }
    if (name == "array2table" || name == "struct2table") {
        auto parsed = parseConversionArguments(call.arguments);
        if (!parsed.succeeded) {
            return failure(call, std::move(parsed.error));
        }
        if (parsed.options.hasDimensionNames) {
            return failure(
                call,
                std::string(name) +
                    " does not accept DimensionNames in this runtime slice");
        }
        const size_t expectedVariables =
            name == "array2table"
                ? runtimeDimension(parsed.value, 1)
                : (parsed.value.kind == RuntimeValueKind::Struct
                       ? runtimeStructFieldOrder(parsed.value).size()
                       : 0);
        if (parsed.options.hasVariableNames &&
            parsed.options.variableNames.empty() &&
            expectedVariables != 0) {
            return failure(
                call,
                "explicit VariableNames cannot be empty for a conversion with variables");
        }
        auto result = name == "array2table"
                          ? runtimeArrayToTable(
                                parsed.value,
                                std::move(parsed.options.variableNames),
                                std::move(parsed.options.rowNames))
                          : runtimeStructToTable(
                                parsed.value,
                                std::move(parsed.options.variableNames),
                                std::move(parsed.options.rowNames));
        return oneOutput(call, std::move(result));
    }
    return failure(call, "unsupported table builtin: " +
                             std::string(name));
}

} // namespace mparser
