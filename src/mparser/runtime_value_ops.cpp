#include "mparser/runtime_value_ops.h"

#include "mparser/runtime_metadata.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_object.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_text.h"

#include <utility>

namespace mparser {

bool isRuntimeCommaSeparatedList(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::CommaSeparatedList;
}

RuntimeValue makeRuntimeCommaSeparatedList(
    std::vector<RuntimeValue> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::CommaSeparatedList;
    result.cells = std::move(values);
    setRuntimeDimensions(result, {1, result.cells.size()});
    return result;
}

void appendRuntimeExpandedValues(
    std::vector<RuntimeValue>& destination,
    const RuntimeValue& value) {
    if (isRuntimeCommaSeparatedList(value)) {
        destination.insert(destination.end(), value.cells.begin(),
                           value.cells.end());
        return;
    }
    destination.push_back(value);
}

std::vector<RuntimeValue> runtimeExpandedValues(
    const std::vector<RuntimeValue>& values) {
    std::vector<RuntimeValue> result;
    for (const RuntimeValue& value : values) {
        appendRuntimeExpandedValues(result, value);
    }
    return result;
}

RuntimeSingleValueResult runtimeRequireSingleValue(
    const RuntimeValue& value, std::string_view context) {
    if (!isRuntimeCommaSeparatedList(value)) {
        return RuntimeSingleValueResult{true, value, {}};
    }
    if (value.cells.size() == 1) {
        return RuntimeSingleValueResult{true, value.cells.front(), {}};
    }

    std::string error(context);
    error += " produced ";
    error += std::to_string(value.cells.size());
    error += " comma-separated values where one was required";
    return RuntimeSingleValueResult{false, {}, std::move(error)};
}

bool runtimeValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    RuntimeNaNEquality nanEquality) {
    if (isRuntimeNumericValue(left) &&
        isRuntimeNumericValue(right)) {
        return runtimeNumericValuesEqual(
            left, right, nanEquality == RuntimeNaNEquality::Equal);
    }
    if (isRuntimeTextValue(left) && isRuntimeTextValue(right)) {
        return runtimeTextPayloadEqual(left, right);
    }
    if (left.kind != right.kind ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }

    switch (left.kind) {
    case RuntimeValueKind::Missing:
        return true;
    case RuntimeValueKind::Number:
    case RuntimeValueKind::CharacterArray:
    case RuntimeValueKind::StringArray:
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        return false;
    case RuntimeValueKind::Cell:
    case RuntimeValueKind::CommaSeparatedList:
        if (left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeValuesEqual(
                    left.cells[index], right.cells[index],
                    nanEquality)) {
                return false;
            }
        }
        return true;
    case RuntimeValueKind::FunctionHandle:
        return left.functionHandle && right.functionHandle &&
               left.functionHandle->identity ==
                   right.functionHandle->identity;
    case RuntimeValueKind::NameValueArgument:
        return left.text == right.text && left.cells.size() == 1 &&
               right.cells.size() == 1 &&
               runtimeValuesEqual(
                   left.cells.front(), right.cells.front(), nanEquality);
    case RuntimeValueKind::Struct:
        if (runtimeStructFieldOrder(left) !=
                runtimeStructFieldOrder(right) ||
            runtimeStructElementCount(left) !=
                runtimeStructElementCount(right)) {
            return false;
        }
        for (size_t offset = 0;
             offset < runtimeStructElementCount(left); ++offset) {
            const auto* leftElement = runtimeStructElement(left, offset);
            const auto* rightElement = runtimeStructElement(right, offset);
            if (!leftElement || !rightElement ||
                leftElement->size() != rightElement->size()) {
                return false;
            }
            for (const auto& [name, value] : *leftElement) {
                const auto other = rightElement->find(name);
                if (other == rightElement->end() ||
                    !runtimeValuesEqual(
                        value, other->second, nanEquality)) {
                    return false;
                }
            }
        }
        return true;
    case RuntimeValueKind::Object:
        break;
    }

    if (isRuntimeMetadataObject(left) ||
        isRuntimeMetadataObject(right)) {
        if (!isRuntimeMetadataObject(left) ||
            !isRuntimeMetadataObject(right) ||
            canonicalRuntimeMetadataClassName(left.className) !=
                canonicalRuntimeMetadataClassName(right.className)) {
            return false;
        }
        if (isRuntimeMetadataScalar(left) ||
            isRuntimeMetadataScalar(right)) {
            return isRuntimeMetadataScalar(left) &&
                   isRuntimeMetadataScalar(right) &&
                   left.text == right.text;
        }
        if (left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeValuesEqual(
                    left.cells[index], right.cells[index],
                    nanEquality)) {
                return false;
            }
        }
        return true;
    }

    if (isRuntimeClassObject(left) &&
        isRuntimeClassObject(right) &&
        (!isRuntimeScalarObject(left) ||
         !isRuntimeScalarObject(right))) {
        return runtimeObjectArraysEqual(
            left, right,
            [nanEquality](const RuntimeValue& leftElement,
                          const RuntimeValue& rightElement) {
                return runtimeValuesEqual(
                    leftElement, rightElement, nanEquality);
            });
    }
    if (!left.enumerationMemberName.empty() ||
        !right.enumerationMemberName.empty()) {
        return left.className == right.className &&
               left.enumerationMemberName ==
                   right.enumerationMemberName;
    }
    if (left.className != right.className ||
        left.handleObject != right.handleObject) {
        return false;
    }
    if (left.handleObject) {
        return left.sharedFields && right.sharedFields &&
               left.sharedFields.get() == right.sharedFields.get();
    }

    const auto& leftFields = left.fields;
    const auto& rightFields = right.fields;
    if (leftFields.size() != rightFields.size()) {
        return false;
    }
    for (const auto& [name, value] : leftFields) {
        const auto other = rightFields.find(name);
        if (other == rightFields.end() ||
            !runtimeValuesEqual(value, other->second, nanEquality)) {
            return false;
        }
    }
    return true;
}

} // namespace mparser
