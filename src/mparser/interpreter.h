#pragma once

#include "mparser/diagnostic.h"
#include "mparser/runtime_session_state.h"
#include "mparser/runtime_value.h"
#include "mparser/semantic.h"

#include <memory>
#include <string>
#include <vector>

namespace mparser {

struct InterpreterResult {
    std::vector<RuntimeVariable> variables;
    std::vector<Diagnostic> diagnostics;
};

struct InterpreterOptions {
    std::shared_ptr<RuntimeSessionState> sessionState;
    std::shared_ptr<RuntimeCallableContext> callableContext;
};

class Interpreter {
public:
    InterpreterResult run(const SemanticResult& semantic);
    InterpreterResult run(const SemanticResult& semantic,
                          const InterpreterOptions& options);
};

} // namespace mparser
