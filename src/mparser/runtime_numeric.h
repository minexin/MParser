#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace mparser {

bool isRuntimeNumericValue(const RuntimeValue& value);

bool isRuntimeLogical(const RuntimeValue& value);

std::string_view runtimeNumericClassName(RuntimeNumericClass numericClass);

std::optional<double> runtimeCoerceNumericElement(
    double value, RuntimeNumericClass numericClass);

std::optional<double> runtimeNumericElement(
    const RuntimeValue& value, size_t logicalIndex);

std::optional<RuntimeValue> runtimeNumericValueFromLogicalOrder(
    std::vector<size_t> dimensions, std::vector<double> values,
    RuntimeNumericClass numericClass);

std::optional<std::vector<RuntimeValue>>
runtimeNumericForLoopColumns(const RuntimeValue& value);

std::optional<RuntimeValue> runtimeConvertNumericClass(
    RuntimeValue value, RuntimeNumericClass numericClass);

} // namespace mparser
