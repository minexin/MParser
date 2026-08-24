#include "mparser/runtime/core/value/runtime_array.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {
namespace {

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value);
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
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

std::optional<RuntimeValue> missingAsNumeric(
    const RuntimeValue& value, RuntimeNumericClass numericClass) {
    if (!isMissingArray(value) ||
        !runtimeNumericClassIsFloating(numericClass)) {
        return std::nullopt;
    }
    return runtimeNumericValueFromLogicalOrder(
        runtimeDimensions(value),
        std::vector<double>(
            runtimeShapeElementCount(value),
            std::numeric_limits<double>::quiet_NaN()),
        numericClass);
}

RuntimeValue missingAsString(const RuntimeValue& value) {
    return makeRuntimeStringArray(
        runtimeDimensions(value),
        std::vector<RuntimeStringElement>(
            runtimeShapeElementCount(value),
            RuntimeStringElement{u"", true}));
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

RuntimeArrayOperationResult runtimeConcatenateValues(
    size_t dimension, const std::vector<RuntimeValue>& values,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (dimension == 0) {
        return failure("concatenation dimension must be positive");
    }
    if (values.empty()) {
        return failure("concatenation requires at least one array");
    }
    if (values.size() > 1) {
        std::vector<RuntimeValue> nonempty;
        nonempty.reserve(values.size());
        for (const auto& value : values) {
            if (runtimeShapeElementCount(value) != 0 ||
                runtimeDimensions(value) != std::vector<size_t>{0, 0}) {
                nonempty.push_back(value);
            }
        }
        if (nonempty.empty()) {
            return success(values.front());
        }
        if (nonempty.size() != values.size()) {
            return runtimeConcatenateValues(
                dimension, nonempty, objectPolicy);
        }
    }

    const bool hasMissing = std::any_of(
        values.begin(), values.end(), isMissingArray);
    if (hasMissing) {
        const bool hasString = std::any_of(
            values.begin(), values.end(), isRuntimeStringArray);
        const bool hasNumeric = std::any_of(
            values.begin(), values.end(), isNumeric);
        if (hasString) {
            if (!std::all_of(
                    values.begin(), values.end(),
                    [](const RuntimeValue& value) {
                        return isMissingArray(value) ||
                               isRuntimeStringArray(value);
                    })) {
                return failure(
                    "missing concatenation with text requires string inputs");
            }
            std::vector<RuntimeValue> converted;
            converted.reserve(values.size());
            for (const RuntimeValue& value : values) {
                converted.push_back(
                    isMissingArray(value) ? missingAsString(value) : value);
            }
            return runtimeConcatenateValues(
                dimension, converted, objectPolicy);
        }
        if (hasNumeric) {
            if (!std::all_of(
                    values.begin(), values.end(),
                    [](const RuntimeValue& value) {
                        return isMissingArray(value) || isNumeric(value);
                    })) {
                return failure(
                    "missing concatenation inputs have incompatible types");
            }
            const auto numeric = std::find_if(
                values.begin(), values.end(), isNumeric);
            if (numeric == values.end() ||
                !runtimeNumericClassIsFloating(numeric->numericClass)) {
                return failure(
                    "missing cannot convert to a non-floating numeric class");
            }
            std::vector<RuntimeValue> converted;
            converted.reserve(values.size());
            for (const RuntimeValue& value : values) {
                if (!isMissingArray(value)) {
                    converted.push_back(value);
                    continue;
                }
                auto convertedMissing = missingAsNumeric(
                    value, numeric->numericClass);
                if (!convertedMissing) {
                    return failure(
                        "missing could not convert to the numeric class");
                }
                converted.push_back(std::move(*convertedMissing));
            }
            return runtimeConcatenateValues(
                dimension, converted, objectPolicy);
        }
        if (!std::all_of(values.begin(), values.end(), isMissingArray)) {
            return failure(
                "missing concatenation inputs have incompatible types");
        }
    }
    const bool text = isRuntimeTextValue(values.front());
    if (text || std::any_of(values.begin(), values.end(),
                            isRuntimeTextValue)) {
        if (!std::all_of(values.begin(), values.end(),
                         isRuntimeTextValue)) {
            return failure(
                "concatenation inputs must have compatible types");
        }
        auto result = runtimeConcatenateText(dimension, values);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    const bool objects = isRuntimeClassObject(values.front());
    if (objects || std::any_of(values.begin(), values.end(),
                               isRuntimeClassObject)) {
        if (!std::all_of(values.begin(), values.end(),
                         isRuntimeClassObject)) {
            return failure(
                "concatenation inputs must have compatible types");
        }
        auto result = runtimeConcatenateObject(
            dimension, values, objectPolicy);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    const bool cells = isCell(values.front());
    const bool numeric = isNumeric(values.front());
    const bool missing = isMissingArray(values.front());
    if (!cells && !numeric && !missing) {
        return failure(
            "concatenation supports missing, numeric, and cell arrays");
    }
    for (const auto& value : values) {
        if ((cells && !isCell(value)) || (numeric && !isNumeric(value))) {
            return failure("concatenation inputs must have compatible types");
        }
        if (missing && !isMissingArray(value)) {
            return failure("concatenation inputs must have compatible types");
        }
    }

    size_t dimensionCount = dimension;
    for (const auto& value : values) {
        dimensionCount =
            std::max(dimensionCount, runtimeDimensionCount(value));
    }
    dimensionCount = std::max<size_t>(dimensionCount, 2);
    const size_t axis = dimension - 1;

    auto outputDimensions = runtimeDimensions(values.front());
    outputDimensions.resize(dimensionCount, 1);
    outputDimensions[axis] = 0;
    std::vector<std::vector<size_t>> inputDimensions;
    inputDimensions.reserve(values.size());
    for (const auto& value : values) {
        auto dimensions = runtimeDimensions(value);
        dimensions.resize(dimensionCount, 1);
        if (!inputDimensions.empty()) {
            for (size_t index = 0; index < dimensionCount; ++index) {
                if (index == axis) {
                    continue;
                }
                if (dimensions[index] != inputDimensions.front()[index]) {
                    return failure(
                        "concatenation dimensions must agree outside the "
                        "selected dimension");
                }
            }
        }
        if (outputDimensions[axis] >
            std::numeric_limits<size_t>::max() - dimensions[axis]) {
            return failure("concatenated dimensions are too large");
        }
        outputDimensions[axis] += dimensions[axis];
        inputDimensions.push_back(std::move(dimensions));
    }

    const auto outputCount =
        checkedRuntimeDimensionProduct(outputDimensions);
    if (!outputCount) {
        return failure("concatenated dimensions are too large");
    }

    if (missing) {
        return success(makeRuntimeMissingArrayValue(
            std::move(outputDimensions)));
    }

    if (numeric) {
        std::vector<RuntimeNumericElementValue> elements(*outputCount);
        size_t axisOffset = 0;
        bool preferNumber = true;
        const RuntimeNumericClass numericClass =
            values.front().numericClass;
        for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
            const RuntimeValue& value = values[valueIndex];
            preferNumber = preferNumber && isNumber(value);
            const size_t inputCount = runtimeShapeElementCount(value);
            for (size_t sourceOffset = 0; sourceOffset < inputCount;
                 ++sourceOffset) {
                auto coordinates = runtimeRowMajorCoordinates(
                    sourceOffset, inputDimensions[valueIndex]);
                coordinates[axis] += axisOffset;
                const auto outputOffset = runtimeRowMajorStorageOffset(
                    coordinates, outputDimensions);
                const auto element = runtimeNumericStorageElementValue(
                    value, sourceOffset);
                if (!outputOffset || !element) {
                    return failure(
                        "concatenation could not map a numeric element");
                }
                elements[*outputOffset] = *element;
            }
            axisOffset += inputDimensions[valueIndex][axis];
        }
        auto result = numericResult(
            outputDimensions, std::move(elements), preferNumber,
            numericClass);
        if (!result) {
            return failure(
                numericClass == RuntimeNumericClass::Logical
                    ? "concatenation cannot convert NaN to logical"
                    : "concatenation inputs have incompatible numeric classes");
        }
        return success(std::move(*result));
    }

    std::vector<RuntimeValue> outputCells(*outputCount);
    size_t axisOffset = 0;
    for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
        const RuntimeValue& value = values[valueIndex];
        const size_t inputCount = runtimeShapeElementCount(value);
        for (size_t sourceOffset = 0; sourceOffset < inputCount;
             ++sourceOffset) {
            auto coordinates = runtimeRowMajorCoordinates(
                sourceOffset, inputDimensions[valueIndex]);
            coordinates[axis] += axisOffset;
            const auto outputOffset = runtimeRowMajorStorageOffset(
                coordinates, outputDimensions);
            if (!outputOffset || sourceOffset >= value.cells.size()) {
                return failure("concatenation could not map a cell element");
            }
            outputCells[*outputOffset] = value.cells[sourceOffset];
        }
        axisOffset += inputDimensions[valueIndex][axis];
    }
    return success(cellResult(outputDimensions, std::move(outputCells)));
}

} // namespace mparser
