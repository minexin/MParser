#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeSetLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeSetLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
