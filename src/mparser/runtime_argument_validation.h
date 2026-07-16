#pragma once

#include "mparser/interpreter.h"
#include "mparser/property_spec.h"

#include <functional>
#include <string>

namespace mparser {

struct RuntimeArgumentValidationOptions {
    std::function<bool(const std::string&, const std::string&)> objectIsA;
    std::function<bool(const std::string&)> classAvailable;
};

struct RuntimeArgumentValidationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeArgumentValidationResult validateRuntimeArgument(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options = {});

} // namespace mparser
