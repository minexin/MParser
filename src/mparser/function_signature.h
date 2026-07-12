#pragma once

#include "mparser/semantic.h"

#include <string>
#include <vector>

namespace mparser {

struct FunctionSignature {
    std::vector<std::string> outputs;
    std::vector<std::string> parameters;
    bool hasVarargout = false;
    bool hasVarargin = false;
};

FunctionSignature parseFunctionSignature(const HirNode& functionNode);

} // namespace mparser
