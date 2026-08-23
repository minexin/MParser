#include "mparser/runtime/builtins/array/runtime_array_ops.h"

#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_object.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"

#include <algorithm>
#include <bit>
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

bool isMissingArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::MissingArray;
}

bool isSupportedArray(const RuntimeValue& value) {
    return isMissingArray(value) || isNumeric(value) || isCell(value) ||
           isRuntimeTextValue(value) || isRuntimeClassObject(value);
}

RuntimeArrayOperationResult failure(std::string message) {
    return RuntimeArrayOperationResult{false, RuntimeValue{},
                                       std::move(message)};
}

RuntimeArrayOperationResult success(RuntimeValue value) {
    return RuntimeArrayOperationResult{true, std::move(value), {}};
}

RuntimeArrayOutputsResult outputsFailure(std::string message) {
    return RuntimeArrayOutputsResult{false, {}, std::move(message)};
}

RuntimeArrayOutputsResult outputsSuccess(
    std::vector<RuntimeValue> outputs) {
    return RuntimeArrayOutputsResult{true, std::move(outputs), {}};
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

std::optional<std::vector<RuntimeNumericElementValue>>
logicalRealNumericValues(
    const RuntimeValue& value) {
    if (!isNumeric(value)) {
        return std::nullopt;
    }

    const size_t count = runtimeShapeElementCount(value);
    std::vector<RuntimeNumericElementValue> result;
    result.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        const auto element =
            runtimeNumericElementValue(value, logicalIndex);
        if (!element || element->complex) {
            return std::nullopt;
        }
        result.push_back(*element);
    }
    return result;
}

bool isNumericRowVector(const RuntimeValue& value) {
    return isNumericArray(value) && runtimeDimension(value, 0) == 1;
}

std::optional<RuntimeNumericElementValue> realNumericScalar(
    const RuntimeValue& value) {
    if (!isNumber(value)) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    return element && !element->complex ? element : std::nullopt;
}

std::optional<size_t> nonnegativeDimension(
    const RuntimeNumericElementValue& raw) {
    return runtimeNumericElementAsNonnegativeSize(raw);
}

std::optional<size_t> positiveDimension(
    const RuntimeNumericElementValue& raw) {
    const auto dimension = runtimeNumericElementAsNonnegativeSize(raw);
    if (!dimension || *dimension == 0) {
        return std::nullopt;
    }
    return dimension;
}

std::optional<size_t> repetitionFactor(
    const RuntimeNumericElementValue& raw) {
    if (raw.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(raw.numericClass)) {
        if (runtimeNumericClassIsSignedInteger(raw.numericClass) &&
            std::bit_cast<std::int64_t>(raw.integerRealBits) < 0) {
            return size_t{0};
        }
        return runtimeNumericElementAsNonnegativeSize(raw);
    }
    if (!std::isfinite(raw.real) ||
        std::floor(raw.real) != raw.real) {
        return std::nullopt;
    }
    if (raw.real <= 0.0) {
        return size_t{0};
    }
    return runtimeNumericElementAsNonnegativeSize(raw);
}

std::optional<std::vector<size_t>> constructorShape(
    std::string_view name,
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        if (name == "cell") {
            return std::vector<size_t>{0, 0};
        }
        return std::vector<size_t>{1, 1};
    }

    const auto appendDimension = [](
                                     std::vector<size_t>& dimensions,
                                     const RuntimeNumericElementValue& raw) {
        const auto dimension = nonnegativeDimension(raw);
        if (!dimension) {
            return false;
        }
        dimensions.push_back(*dimension);
        return true;
    };

    if (arguments.size() > 1) {
        std::vector<size_t> dimensions;
        dimensions.reserve(arguments.size());
        for (const RuntimeValue& argument : arguments) {
            const auto raw = realNumericScalar(argument);
            if (!raw || !appendDimension(dimensions, *raw)) {
                return std::nullopt;
            }
        }
        return normalizeRuntimeDimensions(std::move(dimensions));
    }

    const RuntimeValue& shape = arguments.front();
    if (!isNumeric(shape)) {
        return std::nullopt;
    }
    const size_t count = runtimeShapeElementCount(shape);
    if (count == 0) {
        return std::vector<size_t>{0, 0};
    }

    std::vector<size_t> dimensions;
    dimensions.reserve(count == 1 ? 2 : count);
    for (size_t index = 0; index < count; ++index) {
        const auto raw = runtimeNumericElementValue(shape, index);
        if (!raw || raw->complex ||
            !appendDimension(dimensions, *raw)) {
            return std::nullopt;
        }
    }
    if (dimensions.size() == 1) {
        dimensions.push_back(dimensions.front());
    }
    return normalizeRuntimeDimensions(std::move(dimensions));
}

std::optional<RuntimeNumericClass> constructorNumericClass(
    std::string_view name, std::vector<RuntimeValue>& arguments) {
    if (name == "true" || name == "false") {
        return RuntimeNumericClass::Logical;
    }
    if (name == "cell" || name == "strings") {
        return RuntimeNumericClass::Double;
    }
    if (arguments.empty()) {
        return RuntimeNumericClass::Double;
    }

    const auto className = runtimeTextScalarUtf8(arguments.back());
    if (!className) {
        return RuntimeNumericClass::Double;
    }
    const auto numericClass = runtimeNumericClassFromName(*className);
    if (!numericClass) {
        return std::nullopt;
    }
    if ((name == "inf" || name == "nan") &&
        !runtimeNumericClassIsFloating(*numericClass)) {
        return std::nullopt;
    }
    arguments.pop_back();
    return numericClass;
}

RuntimeArrayOperationResult numericConstructor(
    std::string_view name, std::vector<RuntimeValue> arguments) {
    const auto numericClass = constructorNumericClass(name, arguments);
    if (!numericClass) {
        return failure(std::string(name) +
                       " trailing class name is not supported");
    }
    const auto dimensions = constructorShape(name, arguments);
    if (!dimensions) {
        return failure(std::string(name) +
                       " dimensions must be real, representable nonnegative integers");
    }
    if (name == "eye" && dimensions->size() > 2) {
        return failure("eye creates only two-dimensional arrays");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure(std::string(name) + " dimensions are too large");
    }

    RuntimeNumericElementValue fill;
    fill.real = name == "ones" || name == "true"
                    ? 1.0
                : name == "inf"
                    ? std::numeric_limits<double>::infinity()
                : name == "nan"
                    ? std::numeric_limits<double>::quiet_NaN()
                    : 0.0;
    std::vector<RuntimeNumericElementValue> elements(*count, fill);
    if (name == "eye") {
        const size_t rows = dimensions->front();
        const size_t columns = (*dimensions)[1];
        for (size_t index = 0; index < std::min(rows, columns);
             ++index) {
            elements[index * (rows + 1)].real = 1.0;
        }
    }

    auto result = runtimeNumericValueFromElements(
        *dimensions, std::move(elements), *numericClass);
    return result ? success(std::move(*result))
                  : failure(std::string(name) +
                            " could not construct the requested numeric class");
}

std::optional<RuntimeNumericElementValue> numericScalar(
    const RuntimeValue& value) {
    if (!isNumeric(value) || runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericElementValue(value, 0);
}

RuntimeArrayOperationResult reshapeBuiltin(
    const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (arguments.size() < 2) {
        return failure("reshape expects an array and at least two dimensions");
    }
    if (!isSupportedArray(arguments.front())) {
        return failure(
            "reshape supports missing, numeric, text, cell, and object arrays");
    }

    std::vector<std::optional<size_t>> requested;
    if (arguments.size() == 2) {
        const RuntimeValue& shape = arguments[1];
        if (!isNumericRowVector(shape)) {
            return failure("reshape size must be a numeric row vector");
        }
        const auto values = logicalRealNumericValues(shape);
        if (!values || values->size() < 2) {
            return failure(
                "reshape size vector must contain at least two real dimensions");
        }
        requested.reserve(values->size());
        for (const auto& raw : *values) {
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
            const auto raw = realNumericScalar(argument);
            if (!raw) {
                return failure(
                    "reshape dimensions must be real scalar numbers or one empty value");
            }
            const auto dimension = nonnegativeDimension(*raw);
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

    return runtimeReshapeValue(
        arguments.front(), std::move(dimensions), objectPolicy);
}

std::optional<std::vector<size_t>> permutationOrder(
    const RuntimeValue& value, const RuntimeValue& orderValue,
    std::string& error) {
    if (!isNumericRowVector(orderValue)) {
        error = "dimension order must be a numeric row vector";
        return std::nullopt;
    }
    const auto values = logicalRealNumericValues(orderValue);
    if (!values || values->size() < runtimeDimensionCount(value)) {
        error = "dimension order must include every input dimension";
        return std::nullopt;
    }

    std::vector<size_t> order;
    order.reserve(values->size());
    for (const auto& raw : *values) {
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
    const RuntimeValue& value, const std::vector<size_t>& order,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (!isSupportedArray(value)) {
        return failure(
            "permute supports missing, numeric, text, cell, and object arrays");
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

    if (isMissingArray(value)) {
        return success(makeRuntimeMissingArrayValue(
            std::move(outputDimensions)));
    }

    if (isNumeric(value)) {
        std::vector<RuntimeNumericElementValue> elements(*count);
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
            const auto element = runtimeNumericStorageElementValue(
                value, sourceOffset);
            if (!outputOffset || !element) {
                return failure("permute could not map an array element");
            }
            elements[*outputOffset] = *element;
        }
        auto result = numericResult(
            outputDimensions, std::move(elements), isNumber(value),
            value.numericClass);
        return result
                   ? success(std::move(*result))
                   : failure("permute could not construct a numeric result");
    }

    if (isRuntimeCharacterArray(value)) {
        std::u16string elements(*count, u'\0');
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
            if (!outputOffset ||
                sourceOffset >= value.characterElements.size()) {
                return failure("permute could not map a character element");
            }
            elements[*outputOffset] = value.characterElements[sourceOffset];
        }
        return success(makeRuntimeCharacterArray(
            outputDimensions, std::move(elements)));
    }

    if (isRuntimeStringArray(value)) {
        std::vector<RuntimeStringElement> elements(*count);
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
            if (!outputOffset || sourceOffset >= value.stringElements.size()) {
                return failure("permute could not map a string element");
            }
            elements[*outputOffset] = value.stringElements[sourceOffset];
        }
        return success(makeRuntimeStringArray(
            outputDimensions, std::move(elements)));
    }

    if (isRuntimeClassObject(value)) {
        std::vector<RuntimeValue> elements(*count);
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
            const auto* element = runtimeObjectElement(value, sourceOffset);
            if (!outputOffset || !element) {
                return failure("permute could not map an object element");
            }
            elements[*outputOffset] = *element;
        }
        auto result = runtimeMakeObjectArrayFromStorageOrder(
            std::move(elements), outputDimensions, value.className,
            value.handleObject, objectPolicy, value.className);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
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
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
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
    return permuteValue(arguments.front(), *order, objectPolicy);
}

size_t firstNonsingletonDimension(const RuntimeValue& value) {
    const auto dimensions = runtimeDimensions(value);
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] != 1) {
            return index + 1;
        }
    }
    return 1;
}

RuntimeArrayOperationResult flipValue(
    const RuntimeValue& value, size_t dimension,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (!isSupportedArray(value)) {
        return failure(
            "flip supports missing, numeric, text, cell, and object arrays");
    }
    auto dimensions = runtimeDimensions(value);
    dimensions.resize(std::max(dimensions.size(), dimension), 1);
    const size_t axis = dimension - 1;
    const size_t count = runtimeShapeElementCount(value);
    if (dimensions[axis] <= 1 || count == 0 || isMissingArray(value)) {
        return success(value);
    }

    const auto sourceLogicalIndex =
        [&dimensions, axis](size_t outputLogical) -> std::optional<size_t> {
        auto coordinates = runtimeColumnMajorCoordinates(
            outputLogical, dimensions);
        if (!coordinates) {
            return std::nullopt;
        }
        (*coordinates)[axis] =
            dimensions[axis] - 1 - (*coordinates)[axis];
        return runtimeColumnMajorLinearIndex(*coordinates, dimensions);
    };

    if (isNumeric(value)) {
        RuntimeValue output = value;
        for (size_t logical = 0; logical < count; ++logical) {
            const auto source = sourceLogicalIndex(logical);
            const auto element = source
                                     ? runtimeNumericElementValue(value,
                                                                  *source)
                                     : std::nullopt;
            if (!source || !element ||
                !runtimeStoreNumericElementValue(output, logical,
                                                 *element)) {
                return failure("flip could not map a numeric element");
            }
        }
        return success(std::move(output));
    }

    if (isRuntimeCharacterArray(value)) {
        RuntimeValue output = value;
        for (size_t logical = 0; logical < count; ++logical) {
            const auto source = sourceLogicalIndex(logical);
            const auto targetOffset =
                runtimeColumnMajorLinearToStorageOffset(output, logical);
            const auto element = source
                                     ? runtimeCharacterElement(value,
                                                               *source)
                                     : std::nullopt;
            if (!source || !targetOffset || !element ||
                *targetOffset >= output.characterElements.size()) {
                return failure("flip could not map a character element");
            }
            output.characterElements[*targetOffset] = *element;
        }
        return success(std::move(output));
    }

    if (isRuntimeStringArray(value)) {
        RuntimeValue output = value;
        for (size_t logical = 0; logical < count; ++logical) {
            const auto source = sourceLogicalIndex(logical);
            const auto targetOffset =
                runtimeColumnMajorLinearToStorageOffset(output, logical);
            const auto* element = source
                                      ? runtimeStringElement(value, *source)
                                      : nullptr;
            if (!source || !targetOffset || !element ||
                *targetOffset >= output.stringElements.size()) {
                return failure("flip could not map a string element");
            }
            output.stringElements[*targetOffset] = *element;
        }
        return success(std::move(output));
    }

    if (isRuntimeClassObject(value)) {
        std::vector<RuntimeValue> elements(count);
        for (size_t logical = 0; logical < count; ++logical) {
            const auto source = sourceLogicalIndex(logical);
            const auto sourceOffset = source
                ? runtimeColumnMajorLinearToStorageOffset(value, *source)
                : std::nullopt;
            const auto targetOffset =
                runtimeColumnMajorLinearToStorageOffset(value, logical);
            const auto* element = sourceOffset
                                      ? runtimeObjectElement(value,
                                                             *sourceOffset)
                                      : nullptr;
            if (!source || !sourceOffset || !targetOffset || !element ||
                *targetOffset >= elements.size()) {
                return failure("flip could not map an object element");
            }
            elements[*targetOffset] = *element;
        }
        auto result = runtimeMakeObjectArrayFromStorageOrder(
            std::move(elements), runtimeDimensions(value), value.className,
            value.handleObject, objectPolicy, value.className);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }

    RuntimeValue output = value;
    for (size_t logical = 0; logical < count; ++logical) {
        const auto source = sourceLogicalIndex(logical);
        const auto sourceOffset = source
            ? runtimeColumnMajorLinearToStorageOffset(value, *source)
            : std::nullopt;
        const auto targetOffset =
            runtimeColumnMajorLinearToStorageOffset(output, logical);
        if (!source || !sourceOffset || !targetOffset ||
            *sourceOffset >= value.cells.size() ||
            *targetOffset >= output.cells.size()) {
            return failure("flip could not map a cell element");
        }
        output.cells[*targetOffset] = value.cells[*sourceOffset];
    }
    return success(std::move(output));
}

RuntimeArrayOperationResult flipBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    const bool generic = name == "flip";
    if (arguments.empty() || arguments.size() > (generic ? 2U : 1U)) {
        return failure(std::string(name) +
                       (generic
                            ? " expects an array and optional dimension"
                            : " expects one array"));
    }
    size_t dimension = name == "flipud"
                           ? 1
                           : name == "fliplr"
                               ? 2
                               : firstNonsingletonDimension(
                                     arguments.front());
    if (generic && arguments.size() == 2) {
        const auto rawDimension = realNumericScalar(arguments[1]);
        const auto selected = rawDimension
                                  ? positiveDimension(*rawDimension)
                                  : std::nullopt;
        if (!selected) {
            return failure("flip dimension must be a positive integer scalar");
        }
        dimension = *selected;
    }
    return flipValue(arguments.front(), dimension, objectPolicy);
}

RuntimeArrayOperationResult squeezeBuiltin(
    const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (arguments.size() != 1 || !isSupportedArray(arguments.front())) {
        return failure(
            "squeeze expects one missing, numeric, text, cell, or object array");
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
    return runtimeReshapeValue(
        arguments.front(), std::move(squeezed), objectPolicy);
}

std::optional<std::vector<size_t>> repetitionFactors(
    const std::vector<RuntimeValue>& arguments, std::string& error) {
    if (arguments.size() < 2) {
        error = "repmat expects an array and repetition factors";
        return std::nullopt;
    }

    std::vector<RuntimeNumericElementValue> rawFactors;
    if (arguments.size() == 2) {
        if (const auto scalar = realNumericScalar(arguments[1])) {
            rawFactors = {*scalar, *scalar};
        } else if (isNumericRowVector(arguments[1])) {
            const auto values = logicalRealNumericValues(arguments[1]);
            if (!values || values->empty()) {
                error =
                    "repmat repetition vector must contain real values";
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
            const auto scalar = realNumericScalar(arguments[index]);
            if (!scalar) {
                error =
                    "repmat separate repetition factors must be real scalars";
                return std::nullopt;
            }
            rawFactors.push_back(*scalar);
        }
    }

    std::vector<size_t> factors;
    factors.reserve(rawFactors.size());
    for (const auto& raw : rawFactors) {
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
    const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (arguments.empty() || !isSupportedArray(arguments.front())) {
        return failure(
            "repmat supports missing, numeric, text, cell, and object arrays");
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

    if (isMissingArray(arguments.front())) {
        return success(makeRuntimeMissingArrayValue(
            std::move(outputDimensions)));
    }

    if (isNumeric(arguments.front())) {
        std::vector<RuntimeNumericElementValue> elements(*outputCount);
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
                                     ? runtimeNumericStorageElementValue(
                                           arguments.front(), *sourceOffset)
                                     : std::nullopt;
            if (!sourceOffset || !element) {
                return failure("repmat could not map an array element");
            }
            elements[outputOffset] = *element;
        }
        auto result = numericResult(
            outputDimensions, std::move(elements),
            isNumber(arguments.front()),
            arguments.front().numericClass);
        return result
                   ? success(std::move(*result))
                   : failure("repmat could not construct a numeric result");
    }

    if (isRuntimeCharacterArray(arguments.front())) {
        std::u16string elements(*outputCount, u'\0');
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
            if (!sourceOffset ||
                *sourceOffset >= arguments.front().characterElements.size()) {
                return failure("repmat could not map a character element");
            }
            elements[outputOffset] =
                arguments.front().characterElements[*sourceOffset];
        }
        return success(makeRuntimeCharacterArray(
            outputDimensions, std::move(elements)));
    }

    if (isRuntimeStringArray(arguments.front())) {
        std::vector<RuntimeStringElement> elements(*outputCount);
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
            if (!sourceOffset ||
                *sourceOffset >= arguments.front().stringElements.size()) {
                return failure("repmat could not map a string element");
            }
            elements[outputOffset] =
                arguments.front().stringElements[*sourceOffset];
        }
        return success(makeRuntimeStringArray(
            outputDimensions, std::move(elements)));
    }

    if (isRuntimeClassObject(arguments.front())) {
        std::vector<RuntimeValue> elements(*outputCount);
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
            const auto* element = sourceOffset
                                      ? runtimeObjectElement(
                                            arguments.front(), *sourceOffset)
                                      : nullptr;
            if (!sourceOffset || !element) {
                return failure("repmat could not map an object element");
            }
            elements[outputOffset] = *element;
        }
        auto result = runtimeMakeObjectArrayFromStorageOrder(
            std::move(elements), outputDimensions,
            arguments.front().className,
            arguments.front().handleObject, objectPolicy,
            arguments.front().className);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
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
    size_t dimension, const std::vector<RuntimeValue>& values,
    const RuntimeObjectArrayPolicy& objectPolicy) {
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
            return concatenate(dimension, nonempty, objectPolicy);
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
            return concatenate(dimension, converted, objectPolicy);
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
            return concatenate(dimension, converted, objectPolicy);
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

RuntimeArrayOperationResult concatenateBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (name == "cat") {
        if (arguments.size() < 2) {
            return failure("cat expects a positive dimension and arrays");
        }
        const auto rawDimension = realNumericScalar(arguments.front());
        const auto dimension = rawDimension
                                   ? positiveDimension(*rawDimension)
                                   : std::nullopt;
        if (!dimension) {
            return failure(
                "cat dimension must be a positive real integer");
        }
        return concatenate(
            *dimension,
            std::vector<RuntimeValue>(arguments.begin() + 1,
                                      arguments.end()),
            objectPolicy);
    }
    return concatenate(
        name == "vertcat" ? 1 : 2, arguments, objectPolicy);
}

} // namespace

bool isRuntimeArrayOperationBuiltin(std::string_view name) {
    return name == "reshape" || name == "permute" ||
           name == "ipermute" || name == "squeeze" ||
           name == "repmat" || name == "flip" ||
           name == "flipud" || name == "fliplr" ||
           name == "cat" || name == "horzcat" || name == "vertcat";
}

RuntimeArrayOperationResult runtimeArrayOperationBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    const RuntimeObjectArrayPolicy& objectPolicy) {
    if (name == "reshape") {
        return reshapeBuiltin(arguments, objectPolicy);
    }
    if (name == "permute" || name == "ipermute") {
        return permutationBuiltin(name, arguments, objectPolicy);
    }
    if (name == "flip" || name == "flipud" || name == "fliplr") {
        return flipBuiltin(name, arguments, objectPolicy);
    }
    if (name == "squeeze") {
        return squeezeBuiltin(arguments, objectPolicy);
    }
    if (name == "repmat") {
        return repmatBuiltin(arguments, objectPolicy);
    }
    if (name == "cat" || name == "horzcat" || name == "vertcat") {
        return concatenateBuiltin(name, arguments, objectPolicy);
    }
    return failure("unknown array operation builtin");
}

bool isRuntimeArrayConstructorBuiltin(std::string_view name) {
    return name == "zeros" || name == "ones" || name == "eye" ||
           name == "true" || name == "false" || name == "cell" ||
           name == "strings" || name == "inf" || name == "nan";
}

RuntimeArrayOperationResult runtimeArrayConstructorBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments) {
    if (!isRuntimeArrayConstructorBuiltin(name)) {
        return failure("unknown array constructor builtin");
    }
    if (name != "cell" && name != "strings") {
        return numericConstructor(name, arguments);
    }

    const auto dimensions = constructorShape(name, arguments);
    if (!dimensions) {
        return failure(std::string(name) +
                       " dimensions must be real, representable nonnegative integers");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure(std::string(name) + " dimensions are too large");
    }
    if (name == "cell") {
        return success(cellResult(
            *dimensions, std::vector<RuntimeValue>(*count)));
    }
    return success(makeRuntimeStringArray(
        *dimensions, std::vector<RuntimeStringElement>(*count)));
}

RuntimeArrayOperationResult runtimeLinspaceBuiltin(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 2 && arguments.size() != 3) {
        return failure("linspace expects two or three scalar numeric arguments");
    }
    const auto start = numericScalar(arguments[0]);
    const auto stop = numericScalar(arguments[1]);
    if (!start || !stop) {
        return failure("linspace endpoints must be scalar numeric values");
    }

    size_t count = 100;
    if (arguments.size() == 3) {
        const auto requested = numericScalar(arguments[2]);
        const auto converted = requested
                                   ? runtimeNumericElementAsNonnegativeSize(
                                         *requested)
                                   : std::nullopt;
        if (!converted) {
            return failure(
                "linspace count must be a real, representable nonnegative integer");
        }
        count = *converted;
    }

    const RuntimeNumericClass resultClass =
        start->numericClass == RuntimeNumericClass::Single ||
                stop->numericClass == RuntimeNumericClass::Single
            ? RuntimeNumericClass::Single
            : RuntimeNumericClass::Double;
    const bool complex = start->complex || stop->complex;
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(count);
    if (count == 1) {
        elements.push_back(*stop);
    } else if (count > 1) {
        const double denominator = static_cast<double>(count - 1);
        for (size_t index = 0; index < count; ++index) {
            RuntimeNumericElementValue value;
            value.numericClass = resultClass;
            value.complex = complex;
            if (index == 0) {
                value.real = start->real;
                value.imaginary = start->imaginary;
            } else if (index + 1 == count) {
                value.real = stop->real;
                value.imaginary = stop->imaginary;
            } else {
                const double t = static_cast<double>(index) / denominator;
                value.real = start->real + (stop->real - start->real) * t;
                value.imaginary =
                    start->imaginary +
                    (stop->imaginary - start->imaginary) * t;
            }
            elements.push_back(value);
        }
    }

    auto result = runtimeNumericValueFromElements(
        {1, count}, std::move(elements), resultClass);
    if (!result) {
        return failure("linspace could not construct its numeric result");
    }
    if (count == 0 && complex) {
        result->numericComplex = true;
    }
    return success(std::move(*result));
}

RuntimeArrayOutputsResult runtimeSizeBuiltin(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (arguments.empty() || arguments.size() > 2) {
        return outputsFailure("size expects an array and optional dimension");
    }
    if (requestedOutputCount == 0) {
        return outputsSuccess({});
    }

    const RuntimeValue& value = arguments.front();
    const auto dimensions = runtimeDimensions(value);
    if (arguments.size() == 2) {
        if (requestedOutputCount != 1) {
            return outputsFailure(
                "size with a dimension produces one output");
        }
        if (!isNumeric(arguments[1])) {
            return outputsFailure("size dimensions must be numeric");
        }

        const size_t count = runtimeShapeElementCount(arguments[1]);
        std::vector<double> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto raw = runtimeNumericElementValue(
                arguments[1], index);
            const auto dimension = raw && !raw->complex
                                       ? positiveDimension(*raw)
                                       : std::nullopt;
            if (!dimension) {
                return outputsFailure(
                    "size dimension must be a positive real integer");
            }
            values.push_back(static_cast<double>(
                runtimeDimension(value, *dimension - 1)));
        }
        auto output = runtimeNumericValueFromLogicalOrder(
            {1, count}, std::move(values), RuntimeNumericClass::Double);
        return output
                   ? outputsSuccess({std::move(*output)})
                   : outputsFailure("size could not construct its result");
    }

    if (requestedOutputCount == 1) {
        std::vector<double> values;
        values.reserve(dimensions.size());
        for (const size_t dimension : dimensions) {
            values.push_back(static_cast<double>(dimension));
        }
        auto output = runtimeNumericValueFromLogicalOrder(
            {1, dimensions.size()}, std::move(values),
            RuntimeNumericClass::Double);
        return output
                   ? outputsSuccess({std::move(*output)})
                   : outputsFailure("size could not construct its result");
    }

    std::vector<RuntimeValue> outputs;
    outputs.reserve(requestedOutputCount);
    for (size_t index = 0; index < requestedOutputCount; ++index) {
        size_t dimension = 1;
        if (index + 1 == requestedOutputCount &&
            requestedOutputCount < dimensions.size()) {
            const std::vector<size_t> folded(
                dimensions.begin() + index, dimensions.end());
            const auto product = checkedRuntimeDimensionProduct(folded);
            if (!product) {
                return outputsFailure("size dimensions are too large");
            }
            dimension = *product;
        } else if (index < dimensions.size()) {
            dimension = dimensions[index];
        }
        outputs.push_back(
            makeRuntimeNumberValue(static_cast<double>(dimension)));
    }
    return outputsSuccess(std::move(outputs));
}

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
        return success(makeRuntimeMissingArrayValue(
            std::move(dimensions)));
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
                return failure("reshape could not preserve logical element order");
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
            return failure("reshape could not preserve logical cell order");
        }
        cells[*targetOffset] = value.cells[*sourceOffset];
    }
    return success(cellResult(dimensions, std::move(cells)));
}

} // namespace mparser
