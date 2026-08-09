#include "mparser/runtime_numeric.h"

#include "mparser/runtime_shape.h"

#include <cmath>
#include <utility>

namespace mparser {

bool isRuntimeNumericValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number ||
           value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isRuntimeLogical(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           value.numericClass == RuntimeNumericClass::Logical;
}

std::string_view runtimeNumericClassName(
    RuntimeNumericClass numericClass) {
    return numericClass == RuntimeNumericClass::Logical ? "logical"
                                                        : "double";
}

std::optional<double> runtimeCoerceNumericElement(
    double value, RuntimeNumericClass numericClass) {
    if (numericClass == RuntimeNumericClass::Double) {
        return value;
    }
    if (std::isnan(value)) {
        return std::nullopt;
    }
    return value == 0.0 ? 0.0 : 1.0;
}

std::optional<double> runtimeNumericElement(
    const RuntimeValue& value, size_t logicalIndex) {
    if (value.kind == RuntimeValueKind::Number) {
        return logicalIndex == 0 ? std::optional<double>(value.number)
                                 : std::nullopt;
    }
    if (value.kind != RuntimeValueKind::Vector &&
        value.kind != RuntimeValueKind::Matrix) {
        return std::nullopt;
    }
    const auto storageOffset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    if (!storageOffset || *storageOffset >= value.elements.size()) {
        return std::nullopt;
    }
    return value.elements[*storageOffset];
}

std::optional<RuntimeValue> runtimeNumericValueFromLogicalOrder(
    std::vector<size_t> dimensions, std::vector<double> values,
    RuntimeNumericClass numericClass) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != values.size()) {
        return std::nullopt;
    }

    for (double& value : values) {
        const auto converted =
            runtimeCoerceNumericElement(value, numericClass);
        if (!converted) {
            return std::nullopt;
        }
        value = *converted;
    }

    RuntimeValue result;
    result.numericClass = numericClass;
    if (*count == 1) {
        result.kind = RuntimeValueKind::Number;
        result.number = values.front();
        setRuntimeDimensions(result, {1, 1});
        return result;
    }

    result.kind = dimensions.size() == 2 && dimensions[0] == 1
                      ? RuntimeValueKind::Vector
                      : RuntimeValueKind::Matrix;
    result.elements.resize(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= result.elements.size()) {
            return std::nullopt;
        }
        result.elements[*storageOffset] = values[logicalIndex];
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

std::optional<std::vector<RuntimeValue>>
runtimeNumericForLoopColumns(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    const auto dimensions = runtimeDimensions(value);
    const size_t rowCount = dimensions.front();
    const std::vector<size_t> trailingDimensions(
        dimensions.begin() + 1, dimensions.end());
    const auto columnCount =
        checkedRuntimeDimensionProduct(trailingDimensions);
    if (!columnCount) {
        return std::nullopt;
    }

    std::vector<RuntimeValue> columns;
    columns.reserve(*columnCount);
    for (size_t column = 0; column < *columnCount; ++column) {
        std::vector<double> elements;
        elements.reserve(rowCount);
        for (size_t row = 0; row < rowCount; ++row) {
            const auto element = runtimeNumericElement(
                value, column * rowCount + row);
            if (!element) {
                return std::nullopt;
            }
            elements.push_back(*element);
        }
        auto columnValue = runtimeNumericValueFromLogicalOrder(
            {rowCount, 1}, std::move(elements), value.numericClass);
        if (!columnValue) {
            return std::nullopt;
        }
        columns.push_back(std::move(*columnValue));
    }
    return columns;
}

std::optional<RuntimeValue> runtimeConvertNumericClass(
    RuntimeValue value, RuntimeNumericClass numericClass) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    if (value.kind == RuntimeValueKind::Number) {
        const auto converted =
            runtimeCoerceNumericElement(value.number, numericClass);
        if (!converted) {
            return std::nullopt;
        }
        value.number = *converted;
    } else {
        for (double& element : value.elements) {
            const auto converted =
                runtimeCoerceNumericElement(element, numericClass);
            if (!converted) {
                return std::nullopt;
            }
            element = *converted;
        }
    }
    value.numericClass = numericClass;
    return value;
}

} // namespace mparser
