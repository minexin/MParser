#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeAdvancedNumericBuiltin(std::string_view name);

BuiltinResult invokeRuntimeAdvancedNumericBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
