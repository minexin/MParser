#pragma once

#include "mparser/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeSystemBuiltin(std::string_view name);

BuiltinResult invokeRuntimeSystemBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
