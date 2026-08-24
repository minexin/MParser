#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeCategoricalBuiltin(std::string_view name);

BuiltinResult invokeRuntimeCategoricalBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
