#pragma once

#include "mparser/interpreter.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeWarningRecord {
    std::string identifier;
    std::string message;
};

struct RuntimeWarningState {
    bool allEnabled = true;
    bool backtraceEnabled = true;
    bool verboseEnabled = false;
    std::map<std::string, bool> identifierStates;
    std::string lastMessage;
    std::string lastIdentifier;
};

struct RuntimeWarningOperationResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::optional<RuntimeWarningRecord> emitted;
    std::string error;
};

RuntimeWarningOperationResult runtimeWarning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount, RuntimeWarningState& state);

RuntimeWarningOperationResult runtimeLastWarning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount, RuntimeWarningState& state);

bool runtimeWarningIsEnabled(const RuntimeWarningState& state,
                             std::string_view identifier);

} // namespace mparser
