#include "mparser/runtime/builtins/timetable/runtime_timetable_builtins.h"

#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_timetable.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinResult failure(const BuiltinCall& call, std::string message) {
    return BuiltinResult::failure(
        call.span, std::move(message),
        "MParser:InvalidTimetableCall");
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

struct TimetableOptions {
    std::optional<RuntimeValue> rowTimes;
    std::vector<std::string> variableNames;
    std::vector<std::string> dimensionNames;
    bool hasVariableNames = false;
    bool hasDimensionNames = false;
    bool hasConvertRowTimes = false;
    bool convertRowTimes = true;
};

bool logicalScalar(const RuntimeValue& value, bool& result) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return false;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex || !std::isfinite(element->real)) {
        return false;
    }
    result = element->real != 0.0;
    return true;
}

bool storeOption(TimetableOptions& options, std::string_view name,
                 const RuntimeValue& value, std::string& error) {
    if (name == "RowTimes") {
        if (options.rowTimes) {
            error = "RowTimes was supplied more than once";
            return false;
        }
        options.rowTimes = value;
        return true;
    }
    if (name == "VariableNames" || name == "DimensionNames") {
        auto names = runtimeTableNames(value, name);
        if (!names.succeeded) {
            error = std::move(names.error);
            return false;
        }
        bool& supplied = name == "VariableNames"
                             ? options.hasVariableNames
                             : options.hasDimensionNames;
        if (supplied) {
            error = std::string(name) + " was supplied more than once";
            return false;
        }
        supplied = true;
        if (name == "VariableNames") {
            options.variableNames = std::move(names.names);
        } else {
            options.dimensionNames = std::move(names.names);
        }
        return true;
    }
    if (name == "ConvertRowTimes") {
        if (options.hasConvertRowTimes) {
            error = "ConvertRowTimes was supplied more than once";
            return false;
        }
        if (!logicalScalar(value, options.convertRowTimes)) {
            error = "ConvertRowTimes must be a logical scalar";
            return false;
        }
        options.hasConvertRowTimes = true;
        return true;
    }
    error = "unsupported timetable option: " + std::string(name);
    return false;
}

bool consumeOption(const std::vector<RuntimeValue>& arguments,
                   size_t& index, TimetableOptions& options,
                   bool requireOption, std::string& error) {
    const RuntimeValue& argument = arguments[index];
    if (argument.kind == RuntimeValueKind::NameValueArgument) {
        if (argument.cells.size() != 1 ||
            !storeOption(options, argument.text,
                         argument.cells.front(), error)) {
            if (error.empty()) {
                error = "invalid timetable name-value argument";
            }
            return false;
        }
        ++index;
        return true;
    }
    const auto name = runtimeTextScalarUtf8(argument);
    if (!name) {
        if (requireOption) {
            error = "timetable options must use name-value pairs";
        }
        return false;
    }
    if (index + 1 >= arguments.size()) {
        if (requireOption) {
            error = "timetable option is missing a value: " + *name;
        }
        return false;
    }
    TimetableOptions candidate = options;
    std::string candidateError;
    if (!storeOption(candidate, *name, arguments[index + 1],
                     candidateError)) {
        if (requireOption ||
            candidateError.find("unsupported") == std::string::npos) {
            error = std::move(candidateError);
        }
        return false;
    }
    options = std::move(candidate);
    index += 2;
    return true;
}

RuntimeTableOperationResult emptyTimetable() {
    auto rowTimes = runtimeConstructNaT(
        {makeRuntimeNumberValue(0.0), makeRuntimeNumberValue(1.0)});
    return rowTimes.succeeded
               ? runtimeMakeTimetable(std::move(rowTimes.value))
               : RuntimeTableOperationResult{
                     false, {}, std::move(rowTimes.error)};
}

BuiltinResult constructTimetable(const BuiltinCall& call) {
    if (call.arguments.empty()) {
        return oneOutput(call, emptyTimetable());
    }
    TimetableOptions options;
    std::vector<RuntimeValue> positional;
    size_t index = 0;
    while (index < call.arguments.size()) {
        std::string error;
        const size_t before = index;
        if (consumeOption(call.arguments, index, options, false, error)) {
            continue;
        }
        if (!error.empty()) {
            return failure(call, std::move(error));
        }
        index = before;
        positional.push_back(call.arguments[index++]);
    }
    RuntimeValue rowTimes;
    if (options.rowTimes) {
        rowTimes = std::move(*options.rowTimes);
    } else {
        if (positional.empty() ||
            !isRuntimeTemporalValue(positional.front())) {
            return failure(
                call,
                "timetable requires datetime or duration RowTimes");
        }
        rowTimes = std::move(positional.front());
        positional.erase(positional.begin());
    }
    return oneOutput(
        call, runtimeMakeTimetable(
                  std::move(rowTimes), std::move(positional),
                  std::move(options.variableNames),
                  std::move(options.dimensionNames)));
}

BuiltinResult arrayToTimetable(const BuiltinCall& call) {
    if (call.arguments.empty()) {
        return failure(call, "array2timetable requires an input array");
    }
    TimetableOptions options;
    size_t index = 1;
    while (index < call.arguments.size()) {
        std::string error;
        if (!consumeOption(
                call.arguments, index, options, true, error)) {
            return failure(call, std::move(error));
        }
    }
    if (!options.rowTimes) {
        return failure(
            call,
            "array2timetable requires the RowTimes option");
    }
    if (options.hasDimensionNames || options.hasConvertRowTimes) {
        return failure(
            call,
            "array2timetable accepts RowTimes and VariableNames only");
    }
    return oneOutput(
        call, runtimeArrayToTimetable(
                  call.arguments.front(), *options.rowTimes,
                  std::move(options.variableNames)));
}

BuiltinResult tableToTimetable(const BuiltinCall& call) {
    if (call.arguments.empty() ||
        !isRuntimeTableValue(call.arguments.front())) {
        return failure(call, "table2timetable expects a table input");
    }
    TimetableOptions options;
    size_t index = 1;
    while (index < call.arguments.size()) {
        std::string error;
        if (!consumeOption(
                call.arguments, index, options, true, error)) {
            return failure(call, std::move(error));
        }
    }
    if (options.hasVariableNames || options.hasDimensionNames ||
        options.hasConvertRowTimes) {
        return failure(
            call, "table2timetable accepts RowTimes only");
    }
    return oneOutput(
        call, runtimeTableToTimetable(
                  call.arguments.front(),
                  options.rowTimes ? &*options.rowTimes : nullptr));
}

BuiltinResult timetableToTable(const BuiltinCall& call) {
    if (call.arguments.empty() ||
        !isRuntimeTimetableValue(call.arguments.front())) {
        return failure(call, "timetable2table expects a timetable input");
    }
    TimetableOptions options;
    size_t index = 1;
    while (index < call.arguments.size()) {
        std::string error;
        if (!consumeOption(
                call.arguments, index, options, true, error)) {
            return failure(call, std::move(error));
        }
    }
    if (options.rowTimes || options.hasVariableNames ||
        options.hasDimensionNames) {
        return failure(
            call, "timetable2table accepts ConvertRowTimes only");
    }
    return oneOutput(
        call, runtimeTimetableToTable(
                  call.arguments.front(), options.convertRowTimes));
}

} // namespace

bool isRuntimeTimetableBuiltin(std::string_view name) {
    return matches(name, {"timetable", "istimetable",
                          "array2timetable", "table2timetable",
                          "timetable2table"});
}

BuiltinResult invokeRuntimeTimetableBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "timetable") {
        return constructTimetable(call);
    }
    if (name == "istimetable") {
        if (call.arguments.size() != 1) {
            return failure(call, "istimetable expects one input");
        }
        return scalarOutput(
            call, makeRuntimeLogicalValue(
                      isRuntimeTimetableValue(call.arguments.front())));
    }
    if (name == "array2timetable") {
        return arrayToTimetable(call);
    }
    if (name == "table2timetable") {
        return tableToTimetable(call);
    }
    if (name == "timetable2table") {
        return timetableToTable(call);
    }
    return failure(call, "unsupported timetable builtin: " +
                             std::string(name));
}

} // namespace mparser
