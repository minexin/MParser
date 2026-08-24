#include "mparser/runtime/builtins/datetime/runtime_datetime_builtins.h"

#include "mparser/runtime/core/value/runtime_datetime.h"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace mparser {
namespace {

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinResult failure(const BuiltinCall& call, std::string message) {
    return BuiltinResult::failure(
        call.span, std::move(message), "MParser:InvalidTemporalCall");
}

BuiltinResult oneOutput(const BuiltinCall& call,
                        RuntimeTemporalOperationResult result) {
    if (!result.succeeded) {
        return failure(call, std::move(result.error));
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    return BuiltinResult::success({std::move(result.value)});
}

} // namespace

bool isRuntimeDateTimeBuiltin(std::string_view name) {
    return matches(name, {
        "datetime", "duration", "NaT", "year", "month", "day",
        "hour", "minute", "second", "days", "hours", "minutes",
        "seconds", "isdatetime", "isduration", "isnat",
    });
}

BuiltinResult invokeRuntimeDateTimeBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "datetime") {
        return oneOutput(call, runtimeConstructDateTime(call.arguments));
    }
    if (name == "duration") {
        return oneOutput(call, runtimeConstructDuration(call.arguments));
    }
    if (name == "NaT") {
        return oneOutput(call, runtimeConstructNaT(call.arguments));
    }
    if (matches(name, {"year", "month", "day", "hour", "minute",
                       "second"})) {
        if (call.arguments.size() != 1) {
            return failure(call, std::string(name) +
                                      " expects one datetime argument");
        }
        return oneOutput(call,
                         runtimeTemporalComponent(name,
                                                  call.arguments.front()));
    }
    if (matches(name, {"days", "hours", "minutes", "seconds"})) {
        if (call.arguments.size() != 1) {
            return failure(call, std::string(name) +
                                      " expects one input argument");
        }
        return oneOutput(call,
                         runtimeTemporalUnit(name,
                                             call.arguments.front()));
    }
    if (matches(name, {"isdatetime", "isduration", "isnat"})) {
        if (call.arguments.size() != 1) {
            return failure(call, std::string(name) +
                                      " expects one input argument");
        }
        return oneOutput(call,
                         runtimeTemporalPredicate(name,
                                                   call.arguments.front()));
    }
    return failure(call, "unsupported temporal builtin: " + std::string(name));
}

} // namespace mparser
