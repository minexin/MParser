#include "mparser/runtime_struct.h"

#include "mparser/runtime_index.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value_ops.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace mparser {
namespace {

constexpr size_t kMaximumFieldNameLength = 63;

RuntimeValue cellValue(std::vector<size_t> dimensions,
                       std::vector<RuntimeValue> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells = std::move(values);
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

struct FieldNameListResult {
    bool succeeded = false;
    std::vector<std::string> names;
    std::vector<size_t> dimensions;
    std::string error;
};

FieldNameListResult fieldNameList(const RuntimeValue& value) {
    if (const auto name = runtimeTextScalarUtf8(value)) {
        return FieldNameListResult{true, {*name}, {1, 1}, {}};
    }
    if (value.kind != RuntimeValueKind::Cell) {
        return FieldNameListResult{
            false, {}, {},
            "field names must be a character vector, string scalar, or "
            "Cell of character vectors"};
    }

    std::vector<std::string> names;
    names.reserve(value.cells.size());
    for (const RuntimeValue& element : value.cells) {
        const auto name = runtimeTextScalarUtf8(element);
        if (!name) {
            return FieldNameListResult{
                false, {}, {},
                "every Cell field name must be a character vector or "
                "string scalar"};
        }
        names.push_back(*name);
    }
    return FieldNameListResult{true, std::move(names),
                               runtimeDimensions(value), {}};
}

RuntimeStructOperationResult failure(std::string error) {
    return RuntimeStructOperationResult{false, {}, std::move(error)};
}

RuntimeStructOperationResult success(RuntimeValue value) {
    return RuntimeStructOperationResult{true, std::move(value), {}};
}

std::vector<size_t> selectionDimensions(
    const std::vector<std::vector<size_t>>& selections) {
    std::vector<size_t> result;
    result.reserve(selections.size());
    for (const auto& selection : selections) {
        result.push_back(selection.size());
    }
    return result;
}

std::vector<size_t> nonSingletonDimensions(
    const std::vector<size_t>& dimensions) {
    std::vector<size_t> result;
    std::copy_if(dimensions.begin(), dimensions.end(),
                 std::back_inserter(result),
                 [](size_t dimension) { return dimension != 1; });
    return result;
}

RuntimeValue emptyDoubleValue() {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Matrix;
    result.numericClass = RuntimeNumericClass::Double;
    setRuntimeDimensions(result, {0, 0});
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

RuntimeStructElement emptyStructElement(
    const std::vector<std::string>& fieldOrder) {
    RuntimeStructElement result;
    for (const std::string& name : fieldOrder) {
        result.emplace(name, emptyDoubleValue());
    }
    return result;
}

bool matchingStructSchema(const RuntimeValue& left,
                          const RuntimeValue& right) {
    return runtimeStructFieldOrder(left) == runtimeStructFieldOrder(right);
}

bool growStructTarget(RuntimeValue& target,
                      const std::vector<size_t>& oldViewDimensions,
                      std::vector<size_t> newDimensions) {
    newDimensions = normalizeRuntimeDimensions(std::move(newDimensions));
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    const size_t oldCount = runtimeStructElementCount(target);
    const auto oldViewCount =
        checkedRuntimeDimensionProduct(oldViewDimensions);
    if (!newCount || !oldViewCount || *oldViewCount != oldCount) {
        return false;
    }

    std::vector<RuntimeStructElement> grown(
        *newCount, emptyStructElement(runtimeStructFieldOrder(target)));
    for (size_t logicalIndex = 0; logicalIndex < oldCount;
         ++logicalIndex) {
        const auto sourceOffset =
            runtimeColumnMajorLinearToStorageOffset(target, logicalIndex);
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldViewDimensions);
        if (!sourceOffset || *sourceOffset >= target.structElements.size() ||
            !coordinates) {
            return false;
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto destinationOffset = runtimeRowMajorStorageOffset(
            *coordinates, newDimensions);
        if (!destinationOffset || *destinationOffset >= grown.size()) {
            return false;
        }
        grown[*destinationOffset] = target.structElements[*sourceOffset];
    }

    target.structElements = std::move(grown);
    setRuntimeDimensions(target, std::move(newDimensions));
    return true;
}

RuntimeStructOperationResult resolveSelections(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts, bool allowGrowth,
    std::vector<std::vector<size_t>>& selections) {
    if (subscripts.empty()) {
        return failure("structure indexing requires subscripts");
    }

    const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        structure, subscripts.size());
    selections.clear();
    selections.reserve(subscripts.size());
    for (size_t index = 0; index < subscripts.size(); ++index) {
        const size_t extent = subscripts.size() == 1
                                  ? runtimeStructElementCount(structure)
                                  : effectiveDimensions[index];
        auto selection = runtimeResolveIndexSelection(
            subscripts[index], extent, allowGrowth);
        if (!selection.succeeded) {
            return failure(std::move(selection.error));
        }
        selections.push_back(std::move(selection.indices));
    }
    return success(RuntimeValue{});
}

RuntimeStructOperationResult ensureLinearCapacity(
    RuntimeValue& target, const std::vector<size_t>& indices) {
    const auto extent = requiredExtent(indices);
    if (!extent || *extent <= runtimeStructElementCount(target)) {
        return success(std::move(target));
    }

    const auto oldDimensions = runtimeDimensions(target);
    std::vector<size_t> newDimensions;
    if (oldDimensions.size() == 2 && oldDimensions[0] == 1) {
        newDimensions = {1, *extent};
    } else if (oldDimensions.size() == 2 && oldDimensions[1] == 1) {
        newDimensions = {*extent, 1};
    } else if (runtimeStructElementCount(target) == 0) {
        newDimensions = {1, *extent};
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
                return failure(
                    "structure assignment dimensions are too large");
            }
            newDimensions.back() = quotient + (remainder == 0 ? 0 : 1);
        }
    }

    if (!growStructTarget(target, oldDimensions, std::move(newDimensions))) {
        return failure("structure assignment dimensions are too large");
    }
    return success(std::move(target));
}

RuntimeStructOperationResult ensureSubscriptCapacity(
    RuntimeValue& target,
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
    if (!growStructTarget(target, oldViewDimensions,
                          std::move(newDimensions))) {
        return failure("structure assignment dimensions are too large");
    }
    return success(std::move(target));
}

} // namespace

bool isRuntimeStructFieldName(std::string_view name) {
    if (name.empty() || name.size() > kMaximumFieldNameLength) {
        return false;
    }

    const auto isAsciiLetter = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z');
    };
    if (!isAsciiLetter(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    return std::all_of(
        name.begin() + 1, name.end(), [&](char character) {
            const unsigned char value =
                static_cast<unsigned char>(character);
            return isAsciiLetter(value) ||
                   (value >= '0' && value <= '9') || value == '_';
        });
}

RuntimeStructFieldNameResult
runtimeStructFieldName(const RuntimeValue& value) {
    const auto name = runtimeTextScalarUtf8(value);
    if (!name) {
        return RuntimeStructFieldNameResult{
            false, {},
            "dynamic field name must be a character vector or string "
            "scalar"};
    }
    if (!isRuntimeStructFieldName(*name)) {
        return RuntimeStructFieldNameResult{
            false, {}, "invalid structure field name: " + *name};
    }
    return RuntimeStructFieldNameResult{true, *name, {}};
}

std::vector<std::string>
runtimeStructFieldOrder(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Struct) {
        return {};
    }

    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const std::string& name : value.fieldOrder) {
        if (seen.insert(name).second) {
            result.push_back(name);
        }
    }

    const RuntimeStructElement* representative = nullptr;
    if (!value.structElements.empty()) {
        representative = &value.structElements.front();
    } else if (!value.fields.empty()) {
        representative = &value.fields;
    }
    if (representative) {
        for (const auto& [name, fieldValue] : *representative) {
            (void)fieldValue;
            if (seen.insert(name).second) {
                result.push_back(name);
            }
        }
    }
    return result;
}

RuntimeValue makeRuntimeStructArrayValue(
    std::vector<std::string> fieldOrder,
    std::vector<RuntimeStructElement> elements,
    std::vector<size_t> dimensions) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Struct;
    result.fieldOrder = std::move(fieldOrder);
    result.structElements = std::move(elements);
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

bool isRuntimeScalarStruct(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Struct &&
           runtimeStructElementCount(value) == 1;
}

size_t runtimeStructElementCount(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Struct) {
        return 0;
    }
    if (!value.structElements.empty()) {
        return value.structElements.size();
    }
    return value.fields.empty() ? 0 : 1;
}

const RuntimeStructElement*
runtimeStructElement(const RuntimeValue& value, size_t storageOffset) {
    if (value.kind != RuntimeValueKind::Struct) {
        return nullptr;
    }
    if (storageOffset < value.structElements.size()) {
        return &value.structElements[storageOffset];
    }
    if (storageOffset == 0 && value.structElements.empty() &&
        !value.fields.empty()) {
        return &value.fields;
    }
    return nullptr;
}

const RuntimeValue* runtimeStructField(
    const RuntimeValue& value, std::string_view name,
    size_t storageOffset) {
    const auto* element = runtimeStructElement(value, storageOffset);
    if (!element) {
        return nullptr;
    }
    const auto field = element->find(std::string(name));
    return field == element->end() ? nullptr : &field->second;
}

bool runtimeSetStructField(RuntimeValue& structure, std::string name,
                           RuntimeValue value) {
    if (!isRuntimeScalarStruct(structure)) {
        return false;
    }
    if (structure.structElements.empty()) {
        structure.structElements.push_back(std::move(structure.fields));
        structure.fields.clear();
    }
    if (!structure.structElements.front().contains(name)) {
        structure.fieldOrder.push_back(name);
    }
    structure.structElements.front()[std::move(name)] = std::move(value);
    return true;
}

RuntimeStructOperationResult runtimeConstructScalarStruct(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        return success(makeRuntimeStructArrayValue({}, {{}}, {1, 1}));
    }
    if (arguments.size() == 1 &&
        isRuntimeNumericValue(arguments.front()) &&
        runtimeShapeElementCount(arguments.front()) == 0) {
        return success(makeRuntimeStructArrayValue({}, {}, {0, 0}));
    }
    if (arguments.size() % 2 != 0) {
        return failure("struct constructor expects field/value pairs");
    }

    std::vector<std::string> fieldOrder;
    fieldOrder.reserve(arguments.size() / 2);
    std::set<std::string> seen;
    std::optional<std::vector<size_t>> outputDimensions;
    bool emptyOutput = false;
    for (size_t index = 0; index < arguments.size(); index += 2) {
        const auto fieldName = runtimeStructFieldName(arguments[index]);
        if (!fieldName.succeeded) {
            return failure(fieldName.error);
        }
        if (!seen.insert(fieldName.name).second) {
            return failure("duplicate structure field name: " +
                           fieldName.name);
        }
        fieldOrder.push_back(fieldName.name);

        const RuntimeValue& value = arguments[index + 1];
        if (value.kind != RuntimeValueKind::Cell) {
            continue;
        }
        const size_t count = runtimeShapeElementCount(value);
        if (count == 0) {
            emptyOutput = true;
            continue;
        }
        if (count == 1) {
            continue;
        }
        const auto dimensions = runtimeDimensions(value);
        if (outputDimensions && *outputDimensions != dimensions) {
            return failure(
                "nonscalar Cell values in struct must have matching "
                "dimensions");
        }
        outputDimensions = dimensions;
    }

    if (emptyOutput) {
        if (outputDimensions) {
            return failure(
                "empty and nonscalar Cell values in struct have "
                "incompatible dimensions");
        }
        return success(makeRuntimeStructArrayValue(
            std::move(fieldOrder), {}, {0, 0}));
    }

    const std::vector<size_t> dimensions =
        outputDimensions.value_or(std::vector<size_t>{1, 1});
    const auto elementCount = checkedRuntimeDimensionProduct(dimensions);
    if (!elementCount) {
        return failure("structure dimensions are too large");
    }
    std::vector<RuntimeStructElement> elements(*elementCount);
    for (size_t pairIndex = 0; pairIndex < arguments.size(); pairIndex += 2) {
        const std::string& name = fieldOrder[pairIndex / 2];
        const RuntimeValue& source = arguments[pairIndex + 1];
        if (source.kind == RuntimeValueKind::Cell) {
            const size_t count = runtimeShapeElementCount(source);
            if (count == 1) {
                if (source.cells.size() != 1) {
                    return failure("scalar Cell struct value has an invalid shape");
                }
                for (auto& element : elements) {
                    element.emplace(name, source.cells.front());
                }
                continue;
            }
            if (source.cells.size() != *elementCount ||
                runtimeDimensions(source) != dimensions) {
                return failure(
                    "nonscalar Cell struct value has an invalid shape");
            }
            for (size_t offset = 0; offset < elements.size(); ++offset) {
                elements[offset].emplace(name, source.cells[offset]);
            }
            continue;
        }
        for (auto& element : elements) {
            element.emplace(name, source);
        }
    }
    return success(makeRuntimeStructArrayValue(
        std::move(fieldOrder), std::move(elements), dimensions));
}

RuntimeStructOperationResult runtimeStructFieldValues(
    const RuntimeValue& structure, std::string_view name) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure("field access requires a structure value");
    }
    const auto order = runtimeStructFieldOrder(structure);
    if (std::find(order.begin(), order.end(), name) == order.end()) {
        return failure("structure field is not available: " +
                       std::string(name));
    }

    const size_t count = runtimeStructElementCount(structure);
    std::vector<RuntimeValue> values;
    values.reserve(count);
    for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            structure, logicalIndex);
        const RuntimeValue* field =
            offset ? runtimeStructField(structure, name, *offset) : nullptr;
        if (!field) {
            return failure("structure storage is missing field: " +
                           std::string(name));
        }
        values.push_back(*field);
    }
    if (values.size() == 1) {
        return success(std::move(values.front()));
    }
    return success(makeRuntimeCommaSeparatedList(std::move(values)));
}

RuntimeStructOperationResult runtimeIndexStruct(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure("structure indexing requires a structure target");
    }

    std::vector<std::vector<size_t>> selections;
    auto resolved = resolveSelections(structure, subscripts, false,
                                      selections);
    if (!resolved.succeeded) {
        return resolved;
    }

    std::vector<size_t> resultDimensions;
    std::vector<RuntimeStructElement> resultElements;
    if (selections.size() == 1) {
        const auto selection = runtimeResolveIndexSelection(
            subscripts.front(), runtimeStructElementCount(structure), false);
        resultDimensions = runtimeLinearIndexResultDimensions(
            structure, subscripts.front(), selection.indices.size(),
            selection.logicalMask);
        resultElements.reserve(selection.indices.size());
        for (const size_t logicalIndex : selection.indices) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                structure, logicalIndex);
            if (!offset || *offset >= structure.structElements.size()) {
                return failure("structure indexing could not map a linear index");
            }
            resultElements.push_back(structure.structElements[*offset]);
        }
    } else {
        resultDimensions = selectionDimensions(selections);
        const auto count = checkedRuntimeDimensionProduct(resultDimensions);
        if (!count) {
            return failure("structure indexed result dimensions are too large");
        }
        resultElements.reserve(*count);
        const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
            structure, subscripts.size());
        for (size_t outputOffset = 0; outputOffset < *count;
             ++outputOffset) {
            const auto outputCoordinates = runtimeRowMajorCoordinates(
                outputOffset, resultDimensions);
            std::vector<size_t> sourceCoordinates(subscripts.size(), 0);
            for (size_t index = 0; index < subscripts.size(); ++index) {
                sourceCoordinates[index] =
                    selections[index][outputCoordinates[index]];
            }
            const auto sourceOffset = runtimeSubscriptsToStorageOffset(
                structure, sourceCoordinates, effectiveDimensions);
            if (!sourceOffset ||
                *sourceOffset >= structure.structElements.size()) {
                return failure("structure indexing could not map subscripts");
            }
            resultElements.push_back(structure.structElements[*sourceOffset]);
        }
    }

    return success(makeRuntimeStructArrayValue(
        runtimeStructFieldOrder(structure), std::move(resultElements),
        std::move(resultDimensions)));
}

RuntimeStructOperationResult runtimeEnsureStructIndexedCapacity(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure(
            "structure indexed growth requires a structure target");
    }

    RuntimeValue result = structure;
    std::vector<std::vector<size_t>> selections;
    auto resolved = resolveSelections(result, subscripts, true, selections);
    if (!resolved.succeeded) {
        return resolved;
    }
    if (selections.size() == 1) {
        return ensureLinearCapacity(result, selections.front());
    }
    return ensureSubscriptCapacity(result, selections);
}

RuntimeStructOperationResult runtimeAlignStructSchemaForCopyback(
    const RuntimeValue& structure,
    const RuntimeValue& nestedValue) {
    if (structure.kind != RuntimeValueKind::Struct ||
        nestedValue.kind != RuntimeValueKind::Struct) {
        return failure(
            "nested structure copyback requires structure values");
    }

    const auto parentOrder = runtimeStructFieldOrder(structure);
    const auto nestedOrder = runtimeStructFieldOrder(nestedValue);
    if (nestedOrder.size() < parentOrder.size() ||
        !std::equal(parentOrder.begin(), parentOrder.end(),
                    nestedOrder.begin())) {
        return failure(
            "subscripted assignment between dissimilar structures");
    }

    RuntimeValue result = structure;
    for (size_t index = parentOrder.size(); index < nestedOrder.size();
         ++index) {
        const std::string& name = nestedOrder[index];
        result.fieldOrder.push_back(name);
        for (auto& element : result.structElements) {
            element.emplace(name, emptyDoubleValue());
        }
        if (result.structElements.empty() && !result.fields.empty()) {
            result.fields.emplace(name, emptyDoubleValue());
        }
    }
    return success(std::move(result));
}

RuntimeStructOperationResult runtimeAssignStructIndexed(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    if (structure.kind != RuntimeValueKind::Struct ||
        value.kind != RuntimeValueKind::Struct) {
        return failure(
            "structure indexed assignment requires structure values");
    }
    if (!matchingStructSchema(structure, value)) {
        return failure(
            "subscripted assignment between dissimilar structures");
    }
    if (runtimeStructElementCount(value) == 0) {
        return failure(
            "structure indexed assignment requires a nonempty value");
    }

    RuntimeValue result = structure;
    std::vector<std::vector<size_t>> selections;
    auto resolved = resolveSelections(result, subscripts, true, selections);
    if (!resolved.succeeded) {
        return resolved;
    }

    const auto dimensions = selectionDimensions(selections);
    const auto selectionCount = checkedRuntimeDimensionProduct(dimensions);
    if (!selectionCount) {
        return failure("structure assignment dimensions are too large");
    }
    const size_t valueCount = runtimeStructElementCount(value);
    const bool scalarExpansion = valueCount == 1;
    if (!scalarExpansion && selections.size() > 1 &&
        nonSingletonDimensions(dimensions) !=
            nonSingletonDimensions(runtimeDimensions(value))) {
        return failure(
            "structure assignment dimensions do not match the right-hand "
            "value");
    }
    if (!scalarExpansion && valueCount != *selectionCount) {
        return failure(
            "structure assignment requires the same number of elements on "
            "both sides");
    }

    RuntimeStructOperationResult capacity;
    if (selections.size() == 1) {
        capacity = ensureLinearCapacity(result, selections.front());
    } else {
        capacity = ensureSubscriptCapacity(result, selections);
    }
    if (!capacity.succeeded) {
        return capacity;
    }
    result = std::move(capacity.value);

    const auto effectiveDimensions = runtimeEffectiveSubscriptDimensions(
        result, selections.size());
    for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
        size_t targetLogicalIndex = 0;
        std::optional<size_t> targetOffset;
        if (selections.size() == 1) {
            targetLogicalIndex = selections.front()[ordinal];
            targetOffset = runtimeColumnMajorLinearToStorageOffset(
                result, targetLogicalIndex);
        } else {
            const auto coordinates = runtimeColumnMajorCoordinates(
                ordinal, dimensions);
            if (!coordinates) {
                return failure("structure assignment has an invalid shape");
            }
            std::vector<size_t> targetCoordinates(selections.size(), 0);
            for (size_t index = 0; index < selections.size(); ++index) {
                targetCoordinates[index] =
                    selections[index][(*coordinates)[index]];
            }
            targetOffset = runtimeSubscriptsToStorageOffset(
                result, targetCoordinates, effectiveDimensions);
        }

        const size_t sourceLogicalIndex = scalarExpansion ? 0 : ordinal;
        const auto sourceOffset = runtimeColumnMajorLinearToStorageOffset(
            value, sourceLogicalIndex);
        if (!targetOffset || *targetOffset >= result.structElements.size() ||
            !sourceOffset || *sourceOffset >= value.structElements.size()) {
            return failure("structure assignment could not map an element");
        }
        result.structElements[*targetOffset] =
            value.structElements[*sourceOffset];
    }
    return success(std::move(result));
}

RuntimeStructOperationResult runtimeDeleteStructIndexed(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure("structure null assignment requires a structure target");
    }
    if (subscripts.size() != 1) {
        return failure(
            "structure null assignment currently requires one linear subscript");
    }

    auto selection = runtimeResolveIndexSelection(
        subscripts.front(), runtimeStructElementCount(structure), false);
    if (!selection.succeeded) {
        return failure(std::move(selection.error));
    }
    std::sort(selection.indices.begin(), selection.indices.end());
    selection.indices.erase(
        std::unique(selection.indices.begin(), selection.indices.end()),
        selection.indices.end());

    const auto oldDimensions = runtimeDimensions(structure);
    const bool vectorShape = oldDimensions.size() == 2 &&
                             (oldDimensions[0] == 1 ||
                              oldDimensions[1] == 1);
    const size_t oldCount = runtimeStructElementCount(structure);
    if (!vectorShape && selection.indices.size() != oldCount) {
        return failure(
            "linear structure null assignment requires a vector or all elements");
    }

    std::vector<bool> removed(oldCount, false);
    for (const size_t index : selection.indices) {
        if (index >= oldCount) {
            return failure("index is out of bounds");
        }
        removed[index] = true;
    }
    std::vector<RuntimeStructElement> kept;
    kept.reserve(oldCount - selection.indices.size());
    for (size_t logicalIndex = 0; logicalIndex < oldCount;
         ++logicalIndex) {
        if (removed[logicalIndex]) {
            continue;
        }
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            structure, logicalIndex);
        if (!offset || *offset >= structure.structElements.size()) {
            return failure("structure null assignment could not map an element");
        }
        kept.push_back(structure.structElements[*offset]);
    }

    std::vector<size_t> newDimensions;
    if (kept.empty() && (oldCount == 1 || !vectorShape)) {
        newDimensions = {0, 0};
    } else if (oldDimensions[0] == 1) {
        newDimensions = {1, kept.size()};
    } else {
        newDimensions = {kept.size(), 1};
    }
    return success(makeRuntimeStructArrayValue(
        runtimeStructFieldOrder(structure), std::move(kept),
        std::move(newDimensions)));
}

RuntimeStructOperationResult
runtimeStructFieldNames(const RuntimeValue& structure) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure("fieldnames expects a structure value");
    }

    const auto names = runtimeStructFieldOrder(structure);
    std::vector<RuntimeValue> values;
    values.reserve(names.size());
    for (const std::string& name : names) {
        values.push_back(makeRuntimeCharacterVectorUtf8(name));
    }
    return success(cellValue({names.size(), 1}, std::move(values)));
}

RuntimeStructOperationResult runtimeStructIsField(
    const RuntimeValue& value, const RuntimeValue& names) {
    const auto requested = fieldNameList(names);
    if (!requested.succeeded) {
        return failure(requested.error);
    }

    const auto available = runtimeStructFieldOrder(value);
    std::vector<double> matches;
    matches.reserve(requested.names.size());
    for (const std::string& name : requested.names) {
        matches.push_back(value.kind == RuntimeValueKind::Struct &&
                                  std::find(available.begin(), available.end(),
                                            name) != available.end()
                              ? 1.0
                              : 0.0);
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        requested.dimensions, std::move(matches),
        RuntimeNumericClass::Logical);
    if (!result) {
        return failure("isfield could not preserve the field-name shape");
    }
    return success(std::move(*result));
}

RuntimeStructOperationResult runtimeRemoveStructFields(
    const RuntimeValue& structure, const RuntimeValue& names) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure("rmfield expects a structure value");
    }
    const auto requested = fieldNameList(names);
    if (!requested.succeeded) {
        return failure(requested.error);
    }

    const auto available = runtimeStructFieldOrder(structure);
    for (const std::string& name : requested.names) {
        if (std::find(available.begin(), available.end(), name) ==
            available.end()) {
            return failure("structure field is not available: " + name);
        }
    }

    RuntimeValue result = structure;
    for (const std::string& name : requested.names) {
        for (auto& element : result.structElements) {
            element.erase(name);
        }
        std::erase(result.fieldOrder, name);
    }
    return success(std::move(result));
}

} // namespace mparser
