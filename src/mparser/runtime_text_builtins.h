#pragma once

#include "mparser/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTextLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTextLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
