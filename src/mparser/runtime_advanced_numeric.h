#pragma once

#include "mparser/builtin_registry.h"
#include "mparser/runtime_numeric.h"

#include <string_view>

namespace mparser {

bool isRuntimeAdvancedNumericBuiltin(std::string_view name);

BuiltinResult invokeRuntimeAdvancedNumericBuiltin(
    std::string_view name, const BuiltinCall& call);

RuntimeNumericOperationResult runtimeApplyDenseMatrixDivision(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);

} // namespace mparser
