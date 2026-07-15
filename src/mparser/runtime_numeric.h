#pragma once

#include "mparser/interpreter.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace mparser {

bool isRuntimeNumericValue(const RuntimeValue& value);

bool isRuntimeLogical(const RuntimeValue& value);

std::string_view runtimeNumericClassName(RuntimeNumericClass numericClass);

std::optional<double> runtimeCoerceNumericElement(
    double value, RuntimeNumericClass numericClass);

std::optional<double> runtimeNumericElement(
    const RuntimeValue& value, size_t logicalIndex);

std::optional<RuntimeValue> runtimeConvertNumericClass(
    RuntimeValue value, RuntimeNumericClass numericClass);

} // namespace mparser
