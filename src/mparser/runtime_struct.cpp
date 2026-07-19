#include "mparser/runtime_struct.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <set>
#include <utility>

namespace mparser {
namespace {

constexpr size_t kMaximumFieldNameLength = 63;

RuntimeValue stringValue(std::string value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::String;
    result.text = std::move(value);
    setRuntimeDimensions(result, {1, result.text.size()});
    return result;
}

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
    if (value.kind == RuntimeValueKind::String) {
        return FieldNameListResult{true, {value.text}, {1, 1}, {}};
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
        if (element.kind != RuntimeValueKind::String) {
            return FieldNameListResult{
                false, {}, {},
                "every Cell field name must be a character vector or "
                "string scalar"};
        }
        names.push_back(element.text);
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
    if (value.kind != RuntimeValueKind::String) {
        return RuntimeStructFieldNameResult{
            false, {},
            "dynamic field name must be a character vector or string "
            "scalar"};
    }
    if (!isRuntimeStructFieldName(value.text)) {
        return RuntimeStructFieldNameResult{
            false, {}, "invalid structure field name: " + value.text};
    }
    return RuntimeStructFieldNameResult{true, value.text, {}};
}

std::vector<std::string>
runtimeStructFieldOrder(const RuntimeValue& value) {
    std::vector<std::string> result;
    result.reserve(value.fields.size());
    std::set<std::string> seen;

    for (const std::string& name : value.fieldOrder) {
        if (value.fields.contains(name) && seen.insert(name).second) {
            result.push_back(name);
        }
    }
    for (const auto& [name, fieldValue] : value.fields) {
        (void)fieldValue;
        if (seen.insert(name).second) {
            result.push_back(name);
        }
    }
    return result;
}

bool runtimeSetStructField(RuntimeValue& structure, std::string name,
                           RuntimeValue value) {
    if (structure.kind != RuntimeValueKind::Struct) {
        return false;
    }
    if (!structure.fields.contains(name)) {
        structure.fieldOrder.push_back(name);
    }
    structure.fields[std::move(name)] = std::move(value);
    return true;
}

RuntimeStructOperationResult runtimeConstructScalarStruct(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        return success(makeRuntimeStructValue());
    }
    if (arguments.size() % 2 != 0) {
        return failure(
            "scalar struct constructor expects field/value pairs");
    }

    RuntimeValue structure = makeRuntimeStructValue();
    for (size_t index = 0; index < arguments.size(); index += 2) {
        const auto fieldName = runtimeStructFieldName(arguments[index]);
        if (!fieldName.succeeded) {
            return failure(fieldName.error);
        }
        if (structure.fields.contains(fieldName.name)) {
            return failure("duplicate structure field name: " +
                           fieldName.name);
        }

        RuntimeValue value = arguments[index + 1];
        if (value.kind == RuntimeValueKind::Cell) {
            if (runtimeShapeElementCount(value) != 1 ||
                value.cells.size() != 1) {
                return failure(
                    "nonscalar Cell values create structure arrays, which "
                    "are not supported by the scalar struct runtime");
            }
            value = value.cells.front();
        }
        runtimeSetStructField(structure, fieldName.name, std::move(value));
    }
    return success(std::move(structure));
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
        values.push_back(stringValue(name));
    }
    return success(cellValue({names.size(), 1}, std::move(values)));
}

RuntimeStructOperationResult runtimeStructIsField(
    const RuntimeValue& value, const RuntimeValue& names) {
    const auto requested = fieldNameList(names);
    if (!requested.succeeded) {
        return failure(requested.error);
    }

    std::vector<double> matches;
    matches.reserve(requested.names.size());
    for (const std::string& name : requested.names) {
        matches.push_back(value.kind == RuntimeValueKind::Struct &&
                                  value.fields.contains(name)
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

    for (const std::string& name : requested.names) {
        if (!structure.fields.contains(name)) {
            return failure("structure field is not available: " + name);
        }
    }

    RuntimeValue result = structure;
    for (const std::string& name : requested.names) {
        result.fields.erase(name);
        std::erase(result.fieldOrder, name);
    }
    return success(std::move(result));
}

} // namespace mparser
