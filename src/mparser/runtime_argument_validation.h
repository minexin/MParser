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
    std::map<std::string, RuntimeValue>& frame,
    const FunctionSignature& signature);

RuntimeOutputValidationResult validateRuntimeFunctionOutputs(
    std::map<std::string, RuntimeValue>& frame,
    const std::vector<RuntimeOutputArgumentContract>& contracts,
    const RuntimeArgumentValidationOptions& options = {});

std::vector<RuntimeValue> collectRuntimeFunctionOutputs(
    const std::map<std::string, RuntimeValue>& frame,
    const FunctionSignature& signature, size_t requestedOutputCount);

std::vector<std::string> runtimeFunctionOutputNames(
    const FunctionSignature& signature, size_t requestedOutputCount);

} // namespace mparser
