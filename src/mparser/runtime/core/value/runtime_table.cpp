#include "mparser/runtime/core/value/runtime_table.h"

#include "mparser/runtime/core/indexing/runtime_assignment.h"
#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_array.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_cell.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_timetable.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mparser {
namespace {

RuntimeTableOperationResult failure(std::string error) {
    return RuntimeTableOperationResult{false, {}, std::move(error)};
}

RuntimeTableOperationResult success(RuntimeValue value) {
    return RuntimeTableOperationResult{true, std::move(value), {}};
}

RuntimeTableOperationResult validatedTable(RuntimeValue value) {
    std::string error;
    const bool valid = isRuntimeTableValue(value)
                           ? validateRuntimeTableStorage(value, error)
                       : isRuntimeTimetableValue(value)
                           ? validateRuntimeTimetableStorage(value, error)
                           : false;
    if (!valid) {
        if (error.empty()) {
            error = "tabular value has no recognized storage";
        }
        return failure(std::move(error));
    }
    return success(std::move(value));
}

RuntimeValue makeTabularValue(
    std::shared_ptr<RuntimeTabularStorage> storage) {
    return storage && storage->kind == RuntimeTabularKind::Timetable
               ? makeRuntimeTimetableValue(std::move(storage))
               : makeRuntimeTableValue(std::move(storage));
}

RuntimeTableContentsResult contentsFailure(std::string error) {
    return RuntimeTableContentsResult{false, {}, 0, std::move(error)};
}

RuntimeValue emptyDoubleValue() {
    if (auto value = runtimeNumericValueFromLogicalOrder(
            {0, 0}, {}, RuntimeNumericClass::Double)) {
        return std::move(*value);
    }
    RuntimeValue value;
    value.kind = RuntimeValueKind::Matrix;
    value.numericClass = RuntimeNumericClass::Double;
    setRuntimeDimensions(value, {0, 0});
    return value;
}

RuntimeValue oneBasedSelection(const std::vector<size_t>& indices) {
    std::vector<double> values;
    values.reserve(indices.size());
    for (const size_t index : indices) {
        values.push_back(static_cast<double>(index + 1));
    }
    return makeRuntimeVectorValue(std::move(values));
}

std::vector<size_t> fullSelection(size_t extent) {
    std::vector<size_t> result(extent);
    for (size_t index = 0; index < extent; ++index) {
        result[index] = index;
    }
    return result;
}

bool isFullSelection(const std::vector<size_t>& selection,
                     size_t extent) {
    if (selection.size() != extent) {
        return false;
    }
    std::vector<bool> seen(extent, false);
    for (const size_t index : selection) {
        if (index >= extent || seen[index]) {
            return false;
        }
        seen[index] = true;
    }
    return true;
}

bool uniqueNonemptyNames(const std::vector<std::string>& names) {
    std::set<std::string> seen;
    return std::all_of(
        names.begin(), names.end(), [&](const std::string& name) {
            return !name.empty() && seen.insert(name).second;
        });
}

bool validateTableNameLayout(
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
                  "Properties") != variableNames.end()) {
        error = "Properties is a reserved table variable name";
        return false;
    }
    if (std::find(dimensionNames.begin(), dimensionNames.end(),
                  "Properties") != dimensionNames.end()) {
        error = "Properties is a reserved table dimension name";
        return false;
    }
    for (const std::string& name : variableNames) {
        if (std::find(dimensionNames.begin(), dimensionNames.end(), name) !=
            dimensionNames.end()) {
            error = "table variable and dimension names must not overlap: " +
                    name;
            return false;
        }
    }
    return true;
}

std::vector<std::string> makeUniqueNames(
    const std::vector<std::string>& names,
    const std::vector<std::string>& forbidden = {}) {
    const std::set<std::string> originalNames(names.begin(), names.end());
    std::set<std::string> used(forbidden.begin(), forbidden.end());
    std::vector<std::string> result;
    result.reserve(names.size());
    for (const std::string& name : names) {
        if (used.insert(name).second) {
            result.push_back(name);
            continue;
        }
        size_t suffix = 1;
        std::string candidate;
        do {
            candidate = name + "_" + std::to_string(suffix++);
        } while (used.contains(candidate) ||
                 originalNames.contains(candidate));
        used.insert(candidate);
        result.push_back(std::move(candidate));
    }
    return result;
}

bool hasDuplicateIndices(const std::vector<size_t>& indices) {
    return std::set<size_t>(indices.begin(), indices.end()).size() !=
           indices.size();
}

std::vector<size_t> uniqueIndices(std::vector<size_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

std::vector<std::string> defaultVariableNames(size_t count) {
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        names.push_back("Var" + std::to_string(index + 1));
    }
    return names;
}

RuntimeValue textCell(const std::vector<std::string>& names,
                      std::vector<size_t> dimensions) {
    std::vector<RuntimeValue> values;
    values.reserve(names.size());
    for (const std::string& name : names) {
        values.push_back(makeRuntimeCharacterVectorUtf8(name));
    }
    return makeRuntimeCellValue(std::move(dimensions), std::move(values));
}

struct TableSelections {
    bool succeeded = false;
    std::vector<size_t> rows;
    std::vector<size_t> variables;
    std::string error;
};

struct SelectorResult {
    bool succeeded = false;
    std::vector<size_t> indices;
    std::string error;
};

SelectorResult resolveSelector(
    const RuntimeValue& selector, size_t extent,
    const std::vector<std::string>& names, std::string_view role) {
    if (isRuntimeNumericValue(selector)) {
        auto resolved = runtimeResolveIndexSelection(
            selector, extent, false);
        return SelectorResult{resolved.succeeded,
                              std::move(resolved.indices),
                              std::move(resolved.error)};
    }

    auto requested = runtimeTableNames(selector, role);
    if (!requested.succeeded) {
        return SelectorResult{false, {}, std::move(requested.error)};
    }
    if (names.empty() && !requested.names.empty()) {
        return SelectorResult{
            false, {}, std::string(role) +
                           " names are not defined for this table"};
    }

    std::vector<size_t> indices;
    indices.reserve(requested.names.size());
    for (const std::string& name : requested.names) {
        const auto found = std::find(names.begin(), names.end(), name);
        if (found == names.end()) {
            return SelectorResult{
                false, {}, std::string(role) +
                               " name is not available: " + name};
        }
        indices.push_back(static_cast<size_t>(
            std::distance(names.begin(), found)));
    }
    return SelectorResult{true, std::move(indices), {}};
}

TableSelections resolveTableSelections(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    if (!storage) {
        return TableSelections{false, {}, {},
                               "tabular indexing requires a table or timetable target"};
    }
    if (subscripts.size() != 2) {
        return TableSelections{
            false, {}, {},
            "table indexing requires exactly two subscripts"};
    }

    SelectorResult rows;
    if (isRuntimeTimetableValue(table) &&
        isRuntimeTemporalValue(subscripts[0])) {
        auto selected = runtimeResolveTimetableRowSelector(
            table, subscripts[0]);
        rows = SelectorResult{selected.succeeded,
                              std::move(selected.indices),
                              std::move(selected.error)};
    } else {
        rows = resolveSelector(
            subscripts[0], storage->rowCount, storage->rowNames, "row");
    }
    if (!rows.succeeded) {
        return TableSelections{false, {}, {}, rows.error};
    }
    std::vector<std::string> variableNames;
    variableNames.reserve(storage->variables.size());
    for (const auto& variable : storage->variables) {
        variableNames.push_back(variable.name);
    }
    const auto variables = resolveSelector(
        subscripts[1], storage->variables.size(), variableNames,
        "variable");
    if (!variables.succeeded) {
        return TableSelections{false, {}, {}, variables.error};
    }
    return TableSelections{true, rows.indices, variables.indices, {}};
}

std::vector<RuntimeValue> rowSubscripts(
    const RuntimeValue& value,
    const std::vector<size_t>& rowSelection) {
    const auto dimensions = runtimeDimensions(value);
    std::vector<RuntimeValue> subscripts;
    subscripts.reserve(dimensions.size());
    subscripts.push_back(oneBasedSelection(rowSelection));
    for (size_t index = 1; index < dimensions.size(); ++index) {
        subscripts.push_back(oneBasedSelection(
            fullSelection(dimensions[index])));
    }
    return subscripts;
}

RuntimeTableOperationResult selectRows(
    const RuntimeValue& value,
    const std::vector<size_t>& rowSelection) {
    const auto subscripts = rowSubscripts(value, rowSelection);
    if (isRuntimeTabularValue(value)) {
        const auto* storage = runtimeTabularStorage(value);
        return runtimeIndexTable(
            value,
            {oneBasedSelection(rowSelection),
             oneBasedSelection(fullSelection(
                 storage ? storage->variables.size() : 0))});
    }
    if (value.kind == RuntimeValueKind::Struct) {
        auto result = runtimeIndexStruct(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (value.kind == RuntimeValueKind::Cell) {
        auto result = runtimeIndexCell(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeTextValue(value)) {
        auto result = runtimeIndexText(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        auto result = runtimeIndexMissingArray(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeCategoricalValue(value)) {
        auto result = runtimeIndexCategorical(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeClassObject(value)) {
        auto result = runtimeIndexObject(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeNumericValue(value)) {
        auto result = runtimeIndexNumeric(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    return failure("table variables must support row indexing");
}

RuntimeTableOperationResult assignRows(
    const RuntimeValue& target,
    const std::vector<size_t>& rowSelection,
    const RuntimeValue& value) {
    const auto subscripts = rowSubscripts(target, rowSelection);
    if (isRuntimeTabularValue(target)) {
        const auto* storage = runtimeTabularStorage(target);
        return runtimeAssignTableIndexed(
            target,
            {oneBasedSelection(rowSelection),
             oneBasedSelection(fullSelection(
                 storage ? storage->variables.size() : 0))},
            value);
    }
    if (target.kind == RuntimeValueKind::Struct) {
        auto result = runtimeAssignStructIndexed(target, subscripts, value);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (target.kind == RuntimeValueKind::Cell) {
        auto result = runtimeAssignCellIndexed(target, subscripts, value);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeTextValue(target)) {
        RuntimeValue updated = target;
        auto result = runtimeAssignTextIndexed(updated, subscripts, value);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    if (target.kind == RuntimeValueKind::MissingArray) {
        RuntimeValue updated = target;
        auto result = runtimeAssignMissingIndexed(
            updated, subscripts, value);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    if (isRuntimeCategoricalValue(target)) {
        auto result = runtimeAssignCategoricalIndexed(
            target, subscripts, value);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeClassObject(target)) {
        auto result = runtimeAssignObjectIndexed(
            target, subscripts, value);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeNumericValue(target)) {
        RuntimeValue updated = target;
        auto result = runtimeAssignNumericIndexed(
            updated, subscripts, value);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    return failure("table variables must support row assignment");
}

RuntimeTableOperationResult deleteRows(
    const RuntimeValue& target,
    const std::vector<size_t>& rowSelection) {
    const auto subscripts = rowSubscripts(target, rowSelection);
    std::vector<bool> colonSubscripts(subscripts.size(), true);
    if (!colonSubscripts.empty()) {
        colonSubscripts.front() = false;
    }
    if (isRuntimeTabularValue(target)) {
        const auto* storage = runtimeTabularStorage(target);
        auto result = runtimeDeleteTableIndexed(
            target,
            {oneBasedSelection(rowSelection),
             oneBasedSelection(fullSelection(
                 storage ? storage->variables.size() : 0))},
            {false, true});
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (target.kind == RuntimeValueKind::Struct) {
        auto result = runtimeDeleteStructIndexed(target, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (target.kind == RuntimeValueKind::Cell) {
        auto result = runtimeDeleteCellIndexed(
            target, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeTextValue(target)) {
        RuntimeValue updated = target;
        auto result = runtimeDeleteTextIndexed(
            updated, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    if (target.kind == RuntimeValueKind::MissingArray) {
        RuntimeValue updated = target;
        auto result = runtimeDeleteMissingIndexed(
            updated, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    if (isRuntimeCategoricalValue(target)) {
        auto result = runtimeDeleteCategoricalIndexed(
            target, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeClassObject(target)) {
        auto result = runtimeDeleteObjectIndexed(
            target, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeNumericValue(target)) {
        RuntimeValue updated = target;
        auto result = runtimeDeleteNumericIndexed(
            updated, subscripts, colonSubscripts);
        return result.succeeded
                   ? success(std::move(updated))
                   : failure(std::move(result.error));
    }
    return failure("table variables must support row deletion");
}

RuntimeTableOperationResult selectColumns(
    const RuntimeValue& value, size_t firstColumn,
    size_t columnCount) {
    if (runtimeDimensionCount(value) != 2 ||
        firstColumn > runtimeDimension(value, 1) ||
        columnCount > runtimeDimension(value, 1) - firstColumn) {
        return failure(
            "table brace assignment requires two-dimensional column slices");
    }
    const std::vector<RuntimeValue> subscripts{
        oneBasedSelection(fullSelection(runtimeDimension(value, 0))),
        oneBasedSelection([&] {
            std::vector<size_t> columns(columnCount);
            for (size_t index = 0; index < columnCount; ++index) {
                columns[index] = firstColumn + index;
            }
            return columns;
        }())};
    if (value.kind == RuntimeValueKind::Struct) {
        auto result = runtimeIndexStruct(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (value.kind == RuntimeValueKind::Cell) {
        auto result = runtimeIndexCell(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeTextValue(value)) {
        auto result = runtimeIndexText(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        auto result = runtimeIndexMissingArray(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeCategoricalValue(value)) {
        auto result = runtimeIndexCategorical(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeClassObject(value)) {
        auto result = runtimeIndexObject(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    if (isRuntimeNumericValue(value)) {
        auto result = runtimeIndexNumeric(value, subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    return failure(
        "table brace assignment value does not support column slicing");
}

int compareMissingLast(bool leftMissing, bool rightMissing) {
    if (leftMissing == rightMissing) {
        return 0;
    }
    return leftMissing ? 1 : -1;
}

std::optional<int> compareTableVariableRows(
    const RuntimeValue& value, size_t leftRow, size_t rightRow) {
    const size_t rows = runtimeDimension(value, 0);
    if (leftRow >= rows || rightRow >= rows) {
        return std::nullopt;
    }
    const size_t trailingCount = rows == 0
                                     ? 0
                                     : runtimeShapeElementCount(value) / rows;
    for (size_t trailing = 0; trailing < trailingCount; ++trailing) {
        const size_t leftIndex = leftRow + rows * trailing;
        const size_t rightIndex = rightRow + rows * trailing;
        int comparison = 0;
        if (isRuntimeNumericValue(value)) {
            const auto left = runtimeNumericElementValue(value, leftIndex);
            const auto right = runtimeNumericElementValue(value, rightIndex);
            if (!left || !right) {
                return std::nullopt;
            }
            const bool leftMissing =
                !runtimeNumericClassIsInteger(left->numericClass) &&
                left->numericClass != RuntimeNumericClass::Logical &&
                (std::isnan(left->real) ||
                 (left->complex && std::isnan(left->imaginary)));
            const bool rightMissing =
                !runtimeNumericClassIsInteger(right->numericClass) &&
                right->numericClass != RuntimeNumericClass::Logical &&
                (std::isnan(right->real) ||
                 (right->complex && std::isnan(right->imaginary)));
            comparison = compareMissingLast(leftMissing, rightMissing);
            if (comparison == 0 && !leftMissing) {
                comparison = runtimeCompareNumericElementsForExtrema(
                    *left, *right);
            }
        } else if (isRuntimeCategoricalValue(value)) {
            const std::uint32_t left = runtimeCategoricalCode(
                value, leftIndex);
            const std::uint32_t right = runtimeCategoricalCode(
                value, rightIndex);
            comparison = compareMissingLast(left == 0, right == 0);
            if (comparison == 0 && left != 0) {
                comparison = left < right ? -1 : left > right ? 1 : 0;
            }
        } else if (isRuntimeTemporalValue(value)) {
            const auto left = runtimeTemporalPayload(value, leftIndex);
            const auto right = runtimeTemporalPayload(value, rightIndex);
            if (!left || !right) {
                return std::nullopt;
            }
            comparison = compareMissingLast(
                std::isnan(*left), std::isnan(*right));
            if (comparison == 0 && !std::isnan(*left)) {
                comparison = *left < *right ? -1 : *left > *right ? 1 : 0;
            }
        } else if (isRuntimeStringArray(value)) {
            const auto* left = runtimeStringElement(value, leftIndex);
            const auto* right = runtimeStringElement(value, rightIndex);
            if (!left || !right) {
                return std::nullopt;
            }
            comparison = compareMissingLast(left->missing, right->missing);
            if (comparison == 0 && !left->missing) {
                comparison = left->value < right->value
                                 ? -1
                                 : left->value > right->value ? 1 : 0;
            }
        } else if (isRuntimeCharacterArray(value)) {
            const auto left = runtimeCharacterElement(value, leftIndex);
            const auto right = runtimeCharacterElement(value, rightIndex);
            if (!left || !right) {
                return std::nullopt;
            }
            comparison = *left < *right ? -1 : *left > *right ? 1 : 0;
        } else if (value.kind == RuntimeValueKind::Cell) {
            const auto leftOffset = runtimeColumnMajorLinearToStorageOffset(
                value, leftIndex);
            const auto rightOffset = runtimeColumnMajorLinearToStorageOffset(
                value, rightIndex);
            const auto left = leftOffset && *leftOffset < value.cells.size()
                                  ? runtimeTextScalarUtf8(
                                        value.cells[*leftOffset])
                                  : std::nullopt;
            const auto right = rightOffset && *rightOffset < value.cells.size()
                                   ? runtimeTextScalarUtf8(
                                         value.cells[*rightOffset])
                                   : std::nullopt;
            if (!left || !right) {
                return std::nullopt;
            }
            comparison = *left < *right ? -1 : *left > *right ? 1 : 0;
        } else {
            return std::nullopt;
        }
        if (comparison != 0) {
            return comparison;
        }
    }
    return 0;
}

RuntimeTableOperationResult propertiesValue(
    const RuntimeTableStorage& storage) {
    std::vector<std::string> variableNames;
    variableNames.reserve(storage.variables.size());
    for (const auto& variable : storage.variables) {
        variableNames.push_back(variable.name);
    }

    RuntimeStructElement fields;
    fields.emplace("VariableNames",
                   textCell(variableNames,
                            {1, variableNames.size()}));
    std::vector<std::string> fieldOrder{"VariableNames"};
    if (storage.kind == RuntimeTabularKind::Timetable) {
        fields.emplace("RowTimes", storage.rowTimes);
        fieldOrder.push_back("RowTimes");
    } else {
        fields.emplace("RowNames",
                       textCell(storage.rowNames,
                                {storage.rowNames.size(), 1}));
        fieldOrder.push_back("RowNames");
    }
    fields.emplace("DimensionNames",
                   textCell(storage.dimensionNames,
                            {1, storage.dimensionNames.size()}));
    fields.emplace("Description",
                   makeRuntimeCharacterVectorUtf8(storage.description));
    fields.emplace("UserData", storage.userData);
    fieldOrder.insert(fieldOrder.end(),
                      {"DimensionNames", "Description", "UserData"});
    return success(makeRuntimeStructArrayValue(
        std::move(fieldOrder),
        {std::move(fields)}, {1, 1}));
}

RuntimeTableOperationResult applyProperties(
    const RuntimeValue& table, const RuntimeValue& value) {
    const bool timetable = isRuntimeTimetableValue(table);
    if (!isRuntimeScalarStruct(value)) {
        return failure(
            "tabular Properties assignment requires a scalar structure");
    }
    const std::set<std::string, std::less<>> allowed = timetable
        ? std::set<std::string, std::less<>>{
              "VariableNames", "RowTimes", "DimensionNames",
              "Description", "UserData"}
        : std::set<std::string, std::less<>>{
              "VariableNames", "RowNames", "DimensionNames",
              "Description", "UserData"};
    for (const std::string& field : runtimeStructFieldOrder(value)) {
        if (!allowed.contains(field)) {
            return failure("tabular property is not available: " + field);
        }
    }

    RuntimeValue result = table;
    RuntimeTableStorage* storage = runtimeMutableTabularStorage(result);
    if (!storage) {
        return failure(
            "Properties assignment requires a table or timetable");
    }
    if (const RuntimeValue* names = runtimeStructField(
            value, "VariableNames")) {
        auto parsed = runtimeTableNames(*names, "variable");
        if (!parsed.succeeded ||
            parsed.names.size() != storage->variables.size() ||
            !uniqueNonemptyNames(parsed.names)) {
            return failure(parsed.succeeded
                               ? "VariableNames must contain one unique, "
                                 "nonempty name per table variable"
                               : parsed.error);
        }
        for (size_t index = 0; index < parsed.names.size(); ++index) {
            storage->variables[index].name = parsed.names[index];
        }
    }
    if (const RuntimeValue* names = runtimeStructField(value, "RowNames")) {
        auto parsed = runtimeTableNames(*names, "row");
        if (!parsed.succeeded ||
            (!parsed.names.empty() &&
             parsed.names.size() != storage->rowCount) ||
            !uniqueNonemptyNames(parsed.names)) {
            return failure(parsed.succeeded
                               ? "RowNames must be empty or contain one "
                                 "unique, nonempty name per table row"
                               : parsed.error);
        }
        storage->rowNames = std::move(parsed.names);
        storage->rowAxisKind = storage->rowNames.empty()
                                   ? RuntimeTabularRowAxisKind::None
                                   : RuntimeTabularRowAxisKind::Names;
    }
    if (const RuntimeValue* rowTimes = runtimeStructField(
            value, "RowTimes")) {
        auto updated = runtimeSetTimetableRowTimes(result, *rowTimes);
        if (!updated.succeeded) {
            return failure(std::move(updated.error));
        }
        result = std::move(updated.value);
        storage = runtimeMutableTabularStorage(result);
    }
    if (const RuntimeValue* names = runtimeStructField(
            value, "DimensionNames")) {
        auto parsed = runtimeTableNames(*names, "dimension");
        if (!parsed.succeeded || parsed.names.size() != 2 ||
            !uniqueNonemptyNames(parsed.names)) {
            return failure(parsed.succeeded
                               ? "DimensionNames must contain two unique, "
                                 "nonempty names"
                               : parsed.error);
        }
        storage->dimensionNames = std::move(parsed.names);
    }
    if (const RuntimeValue* description = runtimeStructField(
            value, "Description")) {
        const auto text = runtimeTextScalarUtf8(*description);
        if (!text) {
            return failure(
                "tabular Description must be a character vector or string scalar");
        }
        storage->description = *text;
    }
    if (const RuntimeValue* userData = runtimeStructField(
            value, "UserData")) {
        if (!runtimeValueIsStorable(*userData)) {
            return failure("tabular UserData must be a storable value");
        }
        storage->userData = *userData;
    }
    std::vector<std::string> variableNames;
    variableNames.reserve(storage->variables.size());
    for (const auto& variable : storage->variables) {
        variableNames.push_back(variable.name);
    }
    std::string nameError;
    if (!validateTableNameLayout(
            variableNames, storage->dimensionNames, nameError)) {
        return failure(std::move(nameError));
    }
    setRuntimeDimensions(result,
                         {storage->rowCount, storage->variables.size()});
    return validatedTable(std::move(result));
}

RuntimeTableOperationResult scalarFieldColumn(
    const std::vector<RuntimeValue>& fields) {
    if (fields.empty()) {
        return success(makeRuntimeCellValue({0, 1}, {}));
    }
    const bool numericScalars = std::all_of(
        fields.begin(), fields.end(), [](const RuntimeValue& value) {
            return isRuntimeNumericValue(value) &&
                   runtimeShapeElementCount(value) == 1;
        });
    if (numericScalars) {
        const RuntimeNumericClass numericClass = fields.front().numericClass;
        const bool sameClass = std::all_of(
            fields.begin(), fields.end(), [&](const RuntimeValue& value) {
                return value.numericClass == numericClass;
            });
        if (sameClass) {
            std::vector<RuntimeNumericElementValue> elements;
            elements.reserve(fields.size());
            for (const RuntimeValue& value : fields) {
                const auto element = runtimeNumericElementValue(value, 0);
                if (!element) {
                    return failure(
                        "struct2table could not read a numeric field");
                }
                elements.push_back(*element);
            }
            if (auto result = runtimeNumericValueFromElements(
                    {fields.size(), 1}, std::move(elements), numericClass)) {
                return success(std::move(*result));
            }
        }
    }

    const bool stringScalars = std::all_of(
        fields.begin(), fields.end(), isRuntimeStringScalar);
    if (stringScalars) {
        std::vector<RuntimeStringElement> elements;
        elements.reserve(fields.size());
        for (const RuntimeValue& value : fields) {
            elements.push_back(value.stringElements.front());
        }
        return success(makeRuntimeStringArray(
            {fields.size(), 1}, std::move(elements)));
    }

    const RuntimeValue& first = fields.front();
    const auto firstDimensions = runtimeDimensions(first);
    const bool matchingRowShapes =
        firstDimensions.front() == 1 &&
        std::all_of(
            fields.begin(), fields.end(),
            [&](const RuntimeValue& value) {
                return runtimeDimensions(value) == firstDimensions;
            });
    const bool homogeneousRows = std::all_of(
        fields.begin(), fields.end(),
        [&](const RuntimeValue& value) {
            if (isRuntimeNumericValue(first)) {
                return isRuntimeNumericValue(value) &&
                       value.numericClass == first.numericClass;
            }
            if (isRuntimeTextValue(first)) {
                return isRuntimeTextValue(value);
            }
            if (first.kind == RuntimeValueKind::Cell ||
                first.kind == RuntimeValueKind::MissingArray) {
                return value.kind == first.kind;
            }
            if (isRuntimeCategoricalValue(first)) {
                return isRuntimeCategoricalValue(value);
            }
            if (isRuntimeClassObject(first)) {
                return isRuntimeClassObject(value) &&
                       value.className == first.className &&
                       value.handleObject == first.handleObject;
            }
            return false;
        });
    if (matchingRowShapes && homogeneousRows) {
        auto concatenated = runtimeConcatenateValues(1, fields);
        if (concatenated.succeeded) {
            return success(std::move(concatenated.value));
        }
        return failure(
            "struct2table could not concatenate field rows: " +
            concatenated.error);
    }

    const bool scalarFields = std::all_of(
        fields.begin(), fields.end(), [](const RuntimeValue& value) {
            return runtimeShapeElementCount(value) == 1;
        });
    if (!scalarFields) {
        return failure(
            "struct2table cannot preserve incompatible nonscalar field rows");
    }
    return success(makeRuntimeCellValue({fields.size(), 1}, fields));
}

} // namespace

bool isRuntimeTableValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           value.className == kRuntimeTableClassName &&
           value.tabularStorage != nullptr &&
           value.tabularStorage->kind == RuntimeTabularKind::Table;
}

const RuntimeTabularStorage* runtimeTableStorage(
    const RuntimeValue& value) {
    return isRuntimeTableValue(value) ? value.tabularStorage.get() : nullptr;
}

RuntimeTabularStorage* runtimeMutableTableStorage(RuntimeValue& value) {
    if (!isRuntimeTableValue(value)) {
        return nullptr;
    }
    return runtimeMutableTabularStorage(value);
}

RuntimeValue makeRuntimeTableValue(
    std::shared_ptr<RuntimeTabularStorage> storage) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeTableClassName);
    result.handleObject = false;
    result.tabularStorage = std::move(storage);
    if (result.tabularStorage) {
        result.tabularStorage->kind = RuntimeTabularKind::Table;
        result.tabularStorage->rowAxisKind =
            result.tabularStorage->rowNames.empty()
                ? RuntimeTabularRowAxisKind::None
                : RuntimeTabularRowAxisKind::Names;
    }
    if (result.tabularStorage &&
        result.tabularStorage->userData.kind == RuntimeValueKind::Missing) {
        result.tabularStorage->userData = emptyDoubleValue();
    }
    setRuntimeDimensions(
        result,
        result.tabularStorage
            ? std::vector<size_t>{result.tabularStorage->rowCount,
                                  result.tabularStorage->variables.size()}
            : std::vector<size_t>{0, 0});
    return result;
}

RuntimeTableOperationResult runtimeMakeTable(
    std::vector<RuntimeValue> variables,
    std::vector<std::string> variableNames,
    std::vector<std::string> rowNames,
    std::vector<std::string> dimensionNames) {
    const size_t rowCount = variables.empty()
                                ? rowNames.size()
                                : runtimeDimension(variables.front(), 0);
    for (const RuntimeValue& variable : variables) {
        if (!runtimeValueIsStorable(variable) ||
            !runtimeTabularVariableSupportsRows(variable)) {
            return failure(
                "table variables must be storable arrays with row indexing");
        }
        if (runtimeDimension(variable, 0) != rowCount) {
            return failure(
                "all table variables must have the same number of rows");
        }
    }
    if (variableNames.empty()) {
        variableNames = defaultVariableNames(variables.size());
    }
    if (variableNames.size() != variables.size()) {
        return failure(
            "VariableNames must contain one unique, nonempty name per variable");
    }
    if (!rowNames.empty() &&
        (rowNames.size() != rowCount ||
         !uniqueNonemptyNames(rowNames))) {
        return failure(
            "RowNames must contain one unique, nonempty name per row");
    }
    if (dimensionNames.empty()) {
        dimensionNames = {"Row", "Variables"};
    }
    std::string nameError;
    if (!validateTableNameLayout(
            variableNames, dimensionNames, nameError)) {
        return failure(std::move(nameError));
    }

    auto storage = std::make_shared<RuntimeTableStorage>();
    storage->rowCount = rowCount;
    storage->rowNames = std::move(rowNames);
    storage->dimensionNames = std::move(dimensionNames);
    storage->userData = emptyDoubleValue();
    storage->variables.reserve(variables.size());
    for (size_t index = 0; index < variables.size(); ++index) {
        storage->variables.push_back(RuntimeTableVariable{
            std::move(variableNames[index]), std::move(variables[index])});
    }
    return validatedTable(makeRuntimeTableValue(std::move(storage)));
}

RuntimeTableNamesResult runtimeTableNames(
    const RuntimeValue& value, std::string_view role) {
    if (runtimeShapeElementCount(value) == 0) {
        return RuntimeTableNamesResult{true, {}, {}};
    }
    if (const auto scalar = runtimeTextScalarUtf8(value)) {
        return RuntimeTableNamesResult{true, {*scalar}, {}};
    }

    std::vector<std::string> names;
    if (isRuntimeStringArray(value)) {
        const size_t count = runtimeShapeElementCount(value);
        names.reserve(count);
        for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
            const RuntimeStringElement* element =
                runtimeStringElement(value, logicalIndex);
            if (!element || element->missing) {
                return RuntimeTableNamesResult{
                    false, {}, std::string(role) +
                                   " names must not be missing"};
            }
            names.push_back(runtimeUtf16ToUtf8(element->value));
        }
        return RuntimeTableNamesResult{true, std::move(names), {}};
    }
    if (value.kind == RuntimeValueKind::Cell) {
        const size_t count = runtimeShapeElementCount(value);
        names.reserve(count);
        for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                value, logicalIndex);
            const auto name = offset && *offset < value.cells.size()
                                  ? runtimeTextScalarUtf8(
                                        value.cells[*offset])
                                  : std::nullopt;
            if (!name) {
                return RuntimeTableNamesResult{
                    false, {}, std::string(role) +
                                   " names must be text scalars"};
            }
            names.push_back(*name);
        }
        return RuntimeTableNamesResult{true, std::move(names), {}};
    }
    return RuntimeTableNamesResult{
        false, {}, std::string(role) +
                       " names must be text or a cell/string array of text"};
}

RuntimeTableOperationResult runtimeTableMemberValue(
    const RuntimeValue& table, std::string_view member) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    if (!storage) {
        return failure(
            "tabular member access requires a table or timetable target");
    }
    if (member == "Properties") {
        return propertiesValue(*storage);
    }
    if (storage->kind == RuntimeTabularKind::Timetable &&
        !storage->dimensionNames.empty() &&
        member == storage->dimensionNames.front()) {
        return success(storage->rowTimes);
    }
    const auto variable = std::find_if(
        storage->variables.begin(), storage->variables.end(),
        [&](const RuntimeTableVariable& candidate) {
            return candidate.name == member;
        });
    return variable == storage->variables.end()
               ? failure("table variable is not available: " +
                         std::string(member))
               : success(variable->value);
}

RuntimeTableOperationResult runtimeSetTableMember(
    const RuntimeValue& table, std::string member,
    const RuntimeValue& value, bool nullAssignment) {
    if (member == "Properties") {
        if (nullAssignment) {
            return failure("table Properties cannot be deleted");
        }
        return applyProperties(table, value);
    }
    const RuntimeTableStorage* source = runtimeTabularStorage(table);
    if (source && source->kind == RuntimeTabularKind::Timetable &&
        !source->dimensionNames.empty() &&
        member == source->dimensionNames.front()) {
        if (nullAssignment) {
            return failure("timetable RowTimes cannot be deleted");
        }
        return runtimeSetTimetableRowTimes(table, value);
    }
    if (member.empty()) {
        return failure("table variable name must not be empty");
    }
    RuntimeValue result = table;
    RuntimeTableStorage* storage = runtimeMutableTabularStorage(result);
    if (!storage) {
        return failure(
            "tabular member assignment requires a table or timetable target");
    }
    auto variable = std::find_if(
        storage->variables.begin(), storage->variables.end(),
        [&](const RuntimeTableVariable& candidate) {
            return candidate.name == member;
        });
    if (nullAssignment) {
        if (variable == storage->variables.end()) {
            return failure("table variable is not available for deletion: " +
                           member);
        }
        storage->variables.erase(variable);
        setRuntimeDimensions(
            result, {storage->rowCount, storage->variables.size()});
        return validatedTable(std::move(result));
    }
    if (!runtimeValueIsStorable(value) ||
        !runtimeTabularVariableSupportsRows(value)) {
        return failure(
            "table variables must be storable arrays with row indexing");
    }
    const size_t valueRows = runtimeDimension(value, 0);
    if (storage->kind == RuntimeTabularKind::Table &&
        storage->variables.empty() && storage->rowCount == 0 &&
        storage->rowNames.empty()) {
        storage->rowCount = valueRows;
    }
    if (valueRows != storage->rowCount) {
        return failure(
            "table variable row count does not match the table height");
    }
    if (variable == storage->variables.end()) {
        if (std::find(storage->dimensionNames.begin(),
                      storage->dimensionNames.end(), member) !=
            storage->dimensionNames.end()) {
            return failure(
                "table variable and dimension names must not overlap: " +
                member);
        }
        storage->variables.push_back(
            RuntimeTableVariable{std::move(member), value});
    } else {
        variable->value = value;
    }
    setRuntimeDimensions(
        result, {storage->rowCount, storage->variables.size()});
    return validatedTable(std::move(result));
}

RuntimeTableOperationResult runtimeIndexTable(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    const auto selections = resolveTableSelections(table, subscripts);
    if (!storage || !selections.succeeded) {
        return failure(selections.error);
    }

    auto resultStorage = std::make_shared<RuntimeTableStorage>();
    resultStorage->kind = storage->kind;
    resultStorage->rowCount = selections.rows.size();
    resultStorage->dimensionNames = storage->dimensionNames;
    resultStorage->description = storage->description;
    resultStorage->userData = storage->userData;
    if (!storage->rowNames.empty()) {
        std::vector<std::string> selectedRowNames;
        selectedRowNames.reserve(selections.rows.size());
        for (const size_t row : selections.rows) {
            selectedRowNames.push_back(storage->rowNames[row]);
        }
        resultStorage->rowNames = makeUniqueNames(selectedRowNames);
    }
    if (storage->kind == RuntimeTabularKind::Timetable) {
        auto selectedTimes = selectRows(
            storage->rowTimes, selections.rows);
        if (!selectedTimes.succeeded) {
            return failure("timetable RowTimes: " +
                           selectedTimes.error);
        }
        resultStorage->rowAxisKind = RuntimeTabularRowAxisKind::Times;
        resultStorage->rowTimes = std::move(selectedTimes.value);
    }
    std::vector<std::string> selectedVariableNames;
    selectedVariableNames.reserve(selections.variables.size());
    for (const size_t variableIndex : selections.variables) {
        selectedVariableNames.push_back(
            storage->variables[variableIndex].name);
    }
    std::vector<std::string> forbiddenNames = storage->dimensionNames;
    forbiddenNames.push_back("Properties");
    selectedVariableNames = makeUniqueNames(
        selectedVariableNames, forbiddenNames);
    resultStorage->variables.reserve(selections.variables.size());
    for (size_t index = 0; index < selections.variables.size(); ++index) {
        const size_t variableIndex = selections.variables[index];
        const auto& variable = storage->variables[variableIndex];
        auto selected = selectRows(variable.value, selections.rows);
        if (!selected.succeeded) {
            return failure("table variable " + variable.name + ": " +
                           selected.error);
        }
        resultStorage->variables.push_back(
            RuntimeTableVariable{std::move(selectedVariableNames[index]),
                                 std::move(selected.value)});
    }
    return validatedTable(
        makeTabularValue(std::move(resultStorage)));
}

RuntimeTableContentsResult runtimeTableContents(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    const auto selections = resolveTableSelections(table, subscripts);
    if (!storage || !selections.succeeded) {
        return contentsFailure(selections.error);
    }
    std::vector<RuntimeValue> values;
    values.reserve(selections.variables.size());
    for (const size_t variableIndex : selections.variables) {
        const auto& variable = storage->variables[variableIndex];
        auto selected = selectRows(variable.value, selections.rows);
        if (!selected.succeeded) {
            return contentsFailure("table variable " + variable.name +
                                   ": " + selected.error);
        }
        values.push_back(std::move(selected.value));
    }
    return RuntimeTableContentsResult{
        true, std::move(values), selections.rows.size(), {}};
}

RuntimeTableOperationResult runtimeTableContentsValue(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts) {
    auto contents = runtimeTableContents(table, subscripts);
    if (!contents.succeeded) {
        return failure(std::move(contents.error));
    }
    if (contents.values.empty()) {
        return success(makeRuntimeMatrixValue(
            contents.rowCount, 0, {}));
    }
    if (contents.values.size() == 1) {
        return success(std::move(contents.values.front()));
    }
    auto concatenated = runtimeConcatenateValues(
        2, contents.values);
    return concatenated.succeeded
               ? success(std::move(concatenated.value))
               : failure("table contents cannot concatenate: " +
                         concatenated.error);
}

RuntimeTableOperationResult runtimeAssignTableIndexed(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    const RuntimeTableStorage* source = runtimeTabularStorage(value);
    const RuntimeTableStorage* destination = runtimeTabularStorage(table);
    const auto selections = resolveTableSelections(table, subscripts);
    if (!source) {
        return failure(
            "tabular parenthesis assignment requires a table or timetable value");
    }
    if (!destination || destination->kind != source->kind) {
        return failure(
            "tabular parenthesis assignment requires matching table kinds");
    }
    if (!selections.succeeded) {
        return failure(selections.error);
    }
    if (hasDuplicateIndices(selections.variables)) {
        return failure(
            "table assignment variable subscripts must not contain duplicates");
    }
    if (source->rowCount != selections.rows.size() ||
        source->variables.size() != selections.variables.size()) {
        return failure(
            "table assignment dimensions do not match the selected rows and variables");
    }
    if (destination->kind == RuntimeTabularKind::Timetable &&
        (source->dimensionNames.empty() ||
         destination->dimensionNames.empty() ||
         source->dimensionNames.front() !=
             destination->dimensionNames.front())) {
        return failure(
            "timetable assignment requires matching row dimension names");
    }

    RuntimeValue result = table;
    RuntimeTableStorage* target = runtimeMutableTabularStorage(result);
    if (!target) {
        return failure("table assignment requires a table target");
    }
    for (size_t index = 0; index < selections.variables.size(); ++index) {
        auto assigned = assignRows(
            target->variables[selections.variables[index]].value,
            selections.rows, source->variables[index].value);
        if (!assigned.succeeded) {
            return failure("table variable assignment failed: " +
                           assigned.error);
        }
        target->variables[selections.variables[index]].value =
            std::move(assigned.value);
    }
    return validatedTable(std::move(result));
}

RuntimeTableOperationResult runtimeDeleteTableIndexed(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts) {
    const RuntimeTableStorage* source = runtimeTabularStorage(table);
    const auto selections = resolveTableSelections(table, subscripts);
    if (!source || !selections.succeeded) {
        return failure(selections.error);
    }
    if (hasDuplicateIndices(selections.variables)) {
        return failure(
            "table deletion variable subscripts must not contain duplicates");
    }
    if (colonSubscripts.size() != 2) {
        return failure("table deletion subscript metadata is invalid");
    }

    const bool rowDeletion = colonSubscripts[1] &&
        isFullSelection(selections.variables, source->variables.size());
    const bool variableDeletion = colonSubscripts[0] &&
        isFullSelection(selections.rows, source->rowCount);
    if (!rowDeletion && !variableDeletion) {
        return failure(
            "table deletion requires rows with a colon variable selector or variables with a colon row selector");
    }

    RuntimeValue result = table;
    RuntimeTableStorage* storage = runtimeMutableTabularStorage(result);
    if (!storage) {
        return failure("table null assignment requires a table target");
    }
    if (rowDeletion) {
        const auto removedRows = uniqueIndices(selections.rows);
        for (auto& variable : storage->variables) {
            auto deleted = deleteRows(variable.value, removedRows);
            if (!deleted.succeeded) {
                return failure("table variable " + variable.name +
                               ": " + deleted.error);
            }
            variable.value = std::move(deleted.value);
        }
        if (!storage->rowNames.empty()) {
            std::vector<bool> removed(storage->rowCount, false);
            for (const size_t row : removedRows) {
                removed[row] = true;
            }
            std::vector<std::string> names;
            names.reserve(storage->rowCount - removedRows.size());
            for (size_t row = 0; row < storage->rowCount; ++row) {
                if (!removed[row]) {
                    names.push_back(std::move(storage->rowNames[row]));
                }
            }
            storage->rowNames = std::move(names);
        }
        if (storage->kind == RuntimeTabularKind::Timetable) {
            auto deletedTimes = deleteRows(
                storage->rowTimes, removedRows);
            if (!deletedTimes.succeeded) {
                return failure("timetable RowTimes: " +
                               deletedTimes.error);
            }
            storage->rowTimes = std::move(deletedTimes.value);
        }
        storage->rowCount -= removedRows.size();
        storage->rowAxisKind =
            storage->kind == RuntimeTabularKind::Timetable
                ? RuntimeTabularRowAxisKind::Times
                : storage->rowNames.empty()
                      ? RuntimeTabularRowAxisKind::None
                      : RuntimeTabularRowAxisKind::Names;
        setRuntimeDimensions(
            result, {storage->rowCount, storage->variables.size()});
        return validatedTable(std::move(result));
    }

    std::vector<bool> removed(storage->variables.size(), false);
    for (const size_t index : selections.variables) {
        removed[index] = true;
    }
    std::vector<RuntimeTableVariable> kept;
    kept.reserve(storage->variables.size() - selections.variables.size());
    for (size_t index = 0; index < storage->variables.size(); ++index) {
        if (!removed[index]) {
            kept.push_back(std::move(storage->variables[index]));
        }
    }
    storage->variables = std::move(kept);
    setRuntimeDimensions(
        result, {storage->rowCount, storage->variables.size()});
    return validatedTable(std::move(result));
}

RuntimeTableOperationResult runtimeAssignTableContents(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    const auto selections = resolveTableSelections(table, subscripts);
    if (!selections.succeeded) {
        return failure(selections.error);
    }

    RuntimeValue result = table;
    RuntimeTableStorage* storage = runtimeMutableTabularStorage(result);
    if (!storage) {
        return failure("table brace assignment requires a table target");
    }
    if (hasDuplicateIndices(selections.variables)) {
        return failure(
            "table brace assignment variable subscripts must not contain duplicates");
    }
    if (selections.variables.empty()) {
        return validatedTable(std::move(result));
    }

    const bool scalarExpansion = runtimeShapeElementCount(value) == 1;
    size_t requiredColumns = 0;
    for (const size_t variableIndex : selections.variables) {
        const RuntimeValue& variable =
            storage->variables[variableIndex].value;
        if (runtimeDimensionCount(variable) != 2) {
            return failure(
                "multi-variable table brace assignment currently requires two-dimensional variables");
        }
        if (runtimeDimension(variable, 1) >
            std::numeric_limits<size_t>::max() - requiredColumns) {
            return failure(
                "table brace assignment width is too large");
        }
        requiredColumns += runtimeDimension(variable, 1);
    }
    if (!scalarExpansion &&
        (runtimeDimensionCount(value) != 2 ||
         runtimeDimension(value, 0) != selections.rows.size() ||
         runtimeDimension(value, 1) != requiredColumns)) {
        return failure(
            "table brace assignment value does not match the selected row and variable widths");
    }

    size_t firstColumn = 0;
    for (const size_t variableIndex : selections.variables) {
        const size_t columnCount = runtimeDimension(
            storage->variables[variableIndex].value, 1);
        RuntimeValue assignedValue = value;
        if (!scalarExpansion) {
            auto selected = selectColumns(
                value, firstColumn, columnCount);
            if (!selected.succeeded) {
                return failure(std::move(selected.error));
            }
            assignedValue = std::move(selected.value);
        }
        auto assigned = assignRows(
            storage->variables[variableIndex].value,
            selections.rows, assignedValue);
        if (!assigned.succeeded) {
            return failure(
                "table variable " +
                storage->variables[variableIndex].name + ": " +
                assigned.error);
        }
        storage->variables[variableIndex].value =
            std::move(assigned.value);
        firstColumn += columnCount;
    }
    return validatedTable(std::move(result));
}

RuntimeTableOperationResult runtimeArrayToTable(
    const RuntimeValue& value,
    std::vector<std::string> variableNames,
    std::vector<std::string> rowNames) {
    if (isRuntimeTableValue(value) ||
        runtimeDimensionCount(value) != 2 ||
        !runtimeValueIsStorable(value)) {
        return failure(
            "array2table expects a two-dimensional storable array");
    }
    const size_t rows = runtimeDimension(value, 0);
    const size_t columns = runtimeDimension(value, 1);
    std::vector<RuntimeValue> variables;
    variables.reserve(columns);
    const auto allRows = fullSelection(rows);
    for (size_t column = 0; column < columns; ++column) {
        const std::vector<RuntimeValue> subscripts{
            oneBasedSelection(allRows), oneBasedSelection({column})};
        RuntimeTableOperationResult selected;
        if (value.kind == RuntimeValueKind::Struct) {
            auto result = runtimeIndexStruct(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (value.kind == RuntimeValueKind::Cell) {
            auto result = runtimeIndexCell(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeTextValue(value)) {
            auto result = runtimeIndexText(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (value.kind == RuntimeValueKind::MissingArray) {
            auto result = runtimeIndexMissingArray(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeCategoricalValue(value)) {
            auto result = runtimeIndexCategorical(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeClassObject(value)) {
            auto result = runtimeIndexObject(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeNumericValue(value)) {
            auto result = runtimeIndexNumeric(value, subscripts);
            selected = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else {
            return failure("array2table does not support this array type");
        }
        if (!selected.succeeded) {
            return failure("array2table column selection failed: " +
                           selected.error);
        }
        variables.push_back(std::move(selected.value));
    }

    auto result = runtimeMakeTable(
        std::move(variables), std::move(variableNames),
        std::move(rowNames));
    if (result.succeeded && columns == 0) {
        RuntimeTableStorage* storage =
            runtimeMutableTableStorage(result.value);
        storage->rowCount = rows;
        setRuntimeDimensions(result.value, {rows, 0});
        return validatedTable(std::move(result.value));
    }
    return result;
}

RuntimeTableOperationResult runtimeStructToTable(
    const RuntimeValue& value,
    std::vector<std::string> variableNames,
    std::vector<std::string> rowNames) {
    if (value.kind != RuntimeValueKind::Struct) {
        return failure("struct2table expects a structure array");
    }
    const auto fields = runtimeStructFieldOrder(value);
    const size_t rowCount = runtimeStructElementCount(value);
    std::vector<RuntimeValue> variables;
    variables.reserve(fields.size());
    if (rowCount == 1 && !fields.empty()) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(value, 0);
        for (const std::string& field : fields) {
            const RuntimeValue* fieldValue = offset
                                                 ? runtimeStructField(
                                                       value, field, *offset)
                                                 : nullptr;
            if (!fieldValue) {
                return failure(
                    "struct2table encountered an incomplete scalar structure field");
            }
            variables.push_back(*fieldValue);
        }
        if (variableNames.empty()) {
            variableNames = fields;
        }
        return runtimeMakeTable(
            std::move(variables), std::move(variableNames),
            std::move(rowNames));
    }
    for (const std::string& field : fields) {
        std::vector<RuntimeValue> values;
        values.reserve(rowCount);
        for (size_t logicalIndex = 0; logicalIndex < rowCount;
             ++logicalIndex) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                value, logicalIndex);
            const RuntimeValue* fieldValue = offset
                                                 ? runtimeStructField(
                                                       value, field,
                                                       *offset)
                                                 : nullptr;
            if (!fieldValue) {
                return failure(
                    "struct2table encountered an incomplete structure field");
            }
            values.push_back(*fieldValue);
        }
        auto column = scalarFieldColumn(values);
        if (!column.succeeded) {
            return column;
        }
        variables.push_back(std::move(column.value));
    }
    if (variableNames.empty()) {
        variableNames = fields;
    }
    auto result = runtimeMakeTable(
        std::move(variables), std::move(variableNames),
        std::move(rowNames));
    if (result.succeeded && fields.empty()) {
        RuntimeTableStorage* storage =
            runtimeMutableTableStorage(result.value);
        storage->rowCount = rowCount;
        setRuntimeDimensions(result.value, {rowCount, 0});
        return validatedTable(std::move(result.value));
    }
    return result;
}

RuntimeTableOperationResult runtimeTableToStruct(
    const RuntimeValue& table) {
    const RuntimeTableStorage* storage = runtimeTableStorage(table);
    if (!storage) {
        return failure("table2struct expects a table");
    }
    std::vector<std::string> fieldOrder;
    fieldOrder.reserve(storage->variables.size());
    for (const auto& variable : storage->variables) {
        if (!isRuntimeStructFieldName(variable.name)) {
            return failure(
                "table2struct requires valid structure field names: " +
                variable.name);
        }
        fieldOrder.push_back(variable.name);
    }

    std::vector<RuntimeStructElement> elements(storage->rowCount);
    for (size_t row = 0; row < storage->rowCount; ++row) {
        for (const auto& variable : storage->variables) {
            auto selected = selectRows(variable.value, {row});
            if (!selected.succeeded) {
                return failure("table2struct variable " + variable.name +
                               ": " + selected.error);
            }
            elements[row].emplace(variable.name,
                                  std::move(selected.value));
        }
    }
    return success(makeRuntimeStructArrayValue(
        std::move(fieldOrder), std::move(elements),
        {storage->rowCount, 1}));
}

RuntimeTableOperationResult runtimeConcatenateTables(
    size_t dimension, const std::vector<RuntimeValue>& values) {
    if ((dimension != 1 && dimension != 2) || values.empty() ||
        !std::all_of(values.begin(), values.end(), isRuntimeTableValue)) {
        return failure(
            "table concatenation supports table inputs along dimensions 1 and 2");
    }
    const RuntimeTableStorage* first = runtimeTableStorage(values.front());
    if (!first) {
        return failure("table concatenation has an invalid first input");
    }

    auto storage = std::make_shared<RuntimeTableStorage>();
    storage->kind = RuntimeTabularKind::Table;
    storage->dimensionNames = first->dimensionNames;
    storage->description = first->description;
    storage->userData = first->userData;
    if (dimension == 1) {
        storage->variables.reserve(first->variables.size());
        std::vector<std::vector<RuntimeValue>> columns(
            first->variables.size());
        bool anyRowNames = false;
        bool allRowNames = true;
        for (const RuntimeValue& value : values) {
            const auto* current = runtimeTableStorage(value);
            if (!current ||
                current->variables.size() != first->variables.size()) {
                return failure(
                    "vertical table concatenation requires matching variable counts");
            }
            if (current->rowCount >
                std::numeric_limits<size_t>::max() - storage->rowCount) {
                return failure(
                    "vertical table concatenation height is too large");
            }
            storage->rowCount += current->rowCount;
            anyRowNames = anyRowNames || !current->rowNames.empty();
            allRowNames = allRowNames && !current->rowNames.empty();
            for (size_t index = 0; index < current->variables.size(); ++index) {
                if (current->variables[index].name !=
                    first->variables[index].name) {
                    return failure(
                        "vertical table concatenation requires matching variable names and order");
                }
                columns[index].push_back(current->variables[index].value);
            }
        }
        if (anyRowNames && !allRowNames) {
            return failure(
                "vertical table concatenation requires RowNames on every input or none");
        }
        if (allRowNames) {
            for (const RuntimeValue& value : values) {
                const auto* current = runtimeTableStorage(value);
                storage->rowNames.insert(storage->rowNames.end(),
                                         current->rowNames.begin(),
                                         current->rowNames.end());
            }
            if (!uniqueNonemptyNames(storage->rowNames)) {
                return failure(
                    "vertical table concatenation produces duplicate RowNames");
            }
        }
        for (size_t index = 0; index < columns.size(); ++index) {
            auto concatenated = runtimeConcatenateValues(1, columns[index]);
            if (!concatenated.succeeded) {
                return failure(
                    "vertical table variable " +
                    first->variables[index].name + ": " +
                    concatenated.error);
            }
            storage->variables.push_back(RuntimeTableVariable{
                first->variables[index].name,
                std::move(concatenated.value)});
        }
    } else {
        storage->rowCount = first->rowCount;
        storage->rowNames = first->rowNames;
        std::set<std::string> names;
        for (const RuntimeValue& value : values) {
            const auto* current = runtimeTableStorage(value);
            if (!current || current->rowCount != storage->rowCount) {
                return failure(
                    "horizontal table concatenation requires matching heights");
            }
            if (!storage->rowNames.empty() &&
                !current->rowNames.empty() &&
                storage->rowNames != current->rowNames) {
                return failure(
                    "horizontal table concatenation requires matching RowNames");
            }
            if (storage->rowNames.empty() && !current->rowNames.empty()) {
                storage->rowNames = current->rowNames;
            }
            for (const auto& variable : current->variables) {
                if (!names.insert(variable.name).second) {
                    return failure(
                        "horizontal table concatenation has duplicate variable name: " +
                        variable.name);
                }
                storage->variables.push_back(variable);
            }
        }
    }
    storage->rowAxisKind = storage->rowNames.empty()
                               ? RuntimeTabularRowAxisKind::None
                               : RuntimeTabularRowAxisKind::Names;
    return validatedTable(makeRuntimeTableValue(std::move(storage)));
}

RuntimeTableSortResult runtimeSortTable(
    const RuntimeValue& table,
    const std::vector<RuntimeTableSortKey>& keys) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    if (!storage) {
        return RuntimeTableSortResult{
            false, {}, {},
            "sortrows expects a table or timetable input"};
    }
    std::set<std::pair<RuntimeTableSortKeyKind, size_t>> uniqueKeys;
    for (const RuntimeTableSortKey& key : keys) {
        if (key.kind == RuntimeTableSortKeyKind::RowAxis) {
            if (storage->kind != RuntimeTabularKind::Timetable) {
                return RuntimeTableSortResult{
                    false, {}, {},
                    "sortrows row-axis keys require a timetable"};
            }
            if (!uniqueKeys.emplace(key.kind, 0).second) {
                return RuntimeTableSortResult{
                    false, {}, {},
                    "sortrows keys must not contain duplicates"};
            }
            continue;
        }
        if (key.variableIndex >= storage->variables.size()) {
            return RuntimeTableSortResult{
                false, {}, {},
                "sortrows variable index is out of bounds"};
        }
        if (!uniqueKeys.emplace(key.kind, key.variableIndex).second) {
            return RuntimeTableSortResult{
                false, {}, {},
                "sortrows keys must not contain duplicates"};
        }
        const RuntimeValue& value =
            storage->variables[key.variableIndex].value;
        const bool supported =
            isRuntimeNumericValue(value) ||
            isRuntimeCategoricalValue(value) ||
            isRuntimeTemporalValue(value) ||
            isRuntimeStringArray(value) ||
            isRuntimeCharacterArray(value) ||
            value.kind == RuntimeValueKind::Cell;
        if (!supported) {
            return RuntimeTableSortResult{
                false, {}, {},
                "sortrows does not support table variable: " +
                    storage->variables[key.variableIndex].name};
        }
        if (value.kind == RuntimeValueKind::Cell) {
            for (const RuntimeValue& element : value.cells) {
                if (!runtimeTextScalarUtf8(element)) {
                    return RuntimeTableSortResult{
                        false, {}, {},
                        "sortrows Cell variables must contain text scalars"};
                }
            }
        }
    }

    std::vector<size_t> order = fullSelection(storage->rowCount);
    bool comparisonFailed = false;
    std::stable_sort(
        order.begin(), order.end(), [&](size_t left, size_t right) {
            for (const RuntimeTableSortKey& key : keys) {
                const RuntimeValue& value =
                    key.kind == RuntimeTableSortKeyKind::RowAxis
                        ? storage->rowTimes
                        : storage->variables[key.variableIndex].value;
                const auto comparison = compareTableVariableRows(
                    value, left, right);
                if (!comparison) {
                    comparisonFailed = true;
                    return false;
                }
                if (*comparison != 0) {
                    return key.descending ? *comparison > 0
                                          : *comparison < 0;
                }
            }
            return false;
        });
    if (comparisonFailed) {
        return RuntimeTableSortResult{
            false, {}, {},
            "sortrows could not compare a table variable element"};
    }
    auto sorted = runtimeIndexTable(
        table,
        {oneBasedSelection(order),
         oneBasedSelection(fullSelection(storage->variables.size()))});
    if (!sorted.succeeded) {
        return RuntimeTableSortResult{
            false, {}, {}, std::move(sorted.error)};
    }
    return RuntimeTableSortResult{
        true, std::move(sorted.value), std::move(order), {}};
}

namespace {

struct RuntimeTablePair {
    const RuntimeTableStorage* left = nullptr;
    const RuntimeTableStorage* right = nullptr;

    bool operator==(const RuntimeTablePair&) const = default;
};

struct RuntimeTablePairHash {
    size_t operator()(const RuntimeTablePair& pair) const noexcept {
        const size_t left =
            std::hash<const void*>{}(pair.left);
        const size_t right =
            std::hash<const void*>{}(pair.right);
        return left ^ (right + static_cast<size_t>(0x9e3779b9U) +
                       (left << 6U) + (left >> 2U));
    }
};

struct RuntimeTableComparisonContext {
    std::unordered_map<RuntimeTablePair, RuntimeValue,
                       RuntimeTablePairHash>
        completed;
    std::unordered_set<RuntimeTablePair, RuntimeTablePairHash> active;
};

RuntimeTableOperationResult compareTables(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right,
    RuntimeTableComparisonContext& context) {
    const RuntimeTableStorage* lhs = runtimeTableStorage(left);
    const RuntimeTableStorage* rhs = runtimeTableStorage(right);
    if (!lhs || !rhs) {
        return failure("table comparison requires two table operands");
    }
    if (lhs->rowCount != rhs->rowCount ||
        lhs->variables.size() != rhs->variables.size() ||
        lhs->rowNames != rhs->rowNames) {
        return failure(
            "table comparison requires matching row and variable layouts");
    }

    const RuntimeTablePair pair{lhs, rhs};
    if (const auto cached = context.completed.find(pair);
        cached != context.completed.end()) {
        return success(cached->second);
    }
    if (!context.active.insert(pair).second) {
        return failure("cyclic table comparison is not supported");
    }
    const auto activeFailure = [&](std::string error) {
        context.active.erase(pair);
        return failure(std::move(error));
    };

    auto storage = std::make_shared<RuntimeTableStorage>();
    storage->rowCount = lhs->rowCount;
    storage->rowNames = lhs->rowNames;
    storage->dimensionNames = lhs->dimensionNames;
    storage->description = lhs->description;
    storage->userData = lhs->userData;
    storage->variables.reserve(lhs->variables.size());
    for (size_t index = 0; index < lhs->variables.size(); ++index) {
        const RuntimeTableVariable& leftVariable = lhs->variables[index];
        const RuntimeTableVariable& rightVariable = rhs->variables[index];
        if (leftVariable.name != rightVariable.name) {
            return activeFailure(
                "table comparison requires matching variable names");
        }

        RuntimeTableOperationResult compared;
        if (isRuntimeCategoricalValue(leftVariable.value) ||
            isRuntimeCategoricalValue(rightVariable.value)) {
            auto result = runtimeCompareCategorical(
                operation, leftVariable.value, rightVariable.value);
            compared = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeTableValue(leftVariable.value) ||
            isRuntimeTableValue(rightVariable.value)) {
            compared = compareTables(
                operation, leftVariable.value, rightVariable.value,
                context);
        } else if (isRuntimeTemporalValue(leftVariable.value) ||
                   isRuntimeTemporalValue(rightVariable.value)) {
            auto result = runtimeApplyTemporalBinary(
                operation, leftVariable.value, rightVariable.value);
            compared = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeTextValue(leftVariable.value) ||
                   isRuntimeTextValue(rightVariable.value)) {
            auto result = runtimeCompareText(
                operation, leftVariable.value, rightVariable.value);
            compared = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else if (isRuntimeNumericValue(leftVariable.value) &&
                   isRuntimeNumericValue(rightVariable.value)) {
            auto result = runtimeApplyNumericBinary(
                operation, leftVariable.value, rightVariable.value);
            compared = result.succeeded
                           ? success(std::move(result.value))
                           : failure(std::move(result.error));
        } else {
            return activeFailure(
                "table variable comparison is not supported for: " +
                leftVariable.name);
        }
        if (!compared.succeeded) {
            return activeFailure("table variable " + leftVariable.name +
                                 ": " + compared.error);
        }
        storage->variables.push_back(RuntimeTableVariable{
            leftVariable.name, std::move(compared.value)});
    }

    auto result = validatedTable(makeRuntimeTableValue(std::move(storage)));
    context.active.erase(pair);
    if (result.succeeded) {
        context.completed.emplace(pair, result.value);
    }
    return result;
}

} // namespace

RuntimeTableOperationResult runtimeCompareTables(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    if (operation != "==" && operation != "~=") {
        return failure("table operators currently support == and ~= only");
    }
    RuntimeTableComparisonContext context;
    return compareTables(operation, left, right, context);
}

bool validateRuntimeTableStorage(const RuntimeValue& value,
                                 std::string& error) {
    const RuntimeTableStorage* storage = runtimeTableStorage(value);
    if (!storage) {
        error = "table value has no storage";
        return false;
    }
    if (storage->kind != RuntimeTabularKind::Table) {
        error = "table value uses a non-table tabular storage kind";
        return false;
    }
    if (runtimeDimensions(value) !=
        std::vector<size_t>{storage->rowCount,
                            storage->variables.size()}) {
        error = "table shape does not match its storage";
        return false;
    }
    std::vector<std::string> variableNames;
    variableNames.reserve(storage->variables.size());
    for (const auto& variable : storage->variables) {
        variableNames.push_back(variable.name);
        if (!runtimeValueIsStorable(variable.value) ||
            !runtimeTabularVariableSupportsRows(variable.value) ||
            runtimeDimension(variable.value, 0) != storage->rowCount) {
            error = "table variable storage has an invalid row count or value";
            return false;
        }
    }
    if (!storage->rowNames.empty() &&
        (storage->rowNames.size() != storage->rowCount ||
         !uniqueNonemptyNames(storage->rowNames))) {
        error = "table row names do not match its height";
        return false;
    }
    const RuntimeTabularRowAxisKind expectedRowAxis =
        storage->rowNames.empty()
            ? RuntimeTabularRowAxisKind::None
            : RuntimeTabularRowAxisKind::Names;
    if (storage->rowAxisKind != expectedRowAxis ||
        storage->rowTimes.kind != RuntimeValueKind::Missing) {
        error = "table row axis storage is inconsistent";
        return false;
    }
    if (!validateTableNameLayout(
            variableNames, storage->dimensionNames, error)) {
        return false;
    }
    if (!runtimeValueIsStorable(storage->userData)) {
        error = "table UserData is not storable";
        return false;
    }
    return true;
}

bool runtimeTableValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableValueEquality& valueEquality) {
    const RuntimeTableStorage* lhs = runtimeTableStorage(left);
    const RuntimeTableStorage* rhs = runtimeTableStorage(right);
    if (!lhs || !rhs || lhs->rowCount != rhs->rowCount ||
        lhs->variables.size() != rhs->variables.size() ||
        lhs->rowNames != rhs->rowNames ||
        lhs->dimensionNames != rhs->dimensionNames ||
        lhs->description != rhs->description) {
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
