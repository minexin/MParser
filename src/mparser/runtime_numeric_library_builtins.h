#pragma once

#include "mparser/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeNumericLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeNumericLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
