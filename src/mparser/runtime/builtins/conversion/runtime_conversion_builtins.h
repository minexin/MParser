#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeConversionLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeConversionLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
