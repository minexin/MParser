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

RuntimeValue makeRuntimeStructValue(
    std::map<std::string, RuntimeValue> fields = {});
RuntimeValue makeRuntimeNameValueArgument(std::string name,
                                          RuntimeValue value);
std::shared_ptr<RuntimeCallableContext> makeRuntimeCallableContext();
RuntimeValue makeRuntimeFunctionHandleValue(RuntimeFunctionHandle handle);
std::string runtimeFunctionHandleText(const RuntimeValue& value);
RuntimeValue runtimeFunctionHandleMetadata(const RuntimeValue& value);
std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
