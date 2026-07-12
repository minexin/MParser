#pragma once

#include "mparser/bytecode.h"
#include "mparser/semantic.h"

#include <iosfwd>

namespace mparser {

void dumpBytecode(std::ostream& output, const BytecodeProgram& program,
                  const SemanticResult& semantic);

} // namespace mparser
