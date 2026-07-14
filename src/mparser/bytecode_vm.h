#pragma once

#include "mparser/bytecode.h"
#include "mparser/interpreter.h"
#include "mparser/semantic.h"

#include <optional>

namespace mparser {

struct BytecodeTypedIrModule;

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
    std::vector<BytecodeWorkspaceInputProfile> workspaceInputs;
    std::vector<BytecodeFunctionEntryProfile> functionEntries;
};

enum class BytecodeVmProfilingMode {
    Full,
    Disabled,
};

struct BytecodeVmOptions {
    BytecodeVmProfilingMode profiling = BytecodeVmProfilingMode::Full;
    std::vector<RuntimeVariable> initialWorkspace;
    std::string entryFunction;
    std::vector<RuntimeValue> arguments;
    std::optional<size_t> requestedOutputCount;
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
    size_t executedInstructionCount = 0;
    std::string lastReason;
};

struct BytecodeVmResult {
    std::vector<RuntimeVariable> variables;
    std::string entryFunction;
    std::vector<std::string> outputNames;
    std::vector<RuntimeValue> outputs;
    size_t requestedOutputCount = 0;
    std::vector<Diagnostic> diagnostics;
    size_t executedInstructionCount = 0;
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
};

} // namespace mparser
