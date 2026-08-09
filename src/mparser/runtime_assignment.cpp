#include "mparser/runtime_assignment.h"

#include "mparser/runtime_index.h"
#include "mparser/runtime_numeric.h"
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

    RuntimeNumericElementValue zero;
    zero.numericClass = target.numericClass;
    zero.complex = target.numericComplex;
    std::vector<RuntimeNumericElementValue> grown(*newCount, zero);
    for (size_t logicalIndex = 0; logicalIndex < oldCount; ++logicalIndex) {
        const auto value = runtimeNumericElementValue(target, logicalIndex);
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldViewDimensions);
        if (!value || !coordinates) {
            return false;
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto newLogicalIndex = runtimeColumnMajorLinearIndex(
            *coordinates, newDimensions);
        if (!newLogicalIndex || *newLogicalIndex >= grown.size()) {
            return false;
        }
        grown[*newLogicalIndex] = *value;
    }

    auto result = runtimeNumericValueFromElements(
        newDimensions, std::move(grown), target.numericClass);
    if (!result) {
        return false;
    }
    target = std::move(*result);
    return true;
}

RuntimeNumericAssignmentResult ensureLinearCapacity(
    RuntimeValue& target, const std::vector<size_t>& indices) {
    const auto extent = runtimeIndexSelectionRequiredExtent(indices);
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
        extents.push_back(
            runtimeIndexSelectionRequiredExtent(selections[index]));
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

    std::vector<RuntimeNumericElementValue> assignedValues;
    assignedValues.reserve(indices.size());
    for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        const auto assigned = runtimeNumericElementValue(
            value, scalarExpansion ? 0 : ordinal);
        const auto converted =
            assigned ? runtimeConvertNumericElementValue(
                           *assigned, target.numericClass)
                     : std::nullopt;
        if (!converted) {
            return failure(
                target.numericClass == RuntimeNumericClass::Logical
                    ? "NaN cannot be converted to logical for indexed assignment"
                    : "indexed assignment value has an invalid shape");
        }
        assignedValues.push_back(*converted);
    }

    auto capacity = ensureLinearCapacity(target, indices);
    if (!capacity.succeeded) {
        return capacity;
    }
    for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        if (!runtimeStoreNumericElementValue(
                target, indices[ordinal], assignedValues[ordinal])) {
            return failure("indexed assignment could not map a linear index");
        }
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

    std::vector<RuntimeNumericElementValue> assignedValues;
    assignedValues.reserve(*selectionCount);
    for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
        const auto assigned = runtimeNumericElementValue(
            value, scalarExpansion ? 0 : ordinal);
        const auto converted =
            assigned ? runtimeConvertNumericElementValue(
                           *assigned, target.numericClass)
                     : std::nullopt;
        if (!converted) {
            return failure(
                target.numericClass == RuntimeNumericClass::Logical
                    ? "NaN cannot be converted to logical for indexed assignment"
                    : "indexed assignment value has an invalid shape");
        }
        assignedValues.push_back(*converted);
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
        if (!selectionCoordinates) {
            return failure("indexed assignment value has an invalid shape");
        }

        std::vector<size_t> targetCoordinates(selections.size(), 0);
        for (size_t index = 0; index < selections.size(); ++index) {
            targetCoordinates[index] =
                selections[index][(*selectionCoordinates)[index]];
        }
        const auto logicalIndex = runtimeColumnMajorLinearIndex(
            targetCoordinates, effectiveDimensions);
        if (!logicalIndex ||
            !runtimeStoreNumericElementValue(
                target, *logicalIndex, assignedValues[ordinal])) {
            return failure("indexed assignment could not map subscripts");
        }
    }
    return success();
}

std::vector<size_t> uniqueIndices(std::vector<size_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

bool replaceNumericArray(
    RuntimeValue& target, std::vector<size_t> dimensions,
    std::vector<RuntimeNumericElementValue> elements) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const bool preserveComplex = target.numericComplex;
    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(elements), target.numericClass);
    if (!result) {
        return false;
    }
    if (result->kind == RuntimeValueKind::Number) {
        result->kind = dimensions.size() == 2 && dimensions[0] == 1
                           ? RuntimeValueKind::Vector
                           : RuntimeValueKind::Matrix;
        result->elements = {result->number};
        result->number = 0.0;
    }
    if (runtimeShapeElementCount(*result) == 0 && preserveComplex) {
        result->numericComplex = true;
    }
    setRuntimeDimensions(*result, std::move(dimensions));
    target = std::move(*result);
    return true;
}

RuntimeNumericAssignmentResult deleteLinear(
    RuntimeValue& target, std::vector<size_t> indices) {
    indices = uniqueIndices(std::move(indices));
    if (indices.empty()) {
        return success();
    }

    const size_t oldCount = runtimeShapeElementCount(target);
    const auto oldDimensions = runtimeDimensions(target);
    const bool vectorShape =
        oldDimensions.size() == 2 &&
        (oldDimensions[0] == 1 || oldDimensions[1] == 1);
    if (!isNumber(target) && !vectorShape && indices.size() != oldCount) {
        return failure(
            "linear null assignment requires a vector or all elements");
    }

    std::vector<bool> removed(oldCount, false);
    for (const size_t index : indices) {
        if (index >= oldCount) {
            return failure("index is out of bounds");
        }
        removed[index] = true;
    }

    std::vector<RuntimeNumericElementValue> kept;
    kept.reserve(oldCount - indices.size());
    for (size_t logicalIndex = 0; logicalIndex < oldCount; ++logicalIndex) {
        if (removed[logicalIndex]) {
            continue;
        }
        const auto value = runtimeNumericElementValue(target, logicalIndex);
        if (!value) {
            return failure("null assignment could not read the target array");
        }
        kept.push_back(*value);
    }

    std::vector<size_t> newDimensions;
    if (kept.empty() && (isNumber(target) || !vectorShape)) {
        newDimensions = {0, 0};
    } else if (oldDimensions[0] == 1) {
        newDimensions = {1, kept.size()};
    } else {
        newDimensions = {kept.size(), 1};
    }
    if (!replaceNumericArray(
            target, std::move(newDimensions), std::move(kept))) {
        return failure("null assignment could not construct the result array");
    }
    return success();
}

RuntimeNumericAssignmentResult deleteSlices(
    RuntimeValue& target,
    const std::vector<std::vector<size_t>>& selections,
    const std::vector<bool>& colonSubscripts) {
    size_t deletionDimension = selections.size();
    for (size_t index = 0; index < colonSubscripts.size(); ++index) {
        if (colonSubscripts[index]) {
            continue;
        }
        if (deletionDimension != selections.size()) {
            return failure(
                "null assignment can have only one non-colon subscript");
        }
        deletionDimension = index;
    }
    if (deletionDimension == selections.size()) {
        return failure(
            "null assignment requires one non-colon subscript");
    }

    auto oldDimensions = runtimeDimensions(target);
    if (selections.size() < oldDimensions.size()) {
        return failure(
            "N-dimensional null assignment requires one subscript per "
            "dimension");
    }
    oldDimensions.resize(selections.size(), 1);
    auto removedIndices =
        uniqueIndices(selections[deletionDimension]);
    if (removedIndices.empty()) {
        return success();
    }

    const size_t oldExtent = oldDimensions[deletionDimension];
    std::vector<bool> removed(oldExtent, false);
    for (const size_t index : removedIndices) {
        if (index >= oldExtent) {
            return failure("index is out of bounds");
        }
        removed[index] = true;
    }
    std::vector<size_t> removedBefore(oldExtent + 1, 0);
    for (size_t index = 0; index < oldExtent; ++index) {
        removedBefore[index + 1] =
            removedBefore[index] + (removed[index] ? 1 : 0);
    }

    auto newDimensions = oldDimensions;
    newDimensions[deletionDimension] -= removedIndices.size();
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    const size_t oldCount = runtimeShapeElementCount(target);
    if (!newCount) {
        return failure("null assignment dimensions are too large");
    }
    std::vector<RuntimeNumericElementValue> kept(*newCount);
    for (size_t logicalIndex = 0; logicalIndex < oldCount; ++logicalIndex) {
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldDimensions);
        const auto value = runtimeNumericElementValue(target, logicalIndex);
        if (!coordinates || !value) {
            return failure("null assignment could not map the target array");
        }
        const size_t selected = (*coordinates)[deletionDimension];
        if (removed[selected]) {
            continue;
        }
        (*coordinates)[deletionDimension] -= removedBefore[selected];
        const auto newLogicalIndex = runtimeColumnMajorLinearIndex(
            *coordinates, newDimensions);
        if (!newLogicalIndex || *newLogicalIndex >= kept.size()) {
            return failure("null assignment could not map the result array");
        }
        kept[*newLogicalIndex] = *value;
    }

    if (!replaceNumericArray(
            target, std::move(newDimensions), std::move(kept))) {
        return failure("null assignment could not construct the result array");
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

    auto selections = runtimeResolveIndexSelections(target, subscripts, true);
    if (!selections.succeeded) {
        return failure(std::move(selections.error));
    }

    if (selections.indices.size() == 1) {
        return assignLinear(target, selections.indices.front(), value);
    }
    return assignSubscripts(target, selections.indices, value);
}

RuntimeNumericAssignmentResult runtimeDeleteNumericIndexed(
    RuntimeValue& target, const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts) {
    if (!isNumeric(target)) {
        return failure("null assignment requires a numeric target");
    }
    if (subscripts.empty()) {
        return failure("null assignment requires subscripts");
    }
    if (colonSubscripts.size() != subscripts.size()) {
        return failure("null assignment subscript metadata is inconsistent");
    }

    auto selections = runtimeResolveIndexSelections(target, subscripts, false);
    if (!selections.succeeded) {
        return failure(std::move(selections.error));
    }

    if (selections.indices.size() == 1) {
        return deleteLinear(target, std::move(selections.indices.front()));
    }
    return deleteSlices(target, selections.indices, colonSubscripts);
}

} // namespace mparser
