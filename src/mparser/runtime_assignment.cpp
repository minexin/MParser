#include "mparser/runtime_assignment.h"

#include "mparser/runtime_shape.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {
namespace {

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isNumeric(const RuntimeValue& value) {
    return isNumber(value) || isArray(value);
}

RuntimeNumericAssignmentResult failure(std::string message) {
    return RuntimeNumericAssignmentResult{false, std::move(message)};
}

RuntimeNumericAssignmentResult success() {
    return RuntimeNumericAssignmentResult{true, {}};
}

std::optional<double> logicalElement(const RuntimeValue& value,
                                     size_t logicalIndex) {
    if (isNumber(value)) {
        return logicalIndex == 0 ? std::optional<double>(value.number)
                                 : std::nullopt;
    }
    if (!isArray(value)) {
        return std::nullopt;
    }
    const auto storageOffset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    if (!storageOffset || *storageOffset >= value.elements.size()) {
        return std::nullopt;
    }
    return value.elements[*storageOffset];
}

std::optional<std::vector<size_t>> assignmentIndices(
    const RuntimeValue& subscript, std::string& error) {
    std::vector<size_t> indices;
    const size_t count = runtimeShapeElementCount(subscript);
    if (!isNumeric(subscript)) {
        error = "indexed assignment requires numeric subscripts";
        return std::nullopt;
    }

    indices.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        const auto rawIndex = logicalElement(subscript, logicalIndex);
        const auto oneBasedIndex = rawIndex
                                       ? checkedRuntimeNonnegativeInteger(
                                             *rawIndex)
                                       : std::nullopt;
        if (!oneBasedIndex || *oneBasedIndex == 0) {
            error = "index must be a positive integer";
            return std::nullopt;
        }
        indices.push_back(*oneBasedIndex - 1);
    }
    return indices;
}

std::vector<size_t>
nonSingletonDimensions(const std::vector<size_t>& dimensions) {
    std::vector<size_t> result;
    for (const size_t dimension : dimensions) {
        if (dimension != 1) {
            result.push_back(dimension);
        }
    }
    return result;
}

std::optional<size_t>
requiredExtent(const std::vector<size_t>& selection) {
    if (selection.empty()) {
        return std::nullopt;
    }
    const size_t maximum =
        *std::max_element(selection.begin(), selection.end());
    if (maximum == std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return maximum + 1;
}

bool growNumericTarget(RuntimeValue& target,
                       const std::vector<size_t>& oldViewDimensions,
                       std::vector<size_t> newDimensions) {
    newDimensions = normalizeRuntimeDimensions(std::move(newDimensions));
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    const size_t oldCount = runtimeShapeElementCount(target);
    const auto oldViewCount =
        checkedRuntimeDimensionProduct(oldViewDimensions);
    if (!newCount || !oldViewCount || *oldViewCount != oldCount) {
        return false;
    }

    std::vector<double> grown(*newCount, 0.0);
    for (size_t logicalIndex = 0; logicalIndex < oldCount; ++logicalIndex) {
        const auto value = logicalElement(target, logicalIndex);
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldViewDimensions);
        if (!value || !coordinates) {
            return false;
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto storageOffset =
            runtimeRowMajorStorageOffset(*coordinates, newDimensions);
        if (!storageOffset || *storageOffset >= grown.size()) {
            return false;
        }
        grown[*storageOffset] = *value;
    }

    target.kind = newDimensions.size() == 2 && newDimensions[0] == 1
                      ? RuntimeValueKind::Vector
                      : RuntimeValueKind::Matrix;
    target.number = 0.0;
    target.elements = std::move(grown);
    setRuntimeDimensions(target, std::move(newDimensions));
    return true;
}

RuntimeNumericAssignmentResult ensureLinearCapacity(
    RuntimeValue& target, const std::vector<size_t>& indices) {
    const auto extent = requiredExtent(indices);
    if (!extent || *extent <= runtimeShapeElementCount(target)) {
        return success();
    }

    const auto oldDimensions = runtimeDimensions(target);
    std::vector<size_t> newDimensions;
    if (isNumber(target) ||
        (oldDimensions.size() == 2 && oldDimensions[0] == 1)) {
        newDimensions = {1, *extent};
    } else if (oldDimensions.size() == 2 && oldDimensions[1] == 1) {
        newDimensions = {*extent, 1};
    } else {
        newDimensions = oldDimensions;
        std::vector<size_t> leading(newDimensions.begin(),
                                    newDimensions.end() - 1);
        const auto leadingCount = checkedRuntimeDimensionProduct(leading);
        if (!leadingCount || *leadingCount == 0) {
            newDimensions = {1, *extent};
        } else {
            const size_t quotient = *extent / *leadingCount;
            const size_t remainder = *extent % *leadingCount;
            if (quotient == std::numeric_limits<size_t>::max() &&
                remainder != 0) {
                return failure("indexed assignment dimensions are too large");
            }
            newDimensions.back() = quotient + (remainder == 0 ? 0 : 1);
        }
    }

    if (!growNumericTarget(target, oldDimensions, std::move(newDimensions))) {
        return failure("indexed assignment dimensions are too large");
    }
    return success();
}

RuntimeNumericAssignmentResult ensureSubscriptCapacity(
    RuntimeValue& target,
    const std::vector<std::vector<size_t>>& selections) {
    const auto oldDimensions = runtimeDimensions(target);
    const auto effectiveDimensions =
        runtimeEffectiveSubscriptDimensions(target, selections.size());
    std::vector<std::optional<size_t>> extents;
    extents.reserve(selections.size());
    bool growthRequired = false;
    for (size_t index = 0; index < selections.size(); ++index) {
        extents.push_back(requiredExtent(selections[index]));
        if (extents.back() &&
            *extents.back() > effectiveDimensions[index]) {
            growthRequired = true;
        }
    }
    if (!growthRequired) {
        return success();
    }

    std::vector<size_t> oldViewDimensions = oldDimensions;
    std::vector<size_t> newDimensions = oldDimensions;
    const bool foldsTrailingDimensions =
        selections.size() < oldDimensions.size();
    const size_t finalSubscript = selections.size() - 1;
    const bool growsFoldedDimension =
        foldsTrailingDimensions && extents[finalSubscript] &&
        *extents[finalSubscript] > effectiveDimensions[finalSubscript];
    if (growsFoldedDimension) {
        oldViewDimensions = effectiveDimensions;
        newDimensions = effectiveDimensions;
    } else if (selections.size() > newDimensions.size()) {
        oldViewDimensions.resize(selections.size(), 1);
        newDimensions.resize(selections.size(), 1);
    }

    for (size_t index = 0; index < extents.size(); ++index) {
        if (foldsTrailingDimensions && !growsFoldedDimension &&
            index == finalSubscript) {
            continue;
        }
        if (extents[index]) {
            newDimensions[index] =
                std::max(newDimensions[index], *extents[index]);
        }
    }
    if (!growNumericTarget(target, oldViewDimensions,
                           std::move(newDimensions))) {
        return failure("indexed assignment dimensions are too large");
    }
    return success();
}

RuntimeNumericAssignmentResult assignLinear(
    RuntimeValue& target, const std::vector<size_t>& indices,
    const RuntimeValue& value) {
    const size_t valueCount = runtimeShapeElementCount(value);
    const bool scalarExpansion = valueCount == 1;
    if (!scalarExpansion && valueCount != indices.size()) {
        return failure(
            "linear indexed assignment requires the same number of elements "
            "on both sides");
    }

    auto capacity = ensureLinearCapacity(target, indices);
    if (!capacity.succeeded) {
        return capacity;
    }
    for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        const auto assigned =
            logicalElement(value, scalarExpansion ? 0 : ordinal);
        if (!assigned) {
            return failure("indexed assignment value has an invalid shape");
        }
        if (isNumber(target)) {
            target.number = *assigned;
            continue;
        }
        const auto storageOffset = runtimeColumnMajorLinearToStorageOffset(
            target, indices[ordinal]);
        if (!storageOffset || *storageOffset >= target.elements.size()) {
            return failure("indexed assignment could not map a linear index");
        }
        target.elements[*storageOffset] = *assigned;
    }
    return success();
}

RuntimeNumericAssignmentResult assignSubscripts(
    RuntimeValue& target,
    const std::vector<std::vector<size_t>>& selections,
    const RuntimeValue& value) {
    std::vector<size_t> selectionDimensions;
    selectionDimensions.reserve(selections.size());
    for (const auto& selection : selections) {
        selectionDimensions.push_back(selection.size());
    }
    const auto selectionCount =
        checkedRuntimeDimensionProduct(selectionDimensions);
    if (!selectionCount) {
        return failure("indexed assignment dimensions are too large");
    }

    const size_t valueCount = runtimeShapeElementCount(value);
    const bool scalarExpansion = valueCount == 1;
    if (!scalarExpansion &&
        nonSingletonDimensions(selectionDimensions) !=
            nonSingletonDimensions(runtimeDimensions(value))) {
        return failure(
            "indexed assignment dimensions do not match the right-hand value");
    }
    if (!scalarExpansion && valueCount != *selectionCount) {
        return failure(
            "indexed assignment requires the same number of elements on both "
            "sides");
    }

    auto capacity = ensureSubscriptCapacity(target, selections);
    if (!capacity.succeeded) {
        return capacity;
    }
    const auto effectiveDimensions =
        runtimeEffectiveSubscriptDimensions(target, selections.size());
    for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
        const auto selectionCoordinates = runtimeColumnMajorCoordinates(
            ordinal, selectionDimensions);
        const auto assigned =
            logicalElement(value, scalarExpansion ? 0 : ordinal);
        if (!selectionCoordinates || !assigned) {
            return failure("indexed assignment value has an invalid shape");
        }

        std::vector<size_t> targetCoordinates(selections.size(), 0);
        for (size_t index = 0; index < selections.size(); ++index) {
            targetCoordinates[index] =
                selections[index][(*selectionCoordinates)[index]];
        }
        const auto storageOffset = runtimeSubscriptsToStorageOffset(
            target, targetCoordinates, effectiveDimensions);
        if (!storageOffset) {
            return failure("indexed assignment could not map subscripts");
        }
        if (isNumber(target)) {
            target.number = *assigned;
        } else if (*storageOffset < target.elements.size()) {
            target.elements[*storageOffset] = *assigned;
        } else {
            return failure("indexed assignment could not map subscripts");
        }
    }
    return success();
}

} // namespace

RuntimeNumericAssignmentResult runtimeAssignNumericIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    if (!isNumeric(target)) {
        return failure("indexed assignment requires a numeric target");
    }
    if (!isNumeric(value)) {
        return failure("indexed assignment requires a numeric value");
    }
    if (subscripts.empty()) {
        return failure("indexed assignment requires subscripts");
    }

    std::vector<std::vector<size_t>> selections;
    selections.reserve(subscripts.size());
    for (const auto& subscript : subscripts) {
        std::string error;
        auto selection = assignmentIndices(subscript, error);
        if (!selection) {
            return failure(std::move(error));
        }
        selections.push_back(std::move(*selection));
    }

    if (selections.size() == 1) {
        return assignLinear(target, selections.front(), value);
    }
    return assignSubscripts(target, selections, value);
}

} // namespace mparser
