#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeCallbackLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeCallbackLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
