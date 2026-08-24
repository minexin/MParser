#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTableBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTableBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
