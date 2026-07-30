#include "mparser/adaptive_bytecode_vm.h"

#include "mparser/optimization_plan.h"

#include <algorithm>
#include <utility>

namespace mparser {
namespace {

std::unique_ptr<HirNode> cloneHirNode(const HirNode& source) {
    auto result = std::make_unique<HirNode>(source.kind);
    result->label = source.label;
    result->raw = source.raw;
    result->lexicalClassName = source.lexicalClassName;
    result->span = source.span;
    result->binding = source.binding;
    result->attributes = source.attributes;
    result->argumentBlock = source.argumentBlock;
    result->nameValueSourceClass = source.nameValueSourceClass;
    result->nameValueSourceSpan = source.nameValueSourceSpan;
    result->superclasses = source.superclasses;
    result->property = source.property;
    result->children.reserve(source.children.size());
    for (const auto& child : source.children) {
        result->children.push_back(
            child ? cloneHirNode(*child) : nullptr);
    }
    return result;
}

SemanticResult cloneSemanticResult(const SemanticResult& source) {
    SemanticResult result;
    result.root =
        source.root ? cloneHirNode(*source.root) : nullptr;
    result.scopes = source.scopes;
    result.symbols = source.symbols;
    result.sources = source.sources;
    result.diagnostics = source.diagnostics;
    result.builtinRegistry = source.builtinRegistry;
    return result;
}

void mergeObservation(BytecodeValueObservation& destination,
                      const BytecodeValueObservation& source) {
    if (source.observationCount == 0) {
        return;
    }
    if (destination.observationCount == 0) {
        destination = source;
        return;
    }

    const bool sameShape = destination.kind == source.kind &&
                           destination.numericClass == source.numericClass &&
                           destination.rows == source.rows &&
                           destination.columns == source.columns &&
                           destination.dimensions == source.dimensions;
    destination.observationCount += source.observationCount;
    if (!destination.stable || !source.stable || !sameShape) {
        destination.kind = "mixed";
        destination.numericClass.clear();
        destination.rows = 0;
        destination.columns = 0;
        destination.dimensions.clear();
        destination.stable = false;
    }
}

void mergeObservations(
    std::vector<BytecodeValueObservation>& destination,
    const std::vector<BytecodeValueObservation>& source) {
    const size_t shared = std::min(destination.size(), source.size());
    for (size_t index = 0; index < shared; ++index) {
        mergeObservation(destination[index], source[index]);
    }

    if (destination.empty()) {
        destination = source;
        return;
    }

    if (destination.size() < source.size()) {
        const size_t oldSize = destination.size();
        destination.insert(destination.end(), source.begin() + oldSize,
                           source.end());
        for (size_t index = oldSize; index < destination.size(); ++index) {
            destination[index].kind = "mixed";
            destination[index].numericClass.clear();
            destination[index].rows = 0;
            destination[index].columns = 0;
            destination[index].dimensions.clear();
            destination[index].stable = false;
        }
    } else if (destination.size() > source.size()) {
        for (size_t index = source.size(); index < destination.size();
             ++index) {
            destination[index].kind = "mixed";
            destination[index].numericClass.clear();
            destination[index].rows = 0;
            destination[index].columns = 0;
            destination[index].dimensions.clear();
            destination[index].stable = false;
        }
    }
}

template <typename T, typename Predicate>
T* findProfile(std::vector<T>& profiles, Predicate predicate) {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    std::move(predicate));
    return found == profiles.end() ? nullptr : &*found;
}

} // namespace

std::string_view adaptiveBytecodeTierName(AdaptiveBytecodeTier tier) {
    switch (tier) {
    case AdaptiveBytecodeTier::Profiling:
        return "profiling";
    case AdaptiveBytecodeTier::Typed:
        return "typed";
    }
    return "unknown";
}

std::string_view adaptiveBytecodeEventKindName(
    AdaptiveBytecodeEventKind kind) {
    switch (kind) {
    case AdaptiveBytecodeEventKind::Promotion:
        return "promotion";
    case AdaptiveBytecodeEventKind::TypedExecution:
        return "typed-execution";
    case AdaptiveBytecodeEventKind::TypedFallback:
        return "typed-fallback";
    case AdaptiveBytecodeEventKind::Invalidation:
        return "invalidation";
    case AdaptiveBytecodeEventKind::RetrainingRejected:
        return "retraining-rejected";
    }
    return "unknown";
}

AdaptiveBytecodeVmSession::AdaptiveBytecodeVmSession(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const AdaptiveBytecodeVmOptions& options)
    : program_(program), semantic_(cloneSemanticResult(semantic)),
      options_(options) {
    options_.hotLoopThreshold =
        std::max<size_t>(1, options_.hotLoopThreshold);
    options_.fallbackInvalidationThreshold =
        std::max<size_t>(1, options_.fallbackInvalidationThreshold);
    if (!options_.callableContext) {
        options_.callableContext = makeRuntimeCallableContext();
    }
    if (!options_.sessionState) {
        options_.sessionState =
            std::make_shared<RuntimeSessionState>();
    }
    bytecodeValidated_ =
        validateBytecodeProgram(program_, &semantic_).succeeded;
    reset();
}

AdaptiveBytecodeVmRunResult AdaptiveBytecodeVmSession::run() {
    AdaptiveBytecodeVmRunResult result;
    result.invocation = ++invocationCount_;

    BytecodeVm vm;
    if (hasTypedModule_) {
        result.tier = AdaptiveBytecodeTier::Typed;
        BytecodeVmOptions vmOptions;
        vmOptions.profiling = BytecodeVmProfilingMode::Disabled;
        vmOptions.callableContext = options_.callableContext;
        vmOptions.sessionState = options_.sessionState;
        vmOptions.initialWorkspace = workspace_;
        vmOptions.entryFunction = options_.entryFunction;
        vmOptions.arguments = arguments_;
        vmOptions.requestedOutputCount = options_.requestedOutputCount;
        vmOptions.typedRegionBackend = options_.typedRegionBackend;
        result.runtime = bytecodeValidated_
                             ? vm.runValidated(
                                   program_, semantic_,
                                   typedModule_, vmOptions)
                             : vm.run(program_, semantic_,
                                      typedModule_, vmOptions);
        if (options_.preserveWorkspace &&
            !hasErrorDiagnostics(result.runtime.diagnostics)) {
            workspace_ = result.runtime.variables;
        }
        result.installedRegionCount = typedModule_.regions.size();
        result.executableRegionCount =
            executableRegionCount(typedModule_);
        processTypedExecutions(result);
        populateSessionState(result);
        return result;
    }

    result.tier = AdaptiveBytecodeTier::Profiling;
    BytecodeVmOptions vmOptions;
    vmOptions.callableContext = options_.callableContext;
    vmOptions.sessionState = options_.sessionState;
    vmOptions.initialWorkspace = workspace_;
    vmOptions.entryFunction = options_.entryFunction;
    vmOptions.arguments = arguments_;
    vmOptions.requestedOutputCount = options_.requestedOutputCount;
    vmOptions.typedRegionBackend = options_.typedRegionBackend;
    result.runtime =
        bytecodeValidated_
            ? vm.runValidated(program_, semantic_, vmOptions)
            : vm.run(program_, semantic_, vmOptions);
    if (hasErrorDiagnostics(result.runtime.diagnostics)) {
        populateSessionState(result);
        return result;
    }

    if (options_.preserveWorkspace) {
        workspace_ = result.runtime.variables;
    }

    mergeProfile(result.runtime.profile);
    BytecodeOptimizationPlanner planner;
    BytecodeTypedIrBuilder builder;
    auto candidateModule = builder.build(planner.plan(
        accumulatedProfile_, program_,
        semantic_.builtinRegistry));
    applyRetrainingRequirements(candidateModule);
    const size_t executableCount =
        executableRegionCount(candidateModule);
    if (executableCount > 0) {
        typedModule_ = std::move(candidateModule);
        hasTypedModule_ = true;
        result.promotionOccurred = true;
        result.installedRegionCount = typedModule_.regions.size();
        result.executableRegionCount = executableCount;
        ++promotionCount_;
        for (const auto& region : typedModule_.regions) {
            if (region.kind == "scalar-loop" &&
                region.region.eligibleForTypedExecution) {
                appendEvent(AdaptiveBytecodeEventKind::Promotion,
                            region.id, region.sourcePc, region.target,
                            "typed region installed");
            }
        }
    }
    populateSessionState(result);
    return result;
}

void AdaptiveBytecodeVmSession::reset() {
    invocationCount_ = 0;
    resetAccumulatedProfile();
    typedModule_ = {};
    hasTypedModule_ = false;
    promotionCount_ = 0;
    invalidationCount_ = 0;
    consecutiveFallbacks_.clear();
    retrainingSources_.clear();
    reportedRetrainingRejections_.clear();
    events_.clear();
    workspace_ = options_.initialWorkspace;
    arguments_ = options_.arguments;
}

size_t AdaptiveBytecodeVmSession::invocationCount() const {
    return invocationCount_;
}

bool AdaptiveBytecodeVmSession::hasTypedModule() const {
    return hasTypedModule_;
}

const BytecodeVmProfile&
AdaptiveBytecodeVmSession::accumulatedProfile() const {
    return accumulatedProfile_;
}

const BytecodeTypedIrModule*
AdaptiveBytecodeVmSession::typedModule() const {
    return hasTypedModule_ ? &typedModule_ : nullptr;
}

const std::vector<AdaptiveBytecodeEvent>&
AdaptiveBytecodeVmSession::events() const {
    return events_;
}

const std::vector<RuntimeVariable>&
AdaptiveBytecodeVmSession::workspace() const {
    return workspace_;
}

void AdaptiveBytecodeVmSession::setWorkspace(
    std::vector<RuntimeVariable> workspace) {
    workspace_ = std::move(workspace);
}

const std::vector<RuntimeValue>&
AdaptiveBytecodeVmSession::arguments() const {
    return arguments_;
}

void AdaptiveBytecodeVmSession::setArguments(
    std::vector<RuntimeValue> arguments) {
    arguments_ = std::move(arguments);
}

std::optional<size_t>
AdaptiveBytecodeVmSession::requestedOutputCount() const {
    return options_.requestedOutputCount;
}

void AdaptiveBytecodeVmSession::setRequestedOutputCount(
    std::optional<size_t> count) {
    options_.requestedOutputCount = count;
}

void AdaptiveBytecodeVmSession::mergeProfile(
    const BytecodeVmProfile& profile) {
    if (!profile.collected) {
        return;
    }

    accumulatedProfile_.collected = true;
    accumulatedProfile_.hotLoopThreshold = options_.hotLoopThreshold;

    for (const auto& instruction : profile.instructions) {
        auto* destination = findProfile(
            accumulatedProfile_.instructions,
            [&](const auto& candidate) {
                return candidate.pc == instruction.pc;
            });
        if (!destination) {
            accumulatedProfile_.instructions.push_back(instruction);
        } else {
            destination->executionCount += instruction.executionCount;
        }
    }

    for (const auto& function : profile.functions) {
        auto* destination = findProfile(
            accumulatedProfile_.functions,
            [&](const auto& candidate) {
                return candidate.name == function.name;
            });
        if (!destination) {
            accumulatedProfile_.functions.push_back(function);
        } else {
            destination->callCount += function.callCount;
            destination->executedInstructionCount +=
                function.executedInstructionCount;
        }
    }

    for (const auto& loop : profile.loops) {
        auto* destination = findProfile(
            accumulatedProfile_.loops,
            [&](const auto& candidate) {
                return candidate.headerPc == loop.headerPc;
            });
        if (!destination) {
            accumulatedProfile_.loops.push_back(loop);
            destination = &accumulatedProfile_.loops.back();
        } else {
            destination->entryCount += loop.entryCount;
            destination->iterationCount += loop.iterationCount;
            destination->backedgeCount += loop.backedgeCount;
            destination->completionCount += loop.completionCount;
            destination->breakCount += loop.breakCount;
            destination->continueCount += loop.continueCount;
            mergeObservation(destination->variableObservation,
                             loop.variableObservation);
        }
        destination->hot =
            destination->iterationCount >= options_.hotLoopThreshold ||
            destination->backedgeCount >= options_.hotLoopThreshold;
    }

    for (const auto& site : profile.callSites) {
        auto* destination = findProfile(
            accumulatedProfile_.callSites,
            [&](const auto& candidate) {
                return candidate.pc == site.pc;
            });
        if (!destination) {
            accumulatedProfile_.callSites.push_back(site);
            continue;
        }
        destination->executionCount += site.executionCount;
        if (destination->hasReceiverObservation !=
            site.hasReceiverObservation) {
            destination->receiverObservation.kind = "mixed";
            destination->receiverObservation.numericClass.clear();
            destination->receiverObservation.rows = 0;
            destination->receiverObservation.columns = 0;
            destination->receiverObservation.dimensions.clear();
            destination->receiverObservation.stable = false;
        } else if (site.hasReceiverObservation) {
            mergeObservation(destination->receiverObservation,
                             site.receiverObservation);
        }
        mergeObservations(destination->argumentObservations,
                          site.argumentObservations);
        mergeObservations(destination->resultObservations,
                          site.resultObservations);
    }

    for (const auto& assignment : profile.assignments) {
        auto* destination = findProfile(
            accumulatedProfile_.assignments,
            [&](const auto& candidate) {
                return candidate.pc == assignment.pc;
            });
        if (!destination) {
            accumulatedProfile_.assignments.push_back(assignment);
            continue;
        }
        destination->executionCount += assignment.executionCount;
        destination->inLoop = destination->inLoop || assignment.inLoop;
        mergeObservation(destination->valueObservation,
                         assignment.valueObservation);
    }

    for (const auto& input : profile.workspaceInputs) {
        auto* destination = findProfile(
            accumulatedProfile_.workspaceInputs,
            [&](const auto& candidate) {
                return candidate.name == input.name;
            });
        if (!destination) {
            accumulatedProfile_.workspaceInputs.push_back(input);
            continue;
        }
        mergeObservation(destination->valueObservation,
                         input.valueObservation);
    }

    for (const auto& entry : profile.functionEntries) {
        auto* destination = findProfile(
            accumulatedProfile_.functionEntries,
            [&](const auto& candidate) {
                return candidate.name == entry.name;
            });
        if (!destination) {
            accumulatedProfile_.functionEntries.push_back(entry);
            continue;
        }
        destination->invocationCount += entry.invocationCount;
        if (destination->parameters != entry.parameters ||
            destination->outputs != entry.outputs) {
            for (auto& observation :
                 destination->argumentObservations) {
                observation.kind = "mixed";
                observation.numericClass.clear();
                observation.rows = 0;
                observation.columns = 0;
                observation.dimensions.clear();
                observation.stable = false;
            }
            for (auto& observation : destination->resultObservations) {
                observation.kind = "mixed";
                observation.numericClass.clear();
                observation.rows = 0;
                observation.columns = 0;
                observation.dimensions.clear();
                observation.stable = false;
            }
            continue;
        }
        mergeObservations(destination->argumentObservations,
                          entry.argumentObservations);
        mergeObservations(destination->resultObservations,
                          entry.resultObservations);
    }
}

size_t AdaptiveBytecodeVmSession::executableRegionCount(
    const BytecodeTypedIrModule& module) const {
    return static_cast<size_t>(std::count_if(
        module.regions.begin(), module.regions.end(),
        [](const auto& region) {
            return region.kind == "scalar-loop" &&
                   region.region.eligibleForTypedExecution;
        }));
}

void AdaptiveBytecodeVmSession::populateSessionState(
    AdaptiveBytecodeVmRunResult& result) const {
    for (const auto& loop : accumulatedProfile_.loops) {
        result.accumulatedLoopIterations += loop.iterationCount;
        if (loop.hot) {
            ++result.hotLoopCount;
        }
    }
    result.promotionCount = promotionCount_;
    result.invalidationCount = invalidationCount_;
}

void AdaptiveBytecodeVmSession::processTypedExecutions(
    AdaptiveBytecodeVmRunResult& result) {
    struct FailedRegion {
        size_t regionId = 0;
        size_t sourcePc = 0;
        std::string target;
        std::string reason;
        RuntimeFallbackKind fallbackKind =
            RuntimeFallbackKind::None;
        RuntimeFallbackKind nativeFallbackKind =
            RuntimeFallbackKind::None;
    };
    std::vector<FailedRegion> invalidatedRegions;

    for (const auto& execution : result.runtime.typedRegionExecutions) {
        if (execution.attemptCount == 0) {
            continue;
        }
        const auto* region = findTypedRegion(execution.regionId);
        const size_t sourcePc =
            region ? region->sourcePc : execution.sourcePc;
        const std::string target =
            region ? region->target : execution.target;

        if (execution.executionCount > 0) {
            consecutiveFallbacks_[sourcePc] = 0;
            appendEvent(AdaptiveBytecodeEventKind::TypedExecution,
                        execution.regionId, sourcePc, target,
                        execution.lastReason,
                        execution.lastFallbackKind,
                        execution.nativeFallbackKind);
        }
        if (execution.fallbackCount == 0) {
            continue;
        }

        auto& fallbackCount = consecutiveFallbacks_[sourcePc];
        fallbackCount += execution.fallbackCount;
        appendEvent(AdaptiveBytecodeEventKind::TypedFallback,
                    execution.regionId, sourcePc, target,
                    execution.lastReason,
                    execution.lastFallbackKind,
                    execution.nativeFallbackKind);
        if (fallbackCount >= options_.fallbackInvalidationThreshold) {
            invalidatedRegions.push_back(FailedRegion{
                execution.regionId, sourcePc, target,
                execution.lastReason,
                execution.lastFallbackKind,
                execution.nativeFallbackKind});
        }
    }

    if (invalidatedRegions.empty()) {
        return;
    }

    result.invalidationOccurred = true;
    ++invalidationCount_;
    for (const auto& region : invalidatedRegions) {
        retrainingSources_.insert(region.sourcePc);
        reportedRetrainingRejections_.erase(region.sourcePc);
        consecutiveFallbacks_.erase(region.sourcePc);
        appendEvent(AdaptiveBytecodeEventKind::Invalidation,
                    region.regionId, region.sourcePc, region.target,
                    region.reason, region.fallbackKind,
                    region.nativeFallbackKind);
    }
    typedModule_ = {};
    hasTypedModule_ = false;
    resetAccumulatedProfile();
    result.installedRegionCount = 0;
    result.executableRegionCount = 0;
}

void AdaptiveBytecodeVmSession::applyRetrainingRequirements(
    BytecodeTypedIrModule& module) {
    for (auto& region : module.regions) {
        if (region.kind != "scalar-loop" ||
            !region.region.eligibleForTypedExecution ||
            !retrainingSources_.contains(region.sourcePc)) {
            continue;
        }

        const auto evidence = hasRetrainingEvidence(region);
        if (evidence.first) {
            retrainingSources_.erase(region.sourcePc);
            reportedRetrainingRejections_.erase(region.sourcePc);
            continue;
        }

        region.region.eligibleForTypedExecution = false;
        region.region.fallbackKind =
            RuntimeFallbackKind::AdaptiveRetrainingRejected;
        region.region.reason = "adaptive retraining rejected: " +
                               evidence.second;
        if (reportedRetrainingRejections_.insert(region.sourcePc).second) {
            appendEvent(AdaptiveBytecodeEventKind::RetrainingRejected,
                        region.id, region.sourcePc, region.target,
                        evidence.second,
                        RuntimeFallbackKind::AdaptiveRetrainingRejected);
        }
    }
}

std::pair<bool, std::string>
AdaptiveBytecodeVmSession::hasRetrainingEvidence(
    const BytecodeTypedIrRegion& region) const {
    for (const auto& input : region.region.inputs) {
        bool observedBeforeRegion = false;
        for (const auto& assignment :
             accumulatedProfile_.assignments) {
            if (assignment.target != input ||
                assignment.pc >= region.sourcePc ||
                assignment.valueObservation.observationCount == 0) {
                continue;
            }
            observedBeforeRegion = true;
            const auto& value = assignment.valueObservation;
            if (!value.stable || value.kind != "number" ||
                value.numericClass != "double" ||
                value.rows != 1 || value.columns != 1) {
                return {false,
                        "input is not stable scalar numeric: " + input};
            }
        }
        if (observedBeforeRegion) {
            continue;
        }

        bool observedArgument = false;
        for (const auto& entry : accumulatedProfile_.functionEntries) {
            for (size_t index = 0; index < entry.parameters.size(); ++index) {
                if (entry.parameters[index] != input ||
                    index >= entry.argumentObservations.size()) {
                    continue;
                }
                observedArgument = true;
                const auto& value = entry.argumentObservations[index];
                if (!value.stable || value.kind != "number" ||
                    value.numericClass != "double" ||
                    value.rows != 1 || value.columns != 1) {
                    return {false,
                            "function argument is not stable scalar numeric: " +
                                input};
                }
            }
        }
        if (observedArgument) {
            continue;
        }

        const auto workspaceInput = std::find_if(
            accumulatedProfile_.workspaceInputs.begin(),
            accumulatedProfile_.workspaceInputs.end(),
            [&](const auto& candidate) {
                return candidate.name == input;
            });
        if (workspaceInput == accumulatedProfile_.workspaceInputs.end()) {
            return {false, "input has no entry observation: " + input};
        }
        const auto& value = workspaceInput->valueObservation;
        if (!value.stable || value.kind != "number" ||
            value.numericClass != "double" || value.rows != 1 ||
            value.columns != 1) {
            return {false,
                    "workspace input is not stable scalar numeric: " +
                        input};
        }
    }
    return {true, "all region inputs are stable scalar numeric"};
}

void AdaptiveBytecodeVmSession::resetAccumulatedProfile() {
    accumulatedProfile_ = {};
    accumulatedProfile_.hotLoopThreshold = options_.hotLoopThreshold;
}

const BytecodeTypedIrRegion*
AdaptiveBytecodeVmSession::findTypedRegion(size_t regionId) const {
    const auto found = std::find_if(
        typedModule_.regions.begin(), typedModule_.regions.end(),
        [regionId](const auto& region) {
            return region.id == regionId;
        });
    return found == typedModule_.regions.end() ? nullptr : &*found;
}

void AdaptiveBytecodeVmSession::appendEvent(
    AdaptiveBytecodeEventKind kind, size_t regionId, size_t sourcePc,
    std::string target, std::string reason,
    RuntimeFallbackKind fallbackKind,
    RuntimeFallbackKind nativeFallbackKind) {
    events_.push_back(AdaptiveBytecodeEvent{
        kind, invocationCount_, regionId, sourcePc, std::move(target),
        std::move(reason), fallbackKind, nativeFallbackKind});
}

} // namespace mparser
