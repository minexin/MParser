#include "mparser/runtime/core/value/runtime_timetable.h"

#include "mparser/runtime/core/value/runtime_array.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace mparser {
namespace {

RuntimeTableOperationResult failure(std::string error) {
    return RuntimeTableOperationResult{false, {}, std::move(error)};
}

RuntimeTableOperationResult success(RuntimeValue value) {
    return RuntimeTableOperationResult{true, std::move(value), {}};
}

RuntimeValue emptyDoubleValue() {
    if (auto value = runtimeNumericValueFromLogicalOrder(
            {0, 0}, {}, RuntimeNumericClass::Double)) {
        return std::move(*value);
    }
    return makeRuntimeMatrixValue(0, 0, {});
}

bool uniqueNonemptyNames(const std::vector<std::string>& names) {
    std::set<std::string> seen;
    return std::all_of(
        names.begin(), names.end(), [&](const std::string& name) {
            return !name.empty() && seen.insert(name).second;
        });
}

std::vector<std::string> defaultVariableNames(size_t count) {
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        names.push_back("Var" + std::to_string(index + 1));
    }
    return names;
}

bool validateNameLayout(
    const std::vector<std::string>& variableNames,
    const std::vector<std::string>& dimensionNames,
    std::string& error) {
    if (!uniqueNonemptyNames(variableNames)) {
        error =
            "VariableNames must contain one unique, nonempty name per variable";
        return false;
    }
    if (dimensionNames.size() != 2 ||
        !uniqueNonemptyNames(dimensionNames)) {
        error = "DimensionNames must contain two unique, nonempty names";
        return false;
    }
    if (std::find(variableNames.begin(), variableNames.end(),
                  "Properties") != variableNames.end() ||
        std::find(dimensionNames.begin(), dimensionNames.end(),
                  "Properties") != dimensionNames.end()) {
        error = "Properties is a reserved timetable name";
        return false;
    }
    for (const std::string& name : variableNames) {
        if (std::find(dimensionNames.begin(), dimensionNames.end(), name) !=
            dimensionNames.end()) {
            error =
                "timetable variable and dimension names must not overlap: " +
                name;
            return false;
        }
    }
    return true;
}

std::optional<RuntimeValue> canonicalRowTimes(
    const RuntimeValue& value, std::string& error) {
    if (!isRuntimeTemporalValue(value)) {
        error = "RowTimes must be a datetime or duration vector";
        return std::nullopt;
    }
    const size_t count = runtimeShapeElementCount(value);
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2 ||
        (count != 0 && dimensions[0] != 1 && dimensions[1] != 1)) {
        error = "RowTimes must be a datetime or duration vector";
        return std::nullopt;
    }
    RuntimeValue result = value;
    setRuntimeDimensions(result, {count, 1});
    return result;
}

std::vector<std::string> variableNames(
    const RuntimeTabularStorage& storage) {
    std::vector<std::string> names;
    names.reserve(storage.variables.size());
    for (const auto& variable : storage.variables) {
        names.push_back(variable.name);
    }
    return names;
}

RuntimeTableOperationResult validatedTimetable(RuntimeValue value) {
    std::string error;
    if (!validateRuntimeTimetableStorage(value, error)) {
        return failure(std::move(error));
    }
    return success(std::move(value));
}

bool temporalElementsEqual(const RuntimeValue& left, size_t leftIndex,
                           const RuntimeValue& right,
                           size_t rightIndex) {
    const auto lhs = runtimeTemporalPayload(left, leftIndex);
    const auto rhs = runtimeTemporalPayload(right, rightIndex);
    return lhs && rhs &&
           ((*lhs == *rhs) || (std::isnan(*lhs) && std::isnan(*rhs)));
}

} // namespace

bool isRuntimeTimetableValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           value.className == kRuntimeTimetableClassName &&
           value.tabularStorage != nullptr &&
           value.tabularStorage->kind == RuntimeTabularKind::Timetable;
}

const RuntimeTabularStorage* runtimeTimetableStorage(
    const RuntimeValue& value) {
    return isRuntimeTimetableValue(value) ? value.tabularStorage.get()
                                          : nullptr;
}

RuntimeTabularStorage* runtimeMutableTimetableStorage(RuntimeValue& value) {
    return isRuntimeTimetableValue(value)
               ? runtimeMutableTabularStorage(value)
               : nullptr;
}

RuntimeValue makeRuntimeTimetableValue(
    std::shared_ptr<RuntimeTabularStorage> storage) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeTimetableClassName);
    result.handleObject = false;
    result.tabularStorage = std::move(storage);
    if (result.tabularStorage) {
        result.tabularStorage->kind = RuntimeTabularKind::Timetable;
        result.tabularStorage->rowAxisKind =
            RuntimeTabularRowAxisKind::Times;
        result.tabularStorage->rowNames.clear();
        if (result.tabularStorage->userData.kind ==
            RuntimeValueKind::Missing) {
            result.tabularStorage->userData = emptyDoubleValue();
        }
    }
    setRuntimeDimensions(
        result,
        result.tabularStorage
            ? std::vector<size_t>{result.tabularStorage->rowCount,
                                  result.tabularStorage->variables.size()}
            : std::vector<size_t>{0, 0});
    return result;
}

RuntimeTableOperationResult runtimeMakeTimetable(
    RuntimeValue rowTimes, std::vector<RuntimeValue> variables,
    std::vector<std::string> names,
    std::vector<std::string> dimensionNames) {
    std::string rowTimeError;
    auto canonical = canonicalRowTimes(rowTimes, rowTimeError);
    if (!canonical) {
        return failure(std::move(rowTimeError));
    }
    const size_t rowCount = runtimeShapeElementCount(*canonical);
    for (const RuntimeValue& variable : variables) {
        if (!runtimeValueIsStorable(variable) ||
            !runtimeTabularVariableSupportsRows(variable)) {
            return failure(
                "timetable variables must be storable arrays with row indexing");
        }
        if (runtimeDimension(variable, 0) != rowCount) {
            return failure(
                "all timetable variables must have the same number of rows as RowTimes");
        }
    }
    if (names.empty()) {
        names = defaultVariableNames(variables.size());
    }
    if (names.size() != variables.size()) {
        return failure(
            "VariableNames must contain one unique, nonempty name per variable");
    }
    if (dimensionNames.empty()) {
        dimensionNames = {"Time", "Variables"};
    }
    std::string nameError;
    if (!validateNameLayout(names, dimensionNames, nameError)) {
        return failure(std::move(nameError));
    }

    auto storage = std::make_shared<RuntimeTabularStorage>();
    storage->kind = RuntimeTabularKind::Timetable;
    storage->rowCount = rowCount;
    storage->rowAxisKind = RuntimeTabularRowAxisKind::Times;
    storage->rowTimes = std::move(*canonical);
    storage->dimensionNames = std::move(dimensionNames);
    storage->userData = emptyDoubleValue();
    storage->variables.reserve(variables.size());
    for (size_t index = 0; index < variables.size(); ++index) {
        storage->variables.push_back(RuntimeTabularVariable{
            std::move(names[index]), std::move(variables[index])});
    }
    return validatedTimetable(
        makeRuntimeTimetableValue(std::move(storage)));
}

RuntimeTableOperationResult runtimeArrayToTimetable(
    const RuntimeValue& value, const RuntimeValue& rowTimes,
    std::vector<std::string> names) {
    auto table = runtimeArrayToTable(value, std::move(names));
    if (!table.succeeded) {
        return failure(std::move(table.error));
    }
    return runtimeTableToTimetable(table.value, &rowTimes);
}

RuntimeTableOperationResult runtimeTableToTimetable(
    const RuntimeValue& table, const RuntimeValue* explicitRowTimes) {
    const RuntimeTabularStorage* source = runtimeTableStorage(table);
    if (!source) {
        return failure("table2timetable expects a table input");
    }
    std::vector<RuntimeValue> variables;
    std::vector<std::string> names;
    variables.reserve(source->variables.size());
    names.reserve(source->variables.size());
    RuntimeValue rowTimes;
    std::string rowDimensionName = "Time";
    size_t firstVariable = 0;
    if (explicitRowTimes) {
        rowTimes = *explicitRowTimes;
    } else {
        if (source->variables.empty() ||
            !isRuntimeTemporalValue(source->variables.front().value)) {
            return failure(
                "table2timetable requires a temporal first variable or an explicit RowTimes value");
        }
        rowTimes = source->variables.front().value;
        rowDimensionName = source->variables.front().name;
        firstVariable = 1;
    }
    for (size_t index = firstVariable;
         index < source->variables.size(); ++index) {
        names.push_back(source->variables[index].name);
        variables.push_back(source->variables[index].value);
    }
    auto result = runtimeMakeTimetable(
        std::move(rowTimes), std::move(variables), std::move(names),
        {std::move(rowDimensionName), "Variables"});
    if (!result.succeeded) {
        return result;
    }
    RuntimeTabularStorage* storage =
        runtimeMutableTimetableStorage(result.value);
    storage->description = source->description;
    storage->userData = source->userData;
    return validatedTimetable(std::move(result.value));
}

RuntimeTableOperationResult runtimeTimetableToTable(
    const RuntimeValue& timetable, bool convertRowTimes) {
    const RuntimeTabularStorage* source =
        runtimeTimetableStorage(timetable);
    if (!source) {
        return failure("timetable2table expects a timetable input");
    }
    std::vector<RuntimeValue> variables;
    std::vector<std::string> names;
    variables.reserve(source->variables.size() +
                      (convertRowTimes ? 1U : 0U));
    names.reserve(source->variables.size() +
                  (convertRowTimes ? 1U : 0U));
    if (convertRowTimes) {
        variables.push_back(source->rowTimes);
        names.push_back(source->dimensionNames.front());
    }
    for (const auto& variable : source->variables) {
        variables.push_back(variable.value);
        names.push_back(variable.name);
    }
    auto result = runtimeMakeTable(
        std::move(variables), std::move(names));
    if (!result.succeeded) {
        return result;
    }
    RuntimeTabularStorage* storage = runtimeMutableTableStorage(result.value);
    storage->description = source->description;
    storage->userData = source->userData;
    return result;
}

RuntimeTableOperationResult runtimeSetTimetableRowTimes(
    const RuntimeValue& timetable, const RuntimeValue& rowTimes) {
    const RuntimeTabularStorage* source =
        runtimeTimetableStorage(timetable);
    if (!source) {
        return failure("RowTimes assignment requires a timetable target");
    }
    std::string rowTimeError;
    auto canonical = canonicalRowTimes(rowTimes, rowTimeError);
    if (!canonical) {
        return failure(std::move(rowTimeError));
    }
    if (runtimeShapeElementCount(*canonical) != source->rowCount) {
        return failure(
            "RowTimes must contain one value per timetable row");
    }
    RuntimeValue result = timetable;
    RuntimeTabularStorage* storage =
        runtimeMutableTimetableStorage(result);
    storage->rowTimes = std::move(*canonical);
    return validatedTimetable(std::move(result));
}

RuntimeTableOperationResult runtimeConcatenateTimetables(
    size_t dimension, const std::vector<RuntimeValue>& values) {
    if ((dimension != 1 && dimension != 2) || values.empty() ||
        !std::all_of(values.begin(), values.end(),
                     isRuntimeTimetableValue)) {
        return failure(
            "timetable concatenation supports timetable inputs along dimensions 1 and 2");
    }
    const RuntimeTabularStorage* first =
        runtimeTimetableStorage(values.front());
    if (!first) {
        return failure("timetable concatenation has an invalid first input");
    }

    if (dimension == 1) {
        std::vector<RuntimeValue> rowTimeValues;
        std::vector<std::vector<RuntimeValue>> columns(
            first->variables.size());
        rowTimeValues.reserve(values.size());
        for (const RuntimeValue& value : values) {
            const auto* current = runtimeTimetableStorage(value);
            if (!current ||
                current->variables.size() != first->variables.size()) {
                return failure(
                    "vertical timetable concatenation requires matching variable counts");
            }
            rowTimeValues.push_back(current->rowTimes);
            for (size_t index = 0; index < current->variables.size();
                 ++index) {
                if (current->variables[index].name !=
                    first->variables[index].name) {
                    return failure(
                        "vertical timetable concatenation requires matching variable names and order");
                }
                columns[index].push_back(current->variables[index].value);
            }
        }
        auto rowTimes = runtimeConcatenateValues(1, rowTimeValues);
        if (!rowTimes.succeeded) {
            return failure("timetable RowTimes: " + rowTimes.error);
        }
        std::vector<RuntimeValue> variables;
        variables.reserve(columns.size());
        for (size_t index = 0; index < columns.size(); ++index) {
            auto concatenated = runtimeConcatenateValues(1, columns[index]);
            if (!concatenated.succeeded) {
                return failure(
                    "vertical timetable variable " +
                    first->variables[index].name + ": " +
                    concatenated.error);
            }
            variables.push_back(std::move(concatenated.value));
        }
        auto result = runtimeMakeTimetable(
            std::move(rowTimes.value), std::move(variables),
            variableNames(*first), first->dimensionNames);
        if (result.succeeded) {
            auto* storage = runtimeMutableTimetableStorage(result.value);
            storage->description = first->description;
            storage->userData = first->userData;
        }
        return result;
    }

    std::vector<RuntimeValue> variables;
    std::vector<std::string> names;
    std::set<std::string> usedNames;
    for (const RuntimeValue& value : values) {
        const auto* current = runtimeTimetableStorage(value);
        if (!current || current->rowCount != first->rowCount ||
            !runtimeTemporalValuesEqual(first->rowTimes,
                                        current->rowTimes, true)) {
            return failure(
                "horizontal timetable concatenation requires matching RowTimes");
        }
        for (const auto& variable : current->variables) {
            if (!usedNames.insert(variable.name).second) {
                return failure(
                    "horizontal timetable concatenation has duplicate variable name: " +
                    variable.name);
            }
            names.push_back(variable.name);
            variables.push_back(variable.value);
        }
    }
    auto result = runtimeMakeTimetable(
        first->rowTimes, std::move(variables), std::move(names),
        first->dimensionNames);
    if (result.succeeded) {
        auto* storage = runtimeMutableTimetableStorage(result.value);
        storage->description = first->description;
        storage->userData = first->userData;
    }
    return result;
}

RuntimeTimetableRowSelectionResult runtimeResolveTimetableRowSelector(
    const RuntimeValue& timetable, const RuntimeValue& selector) {
    const RuntimeTabularStorage* storage =
        runtimeTimetableStorage(timetable);
    if (!storage || !isRuntimeTemporalValue(selector) ||
        runtimeTemporalKind(storage->rowTimes) !=
            runtimeTemporalKind(selector)) {
        return RuntimeTimetableRowSelectionResult{
            false, {},
            "timetable time selector must match the RowTimes type"};
    }
    std::vector<size_t> indices;
    const size_t requestedCount = runtimeShapeElementCount(selector);
    for (size_t requested = 0; requested < requestedCount; ++requested) {
        const size_t before = indices.size();
        for (size_t row = 0; row < storage->rowCount; ++row) {
            if (temporalElementsEqual(
                    selector, requested, storage->rowTimes, row)) {
                indices.push_back(row);
            }
        }
        if (indices.size() == before) {
            return RuntimeTimetableRowSelectionResult{
                false, {}, "timetable row time is not present"};
        }
    }
    return RuntimeTimetableRowSelectionResult{
        true, std::move(indices), {}};
}

bool validateRuntimeTimetableStorage(const RuntimeValue& value,
                                     std::string& error) {
    const RuntimeTabularStorage* storage =
        runtimeTimetableStorage(value);
    if (!storage) {
        error = "timetable value has no storage";
        return false;
    }
    if (runtimeDimensions(value) !=
        std::vector<size_t>{storage->rowCount,
                            storage->variables.size()}) {
        error = "timetable shape does not match its storage";
        return false;
    }
    if (storage->rowAxisKind != RuntimeTabularRowAxisKind::Times ||
        !storage->rowNames.empty() ||
        !isRuntimeTemporalValue(storage->rowTimes) ||
        runtimeDimensions(storage->rowTimes) !=
            std::vector<size_t>{storage->rowCount, 1}) {
        error = "timetable RowTimes storage is inconsistent";
        return false;
    }
    std::vector<std::string> names;
    names.reserve(storage->variables.size());
    for (const auto& variable : storage->variables) {
        names.push_back(variable.name);
        if (!runtimeValueIsStorable(variable.value) ||
            !runtimeTabularVariableSupportsRows(variable.value) ||
            runtimeDimension(variable.value, 0) != storage->rowCount) {
            error =
                "timetable variable storage has an invalid row count or value";
            return false;
        }
    }
    if (!validateNameLayout(names, storage->dimensionNames, error)) {
        return false;
    }
    if (!runtimeValueIsStorable(storage->userData)) {
        error = "timetable UserData is not storable";
        return false;
    }
    return true;
}

bool runtimeTimetableValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableValueEquality& valueEquality) {
    const RuntimeTabularStorage* lhs = runtimeTimetableStorage(left);
    const RuntimeTabularStorage* rhs = runtimeTimetableStorage(right);
    if (!lhs || !rhs || lhs->rowCount != rhs->rowCount ||
        lhs->variables.size() != rhs->variables.size() ||
        lhs->dimensionNames != rhs->dimensionNames ||
        lhs->description != rhs->description ||
        !valueEquality(lhs->rowTimes, rhs->rowTimes)) {
        return false;
    }
    for (size_t index = 0; index < lhs->variables.size(); ++index) {
        if (lhs->variables[index].name != rhs->variables[index].name ||
            !valueEquality(lhs->variables[index].value,
                           rhs->variables[index].value)) {
            return false;
        }
    }
    return valueEquality(lhs->userData, rhs->userData);
}

} // namespace mparser
