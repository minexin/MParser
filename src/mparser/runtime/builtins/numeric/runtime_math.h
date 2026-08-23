#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <optional>
#include <string_view>
#include <vector>

namespace mparser {

bool isRuntimePureUnaryMathBuiltin(std::string_view name);

std::optional<double>
runtimeApplyPureUnaryMathBuiltin(std::string_view name, double value);

std::optional<RuntimeValue>
runtimeApplyPureUnaryMathBuiltin(std::string_view name,
                                 const RuntimeValue& value);

bool isRuntimePureBinaryMathBuiltin(std::string_view name);

std::optional<RuntimeValue>
runtimeApplyPureBinaryMathBuiltin(std::string_view name,
                                  const RuntimeValue& left,
                                  const RuntimeValue& right);

std::optional<RuntimeValue>
runtimeEpsilonBuiltin(const std::vector<RuntimeValue>& arguments);

} // namespace mparser
