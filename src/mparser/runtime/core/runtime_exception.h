#pragma once

#include "mparser/frontend/diagnostic.h"
#include "mparser/runtime/core/runtime_value.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::string_view kRuntimeExceptionClassName = "MException";
inline constexpr std::string_view kRuntimeErrorIdentifier =
    "MParser:RuntimeError";

using RuntimeExceptionFrame = DiagnosticFrame;

enum class RuntimeExceptionStackPolicy {
    Replace,
    Preserve,
    AsCaller,
};

struct RuntimeExceptionOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

bool isRuntimeException(const RuntimeValue& value);
bool isRuntimeExceptionIdentifier(std::string_view identifier);

RuntimeExceptionOperationResult runtimeConstructMException(
    const std::vector<RuntimeValue>& arguments);

RuntimeExceptionOperationResult runtimeCreateErrorException(
    const std::vector<RuntimeValue>& arguments);

RuntimeExceptionOperationResult runtimePrepareExceptionForThrow(
    const RuntimeValue& exception,
    const std::vector<RuntimeExceptionFrame>& frames,
    RuntimeExceptionStackPolicy policy);

RuntimeExceptionOperationResult runtimeAddExceptionCause(
    const std::vector<RuntimeValue>& arguments);

RuntimeExceptionOperationResult runtimeGetExceptionReport(
    const std::vector<RuntimeValue>& arguments);

RuntimeValue runtimeExceptionFromDiagnostic(
    const Diagnostic& diagnostic,
    const std::vector<RuntimeExceptionFrame>& frames);

Diagnostic runtimeDiagnosticFromException(
    const RuntimeValue& exception, SourceSpan fallbackSpan);

const RuntimeValue* runtimeExceptionProperty(
    const RuntimeValue& exception, std::string_view name);

std::vector<RuntimeExceptionFrame> runtimeExceptionFrames(
    const RuntimeValue& exception);

size_t runtimeExceptionFrameCount(const RuntimeValue& exception);
size_t runtimeExceptionCauseCount(const RuntimeValue& exception);

} // namespace mparser
