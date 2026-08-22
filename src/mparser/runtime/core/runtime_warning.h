#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <map>
#include <mutex>
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

class RuntimeWarningContext {
public:
    RuntimeWarningOperationResult warning(
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount);
    RuntimeWarningOperationResult lastWarning(
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount);

    RuntimeWarningState snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    RuntimeWarningState state_;
};

} // namespace mparser
