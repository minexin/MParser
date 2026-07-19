#pragma once

#include "mparser/interpreter.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::string_view kRuntimeExceptionClassName = "MException";
inline constexpr std::string_view kRuntimeErrorIdentifier =
    "MParser:RuntimeError";

struct RuntimeExceptionFrame {
    std::string file;
    std::string name;
    int line = 1;
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
    bool preserveExistingStack);

RuntimeValue runtimeExceptionFromDiagnostic(
    const Diagnostic& diagnostic,
    const std::vector<RuntimeExceptionFrame>& frames);

Diagnostic runtimeDiagnosticFromException(
    const RuntimeValue& exception, SourceSpan fallbackSpan);

const RuntimeValue* runtimeExceptionProperty(
    const RuntimeValue& exception, std::string_view name);

size_t runtimeExceptionFrameCount(const RuntimeValue& exception);

} // namespace mparser
