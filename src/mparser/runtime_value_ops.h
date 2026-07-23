#pragma once

#include "mparser/runtime_value.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeSingleValueResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

bool isRuntimeCommaSeparatedList(const RuntimeValue& value);

RuntimeValue makeRuntimeCommaSeparatedList(
    std::vector<RuntimeValue> values);

void appendRuntimeExpandedValues(
    std::vector<RuntimeValue>& destination,
    const RuntimeValue& value);

std::vector<RuntimeValue> runtimeExpandedValues(
    const std::vector<RuntimeValue>& values);

RuntimeSingleValueResult runtimeRequireSingleValue(
    const RuntimeValue& value, std::string_view context);

} // namespace mparser
