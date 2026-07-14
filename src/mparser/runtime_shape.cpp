#include "mparser/runtime_shape.h"

#include <cmath>
#include <limits>

namespace mparser {

std::vector<size_t>
normalizeRuntimeDimensions(std::vector<size_t> dimensions) {
    if (dimensions.empty()) {
        dimensions = {0, 0};
    } else if (dimensions.size() == 1) {
        dimensions.push_back(1);
    }

    while (dimensions.size() > 2 && dimensions.back() == 1) {
        dimensions.pop_back();
    }
    return dimensions;
}

std::vector<size_t> runtimeDimensions(const RuntimeValue& value) {
    if (!value.dimensions.empty()) {
        return normalizeRuntimeDimensions(value.dimensions);
    }

    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return {0, 0};
    case RuntimeValueKind::Number:
    case RuntimeValueKind::FunctionHandle:
    case RuntimeValueKind::Object:
        return {1, 1};
    case RuntimeValueKind::String:
        return {1, value.text.size()};
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
    case RuntimeValueKind::Cell:
        return {value.rows, value.columns};
    }
    return {value.rows, value.columns};
}

void setRuntimeDimensions(RuntimeValue& value,
                          std::vector<size_t> dimensions) {
    value.dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    value.rows = value.dimensions[0];
    value.columns = value.dimensions[1];
}

size_t runtimeDimension(const RuntimeValue& value, size_t dimension) {
    const auto dimensions = runtimeDimensions(value);
    return dimension < dimensions.size() ? dimensions[dimension] : 1;
}

size_t runtimeDimensionCount(const RuntimeValue& value) {
    return runtimeDimensions(value).size();
}

std::optional<size_t>
checkedRuntimeDimensionProduct(const std::vector<size_t>& dimensions) {
    size_t product = 1;
    for (const size_t dimension : dimensions) {
        if (dimension != 0 &&
            product > std::numeric_limits<size_t>::max() / dimension) {
            return std::nullopt;
        }
        product *= dimension;
    }
    return product;
}

std::optional<size_t> checkedRuntimeNonnegativeInteger(double value) {
    const double exclusiveUpperBound =
        std::ldexp(1.0, std::numeric_limits<size_t>::digits);
    if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
        value >= exclusiveUpperBound) {
        return std::nullopt;
    }
    return static_cast<size_t>(value);
}

size_t runtimeShapeElementCount(const RuntimeValue& value) {
    const auto product =
        checkedRuntimeDimensionProduct(runtimeDimensions(value));
    return product.value_or(0);
}

std::vector<size_t>
runtimeEffectiveSubscriptDimensions(const RuntimeValue& value,
                                    size_t subscriptCount) {
    const auto dimensions = runtimeDimensions(value);
    if (subscriptCount == 0) {
        return {};
    }
    if (subscriptCount == 1) {
        return {runtimeShapeElementCount(value)};
    }

    std::vector<size_t> effective(subscriptCount, 1);
    for (size_t index = 0; index + 1 < subscriptCount; ++index) {
        if (index < dimensions.size()) {
            effective[index] = dimensions[index];
        }
    }

    const size_t foldedFrom = subscriptCount - 1;
    if (foldedFrom < dimensions.size()) {
        std::vector<size_t> folded(dimensions.begin() + foldedFrom,
                                   dimensions.end());
        effective.back() =
            checkedRuntimeDimensionProduct(folded).value_or(0);
    }
    return effective;
}

std::optional<size_t> runtimeColumnMajorLinearIndex(
    const std::vector<size_t>& coordinates,
    const std::vector<size_t>& dimensions) {
    if (coordinates.size() != dimensions.size()) {
        return std::nullopt;
    }

    size_t linearIndex = 0;
    size_t stride = 1;
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (coordinates[index] >= dimensions[index]) {
            return std::nullopt;
        }
        if (coordinates[index] != 0 &&
            stride > std::numeric_limits<size_t>::max() /
                         coordinates[index]) {
            return std::nullopt;
        }
        const size_t contribution = coordinates[index] * stride;
        if (linearIndex > std::numeric_limits<size_t>::max() - contribution) {
            return std::nullopt;
        }
        linearIndex += contribution;

        if (index + 1 < dimensions.size()) {
            if (dimensions[index] != 0 &&
                stride > std::numeric_limits<size_t>::max() /
                             dimensions[index]) {
                return std::nullopt;
            }
            stride *= dimensions[index];
        }
    }
    return linearIndex;
}

std::vector<size_t> runtimeRowMajorCoordinates(
    size_t storageOffset, const std::vector<size_t>& dimensions) {
    std::vector<size_t> coordinates(dimensions.size(), 0);
    for (size_t index = dimensions.size(); index > 0; --index) {
        const size_t dimension = dimensions[index - 1];
        if (dimension == 0) {
            return coordinates;
        }
        coordinates[index - 1] = storageOffset % dimension;
        storageOffset /= dimension;
    }
    return coordinates;
}

std::optional<size_t> runtimeColumnMajorLinearToStorageOffset(
    const RuntimeValue& value, size_t linearIndex) {
    const auto dimensions = runtimeDimensions(value);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || linearIndex >= *count) {
        return std::nullopt;
    }

    std::vector<size_t> coordinates(dimensions.size(), 0);
    size_t remaining = linearIndex;
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] == 0) {
            return std::nullopt;
        }
        coordinates[index] = remaining % dimensions[index];
        remaining /= dimensions[index];
    }

    size_t storageOffset = 0;
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (storageOffset >
            (std::numeric_limits<size_t>::max() - coordinates[index]) /
                dimensions[index]) {
            return std::nullopt;
        }
        storageOffset =
            storageOffset * dimensions[index] + coordinates[index];
    }
    return storageOffset;
}

std::optional<size_t> runtimeSubscriptsToStorageOffset(
    const RuntimeValue& value, const std::vector<size_t>& coordinates,
    const std::vector<size_t>& effectiveDimensions) {
    const auto linearIndex =
        runtimeColumnMajorLinearIndex(coordinates, effectiveDimensions);
    if (!linearIndex) {
        return std::nullopt;
    }
    return runtimeColumnMajorLinearToStorageOffset(value, *linearIndex);
}

} // namespace mparser
