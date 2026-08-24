#pragma once

#include "mparser/runtime/core/value/runtime_tabular.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

using RuntimeTableVariable = RuntimeTabularVariable;
using RuntimeTableStorage = RuntimeTabularStorage;

struct RuntimeTableOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

struct RuntimeTableContentsResult {
    bool succeeded = false;
    std::vector<RuntimeValue> values;
    size_t rowCount = 0;
    std::string error;
};

struct RuntimeTableNamesResult {
    bool succeeded = false;
    std::vector<std::string> names;
    std::string error;
};

struct RuntimeTableSortResult {
    bool succeeded = false;
    RuntimeValue value;
    std::vector<size_t> order;
    std::string error;
};

enum class RuntimeTableSortKeyKind {
    RowAxis,
    Variable,
};

struct RuntimeTableSortKey {
    RuntimeTableSortKeyKind kind = RuntimeTableSortKeyKind::Variable;
    size_t variableIndex = 0;
    bool descending = false;
};

using RuntimeTableValueEquality = std::function<
    bool(const RuntimeValue&, const RuntimeValue&)>;

bool isRuntimeTableValue(const RuntimeValue& value);

const RuntimeTabularStorage* runtimeTableStorage(
    const RuntimeValue& value);
RuntimeTabularStorage* runtimeMutableTableStorage(RuntimeValue& value);

RuntimeValue makeRuntimeTableValue(
    std::shared_ptr<RuntimeTabularStorage> storage);

RuntimeTableOperationResult runtimeMakeTable(
    std::vector<RuntimeValue> variables,
    std::vector<std::string> variableNames = {},
    std::vector<std::string> rowNames = {},
    std::vector<std::string> dimensionNames = {});

RuntimeTableNamesResult runtimeTableNames(
    const RuntimeValue& value, std::string_view role);

RuntimeTableOperationResult runtimeTableMemberValue(
    const RuntimeValue& table, std::string_view member);
RuntimeTableOperationResult runtimeSetTableMember(
    const RuntimeValue& table, std::string member,
    const RuntimeValue& value, bool nullAssignment = false);

RuntimeTableOperationResult runtimeIndexTable(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts);
RuntimeTableContentsResult runtimeTableContents(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts);
RuntimeTableOperationResult runtimeTableContentsValue(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts);

RuntimeTableOperationResult runtimeAssignTableIndexed(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);
RuntimeTableOperationResult runtimeDeleteTableIndexed(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts);
RuntimeTableOperationResult runtimeAssignTableContents(
    const RuntimeValue& table,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

RuntimeTableOperationResult runtimeArrayToTable(
    const RuntimeValue& value,
    std::vector<std::string> variableNames = {},
    std::vector<std::string> rowNames = {});
RuntimeTableOperationResult runtimeStructToTable(
    const RuntimeValue& value,
    std::vector<std::string> variableNames = {},
    std::vector<std::string> rowNames = {});
RuntimeTableOperationResult runtimeTableToStruct(
    const RuntimeValue& table);

RuntimeTableOperationResult runtimeConcatenateTables(
    size_t dimension, const std::vector<RuntimeValue>& values);
RuntimeTableSortResult runtimeSortTable(
    const RuntimeValue& table,
    const std::vector<RuntimeTableSortKey>& keys);

RuntimeTableOperationResult runtimeCompareTables(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);

bool validateRuntimeTableStorage(const RuntimeValue& value,
                                 std::string& error);
bool runtimeTableValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableValueEquality& valueEquality);

} // namespace mparser
