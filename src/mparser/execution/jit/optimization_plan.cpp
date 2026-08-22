#include "mparser/execution/jit/optimization_plan.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace mparser {
namespace {

bool isStableGuardInput(const BytecodeValueObservation& observation) {
    return observation.observationCount > 0 && observation.stable &&
           observation.kind != "missing" && observation.kind != "mixed";
}

void addGuard(BytecodeOptimizationCandidate& candidate, size_t pc,
              std::string source, std::string role,
              const BytecodeValueObservation& observation) {
    if (!isStableGuardInput(observation)) {
        return;
    }

    candidate.guards.push_back(BytecodeOptimizationGuard{
        pc,
        std::move(source),
        std::move(role),
        observation.kind,
        observation.numericClass,
        observation.rows,
        observation.columns,
        observation.dimensions,
        observation.observationCount});
}

void addValueGuards(BytecodeOptimizationCandidate& candidate, size_t pc,
                    std::string_view source, std::string_view prefix,
                    const std::vector<BytecodeValueObservation>& values) {
    for (size_t index = 0; index < values.size(); ++index) {
        addGuard(candidate, pc, std::string(source),
                 std::string(prefix) + std::to_string(index), values[index]);
    }
}

bool allStable(const std::vector<BytecodeValueObservation>& observations) {
    if (observations.empty()) {
        return true;
    }
    for (const auto& observation : observations) {
        if (!isStableGuardInput(observation)) {
            return false;
        }
    }
    return true;
}

std::map<size_t, bool> hotLoopMap(const BytecodeVmProfile& profile) {
    std::map<size_t, bool> result;
    for (const auto& loop : profile.loops) {
        result[loop.headerPc] = loop.hot;
    }
    return result;
}

} // namespace

BytecodeOptimizationPlan
BytecodeOptimizationPlanner::plan(
    const BytecodeVmProfile& profile,
    const BytecodeProgram& program) const {
    return plan(profile, program, {});
}

BytecodeOptimizationPlan
BytecodeOptimizationPlanner::plan(const BytecodeVmProfile& profile,
                                  const BytecodeProgram& program,
                                  std::shared_ptr<const BuiltinRegistry>
                                      builtinRegistry) const {
    BytecodeOptimizationPlan result;
    result.hotLoopThreshold = profile.hotLoopThreshold;
    if (!validateBytecodeProgram(program).succeeded) {
        return result;
    }
    const auto hotLoops = hotLoopMap(profile);

    for (const auto& loop : profile.loops) {
        if (!loop.hot || !isStableGuardInput(loop.variableObservation)) {
            continue;
        }

        BytecodeOptimizationCandidate candidate;
        candidate.kind = "hot-loop";
        candidate.pc = loop.headerPc;
        candidate.target = loop.variable.empty() ? "<loop>" : loop.variable;
        candidate.executionCount = loop.iterationCount;
        candidate.reason = "hot loop with stable loop variable";
        addGuard(candidate, loop.headerPc, "loop", "variable",
                 loop.variableObservation);
        result.candidates.push_back(std::move(candidate));
    }

    for (const auto& site : profile.callSites) {
        const bool hot = site.executionCount >= profile.hotLoopThreshold;
        const bool receiverStable =
            !site.hasReceiverObservation ||
            isStableGuardInput(site.receiverObservation);
        if (!hot || !receiverStable || !allStable(site.argumentObservations) ||
            !allStable(site.resultObservations)) {
            continue;
        }

        BytecodeOptimizationCandidate candidate;
        candidate.kind = site.kind + "-site";
        candidate.pc = site.pc;
        candidate.target = site.target.empty() ? "<runtime>" : site.target;
        candidate.executionCount = site.executionCount;
        candidate.reason = "hot call/index site with stable value shapes";
        if (site.hasReceiverObservation) {
            addGuard(candidate, site.pc, "call-site", "receiver",
                     site.receiverObservation);
        }
        addValueGuards(candidate, site.pc, "call-site", "arg",
                       site.argumentObservations);
        addValueGuards(candidate, site.pc, "call-site", "result",
                       site.resultObservations);
        if (!candidate.guards.empty()) {
            result.candidates.push_back(std::move(candidate));
        }
    }

    for (const auto& assignment : profile.assignments) {
        const auto hotLoop = hotLoops.find(assignment.loopHeaderPc);
        const bool inHotLoop =
            assignment.inLoop && hotLoop != hotLoops.end() && hotLoop->second;
        const bool hot = assignment.executionCount >= profile.hotLoopThreshold ||
                         inHotLoop;
        if (!hot || !isStableGuardInput(assignment.valueObservation)) {
            continue;
        }

        BytecodeOptimizationCandidate candidate;
        candidate.kind = assignment.kind + "-assignment";
        candidate.pc = assignment.pc;
        candidate.target =
            assignment.target.empty() ? "<unknown>" : assignment.target;
        candidate.executionCount = assignment.executionCount;
        candidate.reason = inHotLoop
                               ? "assignment in hot loop with stable value"
                               : "hot assignment with stable value";
        addGuard(candidate, assignment.pc, "assignment", "value",
                 assignment.valueObservation);
        result.candidates.push_back(std::move(candidate));
    }

    BytecodeRegionAnalyzer regionAnalyzer(
        std::move(builtinRegistry));
    for (auto& candidate : result.candidates) {
        candidate.region = regionAnalyzer.analyzeValidated(
            program, candidate.kind, candidate.pc, candidate.target);
    }

    return result;
}

BytecodeOptimizationPlan
BytecodeOptimizationPlanner::planStaticLoops(
    const BytecodeProgram& program) const {
    return planStaticLoops(program, {});
}

BytecodeOptimizationPlan BytecodeOptimizationPlanner::planStaticLoops(
    const BytecodeProgram& program,
    std::shared_ptr<const BuiltinRegistry> builtinRegistry) const {
    BytecodeOptimizationPlan result;
    if (!validateBytecodeProgram(program).succeeded) {
        return result;
    }
    BytecodeRegionAnalyzer regionAnalyzer(
        std::move(builtinRegistry));

    for (size_t pc = 0; pc < program.instructions.size(); ++pc) {
        const auto& instruction = program.instructions[pc];
        if (instruction.op != BytecodeOp::ForBegin ||
            instruction.operand.empty()) {
            continue;
        }

        auto region = regionAnalyzer.analyzeValidated(
            program, "hot-loop", pc, instruction.operand);
        if (!region.eligibleForTypedExecution) {
            continue;
        }

        BytecodeOptimizationCandidate candidate;
        candidate.kind = "hot-loop";
        candidate.pc = pc;
        candidate.target = instruction.operand;
        candidate.reason =
            "statically eligible loop with runtime scalar guards";
        candidate.guards.push_back(BytecodeOptimizationGuard{
            pc, "loop", "variable", "number", "double", 1, 1,
            {1, 1}, 0});
        candidate.region = std::move(region);
        result.candidates.push_back(std::move(candidate));
    }

    return result;
}

} // namespace mparser
