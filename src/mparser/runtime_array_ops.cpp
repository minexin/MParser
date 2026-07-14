#include "mparser/runtime_array_ops.h"

#include "mparser/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {
namespace {

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isNumericArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isNumeric(const RuntimeValue& value) {
    return isNumber(value) || isNumericArray(value);
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
}

bool isSupportedArray(const RuntimeValue& value) {
    return isNumeric(value) || isCell(value);
}

RuntimeArrayOperationResult failure(std::string message) {
    return RuntimeArrayOperationResult{false, RuntimeValue{},
                                       std::move(message)};
}

RuntimeArrayOperationResult success(RuntimeValue value) {
    return RuntimeArrayOperationResult{true, std::move(value), {}};
}

RuntimeValue numericResult(std::vector<size_t> dimensions,
                           std::vector<double> elements,
                           bool preferNumber) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    if (preferNumber && elements.size() == 1 && dimensions[0] == 1 &&
        dimensions[1] == 1) {
        RuntimeValue result;
        result.kind = RuntimeValueKind::Number;
        result.number = elements.front();
        setRuntimeDimensions(result, {1, 1});
        return result;
    }

    RuntimeValue result;
    result.kind = dimensions.size() == 2 && dimensions[0] == 1
                      ? RuntimeValueKind::Vector
                      : RuntimeValueKind::Matrix;
    result.elements = std::move(elements);
    setRuntimeDimensions(result, std::move(dimensions));
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

std::optional<double> numericStorageElement(const RuntimeValue& value,
                                            size_t storageOffset) {
    if (isNumber(value)) {
        return storageOffset == 0 ? std::optional<double>(value.number)
                                  : std::nullopt;
    }
    if (!isNumericArray(value) || storageOffset >= value.elements.size()) {
        return std::nullopt;
    }
    return value.elements[storageOffset];
}

std::optional<std::vector<double>> logicalNumericValues(
    const RuntimeValue& value) {
    if (!isNumeric(value)) {
        return std::nullopt;
    }

    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> result;
    result.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        if (isNumber(value)) {
            result.push_back(value.number);
            continue;
        }
        const auto storageOffset =
            runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
        if (!storageOffset || *storageOffset >= value.elements.size()) {
            return std::nullopt;
        }
        result.push_back(value.elements[*storageOffset]);
    }
    return result;
}

bool isNumericRowVector(const RuntimeValue& value) {
    return isNumericArray(value) && runtimeDimension(value, 0) == 1;
}

std::optional<size_t> nonnegativeDimension(double raw) {
    return checkedRuntimeNonnegativeInteger(raw);
}

std::optional<size_t> positiveDimension(double raw) {
    const auto dimension = checkedRuntimeNonnegativeInteger(raw);
    if (!dimension || *dimension == 0) {
        return std::nullopt;
    }
    return dimension;
}

std::optional<size_t> repetitionFactor(double raw) {
    if (!std::isfinite(raw) || std::floor(raw) != raw) {
        return std::nullopt;
    }
    if (raw <= 0.0) {
        return size_t{0};
    }
    return checkedRuntimeNonnegativeInteger(raw);
}

RuntimeArrayOperationResult reshapeBuiltin(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() < 2) {
        return failure("reshape expects an array and at least two dimensions");
    }
    if (!isSupportedArray(arguments.front())) {
        return failure("reshape supports numeric and cell arrays");
    }

    std::vector<std::optional<size_t>> requested;
    if (arguments.size() == 2) {
        const RuntimeValue& shape = arguments[1];
        if (!isNumericRowVector(shape)) {
            return failure("reshape size must be a numeric row vector");
        }
        const auto values = logicalNumericValues(shape);
        if (!values || values->size() < 2) {
            return failure("reshape size vector must contain at least two dimensions");
        }
        requested.reserve(values->size());
        for (const double raw : *values) {
            const auto dimension = nonnegativeDimension(raw);
            if (!dimension) {
                return failure(
                    "reshape dimensions must be representable nonnegative integers");
            }
            requested.push_back(*dimension);
        }
    } else {
        if (arguments.size() - 1 < 2) {
            return failure("reshape requires at least two dimensions");
        }
        requested.reserve(arguments.size() - 1);
        bool inferred = false;
        for (size_t index = 1; index < arguments.size(); ++index) {
            const RuntimeValue& argument = arguments[index];
            if (isNumericArray(argument) &&
                runtimeShapeElementCount(argument) == 0) {
                if (inferred) {
                    return failure(
                        "reshape accepts at most one inferred dimension");
                }
                inferred = true;
                requested.push_back(std::nullopt);
                continue;
            }
            if (!isNumber(argument)) {
                return failure(
                    "reshape dimensions must be scalar numbers or one empty value");
            }
            const auto dimension = nonnegativeDimension(argument.number);
            if (!dimension) {
                return failure(
                    "reshape dimensions must be representable nonnegative integers");
            }
            requested.push_back(*dimension);
        }
    }

    size_t inferredIndex = requested.size();
    std::vector<size_t> dimensions(requested.size(), 1);
    for (size_t index = 0; index < requested.size(); ++index) {
        if (requested[index]) {
            dimensions[index] = *requested[index];
        } else {
            inferredIndex = index;
        }
    }

    const size_t inputCount = runtimeShapeElementCount(arguments.front());
    const auto knownProduct = checkedRuntimeDimensionProduct(dimensions);
    if (!knownProduct) {
        return failure("reshape dimensions are too large");
    }
    if (inferredIndex != requested.size()) {
        if (*knownProduct == 0) {
            if (inputCount != 0) {
                return failure(
                    "reshape inferred dimension cannot preserve the element count");
            }
            dimensions[inferredIndex] = 0;
        } else {
            if (inputCount % *knownProduct != 0) {
                return failure(
                    "reshape inferred dimension does not divide the element count");
            }
            dimensions[inferredIndex] = inputCount / *knownProduct;
        }
    }

    return runtimeReshapeValue(arguments.front(), std::move(dimensions));
}

std::optional<std::vector<size_t>> permutationOrder(
    const RuntimeValue& value, const RuntimeValue& orderValue,
    std::string& error) {
    if (!isNumericRowVector(orderValue)) {
        error = "dimension order must be a numeric row vector";
        return std::nullopt;
    }
    const auto values = logicalNumericValues(orderValue);
    if (!values || values->size() < runtimeDimensionCount(value)) {
        error = "dimension order must include every input dimension";
        return std::nullopt;
    }

    std::vector<size_t> order;
    order.reserve(values->size());
    for (const double raw : *values) {
        const auto dimension = positiveDimension(raw);
        if (!dimension) {
            error = "dimension order must contain positive integers";
            return std::nullopt;
        }
        order.push_back(*dimension);
    }
    auto sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (size_t index = 0; index < sorted.size(); ++index) {
        if (sorted[index] != index + 1) {
            error = "dimension order must be a permutation of consecutive dimensions";
            return std::nullopt;
        }
    }
    return order;
}

RuntimeArrayOperationResult permuteValue(
    const RuntimeValue& value, const std::vector<size_t>& order) {
    if (!isSupportedArray(value)) {
        return failure("permute supports numeric and cell arrays");
    }

    auto sourceDimensions = runtimeDimensions(value);
    sourceDimensions.resize(order.size(), 1);
    std::vector<size_t> outputDimensions(order.size(), 1);
    for (size_t index = 0; index < order.size(); ++index) {
        outputDimensions[index] = sourceDimensions[order[index] - 1];
    }
    const auto count = checkedRuntimeDimensionProduct(outputDimensions);
    if (!count || *count != runtimeShapeElementCount(value)) {
        return failure("permuted dimensions are too large");
    }

    if (isNumeric(value)) {
        std::vector<double> elements(*count, 0.0);
        for (size_t sourceOffset = 0; sourceOffset < *count;
             ++sourceOffset) {
            const auto sourceCoordinates = runtimeRowMajorCoordinates(
                sourceOffset, sourceDimensions);
            std::vector<size_t> outputCoordinates(order.size(), 0);
            for (size_t index = 0; index < order.size(); ++index) {
                outputCoordinates[index] =
                    sourceCoordinates[order[index] - 1];
            }
            const auto outputOffset = runtimeRowMajorStorageOffset(
                outputCoordinates, outputDimensions);
            const auto element = numericStorageElement(value, sourceOffset);
            if (!outputOffset || !element) {
                return failure("permute could not map an array element");
            }
            elements[*outputOffset] = *element;
        }
        return success(numericResult(outputDimensions, std::move(elements),
                                     isNumber(value)));
    }

    std::vector<RuntimeValue> cells(*count);
    for (size_t sourceOffset = 0; sourceOffset < *count; ++sourceOffset) {
        const auto sourceCoordinates = runtimeRowMajorCoordinates(
            sourceOffset, sourceDimensions);
        std::vector<size_t> outputCoordinates(order.size(), 0);
        for (size_t index = 0; index < order.size(); ++index) {
            outputCoordinates[index] = sourceCoordinates[order[index] - 1];
        }
        const auto outputOffset = runtimeRowMajorStorageOffset(
            outputCoordinates, outputDimensions);
        if (!outputOffset || sourceOffset >= value.cells.size()) {
            return failure("permute could not map a cell element");
        }
        cells[*outputOffset] = value.cells[sourceOffset];
    }
    return success(cellResult(outputDimensions, std::move(cells)));
}

RuntimeArrayOperationResult permutationBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 2 || !isSupportedArray(arguments.front())) {
        return failure(std::string(name) +
                       " expects an array and a dimension order");
    }
    std::string error;
    auto order = permutationOrder(arguments.front(), arguments[1], error);
    if (!order) {
        return failure(std::string(name) + " " + error);
    }
    if (name == "ipermute") {
        std::vector<size_t> inverse(order->size(), 0);
        for (size_t index = 0; index < order->size(); ++index) {
            inverse[(*order)[index] - 1] = index + 1;
        }
        order = std::move(inverse);
    }
    return permuteValue(arguments.front(), *order);
}

RuntimeArrayOperationResult squeezeBuiltin(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 1 || !isSupportedArray(arguments.front())) {
        return failure("squeeze expects one numeric or cell array");
    }
    const auto dimensions = runtimeDimensions(arguments.front());
    if (dimensions.size() <= 2) {
        return success(arguments.front());
    }

    std::vector<size_t> squeezed;
    for (const size_t dimension : dimensions) {
        if (dimension != 1) {
            squeezed.push_back(dimension);
        }
    }
    if (squeezed.empty()) {
        squeezed = {1, 1};
    } else if (squeezed.size() == 1) {
        squeezed.push_back(1);
    }
    return runtimeReshapeValue(arguments.front(), std::move(squeezed));
}

std::optional<std::vector<size_t>> repetitionFactors(
    const std::vector<RuntimeValue>& arguments, std::string& error) {
    if (arguments.size() < 2) {
        error = "repmat expects an array and repetition factors";
        return std::nullopt;
    }

    std::vector<double> rawFactors;
    if (arguments.size() == 2) {
        if (isNumber(arguments[1])) {
            rawFactors = {arguments[1].number, arguments[1].number};
        } else if (isNumericRowVector(arguments[1])) {
            const auto values = logicalNumericValues(arguments[1]);
            if (!values || values->empty()) {
                error = "repmat repetition vector cannot be empty";
                return std::nullopt;
            }
            rawFactors = *values;
            if (rawFactors.size() == 1) {
                rawFactors.push_back(rawFactors.front());
            }
        } else {
            error = "repmat repetitions must be scalars or a numeric row vector";
            return std::nullopt;
        }
    } else {
        rawFactors.reserve(arguments.size() - 1);
        for (size_t index = 1; index < arguments.size(); ++index) {
            if (!isNumber(arguments[index])) {
                error = "repmat separate repetition factors must be scalar";
                return std::nullopt;
            }
            rawFactors.push_back(arguments[index].number);
        }
    }

    std::vector<size_t> factors;
    factors.reserve(rawFactors.size());
    for (const double raw : rawFactors) {
        const auto factor = repetitionFactor(raw);
        if (!factor) {
            error = "repmat factors must be representable integers";
            return std::nullopt;
        }
        factors.push_back(*factor);
    }
    return factors;
}

RuntimeArrayOperationResult repmatBuiltin(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty() || !isSupportedArray(arguments.front())) {
        return failure("repmat supports numeric and cell arrays");
    }
    std::string error;
    const auto parsedFactors = repetitionFactors(arguments, error);
    if (!parsedFactors) {
        return failure(std::move(error));
    }

    auto sourceDimensions = runtimeDimensions(arguments.front());
    std::vector<size_t> factors = *parsedFactors;
    const size_t dimensionCount =
        std::max(sourceDimensions.size(), factors.size());
    sourceDimensions.resize(dimensionCount, 1);
    factors.resize(dimensionCount, 1);
    std::vector<size_t> outputDimensions(dimensionCount, 1);
    for (size_t index = 0; index < dimensionCount; ++index) {
        if (sourceDimensions[index] != 0 &&
            factors[index] > std::numeric_limits<size_t>::max() /
                                 sourceDimensions[index]) {
            return failure("repmat dimensions are too large");
        }
        outputDimensions[index] = sourceDimensions[index] * factors[index];
    }
    const auto outputCount =
        checkedRuntimeDimensionProduct(outputDimensions);
    if (!outputCount) {
        return failure("repmat dimensions are too large");
    }

    if (isNumeric(arguments.front())) {
        std::vector<double> elements(*outputCount, 0.0);
        for (size_t outputOffset = 0; outputOffset < *outputCount;
             ++outputOffset) {
            const auto outputCoordinates = runtimeRowMajorCoordinates(
                outputOffset, outputDimensions);
            std::vector<size_t> sourceCoordinates(dimensionCount, 0);
            for (size_t index = 0; index < dimensionCount; ++index) {
                sourceCoordinates[index] =
                    outputCoordinates[index] % sourceDimensions[index];
            }
            const auto sourceOffset = runtimeRowMajorStorageOffset(
                sourceCoordinates, sourceDimensions);
            const auto element = sourceOffset
                                     ? numericStorageElement(
                                           arguments.front(), *sourceOffset)
                                     : std::nullopt;
            if (!sourceOffset || !element) {
                return failure("repmat could not map an array element");
            }
            elements[outputOffset] = *element;
        }
        return success(numericResult(outputDimensions, std::move(elements),
                                     isNumber(arguments.front())));
    }

    std::vector<RuntimeValue> cells(*outputCount);
    for (size_t outputOffset = 0; outputOffset < *outputCount;
         ++outputOffset) {
        const auto outputCoordinates = runtimeRowMajorCoordinates(
            outputOffset, outputDimensions);
        std::vector<size_t> sourceCoordinates(dimensionCount, 0);
        for (size_t index = 0; index < dimensionCount; ++index) {
            sourceCoordinates[index] =
                outputCoordinates[index] % sourceDimensions[index];
        }
        const auto sourceOffset = runtimeRowMajorStorageOffset(
            sourceCoordinates, sourceDimensions);
        if (!sourceOffset || *sourceOffset >= arguments.front().cells.size()) {
            return failure("repmat could not map a cell element");
        }
        cells[outputOffset] = arguments.front().cells[*sourceOffset];
    }
    return success(cellResult(outputDimensions, std::move(cells)));
}

RuntimeArrayOperationResult concatenate(
    size_t dimension, const std::vector<RuntimeValue>& values) {
    if (values.empty()) {
        return failure("concatenation requires at least one array");
    }
    const bool cells = isCell(values.front());
    const bool numeric = isNumeric(values.front());
    if (!cells && !numeric) {
        return failure("concatenation supports numeric and cell arrays");
    }
    for (const auto& value : values) {
        if ((cells && !isCell(value)) || (numeric && !isNumeric(value))) {
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

    if (numeric) {
        std::vector<double> elements(*outputCount, 0.0);
        size_t axisOffset = 0;
        bool preferNumber = true;
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
                const auto element = numericStorageElement(value, sourceOffset);
                if (!outputOffset || !element) {
                    return failure(
                        "concatenation could not map a numeric element");
                }
                elements[*outputOffset] = *element;
            }
            axisOffset += inputDimensions[valueIndex][axis];
        }
        return success(numericResult(outputDimensions, std::move(elements),
                                     preferNumber));
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

RuntimeArrayOperationResult concatenateBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments) {
    if (name == "cat") {
        if (arguments.size() < 2 || !isNumber(arguments.front())) {
            return failure("cat expects a positive dimension and arrays");
        }
        const auto dimension = positiveDimension(arguments.front().number);
        if (!dimension) {
            return failure("cat dimension must be a positive integer");
        }
        return concatenate(
            *dimension,
            std::vector<RuntimeValue>(arguments.begin() + 1,
                                      arguments.end()));
    }
    return concatenate(name == "vertcat" ? 1 : 2, arguments);
}

} // namespace

bool isRuntimeArrayOperationBuiltin(std::string_view name) {
    return name == "reshape" || name == "permute" ||
           name == "ipermute" || name == "squeeze" ||
           name == "repmat" || name == "cat" || name == "horzcat" ||
           name == "vertcat";
}

RuntimeArrayOperationResult runtimeArrayOperationBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments) {
    if (name == "reshape") {
        return reshapeBuiltin(arguments);
    }
    if (name == "permute" || name == "ipermute") {
        return permutationBuiltin(name, arguments);
    }
    if (name == "squeeze") {
        return squeezeBuiltin(arguments);
    }
    if (name == "repmat") {
        return repmatBuiltin(arguments);
    }
    if (name == "cat" || name == "horzcat" || name == "vertcat") {
        return concatenateBuiltin(name, arguments);
    }
    return failure("unknown array operation builtin");
}

RuntimeArrayOperationResult runtimeReshapeValue(
    const RuntimeValue& value, std::vector<size_t> dimensions) {
    if (!isSupportedArray(value)) {
        return failure("reshape supports numeric and cell arrays");
    }
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure("reshape dimensions are too large");
    }
    if (*count != runtimeShapeElementCount(value)) {
        return failure("reshape cannot change the number of elements");
    }

    if (isNumeric(value)) {
        std::vector<double> elements(*count, 0.0);
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
                                     ? numericStorageElement(value,
                                                             *sourceOffset)
                                     : std::nullopt;
            if (!sourceOffset || !targetOffset || !element) {
                return failure("reshape could not preserve logical element order");
            }
            elements[*targetOffset] = *element;
        }
        return success(numericResult(dimensions, std::move(elements),
                                     isNumber(value)));
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
            return failure("reshape could not preserve logical cell order");
        }
        cells[*targetOffset] = value.cells[*sourceOffset];
    }
    return success(cellResult(dimensions, std::move(cells)));
}

} // namespace mparser
