#pragma once

#include "mparser/runtime/core/session/runtime_call_frame.h"
#include "mparser/frontend/diagnostic.h"

#include <functional>
#include <deque>
#include <mutex>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace mparser {

class RuntimeSessionState;

enum class RuntimeDebugAction { Continue, StepInto, StepOver, StepOut, Stop };
enum class RuntimeDebugReason { Breakpoint, PauseRequest, Step };

struct RuntimeBreakpoint {
    std::string sourceName;
    int line = 1;
    std::string condition = {};
};

struct RuntimeDebugEvaluationRequest {
    std::string source;
    size_t requestedOutputCount = 1;
};

struct RuntimeDebugEvaluationResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::string capturedOutput;
    std::vector<Diagnostic> diagnostics;
};

using RuntimeDebugEvaluator = std::function<RuntimeDebugEvaluationResult(
    const RuntimeDebugEvaluationRequest&)>;

struct RuntimeDebugFrame {
    RuntimeCallFrameKind kind = RuntimeCallFrameKind::Script;
    std::string functionName;
    std::string sourceName;
    SourceSpan location;
    size_t suppliedArgumentCount = 0;
    size_t requestedOutputCount = 0;
    RuntimeWorkspace variables;

    void bindEvaluator(RuntimeDebugEvaluator evaluator) {
        evaluator_ = std::move(evaluator);
    }
private:
    friend class RuntimeDebugger;
    RuntimeDebugEvaluator evaluator_;
};

struct RuntimeDebugEvent {
    RuntimeDebugReason reason = RuntimeDebugReason::Step;
    size_t sequence = 0;
    // Frames are ordered from the outermost script/function to the current one.
    std::vector<RuntimeDebugFrame> frames;
    std::vector<Diagnostic> conditionDiagnostics;
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

RuntimeDebugEvaluationResult evaluateRuntimeDebugFrame(
    RuntimeCallFrame& frame, const RuntimeSessionState& session,
    std::deque<RuntimeCallFrame>& activeFrames,
    const std::function<RuntimeDebugEvaluationResult()>& evaluate);

class RuntimeDebugger {
public:
    explicit RuntimeDebugger(RuntimeDebugSink sink);

    // Configuration operations may be called from another thread during execution.
    void setBreakpoints(std::vector<RuntimeBreakpoint> breakpoints);
    void setSourceBreakpoint(RuntimeBreakpoint breakpoint);
    void clearSourceBreakpoints(const std::string& sourceName, int line = 0);
    std::vector<RuntimeBreakpoint> breakpoints();
    void requestPause();
    // Only explicit evaluation inside the host pause callback may queue an action.
    bool requestSourceAction(RuntimeDebugAction action);
    // Snapshot the original paused stack, excluding temporary evaluation frames.
    std::optional<std::vector<RuntimeDebugFrame>> sourceStack();
    bool moveSourceFrame(bool towardCaller, size_t count = 1);
    std::optional<size_t> selectedSourceFrame();
    static constexpr size_t selectedFrame = std::numeric_limits<size_t>::max();

    bool statement(const std::string& sourceName, const SourceSpan& location);
    RuntimeDebugEvaluationResult evaluate(size_t eventSequence, size_t frameIndex,
        const RuntimeDebugEvaluationRequest& request);

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
    bool evaluating_ = false;
    bool hostCallbackActive_ = false;
    std::optional<RuntimeDebugAction> sourceAction_;
    size_t pausedSequence_ = 0;
    size_t selectedFrame_ = 0;
    std::vector<RuntimeDebugEvaluator> pausedEvaluators_;
    std::vector<RuntimeDebugFrame> pausedFrames_;
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
