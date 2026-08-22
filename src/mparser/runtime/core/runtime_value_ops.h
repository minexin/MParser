#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeSingleValueResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

enum class RuntimeNaNEquality {
    Unequal,
    Equal,
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

bool runtimeValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    RuntimeNaNEquality nanEquality = RuntimeNaNEquality::Unequal);

} // namespace mparser
