#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <cstddef>
#include <optional>
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

struct RuntimeIndexSelectionsResult {
    bool succeeded = false;
    bool logicalMask = false;
    std::vector<std::vector<size_t>> indices;
    std::vector<size_t> effectiveDimensions;
    std::vector<size_t> resultDimensions;
    std::string error;
};

RuntimeIndexSelectionResult runtimeResolveIndexSelection(
    const RuntimeValue& subscript, size_t extent,
    bool allowNumericGrowth);

RuntimeIndexSelectionsResult runtimeResolveIndexSelections(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool allowNumericGrowth);
RuntimeIndexSelectionsResult runtimeResolveIndexSelections(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool allowNumericGrowth,
    bool linearColon);

std::optional<size_t> runtimeIndexSelectionSourceLogicalIndex(
    const RuntimeIndexSelectionsResult& selections,
    size_t resultLogicalIndex);

std::optional<size_t> runtimeIndexSelectionRequiredExtent(
    const std::vector<size_t>& selection);

RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts);
RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon);

RuntimeIndexOperationResult runtimeIndexMissingArray(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon = false);

std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask);
std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask,
    bool linearColon);

} // namespace mparser
