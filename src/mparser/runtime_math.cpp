#include "mparser/runtime_math.h"

#include <cmath>

namespace mparser {

bool isRuntimePureUnaryMathBuiltin(std::string_view name) {
    return name == "abs" || name == "acos" || name == "asin" ||
           name == "atan" || name == "cos" || name == "exp" ||
           name == "log" || name == "sin" || name == "sqrt" ||
           name == "tan";
}

std::optional<double>
runtimeApplyPureUnaryMathBuiltin(std::string_view name, double value) {
    if (name == "abs") {
        return std::fabs(value);
    }
    if (name == "acos") {
        return std::acos(value);
    }
    if (name == "asin") {
        return std::asin(value);
    }
    if (name == "atan") {
        return std::atan(value);
    }
    if (name == "cos") {
        return std::cos(value);
    }
    if (name == "exp") {
        return std::exp(value);
    }
    if (name == "log") {
        return std::log(value);
    }
    if (name == "sin") {
        return std::sin(value);
    }
    if (name == "sqrt") {
        return std::sqrt(value);
    }
    if (name == "tan") {
        return std::tan(value);
    }
    return std::nullopt;
}

} // namespace mparser
