#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeSparseBuiltin(std::string_view name);

BuiltinResult invokeRuntimeSparseBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
