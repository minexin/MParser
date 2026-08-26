#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTableRelationalBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTableRelationalBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
