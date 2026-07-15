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
