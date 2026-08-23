#include "mparser/runtime/core/object_model/runtime_object.h"

#include "mparser/runtime/core/session/runtime_exception.h"
#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {
namespace {

RuntimeObjectOperationResult failure(std::string error) {
    return RuntimeObjectOperationResult{false, {}, std::move(error)};
}

RuntimeObjectOperationResult success(RuntimeValue value) {
    return RuntimeObjectOperationResult{true, std::move(value), {}};
}

std::vector<size_t> nonSingletonDimensions(
    const std::vector<size_t>& dimensions) {
    std::vector<size_t> result;
    for (const size_t dimension : dimensions) {
        if (dimension != 1) {
            result.push_back(dimension);
        }
    }
    return result;
}

RuntimeObjectClassResolutionResult resolveCommonClass(
    const std::vector<RuntimeValue>& elements,
    std::string_view preferredClassName,
    const RuntimeObjectArrayPolicy& policy) {
    std::vector<std::string> classNames;
    classNames.reserve(elements.size());
    for (const auto& element : elements) {
        if (!isRuntimeScalarObject(element)) {
            return RuntimeObjectClassResolutionResult{
                false, {},
                "object arrays require scalar class object elements"};
        }
        classNames.push_back(element.className);
    }

    if (policy.resolveCommonClass) {
        auto resolved =
            policy.resolveCommonClass(classNames, preferredClassName);
        if (!resolved.succeeded || resolved.className.empty()) {
            if (resolved.error.empty()) {
                resolved.error =
                    "object array classes do not have a compatible common class";
            }
            resolved.succeeded = false;
        }
        return resolved;
    }

    if (classNames.empty()) {
        if (preferredClassName.empty()) {
            return RuntimeObjectClassResolutionResult{
                false, {}, "empty object array requires a class identity"};
        }
        return RuntimeObjectClassResolutionResult{
            true, std::string(preferredClassName), {}};
    }

    const std::string& candidate = classNames.front();
    const bool sameClass = std::all_of(
        classNames.begin(), classNames.end(),
        [&](const std::string& name) { return name == candidate; });
    if (!sameClass) {
        return RuntimeObjectClassResolutionResult{
            false, {},
            "object array classes do not have a compatible common class"};
    }
    if (!preferredClassName.empty() && candidate != preferredClassName) {
        return RuntimeObjectClassResolutionResult{
            false, {}, "object assignment requires class " +
                           std::string(preferredClassName) +
                           ", but received " + candidate};
    }
    return RuntimeObjectClassResolutionResult{true, candidate, {}};
}

RuntimeObjectOperationResult makeObjectArrayFromStorageOrder(
    std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions,
    std::string fallbackClassName,
    bool fallbackHandleObject,
    const RuntimeObjectArrayPolicy& policy,
    std::string preferredClassName) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != elements.size()) {
        return failure("object array dimensions do not match its elements");
    }

    if (elements.empty()) {
        if (fallbackClassName.empty()) {
            return failure("empty object array requires a class identity");
        }
        RuntimeValue result;
        result.kind = RuntimeValueKind::Object;
        result.className = std::move(fallbackClassName);
        result.handleObject = fallbackHandleObject;
        setRuntimeDimensions(result, std::move(dimensions));
        return success(std::move(result));
    }

    const bool handleObject = elements.front().handleObject;
    for (const auto& element : elements) {
        if (!isRuntimeScalarObject(element)) {
            return failure(
                "object arrays require scalar class object elements");
        }
        if (element.handleObject != handleObject) {
            return failure(
                "object arrays cannot mix value and handle objects");
        }
    }

    auto common = resolveCommonClass(
        elements, preferredClassName, policy);
    if (!common.succeeded) {
        return failure(std::move(common.error));
    }

    if (elements.size() == 1) {
        RuntimeValue result = std::move(elements.front());
        result.className = std::move(common.className);
        setRuntimeDimensions(result, {1, 1});
        return success(std::move(result));
    }

    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::move(common.className);
    result.handleObject = handleObject;
    result.objectElements = std::move(elements);
    setRuntimeDimensions(result, std::move(dimensions));
    return success(std::move(result));
}

RuntimeObjectOperationResult makeDefaultObject(
    std::string_view className,
    bool handleObject,
    const RuntimeObjectArrayPolicy& policy) {
    if (!policy.constructDefault) {
        return failure(
            "object array growth requires a default constructor for class " +
            std::string(className));
    }
    auto result = policy.constructDefault(className);
    if (!result.succeeded) {
        if (result.error.empty()) {
            result.error = "default construction failed for class " +
                           std::string(className);
        }
        return result;
    }
    if (!isRuntimeScalarObject(result.value) ||
        result.value.className != className ||
        result.value.handleObject != handleObject) {
        return failure(
            "default constructor returned an incompatible object for class " +
            std::string(className));
    }
    return result;
}

RuntimeObjectOperationResult growObjectTarget(
    RuntimeValue target,
    const std::vector<size_t>& oldViewDimensions,
    std::vector<size_t> newDimensions,
    const RuntimeIndexSelectionsResult& selections,
    bool reserveAssignedSlots,
    const RuntimeObjectArrayPolicy& policy) {
    newDimensions = normalizeRuntimeDimensions(std::move(newDimensions));
    const auto oldViewCount =
        checkedRuntimeDimensionProduct(oldViewDimensions);
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    const size_t oldCount = runtimeObjectElementCount(target);
    if (!oldViewCount || *oldViewCount != oldCount || !newCount) {
        return failure("object array growth dimensions are too large");
    }

    std::vector<RuntimeValue> elements(*newCount);
    std::vector<bool> initialized(*newCount, false);
    for (size_t logicalIndex = 0; logicalIndex < oldCount;
         ++logicalIndex) {
        const auto* source = runtimeObjectLogicalElement(target, logicalIndex);
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldViewDimensions);
        if (!source || !coordinates) {
            return failure("object array growth could not map an element");
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto destination = runtimeRowMajorStorageOffset(
            *coordinates, newDimensions);
        if (!destination || *destination >= elements.size()) {
            return failure("object array growth could not map an element");
        }
        elements[*destination] = *source;
        initialized[*destination] = true;
    }

    RuntimeValue grownShape = target;
    setRuntimeDimensions(grownShape, newDimensions);
    RuntimeIndexSelectionsResult grownSelections = selections;
    grownSelections.effectiveDimensions =
        runtimeEffectiveSubscriptDimensions(
            grownShape, selections.indices.size());
    const auto selectedCount = checkedRuntimeDimensionProduct(
        selections.resultDimensions);
    if (!selectedCount) {
        return failure("object array growth dimensions are too large");
    }
    std::vector<bool> assigned(*newCount, false);
    for (size_t ordinal = 0; ordinal < *selectedCount; ++ordinal) {
        const auto logicalIndex =
            runtimeIndexSelectionSourceLogicalIndex(
                grownSelections, ordinal);
        const auto storageOffset = logicalIndex
                                       ? runtimeColumnMajorLinearToStorageOffset(
                                             grownShape, *logicalIndex)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= assigned.size()) {
            return failure("object array growth could not map an assignment");
        }
        assigned[*storageOffset] = true;
    }

    for (size_t offset = 0; offset < elements.size(); ++offset) {
        if (initialized[offset] ||
            (reserveAssignedSlots && assigned[offset])) {
            continue;
        }
        auto created = makeDefaultObject(
            target.className, target.handleObject, policy);
        if (!created.succeeded) {
            return created;
        }
        elements[offset] = std::move(created.value);
    }
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = target.className;
    result.handleObject = target.handleObject;
    result.objectElements = std::move(elements);
    setRuntimeDimensions(result, std::move(newDimensions));
    return success(std::move(result));
}

RuntimeObjectOperationResult ensureObjectCapacity(
    RuntimeValue target,
    const RuntimeIndexSelectionsResult& selections,
    bool reserveAssignedSlots,
    const RuntimeObjectArrayPolicy& policy) {
    const auto oldDimensions = runtimeDimensions(target);
    if (selections.indices.size() == 1) {
        const auto extent = runtimeIndexSelectionRequiredExtent(
            selections.indices.front());
        if (!extent || *extent <= runtimeObjectElementCount(target)) {
            return success(std::move(target));
        }

        std::vector<size_t> newDimensions;
        if (oldDimensions.size() == 2 && oldDimensions[0] == 1) {
            newDimensions = {1, *extent};
        } else if (oldDimensions.size() == 2 && oldDimensions[1] == 1) {
            newDimensions = {*extent, 1};
        } else if (runtimeObjectElementCount(target) == 0) {
            newDimensions = {1, *extent};
        } else {
            newDimensions = oldDimensions;
            std::vector<size_t> leading(
                newDimensions.begin(), newDimensions.end() - 1);
            const auto leadingCount =
                checkedRuntimeDimensionProduct(leading);
            if (!leadingCount || *leadingCount == 0) {
                newDimensions = {1, *extent};
            } else {
                const size_t quotient = *extent / *leadingCount;
                const size_t remainder = *extent % *leadingCount;
                if (quotient == std::numeric_limits<size_t>::max() &&
                    remainder != 0) {
                    return failure(
                        "object array growth dimensions are too large");
                }
                newDimensions.back() =
                    quotient + (remainder == 0 ? 0 : 1);
            }
        }
        return growObjectTarget(
            std::move(target), oldDimensions, std::move(newDimensions),
            selections, reserveAssignedSlots, policy);
    }

    std::vector<std::optional<size_t>> extents;
    extents.reserve(selections.indices.size());
    bool growthRequired = false;
    for (size_t index = 0; index < selections.indices.size(); ++index) {
        extents.push_back(runtimeIndexSelectionRequiredExtent(
            selections.indices[index]));
        if (extents.back() &&
            *extents.back() > selections.effectiveDimensions[index]) {
            growthRequired = true;
        }
    }
    if (!growthRequired) {
        return success(std::move(target));
    }

    std::vector<size_t> oldViewDimensions = oldDimensions;
    std::vector<size_t> newDimensions = oldDimensions;
    const bool foldsTrailingDimensions =
        selections.indices.size() < oldDimensions.size();
    const size_t finalSubscript = selections.indices.size() - 1;
    const bool growsFoldedDimension =
        foldsTrailingDimensions && extents[finalSubscript] &&
        *extents[finalSubscript] >
            selections.effectiveDimensions[finalSubscript];
    if (growsFoldedDimension) {
        oldViewDimensions = selections.effectiveDimensions;
        newDimensions = selections.effectiveDimensions;
    } else if (selections.indices.size() > newDimensions.size()) {
        oldViewDimensions.resize(selections.indices.size(), 1);
        newDimensions.resize(selections.indices.size(), 1);
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
    return growObjectTarget(
        std::move(target), oldViewDimensions, std::move(newDimensions),
        selections, reserveAssignedSlots, policy);
}

std::vector<size_t> uniqueIndices(std::vector<size_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()),
                  indices.end());
    return indices;
}

} // namespace

bool isRuntimeClassObject(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           !isRuntimeMetadataObject(value) &&
           !isRuntimeException(value);
}

bool isRuntimeScalarObject(const RuntimeValue& value) {
    return isRuntimeClassObject(value) && value.objectElements.empty() &&
           runtimeShapeElementCount(value) == 1;
}

size_t runtimeObjectElementCount(const RuntimeValue& value) {
    if (!isRuntimeClassObject(value)) {
        return 0;
    }
    if (!value.objectElements.empty()) {
        return value.objectElements.size();
    }
    return runtimeShapeElementCount(value) == 1 ? 1 : 0;
}

const RuntimeValue* runtimeObjectElement(
    const RuntimeValue& value, size_t storageOffset) {
    if (!isRuntimeClassObject(value)) {
        return nullptr;
    }
    if (storageOffset < value.objectElements.size()) {
        return &value.objectElements[storageOffset];
    }
    if (storageOffset == 0 && value.objectElements.empty() &&
        runtimeShapeElementCount(value) == 1) {
        return &value;
    }
    return nullptr;
}

RuntimeValue* runtimeObjectElement(
    RuntimeValue& value, size_t storageOffset) {
    return const_cast<RuntimeValue*>(runtimeObjectElement(
        std::as_const(value), storageOffset));
}

const RuntimeValue* runtimeObjectLogicalElement(
    const RuntimeValue& value, size_t logicalIndex) {
    const auto storageOffset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    return storageOffset ? runtimeObjectElement(value, *storageOffset)
                         : nullptr;
}

RuntimeValue* runtimeObjectLogicalElement(
    RuntimeValue& value, size_t logicalIndex) {
    const auto storageOffset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    return storageOffset ? runtimeObjectElement(value, *storageOffset)
                         : nullptr;
}

const std::map<std::string, RuntimeValue>* runtimeObjectFields(
    const RuntimeValue& value) {
    if (!isRuntimeScalarObject(value)) {
        return nullptr;
    }
    if (value.handleObject) {
        return value.sharedFields ? value.sharedFields.get() : nullptr;
    }
    return &value.fields;
}

std::map<std::string, RuntimeValue>* runtimeObjectFields(
    RuntimeValue& value) {
    return const_cast<std::map<std::string, RuntimeValue>*>(
        runtimeObjectFields(std::as_const(value)));
}

RuntimeValue makeRuntimeObjectScalar(
    std::string className,
    std::map<std::string, RuntimeValue> fields,
    bool handleObject) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::move(className);
    result.handleObject = handleObject;
    if (handleObject) {
        result.sharedFields =
            std::make_shared<std::map<std::string, RuntimeValue>>(
                std::move(fields));
    } else {
        result.fields = std::move(fields);
    }
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeObjectOperationResult runtimeMakeObjectArrayFromLogicalOrder(
    std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions,
    std::string fallbackClassName,
    bool fallbackHandleObject,
    const RuntimeObjectArrayPolicy& policy,
    std::string preferredClassName) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != elements.size()) {
        return failure("object array dimensions do not match its elements");
    }

    std::vector<RuntimeValue> storage(elements.size());
    for (size_t logicalIndex = 0; logicalIndex < elements.size();
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= storage.size()) {
            return failure("object array could not map logical elements");
        }
        storage[*storageOffset] = std::move(elements[logicalIndex]);
    }
    return makeObjectArrayFromStorageOrder(
        std::move(storage), std::move(dimensions),
        std::move(fallbackClassName), fallbackHandleObject, policy,
        std::move(preferredClassName));
}

RuntimeObjectOperationResult runtimeMakeObjectArrayFromStorageOrder(
    std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions,
    std::string fallbackClassName,
    bool fallbackHandleObject,
    const RuntimeObjectArrayPolicy& policy,
    std::string preferredClassName) {
    return makeObjectArrayFromStorageOrder(
        std::move(elements), std::move(dimensions),
        std::move(fallbackClassName), fallbackHandleObject, policy,
        std::move(preferredClassName));
}

RuntimeObjectOperationResult runtimeIndexObject(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeObjectArrayPolicy& policy) {
    return runtimeIndexObject(target, subscripts, policy, false);
}

RuntimeObjectOperationResult runtimeIndexObject(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeObjectArrayPolicy& policy,
    bool linearColon) {
    if (!isRuntimeClassObject(target)) {
        return failure("object indexing requires a class object target");
    }
    const auto selections =
        runtimeResolveIndexSelections(target, subscripts, false,
                                      linearColon);
    if (!selections.succeeded) {
        return failure(selections.error);
    }
    const auto count =
        checkedRuntimeDimensionProduct(selections.resultDimensions);
    if (!count) {
        return failure("indexed object dimensions are too large");
    }

    std::vector<RuntimeValue> elements;
    elements.reserve(*count);
    for (size_t ordinal = 0; ordinal < *count; ++ordinal) {
        const auto sourceIndex =
            runtimeIndexSelectionSourceLogicalIndex(selections, ordinal);
        const auto* element = sourceIndex
                                  ? runtimeObjectLogicalElement(
                                        target, *sourceIndex)
                                  : nullptr;
        if (!element) {
            return failure("object indexing could not map an element");
        }
        elements.push_back(*element);
    }
    return runtimeMakeObjectArrayFromLogicalOrder(
        std::move(elements), selections.resultDimensions, target.className,
        target.handleObject, policy, target.className);
}

RuntimeObjectOperationResult runtimeAssignObjectIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value,
    const RuntimeObjectArrayPolicy& policy) {
    if (!isRuntimeClassObject(target) || !isRuntimeClassObject(value)) {
        return failure(
            "object indexed assignment requires class object values");
    }
    if (runtimeObjectElementCount(value) == 0) {
        return failure(
            "object indexed assignment requires a nonempty value");
    }

    const auto selections =
        runtimeResolveIndexSelections(target, subscripts, true);
    if (!selections.succeeded) {
        return failure(selections.error);
    }
    const auto selectionCount =
        checkedRuntimeDimensionProduct(selections.resultDimensions);
    if (!selectionCount) {
        return failure("object assignment dimensions are too large");
    }
    const size_t valueCount = runtimeObjectElementCount(value);
    const bool scalarExpansion = valueCount == 1;
    if (!scalarExpansion && valueCount != *selectionCount) {
        return failure(
            "object assignment requires matching element counts");
    }
    if (!scalarExpansion && selections.indices.size() > 1 &&
        nonSingletonDimensions(selections.resultDimensions) !=
            nonSingletonDimensions(runtimeDimensions(value))) {
        return failure("object assignment dimensions do not match");
    }

    auto capacity = ensureObjectCapacity(
        target, selections, true, policy);
    if (!capacity.succeeded) {
        return capacity;
    }
    RuntimeValue result = std::move(capacity.value);
    RuntimeIndexSelectionsResult grown = selections;
    grown.effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        result, subscripts.size());
    for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
        const auto targetIndex =
            runtimeIndexSelectionSourceLogicalIndex(grown, ordinal);
        const auto* source = runtimeObjectLogicalElement(
            value, scalarExpansion ? 0 : ordinal);
        auto* destination = targetIndex
                                ? runtimeObjectLogicalElement(
                                      result, *targetIndex)
                                : nullptr;
        if (!source || !destination) {
            return failure("object assignment could not map an element");
        }
        *destination = *source;
    }

    std::vector<RuntimeValue> logicalElements;
    logicalElements.reserve(runtimeObjectElementCount(result));
    for (size_t logicalIndex = 0;
         logicalIndex < runtimeObjectElementCount(result); ++logicalIndex) {
        const auto* element =
            runtimeObjectLogicalElement(result, logicalIndex);
        if (!element) {
            return failure("object assignment produced invalid storage");
        }
        logicalElements.push_back(*element);
    }
    return runtimeMakeObjectArrayFromLogicalOrder(
        std::move(logicalElements), runtimeDimensions(result),
        target.className, target.handleObject, policy, target.className);
}

RuntimeObjectOperationResult runtimeEnsureObjectIndexedCapacity(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeObjectArrayPolicy& policy) {
    if (!isRuntimeClassObject(target)) {
        return failure(
            "object indexed growth requires a class object target");
    }
    const auto selections =
        runtimeResolveIndexSelections(target, subscripts, true);
    if (!selections.succeeded) {
        return failure(selections.error);
    }
    return ensureObjectCapacity(target, selections, false, policy);
}

RuntimeObjectOperationResult runtimeDeleteObjectIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts,
    const RuntimeObjectArrayPolicy& policy) {
    if (!isRuntimeClassObject(target)) {
        return failure("object deletion requires a class object target");
    }
    if (colonSubscripts.size() != subscripts.size()) {
        return failure(
            "object deletion subscript metadata is inconsistent");
    }
    const auto selections =
        runtimeResolveIndexSelections(target, subscripts, false);
    if (!selections.succeeded) {
        return failure(selections.error);
    }

    const auto oldDimensions = runtimeDimensions(target);
    if (selections.indices.size() == 1) {
        const auto removed = uniqueIndices(selections.indices.front());
        const size_t oldCount = runtimeObjectElementCount(target);
        const bool vectorShape = oldDimensions.size() == 2 &&
                                 (oldDimensions[0] == 1 ||
                                  oldDimensions[1] == 1);
        if (!vectorShape && removed.size() != oldCount) {
            return failure(
                "linear object deletion requires a vector or all elements");
        }
        std::vector<bool> removeMask(oldCount, false);
        for (const size_t index : removed) {
            if (index >= oldCount) {
                return failure("object index is out of bounds");
            }
            removeMask[index] = true;
        }
        std::vector<RuntimeValue> kept;
        kept.reserve(oldCount - removed.size());
        for (size_t logicalIndex = 0; logicalIndex < oldCount;
             ++logicalIndex) {
            if (removeMask[logicalIndex]) {
                continue;
            }
            const auto* element =
                runtimeObjectLogicalElement(target, logicalIndex);
            if (!element) {
                return failure("object deletion could not map an element");
            }
            kept.push_back(*element);
        }
        std::vector<size_t> newDimensions;
        if (kept.empty() && (oldCount == 1 || !vectorShape)) {
            newDimensions = {0, 0};
        } else if (oldDimensions[0] == 1) {
            newDimensions = {1, kept.size()};
        } else {
            newDimensions = {kept.size(), 1};
        }
        return runtimeMakeObjectArrayFromLogicalOrder(
            std::move(kept), std::move(newDimensions), target.className,
            target.handleObject, policy, target.className);
    }

    size_t deletionDimension = selections.indices.size();
    for (size_t index = 0; index < colonSubscripts.size(); ++index) {
        if (colonSubscripts[index]) {
            continue;
        }
        if (deletionDimension != selections.indices.size()) {
            return failure(
                "object deletion can have only one non-colon subscript");
        }
        deletionDimension = index;
    }
    if (deletionDimension == selections.indices.size()) {
        return failure(
            "object deletion requires one non-colon subscript");
    }
    if (selections.indices.size() < oldDimensions.size()) {
        return failure(
            "N-dimensional object deletion requires one subscript per dimension");
    }

    auto sourceDimensions = oldDimensions;
    sourceDimensions.resize(selections.indices.size(), 1);
    const auto removed =
        uniqueIndices(selections.indices[deletionDimension]);
    std::vector<bool> removeMask(
        sourceDimensions[deletionDimension], false);
    for (const size_t index : removed) {
        if (index >= removeMask.size()) {
            return failure("object index is out of bounds");
        }
        removeMask[index] = true;
    }
    std::vector<size_t> removedBefore(removeMask.size() + 1, 0);
    for (size_t index = 0; index < removeMask.size(); ++index) {
        removedBefore[index + 1] =
            removedBefore[index] + (removeMask[index] ? 1 : 0);
    }
    auto newDimensions = sourceDimensions;
    newDimensions[deletionDimension] -= removed.size();
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    if (!newCount) {
        return failure("object deletion dimensions are too large");
    }
    std::vector<RuntimeValue> storage(*newCount);
    for (size_t sourceOffset = 0;
         sourceOffset < runtimeObjectElementCount(target); ++sourceOffset) {
        auto coordinates =
            runtimeRowMajorCoordinates(sourceOffset, sourceDimensions);
        const size_t selected = coordinates[deletionDimension];
        if (removeMask[selected]) {
            continue;
        }
        coordinates[deletionDimension] -= removedBefore[selected];
        const auto destination = runtimeRowMajorStorageOffset(
            coordinates, newDimensions);
        const auto* element = runtimeObjectElement(target, sourceOffset);
        if (!destination || !element) {
            return failure("object deletion could not map an element");
        }
        storage[*destination] = *element;
    }
    return runtimeMakeObjectArrayFromStorageOrder(
        std::move(storage), std::move(newDimensions), target.className,
        target.handleObject, policy, target.className);
}

RuntimeObjectOperationResult runtimeConcatenateObject(
    size_t dimension,
    const std::vector<RuntimeValue>& values,
    const RuntimeObjectArrayPolicy& policy) {
    if (dimension == 0 || values.empty()) {
        return failure(
            "object concatenation requires a positive dimension and values");
    }
    for (const auto& value : values) {
        if (!isRuntimeClassObject(value)) {
            return failure(
                "object concatenation requires class object arrays");
        }
    }

    size_t dimensionCount = std::max<size_t>(dimension, 2);
    for (const auto& value : values) {
        dimensionCount =
            std::max(dimensionCount, runtimeDimensionCount(value));
    }
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
                if (index != axis &&
                    dimensions[index] != inputDimensions.front()[index]) {
                    return failure(
                        "object concatenation dimensions must agree outside the selected dimension");
                }
            }
        }
        if (outputDimensions[axis] >
            std::numeric_limits<size_t>::max() - dimensions[axis]) {
            return failure(
                "object concatenation dimensions are too large");
        }
        outputDimensions[axis] += dimensions[axis];
        inputDimensions.push_back(std::move(dimensions));
    }
    const auto count = checkedRuntimeDimensionProduct(outputDimensions);
    if (!count) {
        return failure("object concatenation dimensions are too large");
    }

    std::vector<RuntimeValue> storage(*count);
    size_t axisOffset = 0;
    for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
        const auto& value = values[valueIndex];
        for (size_t sourceOffset = 0;
             sourceOffset < runtimeObjectElementCount(value);
             ++sourceOffset) {
            auto coordinates = runtimeRowMajorCoordinates(
                sourceOffset, inputDimensions[valueIndex]);
            coordinates[axis] += axisOffset;
            const auto destination = runtimeRowMajorStorageOffset(
                coordinates, outputDimensions);
            const auto* element = runtimeObjectElement(value, sourceOffset);
            if (!destination || !element) {
                return failure(
                    "object concatenation could not map an element");
            }
            storage[*destination] = *element;
        }
        axisOffset += inputDimensions[valueIndex][axis];
    }
    return runtimeMakeObjectArrayFromStorageOrder(
        std::move(storage), std::move(outputDimensions),
        values.front().className, values.front().handleObject, policy);
}

bool runtimeObjectArraysEqual(
    const RuntimeValue& left,
    const RuntimeValue& right,
    const RuntimeObjectElementEquality& elementEquality) {
    if (!isRuntimeClassObject(left) || !isRuntimeClassObject(right) ||
        !elementEquality ||
        left.className != right.className ||
        left.handleObject != right.handleObject ||
        runtimeDimensions(left) != runtimeDimensions(right) ||
        runtimeObjectElementCount(left) !=
            runtimeObjectElementCount(right)) {
        return false;
    }
    for (size_t logicalIndex = 0;
         logicalIndex < runtimeObjectElementCount(left);
         ++logicalIndex) {
        const auto* leftElement =
            runtimeObjectLogicalElement(left, logicalIndex);
        const auto* rightElement =
            runtimeObjectLogicalElement(right, logicalIndex);
        if (!leftElement || !rightElement ||
            !elementEquality(*leftElement, *rightElement)) {
            return false;
        }
    }
    return true;
}

RuntimeObjectOperationResult runtimeCompareObjectArrays(
    const RuntimeValue& left,
    const RuntimeValue& right,
    bool negate,
    const RuntimeObjectElementEquality& elementEquality) {
    if (!isRuntimeClassObject(left) || !isRuntimeClassObject(right) ||
        !elementEquality) {
        return failure("object comparison requires class object values");
    }
    const auto dimensions = runtimeImplicitExpansionDimensions(
        runtimeDimensions(left), runtimeDimensions(right));
    if (!dimensions) {
        return failure(
            "object comparison dimensions are not compatible");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure("object comparison dimensions are too large");
    }

    const auto leftDimensions = runtimeDimensions(left);
    const auto rightDimensions = runtimeDimensions(right);
    std::vector<double> values;
    values.reserve(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count;
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, *dimensions);
        const auto leftOffset = coordinates
                                    ? runtimeImplicitExpansionStorageOffset(
                                          *coordinates, leftDimensions)
                                    : std::nullopt;
        const auto rightOffset = coordinates
                                     ? runtimeImplicitExpansionStorageOffset(
                                           *coordinates, rightDimensions)
                                     : std::nullopt;
        const auto* leftElement = leftOffset
                                      ? runtimeObjectElement(
                                            left, *leftOffset)
                                      : nullptr;
        const auto* rightElement = rightOffset
                                       ? runtimeObjectElement(
                                             right, *rightOffset)
                                       : nullptr;
        if (!leftElement || !rightElement) {
            return failure("object comparison could not map an element");
        }
        const bool equal = elementEquality(*leftElement, *rightElement);
        values.push_back((negate ? !equal : equal) ? 1.0 : 0.0);
    }
    const auto result = runtimeNumericValueFromLogicalOrder(
        *dimensions, std::move(values), RuntimeNumericClass::Logical);
    return result ? success(*result)
                  : failure("object comparison result has an invalid shape");
}

} // namespace mparser
