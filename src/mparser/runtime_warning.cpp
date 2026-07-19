#include "mparser/runtime_warning.h"

#include "mparser/runtime_exception.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"

#include <utility>

namespace mparser {
namespace {

RuntimeValue stringValue(std::string text) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::String;
    result.text = std::move(text);
    setRuntimeDimensions(result, {1, result.text.size()});
    return result;
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
    result.emplace("identifier", stringValue(std::move(identifier)));
    result.emplace("state", stringValue(stateText(enabled)));
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
    if (value.kind != RuntimeValueKind::String) {
        error = std::string(context) + " must be text";
        return std::nullopt;
    }
    return value.text;
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
        if (!identifier || !setting ||
            identifier->kind != RuntimeValueKind::String ||
            setting->kind != RuntimeValueKind::String ||
            (setting->text != "on" && setting->text != "off") ||
            !validSettingIdentifier(identifier->text)) {
            error = "warning setting structures require valid text "
                    "identifier and on/off state fields";
            return std::nullopt;
        }
        std::string resolved =
            resolvedSettingIdentifier(identifier->text, restored);
        if (resolved == "all" && !clearedOverrides) {
            restored.identifierStates.clear();
            clearedOverrides = true;
        }
        setSetting(restored, resolved, setting->text == "on");
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

    if (arguments.front().kind == RuntimeValueKind::String &&
        (arguments.front().text == "on" ||
         arguments.front().text == "off" ||
         arguments.front().text == "query")) {
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
        if (arguments.front().text != "query") {
            setSetting(state, identifier,
                       arguments.front().text == "on");
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
    if (message->text.empty()) {
        state.lastIdentifier.clear();
        state.lastMessage.clear();
        return success();
    }
    state.lastIdentifier = identifier->text;
    state.lastMessage = message->text;
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
        if (arguments[0].kind != RuntimeValueKind::String ||
            (arguments.size() == 2 &&
             arguments[1].kind != RuntimeValueKind::String)) {
            return failure("lastwarn inputs must be text");
        }
        const std::string nextIdentifier =
            arguments.size() == 2 ? arguments[1].text : std::string{};
        if (!nextIdentifier.empty() &&
            !isRuntimeExceptionIdentifier(nextIdentifier)) {
            return failure("invalid lastwarn identifier: " +
                           nextIdentifier);
        }
        state.lastMessage = arguments[0].text;
        state.lastIdentifier = nextIdentifier;
    }

    std::vector<RuntimeValue> outputs;
    if (requestedOutputCount >= 1) {
        outputs.push_back(stringValue(state.lastMessage));
    }
    if (requestedOutputCount >= 2) {
        outputs.push_back(stringValue(state.lastIdentifier));
    }
    return success(std::move(outputs));
}

} // namespace mparser
