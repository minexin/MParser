#pragma once

#include "mparser/frontend/diagnostic.h"
#include "mparser/runtime/core/session/runtime_output.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/semantic/semantic.h"

#include <memory>
#include <string>
#include <vector>

namespace mparser {

struct InterpreterResult {
    std::vector<RuntimeVariable> variables;
    std::vector<RuntimeOutputEvent> outputEvents;
    std::vector<RuntimeExpressionResult> expressionResults;
    std::vector<Diagnostic> diagnostics;
};

struct InterpreterOptions {
    std::shared_ptr<RuntimeSessionState> sessionState;
    std::shared_ptr<RuntimeCallableContext> callableContext;
    RuntimeOutputSink outputSink;
    std::shared_ptr<RuntimeExecutionControl> executionControl = {};
};

class Interpreter {
public:
    InterpreterResult run(const SemanticResult& semantic);
    InterpreterResult run(const SemanticResult& semantic,
                          const InterpreterOptions& options);
};

} // namespace mparser
