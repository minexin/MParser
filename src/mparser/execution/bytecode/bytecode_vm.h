#pragma once

#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/runtime_source_evaluation.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/session/runtime_output.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/semantic/semantic.h"
#include "mparser/execution/jit/typed_region_executor.h"

#include <optional>

namespace mparser {

struct BytecodeTypedIrModule;
class AdaptiveBytecodeVmSession;
class CompiledModule;
class RuntimeBenchmarkRunner;

struct BytecodeInstructionProfile {
    size_t pc = 0;
    std::string opcode;
    std::string operand;
    SourceSpan span;
    size_t executionCount = 0;
};

struct BytecodeFunctionProfile {
    std::string name;
    SourceSpan span;
    size_t callCount = 0;
    size_t executedInstructionCount = 0;
};

struct BytecodeValueObservation {
    std::string kind;
    std::string numericClass;
    size_t rows = 0;
    size_t columns = 0;
    std::vector<size_t> dimensions;
    size_t observationCount = 0;
    bool stable = true;
};

struct BytecodeLoopProfile {
    size_t headerPc = 0;
    std::string variable;
    SourceSpan span;
    size_t entryCount = 0;
    size_t iterationCount = 0;
    size_t backedgeCount = 0;
    size_t completionCount = 0;
    size_t breakCount = 0;
    size_t continueCount = 0;
    BytecodeValueObservation variableObservation;
    bool hot = false;
};

struct BytecodeCallSiteProfile {
    size_t pc = 0;
    std::string target;
    std::string kind;
    SourceSpan span;
    int resultCount = 0;
    size_t executionCount = 0;
    bool hasReceiverObservation = false;
    BytecodeValueObservation receiverObservation;
    std::vector<BytecodeValueObservation> argumentObservations;
    std::vector<BytecodeValueObservation> resultObservations;
};

struct BytecodeAssignmentProfile {
    size_t pc = 0;
    std::string target;
    std::string kind;
    SourceSpan span;
    size_t executionCount = 0;
    bool inLoop = false;
    size_t loopHeaderPc = 0;
    BytecodeValueObservation valueObservation;
};

struct BytecodeLoadProfile {
    size_t pc = 0;
    std::string name;
    SourceSpan span;
    BytecodeValueObservation valueObservation;
};

struct BytecodeWorkspaceInputProfile {
    std::string name;
    BytecodeValueObservation valueObservation;
};

struct BytecodeFunctionEntryProfile {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::string> outputs;
    size_t invocationCount = 0;
    std::vector<BytecodeValueObservation> argumentObservations;
    std::vector<BytecodeValueObservation> resultObservations;
};

struct BytecodeVmProfile {
    bool collected = false;
    size_t hotLoopThreshold = 0;
    std::vector<BytecodeInstructionProfile> instructions;
    std::vector<BytecodeFunctionProfile> functions;
    std::vector<BytecodeLoopProfile> loops;
    std::vector<BytecodeCallSiteProfile> callSites;
    std::vector<BytecodeAssignmentProfile> assignments;
    std::vector<BytecodeLoadProfile> loads;
    std::vector<BytecodeWorkspaceInputProfile> workspaceInputs;
    std::vector<BytecodeFunctionEntryProfile> functionEntries;
};

enum class BytecodeVmProfilingMode {
    Full,
    Disabled,
};

struct BytecodeVmOptions {
    BytecodeVmProfilingMode profiling = BytecodeVmProfilingMode::Full;
    std::shared_ptr<RuntimeCallableContext> callableContext;
    std::shared_ptr<RuntimeSessionState> sessionState;
    std::vector<RuntimeVariable> initialWorkspace;
    std::vector<RuntimeWorkspace*> inheritedWorkspaceFrames;
    std::string entryFunction;
    std::vector<RuntimeValue> arguments;
    std::optional<size_t> requestedOutputCount;
    TypedRegionBackend typedRegionBackend = TypedRegionBackend::Auto;
    std::shared_ptr<RuntimeExecutionControl> executionControl;
    RuntimeOutputSink outputSink;
    std::vector<RuntimeSourceCallable> inheritedCallables;
    std::vector<RuntimeSourceCallableScope> inheritedCallableScopes;
    RuntimeSourceCallableInvoker inheritedCallableInvoker;
    RuntimeWorkspace* inheritedCallableWorkspace = nullptr;
    RuntimeSourceStorageResolver inheritedStorageResolver;
    RuntimeSourceStorageDeclarer inheritedStorageDeclarer;
    RuntimeSourceStorageClearer inheritedStorageClearer;
    RuntimeWorkspace* inheritedStorageWorkspace = nullptr;
};

struct BytecodeTypedRegionExecutionProfile {
    size_t regionId = 0;
    size_t sourcePc = 0;
    std::string kind;
    std::string target;
    bool eligible = false;
    size_t attemptCount = 0;
    size_t executionCount = 0;
    size_t fallbackCount = 0;
    size_t iterationCount = 0;
    size_t nestedIterationCount = 0;
    size_t executedInstructionCount = 0;
    size_t executedKernelInstructionCount = 0;
    std::string backend;
    size_t nativeCompilationCount = 0;
    size_t nativeCacheHitCount = 0;
    size_t nativeCacheInsertionCount = 0;
    size_t nativeCacheBypassCount = 0;
    size_t nativeCacheEvictionCount = 0;
    size_t nativeCacheEvictedCodeBytes = 0;
    size_t nativeCodeSize = 0;
    std::string nativePlatform;
    RuntimeFallbackKind lastFallbackKind =
        RuntimeFallbackKind::None;
    RuntimeFallbackKind nativeFallbackKind =
        RuntimeFallbackKind::None;
    std::string nativeFallbackReason;
    std::string lastReason;
};

struct BytecodeVmResult {
    std::vector<RuntimeVariable> variables;
    std::string entryFunction;
    std::vector<std::string> outputNames;
    std::vector<RuntimeValue> outputs;
    std::vector<RuntimeOutputEvent> outputEvents;
    std::vector<RuntimeExpressionResult> expressionResults;
    size_t requestedOutputCount = 0;
    std::vector<Diagnostic> diagnostics;
    size_t executedInstructionCount = 0;
    RuntimeExecutionSnapshot execution;
    BytecodeVmProfile profile;
    std::vector<BytecodeTypedRegionExecutionProfile> typedRegionExecutions;
};

class BytecodeVm {
public:
    BytecodeVmResult run(const BytecodeProgram& program,
                         const SemanticResult& semantic);
    BytecodeVmResult run(const BytecodeProgram& program,
                         const SemanticResult& semantic,
                         const BytecodeVmOptions& options);
    BytecodeVmResult run(const BytecodeProgram& program,
                         const SemanticResult& semantic,
                         const BytecodeTypedIrModule& typedIr);
    BytecodeVmResult run(const BytecodeProgram& program,
                         const SemanticResult& semantic,
                         const BytecodeTypedIrModule& typedIr,
                         const BytecodeVmOptions& options);

private:
    friend class AdaptiveBytecodeVmSession;
    friend class CompiledModule;
    friend class RuntimeBenchmarkRunner;

    // Trusted internal path for a program already accepted by the verifier.
    BytecodeVmResult runValidated(
        const BytecodeProgram& program,
        const SemanticResult& semantic,
        const BytecodeVmOptions& options);
    BytecodeVmResult runValidated(
        const BytecodeProgram& program,
        const SemanticResult& semantic,
        const BytecodeTypedIrModule& typedIr,
        const BytecodeVmOptions& options);
};

} // namespace mparser
