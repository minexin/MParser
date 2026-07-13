#pragma once

#include "mparser/source.h"

#include <string>
#include <vector>

namespace mparser {

struct PropertyDimensionSpec {
    std::string text;
    SourceSpan span;
};

struct PropertyValidatorSpec {
    std::string name;
    std::string raw;
    SourceSpan span;
    std::vector<std::string> arguments;
};

struct PropertySpec {
    std::vector<PropertyDimensionSpec> dimensions;
    std::string className;
    SourceSpan classSpan;
    std::vector<PropertyValidatorSpec> validators;
    bool hasExplicitDefault = false;
};

} // namespace mparser
