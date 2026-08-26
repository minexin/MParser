#pragma once

#include "mparser/runtime/core/value/runtime_table.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace mparser {

class RuntimeExecutionControl;

inline constexpr size_t kRuntimeTableUnmatchedRow =
    std::numeric_limits<size_t>::max();

enum class RuntimeTableJoinType {
    Inner,
    Left,
    Right,
    Full,
};

struct RuntimeTableJoinOptions {
    std::vector<std::string> leftKeys;
    std::vector<std::string> rightKeys;
    std::vector<std::string> leftVariables;
    std::vector<std::string> rightVariables;
    bool hasLeftVariables = false;
    bool hasRightVariables = false;
    RuntimeTableJoinType type = RuntimeTableJoinType::Inner;
    bool mergeKeys = true;
    RuntimeExecutionControl* executionControl = nullptr;
};

struct RuntimeTableJoinResult {
    bool succeeded = false;
    RuntimeValue value;
    std::vector<size_t> leftRows;
    std::vector<size_t> rightRows;
    std::string error;
};

struct RuntimeTableGroupingResult {
    bool succeeded = false;
    std::vector<size_t> keyVariableIndices;
    std::vector<std::vector<size_t>> groups;
    std::string error;
};

RuntimeTableJoinResult runtimeJoinTables(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableJoinOptions& options);

RuntimeTableGroupingResult runtimeGroupTableRows(
    const RuntimeValue& table,
    const std::vector<std::string>& keyVariables,
    RuntimeExecutionControl* executionControl = nullptr);

} // namespace mparser
