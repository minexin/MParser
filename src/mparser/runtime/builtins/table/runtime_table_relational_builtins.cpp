#include "mparser/runtime/builtins/table/runtime_table_relational_builtins.h"

#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_array.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_table_relational.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <set>
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

std::string asciiLower(std::string_view value) {
    std::string result(value);
    for (char& codeUnit : result) {
        if (codeUnit >= 'A' && codeUnit <= 'Z') {
            codeUnit = static_cast<char>(codeUnit - 'A' + 'a');
        }
    }
    return result;
}

BuiltinResult failure(
    const BuiltinCall& call, std::string message,
    std::string identifier = "MParser:InvalidTableRelationalCall") {
    return BuiltinResult::failure(
        call.span, std::move(message), std::move(identifier));
}

bool executionStopped(const BuiltinCall& call) {
    return call.context && call.context->executionControl &&
           call.context->executionControl->stopReason() !=
               RuntimeExecutionStopReason::None;
}

BuiltinResult stopped(const BuiltinCall& call,
                      std::string_view operation) {
    return failure(
        call,
        std::string(operation) +
            " was stopped by runtime execution control",
        "MParser:ExecutionStopped");
}

struct NamesResult {
    bool succeeded = false;
    std::vector<std::string> names;
    std::string error;
};

NamesResult variableNames(const RuntimeValue& selector,
                          const RuntimeTableStorage& storage,
                          std::string_view role) {
    if (isRuntimeNumericValue(selector)) {
        auto resolved = runtimeResolveIndexSelection(
            selector, storage.variables.size(), false);
        if (!resolved.succeeded) {
            return NamesResult{false, {}, std::move(resolved.error)};
        }
        std::vector<std::string> names;
        names.reserve(resolved.indices.size());
        for (const size_t index : resolved.indices) {
            names.push_back(storage.variables[index].name);
        }
        return NamesResult{true, std::move(names), {}};
    }
    auto parsed = runtimeTableNames(selector, role);
    return NamesResult{parsed.succeeded, std::move(parsed.names),
                       std::move(parsed.error)};
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    if (runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericTruthValue(value);
}

struct JoinArguments {
    bool succeeded = false;
    RuntimeTableJoinOptions options;
    std::string error;
};

JoinArguments parseJoinArguments(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeTableStorage& left,
    const RuntimeTableStorage& right) {
    JoinArguments result;
    result.options.type = name == "innerjoin"
                              ? RuntimeTableJoinType::Inner
                              : RuntimeTableJoinType::Full;
    result.options.mergeKeys = name == "innerjoin";
    bool hasKeys = false;
    bool hasLeftKeys = false;
    bool hasRightKeys = false;
    bool hasLeftVariables = false;
    bool hasRightVariables = false;
    bool hasMergeKeys = false;
    bool hasType = false;

    auto storeOption = [&](std::string_view option,
                           const RuntimeValue& value) -> bool {
        const std::string normalized = asciiLower(option);
        if (normalized == "keys") {
            if (hasKeys) {
                result.error = "Keys was supplied more than once";
                return false;
            }
            auto leftNames = variableNames(value, left, "join key");
            auto rightNames = variableNames(value, right, "join key");
            if (!leftNames.succeeded || !rightNames.succeeded) {
                result.error = leftNames.succeeded
                                   ? std::move(rightNames.error)
                                   : std::move(leftNames.error);
                return false;
            }
            result.options.leftKeys = std::move(leftNames.names);
            result.options.rightKeys = std::move(rightNames.names);
            hasKeys = true;
            return true;
        }
        if (normalized == "leftkeys" ||
            normalized == "rightkeys") {
            const bool leftOption = normalized == "leftkeys";
            bool& seen = leftOption ? hasLeftKeys : hasRightKeys;
            if (seen) {
                result.error = std::string(option) +
                               " was supplied more than once";
                return false;
            }
            auto names = variableNames(
                value, leftOption ? left : right, option);
            if (!names.succeeded) {
                result.error = std::move(names.error);
                return false;
            }
            (leftOption ? result.options.leftKeys
                        : result.options.rightKeys) =
                std::move(names.names);
            seen = true;
            return true;
        }
        if (normalized == "leftvariables" ||
            normalized == "rightvariables") {
            const bool leftOption = normalized == "leftvariables";
            bool& seen = leftOption ? hasLeftVariables
                                    : hasRightVariables;
            if (seen) {
                result.error = std::string(option) +
                               " was supplied more than once";
                return false;
            }
            auto names = variableNames(
                value, leftOption ? left : right, option);
            if (!names.succeeded) {
                result.error = std::move(names.error);
                return false;
            }
            (leftOption ? result.options.leftVariables
                        : result.options.rightVariables) =
                std::move(names.names);
            if (leftOption) {
                result.options.hasLeftVariables = true;
            } else {
                result.options.hasRightVariables = true;
            }
            seen = true;
            return true;
        }
        if (normalized == "mergekeys") {
            if (name != "outerjoin") {
                result.error =
                    "MergeKeys is only supported by outerjoin";
                return false;
            }
            if (hasMergeKeys) {
                result.error =
                    "MergeKeys was supplied more than once";
                return false;
            }
            const auto enabled = logicalScalar(value);
            if (!enabled) {
                result.error =
                    "MergeKeys must be a logical or numeric scalar";
                return false;
            }
            result.options.mergeKeys = *enabled;
            hasMergeKeys = true;
            return true;
        }
        if (normalized == "type") {
            if (name != "outerjoin") {
                result.error = "Type is only supported by outerjoin";
                return false;
            }
            if (hasType) {
                result.error = "Type was supplied more than once";
                return false;
            }
            const auto typeText = runtimeTextScalarUtf8(value);
            const std::string type =
                typeText ? asciiLower(*typeText) : std::string{};
            if (!typeText ||
                (type != "left" && type != "right" &&
                 type != "full")) {
                result.error =
                    "outerjoin Type must be left, right, or full";
                return false;
            }
            result.options.type =
                type == "left" ? RuntimeTableJoinType::Left
                : type == "right" ? RuntimeTableJoinType::Right
                                  : RuntimeTableJoinType::Full;
            hasType = true;
            return true;
        }
        result.error = "unsupported join option: " +
                       std::string(option);
        return false;
    };

    size_t index = 2;
    while (index < arguments.size()) {
        const RuntimeValue& argument = arguments[index];
        if (argument.kind == RuntimeValueKind::NameValueArgument) {
            if (argument.cells.size() != 1 ||
                !storeOption(argument.text, argument.cells.front())) {
                if (result.error.empty()) {
                    result.error = "invalid join name-value argument";
                }
                return result;
            }
            ++index;
            continue;
        }
        const auto option = runtimeTextScalarUtf8(argument);
        if (!option || index + 1 >= arguments.size()) {
            result.error =
                "join options must use complete name-value pairs";
            return result;
        }
        if (!storeOption(*option, arguments[index + 1])) {
            return result;
        }
        index += 2;
    }

    if (hasKeys && (hasLeftKeys || hasRightKeys)) {
        result.error =
            "Keys cannot be combined with LeftKeys or RightKeys";
        return result;
    }
    if (hasLeftKeys != hasRightKeys) {
        result.error =
            "LeftKeys and RightKeys must be supplied together";
        return result;
    }
    if (!hasKeys && !hasLeftKeys) {
        std::set<std::string> rightNames;
        for (const auto& variable : right.variables) {
            rightNames.insert(variable.name);
        }
        for (const auto& variable : left.variables) {
            if (rightNames.contains(variable.name)) {
                result.options.leftKeys.push_back(variable.name);
                result.options.rightKeys.push_back(variable.name);
            }
        }
    }
    if (result.options.leftKeys.empty()) {
        result.error =
            "join requires Keys or at least one common variable name";
        return result;
    }
    result.succeeded = true;
    return result;
}

RuntimeValue joinIndices(const std::vector<size_t>& rows) {
    std::vector<double> values;
    values.reserve(rows.size());
    for (const size_t row : rows) {
        values.push_back(row == kRuntimeTableUnmatchedRow
                             ? 0.0
                             : static_cast<double>(row + 1));
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        {rows.size(), 1}, std::move(values),
        RuntimeNumericClass::Double);
    return result ? std::move(*result)
                  : makeRuntimeMatrixValue(0, 1, {});
}

BuiltinResult joinTables(std::string_view name,
                         const BuiltinCall& call) {
    if (call.arguments.size() < 2 ||
        call.requestedOutputCount > 3 ||
        !isRuntimeTableValue(call.arguments[0]) ||
        !isRuntimeTableValue(call.arguments[1])) {
        return failure(
            call,
            std::string(name) +
                " expects two tables, optional name-value pairs, and at most three outputs");
    }
    if (call.context && call.context->executionControl &&
        !call.context->executionControl->checkpoint()) {
        return stopped(call, name);
    }
    const RuntimeTableStorage* left =
        runtimeTableStorage(call.arguments[0]);
    const RuntimeTableStorage* right =
        runtimeTableStorage(call.arguments[1]);
    auto parsed = parseJoinArguments(
        name, call.arguments, *left, *right);
    if (!parsed.succeeded) {
        return failure(call, std::move(parsed.error));
    }
    parsed.options.executionControl =
        call.context ? call.context->executionControl : nullptr;
    auto joined = runtimeJoinTables(
        call.arguments[0], call.arguments[1], parsed.options);
    if (!joined.succeeded) {
        if (executionStopped(call)) {
            return stopped(call, name);
        }
        return failure(call, std::move(joined.error));
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    std::vector<RuntimeValue> outputs;
    outputs.push_back(std::move(joined.value));
    if (call.requestedOutputCount > 1) {
        outputs.push_back(joinIndices(joined.leftRows));
    }
    if (call.requestedOutputCount > 2) {
        outputs.push_back(joinIndices(joined.rightRows));
    }
    return BuiltinResult::success(std::move(outputs));
}

std::string uniqueName(std::string base,
                       std::set<std::string>& used) {
    if (used.insert(base).second) {
        return base;
    }
    const std::string stem = std::move(base);
    size_t suffix = 1;
    do {
        base = stem + "_" + std::to_string(suffix++);
    } while (!used.insert(base).second);
    return base;
}

struct GroupTableBase {
    bool succeeded = false;
    RuntimeTableGroupingResult grouping;
    std::vector<RuntimeValue> variables;
    std::vector<std::string> names;
    std::set<std::string> usedNames;
    std::string error;
};

GroupTableBase makeGroupTableBase(
    const RuntimeValue& table,
    const std::vector<std::string>& groupNames,
    RuntimeExecutionControl* executionControl) {
    GroupTableBase result;
    result.grouping = runtimeGroupTableRows(
        table, groupNames, executionControl);
    if (!result.grouping.succeeded) {
        result.error = result.grouping.error;
        return result;
    }
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    std::vector<size_t> representatives;
    representatives.reserve(result.grouping.groups.size());
    for (const auto& group : result.grouping.groups) {
        if (group.empty()) {
            result.error = "grouping produced an empty group";
            return result;
        }
        representatives.push_back(group.front());
    }
    for (size_t ordinal = 0;
         ordinal < result.grouping.keyVariableIndices.size(); ++ordinal) {
        const size_t index =
            result.grouping.keyVariableIndices[ordinal];
        auto selected = runtimeSelectTableVariableRows(
            storage->variables[index].value, representatives);
        if (!selected.succeeded) {
            result.error = "group variable " +
                           storage->variables[index].name + ": " +
                           selected.error;
            return result;
        }
        result.variables.push_back(std::move(selected.value));
        result.names.push_back(uniqueName(
            storage->variables[index].name, result.usedNames));
    }
    std::vector<double> counts;
    counts.reserve(result.grouping.groups.size());
    for (const auto& group : result.grouping.groups) {
        counts.push_back(static_cast<double>(group.size()));
    }
    const size_t groupCount = counts.size();
    auto countValue = runtimeNumericValueFromLogicalOrder(
        {groupCount, 1}, std::move(counts),
        RuntimeNumericClass::Double);
    if (!countValue) {
        result.error = "group counts have an invalid shape";
        return result;
    }
    result.variables.push_back(std::move(*countValue));
    result.names.push_back(uniqueName("GroupCount", result.usedNames));
    result.succeeded = true;
    return result;
}

BuiltinResult groupCounts(const BuiltinCall& call) {
    if (call.arguments.size() != 2 ||
        !isRuntimeTabularValue(call.arguments.front())) {
        return failure(
            call,
            "groupcounts expects a table or timetable and group variables");
    }
    if (call.context && call.context->executionControl &&
        !call.context->executionControl->checkpoint()) {
        return stopped(call, "groupcounts");
    }
    const RuntimeTableStorage* storage =
        runtimeTabularStorage(call.arguments.front());
    auto names = variableNames(call.arguments[1], *storage,
                               "group variable");
    if (!names.succeeded || names.names.empty()) {
        return failure(call, names.succeeded
                                 ? "group variable list must not be empty"
                                 : std::move(names.error));
    }
    auto base = makeGroupTableBase(
        call.arguments.front(), names.names,
        call.context ? call.context->executionControl : nullptr);
    if (!base.succeeded) {
        if (executionStopped(call)) {
            return stopped(call, "groupcounts");
        }
        return failure(call, std::move(base.error));
    }
    std::vector<double> percentages;
    percentages.reserve(base.grouping.groups.size());
    const double denominator = static_cast<double>(storage->rowCount);
    for (const auto& group : base.grouping.groups) {
        percentages.push_back(
            denominator == 0.0
                ? 0.0
                : 100.0 * static_cast<double>(group.size()) /
                      denominator);
    }
    const size_t groupCount = percentages.size();
    auto percentValue = runtimeNumericValueFromLogicalOrder(
        {groupCount, 1}, std::move(percentages),
        RuntimeNumericClass::Double);
    if (!percentValue) {
        return failure(call, "group percentages have an invalid shape");
    }
    base.variables.push_back(std::move(*percentValue));
    base.names.push_back(uniqueName("Percent", base.usedNames));
    auto table = runtimeMakeTable(std::move(base.variables),
                                  std::move(base.names));
    if (!table.succeeded) {
        return failure(call, std::move(table.error));
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(table.value)});
}

bool supportedSummaryMethod(std::string_view name) {
    return matches(name, {"sum", "prod", "mean", "min", "max",
                          "median", "std", "var", "any", "all"});
}

std::string nestedFailureMessage(const BuiltinResult& result,
                                 std::string_view method) {
    if (!result.diagnostics.empty()) {
        return "groupsummary " + std::string(method) +
               " failed: " + result.diagnostics.front().message;
    }
    return "groupsummary " + std::string(method) + " failed";
}

BuiltinResult groupSummary(const BuiltinCall& call) {
    if (call.arguments.size() < 2 || call.arguments.size() > 4 ||
        !isRuntimeTabularValue(call.arguments.front())) {
        return failure(
            call,
            "groupsummary expects a table/timetable, group variables, and optional methods/data variables");
    }
    if (call.context && call.context->executionControl &&
        !call.context->executionControl->checkpoint()) {
        return stopped(call, "groupsummary");
    }
    const RuntimeTableStorage* storage =
        runtimeTabularStorage(call.arguments.front());
    auto groupNames = variableNames(
        call.arguments[1], *storage, "group variable");
    if (!groupNames.succeeded || groupNames.names.empty()) {
        return failure(call, groupNames.succeeded
                                 ? "group variable list must not be empty"
                                 : std::move(groupNames.error));
    }
    auto base = makeGroupTableBase(
        call.arguments.front(), groupNames.names,
        call.context ? call.context->executionControl : nullptr);
    if (!base.succeeded) {
        if (executionStopped(call)) {
            return stopped(call, "groupsummary");
        }
        return failure(call, std::move(base.error));
    }
    if (call.arguments.size() == 2) {
        auto table = runtimeMakeTable(std::move(base.variables),
                                      std::move(base.names));
        if (!table.succeeded) {
            return failure(call, std::move(table.error));
        }
        return call.requestedOutputCount == 0
                   ? BuiltinResult::success()
                   : BuiltinResult::success({std::move(table.value)});
    }

    auto methods = runtimeTableNames(call.arguments[2],
                                     "summary method");
    if (!methods.succeeded || methods.names.empty()) {
        return failure(call, methods.succeeded
                                 ? "summary method list must not be empty"
                                 : std::move(methods.error));
    }
    for (std::string& method : methods.names) {
        method = asciiLower(method);
        if (!supportedSummaryMethod(method)) {
            return failure(call,
                           "unsupported groupsummary method: " + method);
        }
    }

    std::vector<std::string> dataNames;
    if (call.arguments.size() == 4) {
        auto parsed = variableNames(call.arguments[3], *storage,
                                    "data variable");
        if (!parsed.succeeded || parsed.names.empty()) {
            return failure(call, parsed.succeeded
                                     ? "data variable list must not be empty"
                                     : std::move(parsed.error));
        }
        dataNames = std::move(parsed.names);
    } else {
        const std::set<std::string> grouped(groupNames.names.begin(),
                                            groupNames.names.end());
        for (const auto& variable : storage->variables) {
            if (!grouped.contains(variable.name)) {
                dataNames.push_back(variable.name);
            }
        }
        if (dataNames.empty()) {
            return failure(call,
                           "groupsummary found no ungrouped data variables");
        }
    }

    const BuiltinRegistry* registry =
        call.context && call.context->registry
            ? call.context->registry
            : defaultBuiltinRegistry().get();
    for (const std::string& method : methods.names) {
        for (const std::string& dataName : dataNames) {
            const auto found = std::find_if(
                storage->variables.begin(), storage->variables.end(),
                [&](const RuntimeTableVariable& variable) {
                    return variable.name == dataName;
                });
            if (found == storage->variables.end()) {
                return failure(call,
                               "groupsummary data variable is unavailable: " +
                                   dataName);
            }
            if (!isRuntimeNumericValue(found->value)) {
                return failure(
                    call,
                    "groupsummary methods currently require numeric data variables: " +
                        dataName);
            }
            std::vector<RuntimeValue> summaries;
            summaries.reserve(base.grouping.groups.size());
            for (const auto& group : base.grouping.groups) {
                if (call.context && call.context->executionControl &&
                    !call.context->executionControl->checkpoint()) {
                    return stopped(call, "groupsummary");
                }
                auto selected = runtimeSelectTableVariableRows(
                    found->value, group);
                if (!selected.succeeded) {
                    return failure(call, "groupsummary data selection: " +
                                             selected.error);
                }
                std::vector<RuntimeValue> arguments{
                    std::move(selected.value)};
                BuiltinCall nested{arguments, 1, call.span,
                                   call.context, 1};
                auto summarized = registry->invoke(method, nested);
                if (!summarized.succeeded ||
                    summarized.outputs.size() != 1) {
                    return failure(
                        call,
                        nestedFailureMessage(summarized, method));
                }
                summaries.push_back(
                    std::move(summarized.outputs.front()));
            }
            RuntimeValue summaryValue;
            if (summaries.empty()) {
                auto selected = runtimeSelectTableVariableRows(
                    found->value, {});
                std::vector<RuntimeValue> arguments{
                    std::move(selected.value)};
                BuiltinCall nested{arguments, 1, call.span,
                                   call.context, 1};
                auto summarized = registry->invoke(method, nested);
                if (!summarized.succeeded ||
                    summarized.outputs.size() != 1) {
                    return failure(
                        call,
                        nestedFailureMessage(summarized, method));
                }
                auto empty = runtimeSelectTableVariableRows(
                    summarized.outputs.front(), {});
                if (!empty.succeeded) {
                    return failure(call,
                                   "groupsummary empty output: " +
                                       empty.error);
                }
                summaryValue = std::move(empty.value);
            } else {
                auto concatenated =
                    runtimeConcatenateValues(1, summaries);
                if (!concatenated.succeeded) {
                    return failure(
                        call,
                        "groupsummary outputs cannot concatenate: " +
                            concatenated.error);
                }
                summaryValue = std::move(concatenated.value);
            }
            base.variables.push_back(std::move(summaryValue));
            base.names.push_back(uniqueName(
                method + "_" + dataName, base.usedNames));
        }
    }

    auto table = runtimeMakeTable(std::move(base.variables),
                                  std::move(base.names));
    if (!table.succeeded) {
        return failure(call, std::move(table.error));
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(table.value)});
}

} // namespace

bool isRuntimeTableRelationalBuiltin(std::string_view name) {
    return matches(name, {"innerjoin", "outerjoin", "groupcounts",
                          "groupsummary"});
}

BuiltinResult invokeRuntimeTableRelationalBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "innerjoin" || name == "outerjoin") {
        return joinTables(name, call);
    }
    if (name == "groupcounts") {
        return groupCounts(call);
    }
    if (name == "groupsummary") {
        return groupSummary(call);
    }
    return failure(call, "unsupported relational table builtin: " +
                             std::string(name));
}

} // namespace mparser
