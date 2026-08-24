#include "mparser/runtime/core/indexing/runtime_index.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <limits>
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
        const auto element =
            runtimeNumericElementValue(subscript, logicalIndex);
        if (!element) {
            return failure("index subscript has an invalid shape");
        }
        if (element->complex) {
            return failure("index subscript must be real");
        }

        if (result.logicalMask) {
            if (element->real == 0.0) {
                continue;
            }
            if (logicalIndex >= extent) {
                return failure(
                    "logical index contains a true value outside the target extent");
            }
            result.indices.push_back(logicalIndex);
            continue;
        }

        const auto oneBasedIndex =
            runtimeNumericElementAsNonnegativeSize(*element);
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

RuntimeIndexSelectionsResult runtimeResolveIndexSelections(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool allowNumericGrowth) {
    return runtimeResolveIndexSelections(target, subscripts,
                                         allowNumericGrowth, false);
}

RuntimeIndexSelectionsResult runtimeResolveIndexSelections(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool allowNumericGrowth,
    bool linearColon) {
    RuntimeIndexSelectionsResult result;
    if (subscripts.empty()) {
        result.error = "indexing requires subscripts";
        return result;
    }

    result.effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        target, subscripts.size());
    result.indices.reserve(subscripts.size());
    for (size_t index = 0; index < subscripts.size(); ++index) {
        const size_t extent = subscripts.size() == 1
                                  ? runtimeShapeElementCount(target)
                                  : result.effectiveDimensions[index];
        auto selection = runtimeResolveIndexSelection(
            subscripts[index], extent, allowNumericGrowth);
        if (!selection.succeeded) {
            result.error = std::move(selection.error);
            return result;
        }
        if (subscripts.size() == 1) {
            result.logicalMask = selection.logicalMask;
        }
        result.indices.push_back(std::move(selection.indices));
    }

    if (subscripts.size() == 1) {
        result.resultDimensions = runtimeLinearIndexResultDimensions(
            target, subscripts.front(), result.indices.front().size(),
            result.logicalMask, linearColon);
    } else {
        result.resultDimensions.reserve(result.indices.size());
        for (const auto& selection : result.indices) {
            result.resultDimensions.push_back(selection.size());
        }
    }
    result.succeeded = true;
    return result;
}

std::optional<size_t> runtimeIndexSelectionSourceLogicalIndex(
    const RuntimeIndexSelectionsResult& selections,
    size_t resultLogicalIndex) {
    if (!selections.succeeded || selections.indices.empty()) {
        return std::nullopt;
    }
    if (selections.indices.size() == 1) {
        return resultLogicalIndex < selections.indices.front().size()
                   ? std::optional<size_t>(
                         selections.indices.front()[resultLogicalIndex])
                   : std::nullopt;
    }

    const auto coordinates = runtimeColumnMajorCoordinates(
        resultLogicalIndex, selections.resultDimensions);
    if (!coordinates) {
        return std::nullopt;
    }
    std::vector<size_t> sourceCoordinates(selections.indices.size(), 0);
    for (size_t index = 0; index < selections.indices.size(); ++index) {
        if ((*coordinates)[index] >= selections.indices[index].size()) {
            return std::nullopt;
        }
        sourceCoordinates[index] =
            selections.indices[index][(*coordinates)[index]];
    }
    return runtimeColumnMajorLinearIndex(
        sourceCoordinates, selections.effectiveDimensions);
}

std::optional<size_t> runtimeIndexSelectionRequiredExtent(
    const std::vector<size_t>& selection) {
    if (selection.empty()) {
        return std::nullopt;
    }
    const size_t maximum =
        *std::max_element(selection.begin(), selection.end());
    return maximum == std::numeric_limits<size_t>::max()
               ? std::nullopt
               : std::optional<size_t>(maximum + 1);
}

RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts) {
    return runtimeIndexNumeric(target, subscripts, false);
}

RuntimeIndexOperationResult runtimeIndexNumeric(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon) {
    if (!isRuntimeNumericValue(target)) {
        return operationFailure("indexing requires a numeric target");
    }
    if (subscripts.empty()) {
        return operationFailure("indexing requires subscripts");
    }

    const auto selections =
        runtimeResolveIndexSelections(target, subscripts, false,
                                      linearColon);
    if (!selections.succeeded) {
        return operationFailure(selections.error);
    }

    const auto resultCount =
        checkedRuntimeDimensionProduct(selections.resultDimensions);
    if (!resultCount) {
        return operationFailure("indexed result dimensions are too large");
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(*resultCount);
    for (size_t ordinal = 0; ordinal < *resultCount; ++ordinal) {
        const auto sourceLogicalIndex =
            runtimeIndexSelectionSourceLogicalIndex(selections, ordinal);
        const auto value = sourceLogicalIndex
                               ? runtimeNumericElementValue(
                                     target, *sourceLogicalIndex)
                               : std::nullopt;
        if (!value) {
            return operationFailure("indexing could not map subscripts");
        }
        values.push_back(*value);
    }

    const auto result = runtimeNumericValueFromElements(
        selections.resultDimensions, std::move(values),
        target.numericClass);
    if (!result) {
        return operationFailure("indexed result has an invalid shape");
    }
    if (isRuntimeSparseValue(target)) {
        const auto sparse = runtimeSparseFromNumeric(*result);
        return sparse.succeeded
                   ? operationSuccess(std::move(sparse.value))
                   : operationFailure(std::move(sparse.error));
    }
    return operationSuccess(*result);
}

RuntimeIndexOperationResult runtimeIndexMissingArray(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon) {
    if (target.kind != RuntimeValueKind::MissingArray) {
        return operationFailure(
            "missing-array indexing requires a missing target");
    }
    const auto selections = runtimeResolveIndexSelections(
        target, subscripts, false, linearColon);
    if (!selections.succeeded) {
        return operationFailure(selections.error);
    }
    const auto resultCount = checkedRuntimeDimensionProduct(
        selections.resultDimensions);
    if (!resultCount) {
        return operationFailure(
            "indexed missing-array dimensions are too large");
    }
    return operationSuccess(makeRuntimeMissingArrayValue(
        selections.resultDimensions));
}

std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask) {
    return runtimeLinearIndexResultDimensions(
        target, subscript, resultElementCount, logicalMask, false);
}

std::vector<size_t> runtimeLinearIndexResultDimensions(
    const RuntimeValue& target, const RuntimeValue& subscript,
    size_t resultElementCount, bool logicalMask,
    bool linearColon) {
    if (linearColon) {
        return {resultElementCount, 1};
    }
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
