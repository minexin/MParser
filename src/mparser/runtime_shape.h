#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace mparser {

std::vector<size_t>
normalizeRuntimeDimensions(std::vector<size_t> dimensions);

std::vector<size_t> runtimeDimensions(const RuntimeValue& value);

void setRuntimeDimensions(RuntimeValue& value,
                          std::vector<size_t> dimensions);

size_t runtimeDimension(const RuntimeValue& value, size_t dimension);

size_t runtimeDimensionCount(const RuntimeValue& value);

std::optional<size_t>
checkedRuntimeDimensionProduct(const std::vector<size_t>& dimensions);

std::optional<size_t> checkedRuntimeNonnegativeInteger(double value);

size_t runtimeShapeElementCount(const RuntimeValue& value);

std::optional<bool> runtimeShapePredicate(
    std::string_view name, const RuntimeValue& value);

std::vector<size_t>
runtimeEffectiveSubscriptDimensions(const RuntimeValue& value,
                                    size_t subscriptCount);

std::optional<size_t> runtimeColumnMajorLinearIndex(
    const std::vector<size_t>& coordinates,
    const std::vector<size_t>& dimensions);

std::optional<std::vector<size_t>> runtimeColumnMajorCoordinates(
    size_t linearIndex, const std::vector<size_t>& dimensions);

std::optional<size_t> runtimeRowMajorStorageOffset(
    const std::vector<size_t>& coordinates,
    const std::vector<size_t>& dimensions);

std::optional<size_t> runtimeColumnMajorLinearToStorageOffset(
    const RuntimeValue& value, size_t linearIndex);

std::optional<size_t> runtimeSubscriptsToStorageOffset(
    const RuntimeValue& value, const std::vector<size_t>& coordinates,
    const std::vector<size_t>& effectiveDimensions);

std::vector<size_t> runtimeRowMajorCoordinates(
    size_t storageOffset, const std::vector<size_t>& dimensions);

std::optional<std::vector<size_t>> runtimeImplicitExpansionDimensions(
    const std::vector<size_t>& leftDimensions,
    const std::vector<size_t>& rightDimensions);

std::optional<size_t> runtimeImplicitExpansionStorageOffset(
    const std::vector<size_t>& outputCoordinates,
    const std::vector<size_t>& sourceDimensions);

} // namespace mparser
