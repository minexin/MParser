#pragma once

#include "mparser/frontend/source.h"

#include <functional>
#include <string>

namespace mparser {

class RuntimeExecutionControl;

enum class RuntimeInputMode { Expression, Text, Command };
enum class RuntimeInputStatus { Ready, Pending, EndOfInput, Error, Stopped };

struct RuntimeInputRequest {
    std::string prompt;
    RuntimeInputMode mode = RuntimeInputMode::Expression;
    SourceSpan span;
};

struct RuntimeInputResult {
    RuntimeInputStatus status = RuntimeInputStatus::EndOfInput;
    std::string text;
    std::string error;
};

// Polls run on the invocation thread. Hosts must return promptly, use Pending
// when no complete line is available, and must not retain the borrowed request.
// A Ready empty string is an empty line, not end-of-input.
using RuntimeInputSource =
    std::function<RuntimeInputResult(const RuntimeInputRequest&)>;

RuntimeInputResult readRuntimeInput(
    const RuntimeInputSource& source, const RuntimeInputRequest& request,
    RuntimeExecutionControl& control);

} // namespace mparser
