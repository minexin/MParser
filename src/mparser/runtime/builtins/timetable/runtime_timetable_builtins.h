#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeTimetableBuiltin(std::string_view name);

BuiltinResult invokeRuntimeTimetableBuiltin(
    std::string_view name, const BuiltinCall& call);

} // namespace mparser
