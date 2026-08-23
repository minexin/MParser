#include "mparser/runtime/core/session/runtime_warning.h"

#include "mparser/runtime/core/session/runtime_exception.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <utility>

namespace mparser {
namespace {

RuntimeValue characterValue(std::string_view text) {
    return makeRuntimeCharacterVectorUtf8(text);
}

RuntimeWarningOperationResult failure(std::string error) {
    RuntimeWarningOperationResult result;
    result.error = std::move(error);
    return result;
}

RuntimeWarningOperationResult success(
    std::vector<RuntimeValue> outputs = {},
    std::optional<RuntimeWarningRecord> emitted = std::nullopt) {
    RuntimeWarningOperationResult result;
    result.succeeded = true;
    result.outputs = std::move(outputs);
    result.emitted = std::move(emitted);
    return result;
}

std::string stateText(bool enabled) {
    return enabled ? "on" : "off";
}

RuntimeStructElement settingElement(std::string identifier,
                                    bool enabled) {
    RuntimeStructElement result;
    result.emplace("identifier", characterValue(identifier));
    result.emplace("state", characterValue(stateText(enabled)));
    return result;
}

RuntimeValue settingValue(std::string identifier, bool enabled) {
    std::vector<RuntimeStructElement> elements;
    elements.push_back(settingElement(std::move(identifier), enabled));
    return makeRuntimeStructArrayValue(
        {"identifier", "state"}, std::move(elements), {1, 1});
}

RuntimeValue allSettingsValue(const RuntimeWarningState& state) {
    std::vector<RuntimeStructElement> elements;
    elements.reserve(state.identifierStates.size() + 1);
    elements.push_back(settingElement("all", state.allEnabled));
    for (const auto& [identifier, enabled] : state.identifierStates) {
        elements.push_back(settingElement(identifier, enabled));
    }
    return makeRuntimeStructArrayValue(
        {"identifier", "state"}, std::move(elements),
        {state.identifierStates.size() + 1, 1});
}

bool validSettingIdentifier(std::string_view identifier) {
    return identifier == "all" || identifier == "last" ||
           identifier == "backtrace" || identifier == "verbose" ||
           isRuntimeExceptionIdentifier(identifier);
}

std::string resolvedSettingIdentifier(
    std::string identifier, const RuntimeWarningState& state) {
    if (identifier == "last") {
        return state.lastIdentifier.empty() ? std::string("all")
                                            : state.lastIdentifier;
    }
    return identifier;
}

bool settingEnabled(const RuntimeWarningState& state,
                    std::string_view identifier) {
    if (identifier == "all") {
        return state.allEnabled;
    }
    if (identifier == "backtrace") {
        return state.backtraceEnabled;
    }
    if (identifier == "verbose") {
        return state.verboseEnabled;
    }
    return runtimeWarningIsEnabled(state, identifier);
}

void setSetting(RuntimeWarningState& state, const std::string& identifier,
                bool enabled) {
    if (identifier == "all") {
        state.allEnabled = enabled;
        state.identifierStates.clear();
    } else if (identifier == "backtrace") {
        state.backtraceEnabled = enabled;
    } else if (identifier == "verbose") {
        state.verboseEnabled = enabled;
    } else {
        if (enabled == state.allEnabled) {
            state.identifierStates.erase(identifier);
        } else {
            state.identifierStates[identifier] = enabled;
        }
    }
}

std::optional<std::string> textArgument(
    const RuntimeValue& value, std::string_view context,
    std::string& error) {
    const auto text = runtimeTextScalarUtf8(value);
    if (!text) {
        error = std::string(context) + " must be text";
        return std::nullopt;
    }
    return text;
}

std::optional<RuntimeWarningState> restoredState(
    const RuntimeValue& value, const RuntimeWarningState& current,
    std::string& error) {
    if (value.kind != RuntimeValueKind::Struct) {
        error = "warning settings must be a structure array";
        return std::nullopt;
    }
    RuntimeWarningState restored = current;
    bool clearedOverrides = false;
    for (size_t index = 0; index < runtimeStructElementCount(value); ++index) {
        const RuntimeValue* identifier =
            runtimeStructField(value, "identifier", index);
        const RuntimeValue* setting =
            runtimeStructField(value, "state", index);
        const auto identifierText = identifier
            ? runtimeTextScalarUtf8(*identifier) : std::nullopt;
        const auto settingText = setting
            ? runtimeTextScalarUtf8(*setting) : std::nullopt;
        if (!identifierText || !settingText ||
            (*settingText != "on" && *settingText != "off") ||
            !validSettingIdentifier(*identifierText)) {
            error = "warning setting structures require valid text "
                    "identifier and on/off state fields";
            return std::nullopt;
        }
        std::string resolved =
            resolvedSettingIdentifier(*identifierText, restored);
        if (resolved == "all" && !clearedOverrides) {
            restored.identifierStates.clear();
            clearedOverrides = true;
        }
        setSetting(restored, resolved, *settingText == "on");
    }
    return restored;
}

} // namespace

bool runtimeWarningIsEnabled(const RuntimeWarningState& state,
                             std::string_view identifier) {
    if (!identifier.empty()) {
        const auto found = state.identifierStates.find(
            std::string(identifier));
        if (found != state.identifierStates.end()) {
            return found->second;
        }
    }
    return state.allEnabled;
}

RuntimeWarningOperationResult runtimeWarning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount, RuntimeWarningState& state) {
    if (requestedOutputCount > 1) {
        return failure("warning supports at most one output");
    }

    if (arguments.empty()) {
        return success(requestedOutputCount == 0
                           ? std::vector<RuntimeValue>{}
                           : std::vector<RuntimeValue>{
                                 allSettingsValue(state)});
    }

    if (arguments.size() == 1 &&
        arguments.front().kind == RuntimeValueKind::Struct) {
        std::string error;
        const auto restored = restoredState(arguments.front(), state, error);
        if (!restored) {
            return failure(std::move(error));
        }
        RuntimeValue previous = allSettingsValue(state);
        state = *restored;
        return success(requestedOutputCount == 0
                           ? std::vector<RuntimeValue>{}
                           : std::vector<RuntimeValue>{
                                 std::move(previous)});
    }

    const auto operation = runtimeTextScalarUtf8(arguments.front());
    if (operation && (*operation == "on" || *operation == "off" ||
                      *operation == "query")) {
        if (arguments.size() > 2) {
            return failure("warning state operations accept at most one "
                           "identifier or mode");
        }
        std::string identifier = "all";
        if (arguments.size() == 2) {
            std::string error;
            const auto text = textArgument(arguments[1],
                                           "warning identifier", error);
            if (!text) {
                return failure(std::move(error));
            }
            identifier = *text;
            if (!validSettingIdentifier(identifier)) {
                return failure("invalid warning identifier or mode: " +
                               identifier);
            }
        }
        identifier = resolvedSettingIdentifier(std::move(identifier), state);
        const bool previousEnabled = settingEnabled(state, identifier);
        RuntimeValue previous = settingValue(identifier, previousEnabled);
        if (*operation != "query") {
            setSetting(state, identifier, *operation == "on");
        }
        return success(requestedOutputCount == 0
                           ? std::vector<RuntimeValue>{}
                           : std::vector<RuntimeValue>{
                                 std::move(previous)});
    }

    if (requestedOutputCount != 0) {
        return failure("warning message emission does not produce outputs");
    }
    auto warning = runtimeCreateErrorException(arguments);
    if (!warning.succeeded) {
        return failure(std::move(warning.error));
    }
    const RuntimeValue* identifier =
        runtimeExceptionProperty(warning.value, "identifier");
    const RuntimeValue* message =
        runtimeExceptionProperty(warning.value, "message");
    if (!identifier || !message) {
        return failure("warning message construction failed");
    }
    const auto identifierText = runtimeTextScalarUtf8(*identifier);
    const auto messageText = runtimeTextScalarUtf8(*message);
    if (!identifierText || !messageText) {
        return failure("warning message construction produced invalid text");
    }
    if (messageText->empty()) {
        state.lastIdentifier.clear();
        state.lastMessage.clear();
        return success();
    }
    state.lastIdentifier = *identifierText;
    state.lastMessage = *messageText;
    if (!runtimeWarningIsEnabled(state, state.lastIdentifier)) {
        return success();
    }
    return success({}, RuntimeWarningRecord{
                           state.lastIdentifier, state.lastMessage});
}

RuntimeWarningOperationResult runtimeLastWarning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount, RuntimeWarningState& state) {
    if (arguments.size() > 2 || requestedOutputCount > 2) {
        return failure("lastwarn accepts up to two text inputs and returns "
                       "up to two outputs");
    }
    if (!arguments.empty()) {
        const auto message = runtimeTextScalarUtf8(arguments[0]);
        const auto identifier = arguments.size() == 2
            ? runtimeTextScalarUtf8(arguments[1])
            : std::optional<std::string>(std::string{});
        if (!message || !identifier) {
            return failure("lastwarn inputs must be text");
        }
        const std::string& nextIdentifier = *identifier;
        if (!nextIdentifier.empty() &&
            !isRuntimeExceptionIdentifier(nextIdentifier)) {
            return failure("invalid lastwarn identifier: " +
                           nextIdentifier);
        }
        state.lastMessage = *message;
        state.lastIdentifier = nextIdentifier;
    }

    std::vector<RuntimeValue> outputs;
    if (requestedOutputCount >= 1) {
        outputs.push_back(characterValue(state.lastMessage));
    }
    if (requestedOutputCount >= 2) {
        outputs.push_back(characterValue(state.lastIdentifier));
    }
    return success(std::move(outputs));
}

RuntimeWarningOperationResult RuntimeWarningContext::warning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    std::scoped_lock lock(mutex_);
    return runtimeWarning(arguments, requestedOutputCount, state_);
}

RuntimeWarningOperationResult RuntimeWarningContext::lastWarning(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    std::scoped_lock lock(mutex_);
    return runtimeLastWarning(arguments, requestedOutputCount, state_);
}

RuntimeWarningState RuntimeWarningContext::snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

void RuntimeWarningContext::reset() {
    std::scoped_lock lock(mutex_);
    state_ = {};
}

} // namespace mparser
