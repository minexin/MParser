#pragma once

#include "mparser/runtime_value.h"

#include <string>
#include <vector>

namespace mparser {

struct RuntimeNumericAssignmentResult {
    bool succeeded = false;
    std::string error;
};

RuntimeNumericAssignmentResult runtimeAssignNumericIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

RuntimeNumericAssignmentResult runtimeDeleteNumericIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts);

RuntimeNumericAssignmentResult runtimeAssignMissingIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

RuntimeNumericAssignmentResult runtimeDeleteMissingIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts);

} // namespace mparser
