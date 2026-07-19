#include "mparser/runtime_index.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <utility>

namespace mparser {
namespace {

RuntimeIndexSelectionResult failure(std::string message) {
    return RuntimeIndexSelectionResult{false, false, {},
                                       std::move(message)};
}

RuntimeIndexOperationResult operationFailure(std::string message) {
    return RuntimeIndexOperationResult{false, {}, std::move(message)};
}

RuntimeIndexOperationResult operationSuccess(RuntimeValue value) {
    return RuntimeIndexOperationResult{true, std::move(value), {}};
}

bool isVectorShape(const RuntimeValue& value) {
    const auto dimensions = runtimeDimensions(value);
    return dimensions.size() == 2 &&
           (dimensions[0] == 1 || dimensions[1] == 1);
}

std::vector<size_t> vectorDimensionsLike(const RuntimeValue& value,
                                         size_t count) {
    return runtimeDimension(value, 0) == 1
               ? std::vector<size_t>{1, count}
               : std::vector<size_t>{count, 1};
}

} // namespace

RuntimeIndexSelectionResult runtimeResolveIndexSelection(
    const RuntimeValue& subscript, size_t extent,
    bool allowNumericGrowth) {
    if (!isRuntimeNumericValue(subscript)) {
        return failure("indexing requires numeric or logical subscripts");
    }

    RuntimeIndexSelectionResult result;
    result.succeeded = true;
    result.logicalMask = isRuntimeLogical(subscript);
    const size_t count = runtimeShapeElementCount(subscript);
    result.indices.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        const auto raw = runtimeNumericElement(subscript, logicalIndex);
        if (!raw) {
            return failure("index subscript has an invalid shape");
        }

        if (result.logicalMask) {
            if (*raw == 0.0) {
                continue;
            }
            if (logicalIndex >= extent) {
                return failure(
                    "logical index contains a true value outside the target extent");
            }
            result.indices.push_back(logicalIndex);
            continue;
        }

        const auto oneBasedIndex = checkedRuntimeNonnegativeInteger(*raw);
        if (!oneBasedIndex || *oneBasedIndex == 0) {
            return failure("index must be a positive integer");
        }
        if (!allowNumericGrowth && *oneBasedIndex > extent) {
            return failure("index is out of bounds");
        }
        result.indices.push_back(*oneBasedIndex - 1);
    }
    return result;
}

RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts) {
    if (!isRuntimeNumericValue(target)) {
        return operationFailure("indexing requires a numeric target");
    }
    if (subscripts.empty()) {
        return operationFailure("indexing requires subscripts");
    }

    std::vector<std::vector<size_t>> selections;
    selections.reserve(subscripts.size());
    const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        target, subscripts.size());
    for (size_t index = 0; index < subscripts.size(); ++index) {
        const size_t extent = subscripts.size() == 1
                                  ? runtimeShapeElementCount(target)
                                  : effectiveDimensions[index];
        auto selection = runtimeResolveIndexSelection(
            subscripts[index], extent, false);
        if (!selection.succeeded) {
            return operationFailure(std::move(selection.error));
        }
        selections.push_back(std::move(selection.indices));
    }

    std::vector<size_t> resultDimensions;
    if (selections.size() == 1) {
        const auto selection = runtimeResolveIndexSelection(
            subscripts.front(), runtimeShapeElementCount(target), false);
        resultDimensions = runtimeLinearIndexResultDimensions(
            target, subscripts.front(), selection.indices.size(),
            selection.logicalMask);
    } else {
        resultDimensions.reserve(selections.size());
        for (const auto& selection : selections) {
            resultDimensions.push_back(selection.size());
        }
    }

    const auto resultCount =
        checkedRuntimeDimensionProduct(resultDimensions);
    if (!resultCount) {
        return operationFailure("indexed result dimensions are too large");
    }

    std::vector<double> values;
    values.reserve(*resultCount);
    if (selections.size() == 1) {
        for (const size_t logicalIndex : selections.front()) {
            const auto value = runtimeNumericElement(target, logicalIndex);
            if (!value) {
                return operationFailure(
                    "indexing could not read a linear element");
            }
            values.push_back(*value);
        }
    } else {
        for (size_t ordinal = 0; ordinal < *resultCount; ++ordinal) {
            const auto selectionCoordinates =
                runtimeColumnMajorCoordinates(ordinal, resultDimensions);
            if (!selectionCoordinates) {
                return operationFailure(
                    "indexed result has an invalid shape");
            }
            std::vector<size_t> sourceCoordinates(subscripts.size(), 0);
            for (size_t index = 0; index < subscripts.size(); ++index) {
                sourceCoordinates[index] =
                    selections[index][(*selectionCoordinates)[index]];
            }
            const auto sourceLogicalIndex = runtimeColumnMajorLinearIndex(
                sourceCoordinates, effectiveDimensions);
            const auto value = sourceLogicalIndex
                                   ? runtimeNumericElement(
                                         target, *sourceLogicalIndex)
                                   : std::nullopt;
            if (!value) {
                return operationFailure(
                    "indexing could not map subscripts");
            }
            values.push_back(*value);
        }
    }

    const auto result = runtimeNumericValueFromLogicalOrder(
        std::move(resultDimensions), std::move(values),
        target.numericClass);
    if (!result) {
        return operationFailure("indexed result has an invalid shape");
    }
    return operationSuccess(*result);
}

std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask) {
    const bool targetIsVector = isVectorShape(target);
    const bool subscriptIsVector = isVectorShape(subscript);
    if (targetIsVector && subscriptIsVector) {
        return vectorDimensionsLike(target, resultElementCount);
    }
    if (logicalMask) {
        return {resultElementCount, 1};
    }
    return runtimeDimensions(subscript);
}

} // namespace mparser
