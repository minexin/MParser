#pragma once

#include "mparser/semantic.h"

#include <string>
#include <vector>

namespace mparser {

struct FunctionSignature {
    std::vector<std::string> outputs;
    std::vector<std::string> parameters;
};

FunctionSignature parseFunctionSignature(const HirNode& functionNode);

} // namespace mparser
