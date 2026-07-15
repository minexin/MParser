#include "mparser/runtime_numeric.h"

#include "mparser/runtime_shape.h"

#include <cmath>

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
