#pragma once

#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"

#include <cstddef>
#include <string>

namespace mparser {

struct RuntimeBenchmarkOptions {
    size_t warmupIterations = 3;
    size_t measuredIterations = 20;
};

struct RuntimeBenchmarkStatistics {
    size_t completedIterations = 0;
    double totalMilliseconds = 0.0;
    double meanMicroseconds = 0.0;
    double medianMicroseconds = 0.0;
    double minimumMicroseconds = 0.0;
    double maximumMicroseconds = 0.0;
    double meanExecutedInstructionCount = 0.0;
    double meanTypedRegionAttemptCount = 0.0;
    double meanTypedRegionExecutionCount = 0.0;
    double meanTypedRegionFallbackCount = 0.0;
    double meanTypedInstructionCount = 0.0;
};

struct RuntimeBenchmarkResult {
    RuntimeBenchmarkOptions options;
    RuntimeBenchmarkStatistics interpreter;
    RuntimeBenchmarkStatistics profiledBytecodeVm;
    RuntimeBenchmarkStatistics bytecodeVm;
    RuntimeBenchmarkStatistics typedBytecodeVm;
    InterpreterResult lastInterpreterResult;
    BytecodeVmResult lastProfiledBytecodeVmResult;
    BytecodeVmResult lastBytecodeVmResult;
    BytecodeVmResult lastTypedBytecodeVmResult;
    size_t typedRegionCount = 0;
    bool outputsComparable = false;
    bool outputsMatch = false;
    bool interpreterProfiledBytecodeOutputsMatch = false;
    bool profiledSteadyBytecodeOutputsMatch = false;
    bool typedBytecodeOutputsMatch = false;
    std::string comparisonMessage;
};

class RuntimeBenchmarkRunner {
public:
    RuntimeBenchmarkResult run(
        const SemanticResult& semantic, const BytecodeProgram& bytecode,
        const RuntimeBenchmarkOptions& options = {}) const;
};

} // namespace mparser
