#pragma once

#include "mparser/interpreter.h"
#include "mparser/runtime_object.h"

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
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

} // namespace mparser
