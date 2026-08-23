#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

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

struct RuntimeReductionShape {
    std::vector<size_t> inputDimensions;
    std::vector<size_t> reductionDimensions;
    std::vector<size_t> outputDimensions;
};

RuntimeReductionShape runtimeReductionShape(
    std::vector<size_t> inputDimensions, bool dimensionSpecified,
    bool allDimensions, std::vector<size_t> reductionDimensions,
    bool extrema);

bool isRuntimeReductionBuiltin(std::string_view name);

RuntimeReductionResult runtimeReductionBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount);

} // namespace mparser
