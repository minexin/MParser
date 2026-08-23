#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/execution/jit/dense_array_region_executor.h"

#include <map>
#include <optional>
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

bool sameObservedTypeAndShape(const BytecodeValueObservation& left,
                              const BytecodeValueObservation& right) {
    return left.kind == right.kind &&
           left.numericClass == right.numericClass &&
           left.rows == right.rows &&
           left.columns == right.columns &&
           left.dimensions == right.dimensions;
}

std::optional<BytecodeValueObservation> denseInputObservation(
    const BytecodeVmProfile* profile, const BytecodeRegionContract& region,
    std::string_view name) {
    if (!profile) {
        return std::nullopt;
    }
    std::optional<BytecodeValueObservation> result;
    for (const auto& load : profile->loads) {
        if (load.pc < region.beginPc || load.pc >= region.bodyEndPc ||
            load.name != name ||
            !isStableGuardInput(load.valueObservation)) {
            continue;
        }
        if (!result) {
            result = load.valueObservation;
            continue;
        }
        if (!sameObservedTypeAndShape(*result, load.valueObservation)) {
            return std::nullopt;
        }
        result->observationCount +=
            load.valueObservation.observationCount;
    }
    return result;
}

std::vector<bool> forLoopMembership(const BytecodeProgram& program) {
    std::vector<int> depthChanges(program.instructions.size() + 1, 0);
    for (size_t candidate = 0; candidate < program.instructions.size();
         ++candidate) {
        const auto& instruction = program.instructions[candidate];
        if (instruction.op == BytecodeOp::ForBegin &&
            instruction.target > static_cast<int>(candidate) &&
            instruction.target <=
                static_cast<int>(program.instructions.size())) {
            ++depthChanges[candidate + 1];
            --depthChanges[static_cast<size_t>(instruction.target)];
        }
    }
    std::vector<bool> result(program.instructions.size(), false);
    int depth = 0;
    for (size_t pc = 0; pc < result.size(); ++pc) {
        depth += depthChanges[pc];
        result[pc] = depth != 0;
    }
    return result;
}

void addDenseInputGuard(BytecodeOptimizationCandidate& candidate,
                        size_t storePc, std::string_view name,
                        const std::optional<BytecodeValueObservation>&
                            observation) {
    if (observation) {
        addGuard(candidate, storePc, "region-input", std::string(name),
                 *observation);
        return;
    }
    candidate.guards.push_back(BytecodeOptimizationGuard{
        storePc, "region-input", std::string(name), "numeric", "double",
        0, 0, {}, 0, false});
}

void addDenseCandidate(
    BytecodeOptimizationPlan& plan, const BytecodeProgram& program,
    size_t storePc, const BytecodeAssignmentProfile* assignment,
    const BytecodeVmProfile* profile,
    const std::shared_ptr<const BuiltinRegistry>& builtinRegistry,
    const std::vector<bool>& insideForLoops, bool staticPlanning) {
    if (storePc >= insideForLoops.size() || insideForLoops[storePc] ||
        storePc >= program.instructions.size()) {
        return;
    }
    const auto& store = program.instructions[storePc];
    const auto analysis = analyzeDenseArrayAssignmentRegion(
        program, storePc, *builtinRegistry);
    if (!analysis.eligible || analysis.inputs.empty()) {
        return;
    }
    const bool observedDenseOutput =
        assignment &&
        assignment->valueObservation.numericClass == "double" &&
        (assignment->valueObservation.kind == "vector" ||
         assignment->valueObservation.kind == "matrix");
    if (!staticPlanning && !observedDenseOutput &&
        analysis.reductionOperationCount == 0) {
        return;
    }
    if (staticPlanning && !analysis.explicitArraySyntax &&
        analysis.reductionOperationCount == 0) {
        return;
    }

    BytecodeOptimizationCandidate candidate;
    candidate.kind = "dense-array-assignment";
    candidate.pc = storePc;
    candidate.target = store.operand;
    candidate.executionCount =
        assignment ? assignment->executionCount : 0;
    candidate.reason = staticPlanning
                           ? "statically closed dense assignment with runtime shape guards"
                           : "profiled dense assignment with stable double-array output";
    BytecodeRegionAnalyzer analyzer(builtinRegistry);
    candidate.region = analyzer.analyze(
        program, candidate.kind, storePc, candidate.target);
    for (const auto& input : candidate.region.inputs) {
        addDenseInputGuard(
            candidate, storePc, input,
            denseInputObservation(profile, candidate.region, input));
    }
    plan.candidates.push_back(std::move(candidate));
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
    if (!builtinRegistry) {
        builtinRegistry = defaultBuiltinRegistry();
    }
    result.hotLoopThreshold = profile.hotLoopThreshold;
    if (!validateBytecodeProgram(program).succeeded) {
        return result;
    }
    const auto insideForLoops = forLoopMembership(program);
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

    for (const auto& assignment : profile.assignments) {
        addDenseCandidate(result, program, assignment.pc, &assignment,
                          &profile, builtinRegistry, insideForLoops, false);
    }

    BytecodeRegionAnalyzer regionAnalyzer(
        builtinRegistry);
    for (auto& candidate : result.candidates) {
        candidate.region = regionAnalyzer.analyzeValidated(
            program, candidate.kind, candidate.pc, candidate.target);
    }

    return result;
}

BytecodeOptimizationPlan
BytecodeOptimizationPlanner::planStaticRegions(
    const BytecodeProgram& program) const {
    return planStaticRegions(program, {});
}

BytecodeOptimizationPlan BytecodeOptimizationPlanner::planStaticRegions(
    const BytecodeProgram& program,
    std::shared_ptr<const BuiltinRegistry> builtinRegistry) const {
    BytecodeOptimizationPlan result;
    if (!builtinRegistry) {
        builtinRegistry = defaultBuiltinRegistry();
    }
    if (!validateBytecodeProgram(program).succeeded) {
        return result;
    }
    const auto insideForLoops = forLoopMembership(program);
    BytecodeRegionAnalyzer regionAnalyzer(
        builtinRegistry);

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

    for (size_t pc = 0; pc < program.instructions.size(); ++pc) {
        if (program.instructions[pc].op == BytecodeOp::StoreName) {
            addDenseCandidate(result, program, pc, nullptr, nullptr,
                              builtinRegistry, insideForLoops, true);
        }
    }

    return result;
}

} // namespace mparser
