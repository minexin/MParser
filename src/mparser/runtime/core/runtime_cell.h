#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <string>
#include <vector>

namespace mparser {

struct RuntimeCellOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeCellOperationResult runtimeIndexCell(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts);
RuntimeCellOperationResult runtimeIndexCell(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon);

RuntimeCellOperationResult runtimeIndexCellContents(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts);

RuntimeCellOperationResult runtimeAssignCellIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

RuntimeCellOperationResult runtimeDeleteCellIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts);

RuntimeCellOperationResult runtimeAssignCellContents(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

} // namespace mparser
