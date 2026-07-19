#include "mparser/runtime_value_ops.h"

#include "mparser/runtime_shape.h"

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

} // namespace mparser
