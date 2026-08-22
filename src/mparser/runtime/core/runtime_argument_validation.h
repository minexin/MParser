#pragma once

#include "mparser/semantic/function_signature.h"
#include "mparser/semantic/property_spec.h"
#include "mparser/runtime/core/runtime_value.h"

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
    RuntimeWorkspace nameValueArguments;
    size_t positionalArgumentCount = 0;
    std::string error;
};

struct RuntimeOutputArgumentContract {
    std::string name;
    PropertySpec spec;
    SourceSpan span;
    bool repeating = false;
};

struct RuntimeOutputValidationResult {
    bool succeeded = true;
    std::string argumentName;
    SourceSpan span;
    std::string error;
};

RuntimeArgumentValidationResult validateRuntimeArgument(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options = {});

RuntimeInvocationNormalizationResult normalizeRuntimeInvocationArguments(
    const FunctionSignature& signature,
    const std::vector<std::string>& nameValueDeclarations,
    const std::vector<RuntimeValue>& arguments);

void initializeRuntimeFunctionOutputs(
    RuntimeWorkspace& frame,
    const FunctionSignature& signature);

RuntimeOutputValidationResult validateRuntimeFunctionOutputs(
    RuntimeWorkspace& frame,
    const std::vector<RuntimeOutputArgumentContract>& contracts,
    const RuntimeArgumentValidationOptions& options = {});

std::vector<RuntimeValue> collectRuntimeFunctionOutputs(
    const RuntimeWorkspace& frame,
    const FunctionSignature& signature, size_t requestedOutputCount);

std::vector<std::string> runtimeFunctionOutputNames(
    const FunctionSignature& signature, size_t requestedOutputCount);

} // namespace mparser
