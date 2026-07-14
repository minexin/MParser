#pragma once

#include "mparser/interpreter.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeArrayOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

bool isRuntimeArrayOperationBuiltin(std::string_view name);

RuntimeArrayOperationResult runtimeArrayOperationBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments);

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions);

} // namespace mparser
