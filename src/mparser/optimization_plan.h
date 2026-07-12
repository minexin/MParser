#pragma once

#include "mparser/bytecode_region.h"
#include "mparser/bytecode_vm.h"

#include <string>
#include <vector>

namespace mparser {

struct BytecodeOptimizationGuard {
    size_t pc = 0;
    std::string source;
    std::string role;
    std::string kind;
    size_t rows = 0;
    size_t columns = 0;
    size_t observationCount = 0;
};

struct BytecodeOptimizationCandidate {
    std::string kind;
    size_t pc = 0;
    std::string target;
    size_t executionCount = 0;
    std::string reason;
    std::vector<BytecodeOptimizationGuard> guards;
    BytecodeRegionContract region;
};

struct BytecodeOptimizationPlan {
    size_t hotLoopThreshold = 0;
    std::vector<BytecodeOptimizationCandidate> candidates;
};

class BytecodeOptimizationPlanner {
public:
    BytecodeOptimizationPlan plan(const BytecodeVmProfile& profile,
                                  const BytecodeProgram& program) const;
};

} // namespace mparser
