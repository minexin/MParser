#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTextLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTextLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
