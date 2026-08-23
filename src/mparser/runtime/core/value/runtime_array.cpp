#include "mparser/runtime/core/value/runtime_array.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <optional>
#include <utility>

namespace mparser {
namespace {

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isNumeric(const RuntimeValue& value) {
    return isNumber(value) || value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isMissingArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::MissingArray;
}

bool isSupportedArray(const RuntimeValue& value) {
    return isMissingArray(value) || isNumeric(value) ||
           value.kind == RuntimeValueKind::Cell ||
           isRuntimeTextValue(value) || isRuntimeClassObject(value);
}

RuntimeArrayOperationResult failure(std::string message) {
    return RuntimeArrayOperationResult{false, {}, std::move(message)};
}

RuntimeArrayOperationResult success(RuntimeValue value) {
    return RuntimeArrayOperationResult{true, std::move(value), {}};
}

std::optional<RuntimeValue> numericResult(
    std::vector<size_t> dimensions,
    std::vector<RuntimeNumericElementValue> elements,
    bool preferNumber, RuntimeNumericClass numericClass) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != elements.size()) {
        return std::nullopt;
    }

    std::vector<RuntimeNumericElementValue> logicalElements;
    logicalElements.reserve(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count;
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= elements.size()) {
            return std::nullopt;
        }
        logicalElements.push_back(elements[*storageOffset]);
    }

    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(logicalElements), numericClass);
    if (!result) {
        return std::nullopt;
    }
    if (!preferNumber && result->kind == RuntimeValueKind::Number) {
        result->kind = dimensions.size() == 2 && dimensions[0] == 1
                           ? RuntimeValueKind::Vector
                           : RuntimeValueKind::Matrix;
        result->elements = {result->number};
        result->number = 0.0;
        setRuntimeDimensions(*result, std::move(dimensions));
    }
    return result;
}

RuntimeValue cellResult(std::vector<size_t> dimensions,
                        std::vector<RuntimeValue> cells) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells = std::move(cells);
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

} // namespace

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (!isSupportedArray(value)) {
        return failure(
            "reshape supports missing, numeric, text, cell, and object arrays");
    }
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure("reshape dimensions are too large");
    }
    if (*count != runtimeShapeElementCount(value)) {
        return failure("reshape cannot change the number of elements");
    }

    if (isMissingArray(value)) {
        return success(makeRuntimeMissingArrayValue(std::move(dimensions)));
    }

    if (isNumeric(value)) {
        std::vector<RuntimeNumericElementValue> elements(*count);
        for (size_t logicalIndex = 0; logicalIndex < *count;
             ++logicalIndex) {
            const auto sourceOffset = isNumber(value)
                                          ? std::optional<size_t>(0)
                                          : runtimeColumnMajorLinearToStorageOffset(
                                                value, logicalIndex);
            const auto targetCoordinates = runtimeColumnMajorCoordinates(
                logicalIndex, dimensions);
            const auto targetOffset = targetCoordinates
                                          ? runtimeRowMajorStorageOffset(
                                                *targetCoordinates, dimensions)
                                          : std::nullopt;
            const auto element = sourceOffset
                                     ? runtimeNumericStorageElementValue(
                                           value, *sourceOffset)
                                     : std::nullopt;
            if (!sourceOffset || !targetOffset || !element) {
                return failure(
                    "reshape could not preserve logical element order");
            }
            elements[*targetOffset] = *element;
        }
        auto result = numericResult(
            dimensions, std::move(elements), isNumber(value),
            value.numericClass);
        return result
                   ? success(std::move(*result))
                   : failure("reshape could not construct a numeric result");
    }

    if (isRuntimeCharacterArray(value)) {
        std::u16string elements(*count, u'\0');
        for (size_t logicalIndex = 0; logicalIndex < *count;
             ++logicalIndex) {
            const auto sourceOffset =
                runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
            const auto targetCoordinates = runtimeColumnMajorCoordinates(
                logicalIndex, dimensions);
            const auto targetOffset = targetCoordinates
                                          ? runtimeRowMajorStorageOffset(
                                                *targetCoordinates, dimensions)
                                          : std::nullopt;
            if (!sourceOffset || !targetOffset ||
                *sourceOffset >= value.characterElements.size()) {
                return failure(
                    "reshape could not preserve logical character order");
            }
            elements[*targetOffset] = value.characterElements[*sourceOffset];
        }
        return success(makeRuntimeCharacterArray(
            dimensions, std::move(elements)));
    }

    if (isRuntimeStringArray(value)) {
        std::vector<RuntimeStringElement> elements(*count);
        for (size_t logicalIndex = 0; logicalIndex < *count;
             ++logicalIndex) {
            const auto sourceOffset =
                runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
            const auto targetCoordinates = runtimeColumnMajorCoordinates(
                logicalIndex, dimensions);
            const auto targetOffset = targetCoordinates
                                          ? runtimeRowMajorStorageOffset(
                                                *targetCoordinates, dimensions)
                                          : std::nullopt;
            if (!sourceOffset || !targetOffset ||
                *sourceOffset >= value.stringElements.size()) {
                return failure(
                    "reshape could not preserve logical string order");
            }
            elements[*targetOffset] = value.stringElements[*sourceOffset];
        }
        return success(makeRuntimeStringArray(
            dimensions, std::move(elements)));
    }

    if (isRuntimeClassObject(value)) {
        std::vector<RuntimeValue> elements;
        elements.reserve(*count);
        for (size_t logicalIndex = 0; logicalIndex < *count;
             ++logicalIndex) {
            const auto* element =
                runtimeObjectLogicalElement(value, logicalIndex);
            if (!element) {
                return failure(
                    "reshape could not preserve logical object order");
            }
            elements.push_back(*element);
        }
        auto result = runtimeMakeObjectArrayFromLogicalOrder(
            std::move(elements), dimensions, value.className,
            value.handleObject, objectPolicy, value.className);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }

    std::vector<RuntimeValue> cells(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto sourceOffset =
            runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
        const auto targetCoordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto targetOffset = targetCoordinates
                                      ? runtimeRowMajorStorageOffset(
                                            *targetCoordinates, dimensions)
                                      : std::nullopt;
        if (!sourceOffset || !targetOffset ||
            *sourceOffset >= value.cells.size()) {
            return failure(
                "reshape could not preserve logical cell order");
        }
        cells[*targetOffset] = value.cells[*sourceOffset];
    }
    return success(cellResult(dimensions, std::move(cells)));
}

} // namespace mparser
