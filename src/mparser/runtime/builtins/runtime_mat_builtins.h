#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"

#include <string_view>

namespace mparser {

bool isRuntimeMatBuiltin(std::string_view name);

BuiltinResult invokeRuntimeMatBuiltin(std::string_view name,
                                      const BuiltinCall &call);

} // namespace mparser
