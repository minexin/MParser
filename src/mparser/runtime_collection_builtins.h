#pragma once

#include "mparser/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeCollectionLibraryBuiltin(std::string_view name);

BuiltinResult invokeRuntimeCollectionLibraryBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
