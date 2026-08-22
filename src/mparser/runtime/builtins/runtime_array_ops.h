#pragma once

#include "mparser/runtime/core/runtime_value.h"
#include "mparser/runtime/core/runtime_object.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeArrayOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

struct RuntimeArrayOutputsResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::string error;
};

bool isRuntimeArrayOperationBuiltin(std::string_view name);

RuntimeArrayOperationResult runtimeArrayOperationBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

bool isRuntimeArrayConstructorBuiltin(std::string_view name);

RuntimeArrayOperationResult runtimeArrayConstructorBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments);

RuntimeArrayOperationResult runtimeLinspaceBuiltin(
    const std::vector<RuntimeValue>& arguments);

RuntimeArrayOutputsResult runtimeSizeBuiltin(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount);

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

} // namespace mparser
