#pragma once

#include "mparser/runtime/core/session/runtime_input.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"

namespace mparser {
RuntimeInputSource makeConsoleInputSource();
RuntimeConsoleSink makeConsoleOutputSink();
}
