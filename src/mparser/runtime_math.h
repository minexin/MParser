#pragma once

#include "mparser/runtime_value.h"

#include <optional>
#include <string_view>

namespace mparser {

bool isRuntimePureUnaryMathBuiltin(std::string_view name);

std::optional<double>
runtimeApplyPureUnaryMathBuiltin(std::string_view name, double value);

std::optional<RuntimeValue>
runtimeApplyPureUnaryMathBuiltin(std::string_view name,
                                 const RuntimeValue& value);

} // namespace mparser
