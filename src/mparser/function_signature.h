#pragma once

#include "mparser/semantic.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class FunctionParameterKind {
    Positional,
    Repeating,
    NameValue,
};

struct FunctionSignature {
    std::vector<std::string> outputs;
    std::vector<std::string> parameters;
    std::vector<FunctionParameterKind> parameterKinds;
    std::string repeatingOutput;
    size_t requiredPositionalParameterCount = 0;
    bool hasVarargout = false;
    bool hasVarargin = false;
    bool vararginRepeating = false;
};

enum class FunctionArgumentCountStatus {
    Valid,
    Mismatch,
    IncompleteRepeatingGroup,
};

FunctionSignature parseFunctionSignature(const HirNode& functionNode);
FunctionSignature parseFunctionSignature(std::string_view declaration,
                                         std::string_view sourceName);
FunctionParameterKind functionParameterKind(const FunctionSignature& signature,
                                            size_t index);
size_t functionPositionalParameterCount(const FunctionSignature& signature);
size_t functionRequiredPositionalParameterCount(
    const FunctionSignature& signature);
size_t functionRepeatingParameterCount(const FunctionSignature& signature);
bool functionHasNameValueParameters(const FunctionSignature& signature);
bool functionHasRepeatingOutput(const FunctionSignature& signature);
size_t functionFixedOutputCount(const FunctionSignature& signature);
std::string_view functionRepeatingOutputName(
    const FunctionSignature& signature);
bool functionOutputCountIsValid(const FunctionSignature& signature,
                                size_t outputCount);
FunctionArgumentCountStatus functionArgumentCountStatus(
    const FunctionSignature& signature, size_t argumentCount);
FunctionArgumentCountStatus functionPositionalArgumentCountStatus(
    const FunctionSignature& signature, size_t argumentCount);

} // namespace mparser
