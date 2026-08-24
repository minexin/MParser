#pragma once

#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_value.h"

#include <string>
#include <vector>

namespace mparser {

struct RuntimeArrayOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

RuntimeArrayOperationResult runtimeConcatenateValues(
    size_t dimension, const std::vector<RuntimeValue>& values,
    const RuntimeObjectArrayPolicy& objectPolicy = {});

} // namespace mparser
