#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeDateTimeBuiltin(std::string_view name);

BuiltinResult invokeRuntimeDateTimeBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
