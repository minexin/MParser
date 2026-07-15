#pragma once

#include <optional>
#include <string_view>

namespace mparser {

bool isRuntimePureUnaryMathBuiltin(std::string_view name);

std::optional<double>
runtimeApplyPureUnaryMathBuiltin(std::string_view name, double value);

} // namespace mparser
