#pragma once

#include "mparser/execution/runtime_source_evaluation.h"
#include "mparser/runtime/core/runtime_call_frame.h"
#include "mparser/runtime/core/runtime_session_state.h"

#include <optional>
#include <string_view>

namespace mparser {

std::optional<RuntimeSourceStorageBinding> runtimeSourceStorageBinding(
    const RuntimeCallFrame& frame, std::string_view name);

RuntimeSourceStorageDeclarationResult runtimeDeclareSourceStorage(
    RuntimeCallFrame& frame, RuntimeSessionState& session,
    RuntimeSourceStorageKind kind, std::string_view name,
    const RuntimeValue* localValue, SourceSpan span);

void runtimeClearSourceStorage(RuntimeCallFrame& frame,
                               std::string_view name);

} // namespace mparser
