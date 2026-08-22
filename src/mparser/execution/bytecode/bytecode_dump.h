#pragma once

#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/semantic/semantic.h"

#include <iosfwd>

namespace mparser {

void dumpBytecode(std::ostream& output, const BytecodeProgram& program,
                  const SemanticResult& semantic);

} // namespace mparser
