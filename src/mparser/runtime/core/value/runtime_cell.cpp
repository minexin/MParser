#include "mparser/runtime/core/value/runtime_cell.h"

#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {
namespace {

RuntimeCellOperationResult failure(std::string error) {
    return RuntimeCellOperationResult{false, {}, std::move(error)};
}

RuntimeCellOperationResult success(RuntimeValue value) {
    return RuntimeCellOperationResult{true, std::move(value), {}};
}

RuntimeValue emptyDoubleValue() {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Matrix;
    result.numericClass = RuntimeNumericClass::Double;
    setRuntimeDimensions(result, {0, 0});
    return result;
}

RuntimeValue makeCellValue(std::vector<size_t> dimensions,
                           std::vector<RuntimeValue> logicalValues) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells.resize(logicalValues.size());
    for (size_t logicalIndex = 0; logicalIndex < logicalValues.size();
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (storageOffset && *storageOffset < result.cells.size()) {
            result.cells[*storageOffset] =
                std::move(logicalValues[logicalIndex]);
        }
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

std::optional<RuntimeValue> logicalCellElement(
    const RuntimeValue& target, size_t logicalIndex) {
    const auto storageOffset = runtimeColumnMajorLinearToStorageOffset(
        target, logicalIndex);
    if (!storageOffset || *storageOffset >= target.cells.size()) {
        return std::nullopt;
    }
    return target.cells[*storageOffset];
}

struct ResolvedSelections {
    bool succeeded = false;
    std::vector<std::vector<size_t>> selections;
    std::vector<size_t> dimensions;
    std::string error;
};

ResolvedSelections resolveSelections(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool allowGrowth) {
    if (target.kind != RuntimeValueKind::Cell) {
        return ResolvedSelections{false, {}, {},
                                  "cell indexing requires a Cell target"};
    }
    if (subscripts.empty()) {
        return ResolvedSelections{false, {}, {},
                                  "cell indexing requires subscripts"};
    }

    ResolvedSelections result;
    const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        target, subscripts.size());
    result.selections.reserve(subscripts.size());
    result.dimensions.reserve(subscripts.size());
    for (size_t index = 0; index < subscripts.size(); ++index) {
        const size_t extent = subscripts.size() == 1
                                  ? runtimeShapeElementCount(target)
                                  : effectiveDimensions[index];
        auto selection = runtimeResolveIndexSelection(
            subscripts[index], extent, allowGrowth);
        if (!selection.succeeded) {
            return ResolvedSelections{false, {}, {},
                                      std::move(selection.error)};
        }
        result.dimensions.push_back(selection.indices.size());
        result.selections.push_back(std::move(selection.indices));
    }
    result.succeeded = true;
    return result;
}

std::optional<size_t> requiredExtent(
    const std::vector<size_t>& selection) {
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

bool growCellTarget(RuntimeValue& target,
                    const std::vector<size_t>& oldViewDimensions,
                    std::vector<size_t> newDimensions) {
    newDimensions = normalizeRuntimeDimensions(std::move(newDimensions));
    const auto oldCount = checkedRuntimeDimensionProduct(oldViewDimensions);
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    if (!oldCount || *oldCount != target.cells.size() || !newCount) {
        return false;
    }

    std::vector<RuntimeValue> grown(*newCount, emptyDoubleValue());
    for (size_t logicalIndex = 0; logicalIndex < *oldCount;
         ++logicalIndex) {
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldViewDimensions);
        const auto sourceOffset = runtimeColumnMajorLinearToStorageOffset(
            target, logicalIndex);
        if (!coordinates || !sourceOffset ||
            *sourceOffset >= target.cells.size()) {
            return false;
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto destinationOffset = runtimeRowMajorStorageOffset(
            *coordinates, newDimensions);
        if (!destinationOffset || *destinationOffset >= grown.size()) {
            return false;
        }
        grown[*destinationOffset] = target.cells[*sourceOffset];
    }
    target.cells = std::move(grown);
    setRuntimeDimensions(target, std::move(newDimensions));
    return true;
}

RuntimeCellOperationResult ensureCapacity(
    RuntimeValue target,
    const std::vector<std::vector<size_t>>& selections) {
    const auto oldDimensions = runtimeDimensions(target);
    const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        target, selections.size());
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
        return success(std::move(target));
    }

    std::vector<size_t> oldViewDimensions = oldDimensions;
    std::vector<size_t> newDimensions = oldDimensions;
    if (selections.size() == 1) {
        const size_t extent = *extents.front();
        if (oldDimensions.size() == 2 && oldDimensions[0] == 1) {
            newDimensions = {1, extent};
        } else if (oldDimensions.size() == 2 && oldDimensions[1] == 1) {
            newDimensions = {extent, 1};
        } else if (target.cells.empty()) {
            newDimensions = {1, extent};
        } else {
            return failure(
                "linear Cell growth requires a vector-shaped target");
        }
    } else {
        if (selections.size() > newDimensions.size()) {
            oldViewDimensions.resize(selections.size(), 1);
            newDimensions.resize(selections.size(), 1);
        }
        for (size_t index = 0; index < extents.size(); ++index) {
            if (extents[index]) {
                newDimensions[index] =
                    std::max(newDimensions[index], *extents[index]);
            }
        }
    }
    if (!growCellTarget(target, oldViewDimensions,
                        std::move(newDimensions))) {
        return failure("Cell assignment dimensions are too large");
    }
    return success(std::move(target));
}

std::optional<size_t> selectedLogicalIndex(
    const ResolvedSelections& resolved, size_t ordinal,
    const std::vector<size_t>& targetDimensions) {
    if (resolved.selections.size() == 1) {
        return resolved.selections.front()[ordinal];
    }
    const auto coordinates = runtimeColumnMajorCoordinates(
        ordinal, resolved.dimensions);
    if (!coordinates) {
        return std::nullopt;
    }
    std::vector<size_t> targetCoordinates(resolved.selections.size(), 0);
    for (size_t index = 0; index < resolved.selections.size(); ++index) {
        targetCoordinates[index] =
            resolved.selections[index][(*coordinates)[index]];
    }
    return runtimeColumnMajorLinearIndex(targetCoordinates,
                                         targetDimensions);
}

RuntimeCellOperationResult assignSelectedCells(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value,
    bool contents) {
    auto resolved = resolveSelections(target, subscripts, true);
    if (!resolved.succeeded) {
        return failure(std::move(resolved.error));
    }
    const auto selectionCount =
        checkedRuntimeDimensionProduct(resolved.dimensions);
    if (!selectionCount) {
        return failure("Cell assignment dimensions are too large");
    }
    if (contents && *selectionCount != 1) {
        return failure(
            "simple brace assignment requires exactly one selected cell");
    }
    if (!contents && value.kind != RuntimeValueKind::Cell) {
        return failure(
            "parenthesis Cell assignment requires a Cell value");
    }

    const size_t valueCount = contents ? 1 : value.cells.size();
    const bool scalarExpansion = !contents && valueCount == 1;
    if (!contents && !scalarExpansion && valueCount != *selectionCount) {
        return failure(
            "Cell assignment requires the same number of cells on both sides");
    }
    if (!contents && !scalarExpansion &&
        resolved.selections.size() > 1 &&
        normalizeRuntimeDimensions(resolved.dimensions) !=
            runtimeDimensions(value)) {
        return failure(
            "Cell assignment dimensions do not match the right-hand value");
    }

    auto capacity = ensureCapacity(target, resolved.selections);
    if (!capacity.succeeded) {
        return capacity;
    }
    RuntimeValue result = std::move(capacity.value);
    const auto targetDimensions = runtimeEffectiveSubscriptDimensions(
        result, resolved.selections.size());
    for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
        const auto logicalIndex = selectedLogicalIndex(
            resolved, ordinal, targetDimensions);
        const auto storageOffset = logicalIndex
                                       ? runtimeColumnMajorLinearToStorageOffset(
                                             result, *logicalIndex)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= result.cells.size()) {
            return failure("Cell assignment could not map subscripts");
        }
        if (contents) {
            result.cells[*storageOffset] = value;
            continue;
        }
        const size_t sourceLogicalIndex = scalarExpansion ? 0 : ordinal;
        const auto source = logicalCellElement(value, sourceLogicalIndex);
        if (!source) {
            return failure("Cell assignment value has an invalid shape");
        }
        result.cells[*storageOffset] = *source;
    }
    return success(std::move(result));
}

} // namespace

RuntimeCellOperationResult runtimeIndexCell(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts) {
    return runtimeIndexCell(target, subscripts, false);
}

RuntimeCellOperationResult runtimeIndexCell(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon) {
    auto resolved = resolveSelections(target, subscripts, false);
    if (!resolved.succeeded) {
        return failure(std::move(resolved.error));
    }
    if (resolved.selections.size() == 1) {
        const auto selection = runtimeResolveIndexSelection(
            subscripts.front(), runtimeShapeElementCount(target), false);
        resolved.dimensions = runtimeLinearIndexResultDimensions(
            target, subscripts.front(), selection.indices.size(),
            selection.logicalMask, linearColon);
    }
    const auto count = checkedRuntimeDimensionProduct(resolved.dimensions);
    if (!count) {
        return failure("Cell indexed result dimensions are too large");
    }

    const auto sourceDimensions = runtimeEffectiveSubscriptDimensions(
        target, resolved.selections.size());
    std::vector<RuntimeValue> values;
    values.reserve(*count);
    for (size_t ordinal = 0; ordinal < *count; ++ordinal) {
        const auto logicalIndex = selectedLogicalIndex(
            resolved, ordinal, sourceDimensions);
        const auto value = logicalIndex
                               ? logicalCellElement(target, *logicalIndex)
                               : std::nullopt;
        if (!value) {
            return failure("Cell indexing could not map subscripts");
        }
        values.push_back(*value);
    }
    return success(makeCellValue(std::move(resolved.dimensions),
                                 std::move(values)));
}

RuntimeCellOperationResult runtimeIndexCellContents(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts) {
    auto selected = runtimeIndexCell(target, subscripts);
    if (!selected.succeeded) {
        return selected;
    }
    const size_t count = runtimeShapeElementCount(selected.value);
    std::vector<RuntimeValue> values;
    values.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        const auto value = logicalCellElement(selected.value, logicalIndex);
        if (!value) {
            return failure("brace indexing could not read a selected cell");
        }
        values.push_back(*value);
    }
    if (values.size() == 1) {
        return success(std::move(values.front()));
    }
    return success(makeRuntimeCommaSeparatedList(std::move(values)));
}

RuntimeCellOperationResult runtimeAssignCellIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    return assignSelectedCells(target, subscripts, value, false);
}

RuntimeCellOperationResult runtimeDeleteCellIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts) {
    if (target.kind != RuntimeValueKind::Cell) {
        return failure("Cell null assignment requires a Cell target");
    }
    if (subscripts.size() != 1 || colonSubscripts.size() != 1) {
        return failure(
            "Cell null assignment currently requires one linear subscript");
    }
    auto selection = runtimeResolveIndexSelection(
        subscripts.front(), runtimeShapeElementCount(target), false);
    if (!selection.succeeded) {
        return failure(std::move(selection.error));
    }
    std::sort(selection.indices.begin(), selection.indices.end());
    selection.indices.erase(
        std::unique(selection.indices.begin(), selection.indices.end()),
        selection.indices.end());

    const auto oldDimensions = runtimeDimensions(target);
    const bool vectorShape = oldDimensions.size() == 2 &&
                             (oldDimensions[0] == 1 ||
                              oldDimensions[1] == 1);
    const size_t oldCount = runtimeShapeElementCount(target);
    if (!vectorShape && selection.indices.size() != oldCount) {
        return failure(
            "linear Cell null assignment requires a vector or all elements");
    }
    std::vector<bool> removed(oldCount, false);
    for (const size_t index : selection.indices) {
        if (index >= oldCount) {
            return failure("index is out of bounds");
        }
        removed[index] = true;
    }
    std::vector<RuntimeValue> kept;
    kept.reserve(oldCount - selection.indices.size());
    for (size_t logicalIndex = 0; logicalIndex < oldCount;
         ++logicalIndex) {
        if (removed[logicalIndex]) {
            continue;
        }
        const auto value = logicalCellElement(target, logicalIndex);
        if (!value) {
            return failure("Cell null assignment could not read an element");
        }
        kept.push_back(*value);
    }
    std::vector<size_t> dimensions;
    if (kept.empty() && !vectorShape) {
        dimensions = {0, 0};
    } else if (oldDimensions[0] == 1) {
        dimensions = {1, kept.size()};
    } else {
        dimensions = {kept.size(), 1};
    }
    return success(makeCellValue(std::move(dimensions), std::move(kept)));
}

RuntimeCellOperationResult runtimeAssignCellContents(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    return assignSelectedCells(target, subscripts, value, true);
}

} // namespace mparser
