#pragma once

#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/frontend/source.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class RuntimeNumericDisplayFormat {
    Short,
    Long,
    ShortE,
    LongE,
    ShortG,
    LongG,
    ShortEng,
    LongEng,
    Plus,
    Bank,
    Hex,
    Rational,
};

enum class RuntimeLineSpacing {
    Loose,
    Compact,
};

struct RuntimeDisplayFormat {
    RuntimeNumericDisplayFormat numeric =
        RuntimeNumericDisplayFormat::Short;
    RuntimeLineSpacing spacing = RuntimeLineSpacing::Loose;

    bool operator==(const RuntimeDisplayFormat&) const = default;
};

std::string_view runtimeNumericDisplayFormatName(
    RuntimeNumericDisplayFormat format);
std::string_view runtimeLineSpacingName(RuntimeLineSpacing spacing);

enum class RuntimeOutputKind {
    Display,
    StandardOutput,
};

struct RuntimeOutputEvent {
    RuntimeOutputKind kind = RuntimeOutputKind::StandardOutput;
    std::string text;
    SourceSpan span;
    std::uint64_t sequence = 0;
    // Preserves source provenance across compiled-module boundaries.
    std::string sourceName;
};

using RuntimeOutputSink =
    std::function<bool(const RuntimeOutputEvent& event)>;

struct RuntimeExpressionResult {
    RuntimeValue value;
    SourceSpan span;
    bool outputSuppressed = false;
    std::uint64_t sequence = 0;
    std::string displayText = {};
    RuntimeLineSpacing lineSpacing = RuntimeLineSpacing::Loose;
};

struct RuntimeFormatResult {
    bool succeeded = false;
    std::string text;
    std::string error;
};

std::string runtimeFormatConsoleValue(
    const RuntimeValue& value,
    RuntimeDisplayFormat format = {});

RuntimeFormatResult runtimeFormatDisplay(
    const RuntimeValue& value,
    RuntimeDisplayFormat format = {});

RuntimeFormatResult runtimeFormatPrintf(
    const std::vector<RuntimeValue>& arguments);

std::string runtimeRenderConsole(
    const std::vector<RuntimeOutputEvent>& outputEvents,
    const std::vector<RuntimeExpressionResult>& expressionResults);

} // namespace mparser
