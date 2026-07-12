#pragma once

#include "mparser/source.h"

#include <string>

namespace mparser {

struct Diagnostic {
    SourceSpan span;
    std::string message;
};

} // namespace mparser
