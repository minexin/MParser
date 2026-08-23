#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeScanResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::string error;
};

bool isRuntimeScanBuiltin(std::string_view name);

RuntimeScanResult runtimeScanBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount);

} // namespace mparser
