#pragma once

#include "mparser/runtime_value.h"
#include "mparser/source.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mparser {

enum class RuntimeOutputKind {
    Display,
    StandardOutput,
};

struct RuntimeOutputEvent {
    RuntimeOutputKind kind = RuntimeOutputKind::StandardOutput;
    std::string text;
    SourceSpan span;
    std::uint64_t sequence = 0;
};

using RuntimeOutputSink =
    std::function<bool(const RuntimeOutputEvent& event)>;

struct RuntimeExpressionResult {
    RuntimeValue value;
    SourceSpan span;
    bool outputSuppressed = false;
    std::uint64_t sequence = 0;
};

struct RuntimeFormatResult {
    bool succeeded = false;
    std::string text;
    std::string error;
};

RuntimeFormatResult runtimeFormatDisplay(const RuntimeValue& value);

RuntimeFormatResult runtimeFormatPrintf(
    const std::vector<RuntimeValue>& arguments);

} // namespace mparser
