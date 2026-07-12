#pragma once

#include "mparser/syntax.h"

#include <iosfwd>

namespace mparser {

void dumpSyntaxTree(std::ostream& output, const SyntaxNode& node);

} // namespace mparser
