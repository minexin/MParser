#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeReductionResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::string error;
};

bool isRuntimeReductionBuiltin(std::string_view name);

RuntimeReductionResult runtimeReductionBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount);

} // namespace mparser
