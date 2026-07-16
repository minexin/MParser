#pragma once

#include "mparser/interpreter.h"
#include "mparser/function_signature.h"
#include "mparser/property_spec.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

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

struct RuntimeInvocationNormalizationResult {
    bool succeeded = false;
    std::vector<RuntimeValue> positionalArguments;
    std::map<std::string, RuntimeValue> nameValueArguments;
    size_t positionalArgumentCount = 0;
    std::string error;
};

RuntimeArgumentValidationResult validateRuntimeArgument(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options = {});

RuntimeInvocationNormalizationResult normalizeRuntimeInvocationArguments(
    const FunctionSignature& signature,
    const std::vector<std::string>& nameValueDeclarations,
    const std::vector<RuntimeValue>& arguments);

} // namespace mparser
