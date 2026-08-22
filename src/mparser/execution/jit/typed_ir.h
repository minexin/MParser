#pragma once

#include "mparser/execution/jit/optimization_plan.h"

#include <string>
#include <vector>

namespace mparser {

struct BytecodeTypedValue {
    std::string kind;
    std::string numericClass;
    size_t rows = 0;
    size_t columns = 0;
    std::vector<size_t> dimensions;
};

struct BytecodeTypedIrGuard {
    std::string source;
    std::string role;
    BytecodeTypedValue value;
    size_t observationCount = 0;
};

struct BytecodeTypedIrOperation {
    std::string opcode;
    std::string operand;
};

struct BytecodeTypedIrRegion {
    size_t id = 0;
    std::string kind;
    size_t sourcePc = 0;
    std::string target;
    size_t executionCount = 0;
    BytecodeRegionContract region;
    std::vector<BytecodeTypedIrGuard> guards;
    std::vector<BytecodeTypedIrOperation> operations;
};

struct BytecodeTypedIrModule {
    std::vector<BytecodeTypedIrRegion> regions;
};

struct BytecodeTypedIrGuardCheck {
    std::string source;
    std::string role;
    bool checked = false;
    bool passed = false;
    std::string reason;
};

struct BytecodeTypedIrRegionEvaluation {
    size_t regionId = 0;
    std::string kind;
    std::string target;
    size_t checkedCount = 0;
    size_t passedCount = 0;
    size_t failedCount = 0;
    size_t skippedCount = 0;
    bool regionEligible = false;
    std::string regionReason;
    bool canEnterTypedPath = false;
    std::vector<BytecodeTypedIrGuardCheck> checks;
};

struct BytecodeTypedIrEvaluation {
    std::vector<BytecodeTypedIrRegionEvaluation> regions;
};

class BytecodeTypedIrBuilder {
public:
    BytecodeTypedIrModule build(
        const BytecodeOptimizationPlan& plan) const;
};

class BytecodeTypedIrGuardEvaluator {
public:
    BytecodeTypedIrEvaluation evaluate(
        const BytecodeTypedIrModule& module,
        const std::vector<RuntimeVariable>& variables) const;
};

} // namespace mparser
