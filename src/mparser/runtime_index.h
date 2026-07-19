#pragma once

#include "mparser/interpreter.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mparser {

struct RuntimeIndexSelectionResult {
    bool succeeded = false;
    bool logicalMask = false;
    std::vector<size_t> indices;
    std::string error;
};

struct RuntimeIndexOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeIndexSelectionResult runtimeResolveIndexSelection(
    const RuntimeValue& subscript, size_t extent,
    bool allowNumericGrowth);

RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts);

std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask);

} // namespace mparser
