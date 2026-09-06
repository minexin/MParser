#pragma once

#include "mparser/runtime/core/session/runtime_call_frame.h"

#include <functional>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace mparser {

class RuntimeSessionState;

enum class RuntimeDebugAction { Continue, StepInto, StepOver, StepOut, Stop };
enum class RuntimeDebugReason { Breakpoint, PauseRequest, Step };

struct RuntimeBreakpoint {
    std::string sourceName;
    int line = 1;
};

struct RuntimeDebugFrame {
    RuntimeCallFrameKind kind = RuntimeCallFrameKind::Script;
    std::string functionName;
    std::string sourceName;
    SourceSpan location;
    size_t suppliedArgumentCount = 0;
    size_t requestedOutputCount = 0;
    RuntimeWorkspace variables;
};

struct RuntimeDebugEvent {
    RuntimeDebugReason reason = RuntimeDebugReason::Step;
    size_t sequence = 0;
    // Frames are ordered from the outermost script/function to the current one.
    std::vector<RuntimeDebugFrame> frames;
};

using RuntimeDebugSink =
    std::function<RuntimeDebugAction(const RuntimeDebugEvent&)>;
using RuntimeDebugFrameProvider =
    std::function<void(std::vector<RuntimeDebugFrame>&)>;

void appendRuntimeDebugFrame(std::vector<RuntimeDebugFrame>& destination,
    const RuntimeCallFrame& frame,
    const std::function<std::string(size_t)>& sourceName,
    const RuntimeSessionState& session,
    const std::deque<RuntimeCallFrame>& activeFrames);

class RuntimeDebugger {
public:
    explicit RuntimeDebugger(RuntimeDebugSink sink);

    // These two operations may be called from another thread during execution.
    void setBreakpoints(std::vector<RuntimeBreakpoint> breakpoints);
    void requestPause();

    bool statement(const std::string& sourceName, const SourceSpan& location);

private:
    friend class RuntimeDebugScope;
    void enterScope(RuntimeDebugFrameProvider provider);
    void leaveScope() noexcept;

    RuntimeDebugSink sink_;
    std::mutex mutex_;
    std::vector<RuntimeBreakpoint> breakpoints_;
    bool pauseRequested_ = false;
    std::thread::id executionThread_;
    std::vector<RuntimeDebugFrameProvider> scopes_;
    RuntimeDebugAction action_ = RuntimeDebugAction::Continue;
    size_t stepDepth_ = 0;
    size_t sequence_ = 0;
    bool inCallback_ = false;
};

class RuntimeDebugScope {
public:
    RuntimeDebugScope(RuntimeDebugger* debugger,
                      RuntimeDebugFrameProvider provider);
    ~RuntimeDebugScope();
    void leave() noexcept;
    RuntimeDebugScope(const RuntimeDebugScope&) = delete;
    RuntimeDebugScope& operator=(const RuntimeDebugScope&) = delete;

private:
    RuntimeDebugger* debugger_;
};

} // namespace mparser
