#include "mparser/typed_ir.h"

#include <optional>
#include <string>
#include <string_view>

namespace mparser {
namespace {

bool isScalarNumber(const BytecodeOptimizationGuard& guard) {
    return guard.kind == "number" && guard.rows == 1 && guard.columns == 1;
}

bool allScalarNumberGuards(
    const BytecodeOptimizationCandidate& candidate) {
    if (candidate.guards.empty()) {
        return false;
    }
    for (const auto& guard : candidate.guards) {
        if (!isScalarNumber(guard)) {
            return false;
        }
    }
    return true;
}

std::string typedRegionKind(
    const BytecodeOptimizationCandidate& candidate) {
    const bool scalar = allScalarNumberGuards(candidate);
    if (candidate.kind == "hot-loop") {
        return scalar ? "scalar-loop" : "typed-loop";
    }
    if (candidate.kind == "function-site" ||
        candidate.kind == "builtin-site" ||
        candidate.kind == "index-site") {
        return scalar ? "scalar-call-site" : "typed-call-site";
    }
    if (candidate.kind == "name-assignment" ||
        candidate.kind == "index-assignment") {
        return scalar ? "scalar-assignment" : "typed-assignment";
    }
    return scalar ? "scalar-region" : "typed-region";
}

BytecodeTypedIrGuard typedGuard(
    const BytecodeOptimizationGuard& guard) {
    return BytecodeTypedIrGuard{
        guard.source,
        guard.role,
        BytecodeTypedValue{guard.kind, guard.rows, guard.columns},
        guard.observationCount};
}

void addCommonOperations(BytecodeTypedIrRegion& region,
                         const BytecodeOptimizationCandidate& candidate) {
    region.operations.push_back(BytecodeTypedIrOperation{
        "guard-values", "guards=" + std::to_string(region.guards.size())});

    const auto& contract = candidate.region;
    region.operations.push_back(BytecodeTypedIrOperation{
        "region-contract",
        "pc=[" + std::to_string(contract.beginPc) + "," +
            std::to_string(contract.endPc) + ")"});

    if (!contract.eligibleForTypedExecution) {
        region.operations.push_back(BytecodeTypedIrOperation{
            "reject-region", "reason=" + contract.reason});
        region.operations.push_back(BytecodeTypedIrOperation{
            "deopt-on-guard-failure", "fallback=bytecode-vm"});
        return;
    }

    if (candidate.kind == "hot-loop") {
        region.operations.push_back(BytecodeTypedIrOperation{
            "specialize-loop", "target=" + candidate.target});
    } else if (candidate.kind == "function-site" ||
               candidate.kind == "builtin-site") {
        region.operations.push_back(BytecodeTypedIrOperation{
            "specialize-call", "target=" + candidate.target});
    } else if (candidate.kind == "index-site") {
        region.operations.push_back(BytecodeTypedIrOperation{
            "specialize-index", "target=" + candidate.target});
    } else if (candidate.kind == "name-assignment" ||
               candidate.kind == "index-assignment") {
        region.operations.push_back(BytecodeTypedIrOperation{
            "specialize-store", "target=" + candidate.target});
    } else {
        region.operations.push_back(BytecodeTypedIrOperation{
            "specialize-region", "target=" + candidate.target});
    }

    region.operations.push_back(BytecodeTypedIrOperation{
        "deopt-on-guard-failure", "fallback=bytecode-vm"});
}

std::string runtimeKindName(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return "missing";
    case RuntimeValueKind::Number:
        return "number";
    case RuntimeValueKind::String:
        return "string";
    case RuntimeValueKind::Vector:
        return "vector";
    case RuntimeValueKind::Matrix:
        return "matrix";
    }
    return "unknown";
}

size_t runtimeRows(const RuntimeValue& value) {
    if (value.kind == RuntimeValueKind::Number ||
        value.kind == RuntimeValueKind::String) {
        return 1;
    }
    return value.rows;
}

size_t runtimeColumns(const RuntimeValue& value) {
    if (value.kind == RuntimeValueKind::Number) {
        return 1;
    }
    if (value.kind == RuntimeValueKind::String) {
        return value.text.size();
    }
    return value.columns;
}

std::string shapeText(std::string_view kind, size_t rows, size_t columns) {
    return std::string(kind) + "(" + std::to_string(rows) + "x" +
           std::to_string(columns) + ")";
}

const RuntimeValue* findRuntimeVariable(
    const std::vector<RuntimeVariable>& variables, std::string_view name) {
    for (const auto& variable : variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

std::optional<std::string> guardRuntimeName(
    const BytecodeTypedIrRegion& region,
    const BytecodeTypedIrGuard& guard) {
    if (guard.source == "loop" && guard.role == "variable") {
        return region.target;
    }
    if (guard.source == "assignment" && guard.role == "value") {
        return region.target;
    }
    return std::nullopt;
}

BytecodeTypedIrGuardCheck evaluateGuard(
    const BytecodeTypedIrRegion& region, const BytecodeTypedIrGuard& guard,
    const std::vector<RuntimeVariable>& variables) {
    BytecodeTypedIrGuardCheck check;
    check.source = guard.source;
    check.role = guard.role;

    const auto runtimeName = guardRuntimeName(region, guard);
    if (!runtimeName) {
        check.reason = "no live runtime value for guard role";
        return check;
    }

    const RuntimeValue* value = findRuntimeVariable(variables, *runtimeName);
    if (!value) {
        check.reason = "runtime value is not available: " + *runtimeName;
        return check;
    }

    check.checked = true;
    const std::string actualKind = runtimeKindName(*value);
    const size_t actualRows = runtimeRows(*value);
    const size_t actualColumns = runtimeColumns(*value);
    check.passed = actualKind == guard.value.kind &&
                   actualRows == guard.value.rows &&
                   actualColumns == guard.value.columns;
    if (check.passed) {
        check.reason = "matched " +
                       shapeText(actualKind, actualRows, actualColumns);
    } else {
        check.reason = "expected " +
                       shapeText(guard.value.kind, guard.value.rows,
                                 guard.value.columns) +
                       ", got " +
                       shapeText(actualKind, actualRows, actualColumns);
    }
    return check;
}

} // namespace

BytecodeTypedIrModule BytecodeTypedIrBuilder::build(
    const BytecodeOptimizationPlan& plan) const {
    BytecodeTypedIrModule module;
    module.regions.reserve(plan.candidates.size());

    for (const auto& candidate : plan.candidates) {
        if (candidate.guards.empty()) {
            continue;
        }

        BytecodeTypedIrRegion region;
        region.id = module.regions.size();
        region.kind = typedRegionKind(candidate);
        region.sourcePc = candidate.pc;
        region.target = candidate.target;
        region.executionCount = candidate.executionCount;
        region.region = candidate.region;
        region.guards.reserve(candidate.guards.size());
        for (const auto& guard : candidate.guards) {
            region.guards.push_back(typedGuard(guard));
        }
        addCommonOperations(region, candidate);
        module.regions.push_back(std::move(region));
    }

    return module;
}

BytecodeTypedIrEvaluation BytecodeTypedIrGuardEvaluator::evaluate(
    const BytecodeTypedIrModule& module,
    const std::vector<RuntimeVariable>& variables) const {
    BytecodeTypedIrEvaluation evaluation;
    evaluation.regions.reserve(module.regions.size());

    for (const auto& region : module.regions) {
        BytecodeTypedIrRegionEvaluation regionEvaluation;
        regionEvaluation.regionId = region.id;
        regionEvaluation.kind = region.kind;
        regionEvaluation.target = region.target;
        regionEvaluation.regionEligible =
            region.region.eligibleForTypedExecution;
        regionEvaluation.regionReason = region.region.reason;
        regionEvaluation.checks.reserve(region.guards.size());

        for (const auto& guard : region.guards) {
            auto check = evaluateGuard(region, guard, variables);
            if (!check.checked) {
                ++regionEvaluation.skippedCount;
            } else if (check.passed) {
                ++regionEvaluation.checkedCount;
                ++regionEvaluation.passedCount;
            } else {
                ++regionEvaluation.checkedCount;
                ++regionEvaluation.failedCount;
            }
            regionEvaluation.checks.push_back(std::move(check));
        }

        regionEvaluation.canEnterTypedPath =
            region.region.eligibleForTypedExecution &&
            regionEvaluation.checkedCount > 0 &&
            regionEvaluation.failedCount == 0;
        evaluation.regions.push_back(std::move(regionEvaluation));
    }

    return evaluation;
}

} // namespace mparser
