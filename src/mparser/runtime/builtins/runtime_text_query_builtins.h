#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTextQueryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTextQueryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
