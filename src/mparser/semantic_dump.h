#pragma once

#include "mparser/semantic.h"

#include <iosfwd>

namespace mparser {

void dumpSemanticTree(std::ostream& output, const SemanticResult& result);

} // namespace mparser
