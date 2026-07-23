#pragma once

#include "mparser/bytecode.h"
#include "mparser/runtime_fallback.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct BytecodeRegionContract {
    bool available = false;
    bool closed = false;
    size_t beginPc = 0;
    size_t endPc = 0;
    size_t bodyBeginPc = 0;
    size_t bodyEndPc = 0;
    size_t nestedLoopCount = 0;
    size_t maxLoopDepth = 0;
    size_t conditionalBranchCount = 0;
    size_t linearIndexReadCount = 0;
    size_t linearIndexWriteCount = 0;
    size_t stackInputCount = 0;
    size_t stackOutputCount = 0;
    std::vector<std::string> reads;
    std::vector<std::string> inputs;
    std::vector<std::string> writes;
    std::vector<std::string> outputs;
    std::vector<std::string> callTargets;
    bool hasCalls = false;
    bool hasMutation = false;
    bool hasUnsupportedMutation = false;
    bool hasUnsupportedControlFlow = false;
    bool hasUnsupportedOperations = false;
    bool eligibleForTypedExecution = false;
    RuntimeFallbackKind fallbackKind = RuntimeFallbackKind::None;
    std::string reason;
};

class BytecodeRegionAnalyzer {
public:
    BytecodeRegionContract analyze(
        const BytecodeProgram& program, std::string_view candidateKind,
        size_t sourcePc, std::string_view target) const;
};

} // namespace mparser
