#include "mparser/execution/runtime_benchmark.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_timetable.h"
#include "mparser/execution/jit/typed_ir.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

using Clock = std::chrono::steady_clock;

const std::map<std::string, RuntimeValue>& objectFields(
    const RuntimeValue& value) {
    if (value.handleObject && value.sharedFields) {
        return *value.sharedFields;
    }
    return value.fields;
}

using ComparedHandleObjects = std::set<std::pair<const void*, const void*>>;
using ComparedTables = std::set<
    std::pair<const RuntimeTableStorage*, const RuntimeTableStorage*>>;

bool runtimeValuesEqualImpl(const RuntimeValue& left,
                            const RuntimeValue& right,
                            ComparedHandleObjects& comparedHandles,
                            ComparedTables& comparedTables) {
    if (left.kind != right.kind ||
        left.numericClass != right.numericClass ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }

    if (isRuntimeNumericValue(left)) {
        return runtimeNumericValuesIdentical(left, right);
    }
    if (isRuntimeCategoricalValue(left) ||
        isRuntimeCategoricalValue(right)) {
        return isRuntimeCategoricalValue(left) &&
               isRuntimeCategoricalValue(right) &&
               runtimeCategoricalValuesEqual(left, right, true);
    }
    if (isRuntimeTabularValue(left) || isRuntimeTabularValue(right)) {
        if (!isRuntimeTabularValue(left) ||
            !isRuntimeTabularValue(right) ||
            left.className != right.className) {
            return false;
        }
        const auto pair = std::pair{
            runtimeTabularStorage(left), runtimeTabularStorage(right)};
        if (!comparedTables.insert(pair).second) {
            return true;
        }
        const auto valueEquality = [&comparedHandles, &comparedTables](
                const RuntimeValue& leftValue,
                const RuntimeValue& rightValue) {
                return runtimeValuesEqualImpl(
                    leftValue, rightValue, comparedHandles,
                    comparedTables);
            };
        return isRuntimeTimetableValue(left)
                   ? runtimeTimetableValuesEqual(
                         left, right, valueEquality)
                   : runtimeTableValuesEqual(
                         left, right, valueEquality);
    }

    switch (left.kind) {
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::MissingArray:
        return true;
    case RuntimeValueKind::Number:
        return false;
    case RuntimeValueKind::CharacterArray:
        return left.characterElements == right.characterElements;
    case RuntimeValueKind::StringArray:
        return left.stringElements == right.stringElements;
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        return false;
    case RuntimeValueKind::Cell:
    case RuntimeValueKind::CommaSeparatedList:
        if (left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeValuesEqualImpl(
                    left.cells[index], right.cells[index],
                    comparedHandles, comparedTables)) {
                return false;
            }
        }
        return true;
    case RuntimeValueKind::FunctionHandle:
        return left.functionHandle && right.functionHandle &&
               left.functionHandle->identity ==
                   right.functionHandle->identity;
    case RuntimeValueKind::NameValueArgument:
        return left.text == right.text && left.cells.size() == 1 &&
               right.cells.size() == 1 &&
               runtimeValuesEqualImpl(left.cells.front(), right.cells.front(),
                                      comparedHandles, comparedTables);
    case RuntimeValueKind::Struct:
        if (runtimeStructFieldOrder(left) !=
                runtimeStructFieldOrder(right) ||
            runtimeStructElementCount(left) !=
                runtimeStructElementCount(right)) {
            return false;
        }
        for (size_t offset = 0;
             offset < runtimeStructElementCount(left); ++offset) {
            const auto* leftElement = runtimeStructElement(left, offset);
            const auto* rightElement = runtimeStructElement(right, offset);
            if (!leftElement || !rightElement ||
                leftElement->size() != rightElement->size()) {
                return false;
            }
            for (const auto& [name, value] : *leftElement) {
                const auto other = rightElement->find(name);
                if (other == rightElement->end() ||
                    !runtimeValuesEqualImpl(value, other->second,
                                            comparedHandles,
                                            comparedTables)) {
                    return false;
                }
            }
        }
        return true;
    case RuntimeValueKind::Object: {
        if (isRuntimeClassObject(left) &&
            isRuntimeClassObject(right) &&
            (!isRuntimeScalarObject(left) ||
             !isRuntimeScalarObject(right))) {
            if (left.className != right.className ||
                left.handleObject != right.handleObject ||
                runtimeObjectElementCount(left) !=
                    runtimeObjectElementCount(right)) {
                return false;
            }
            for (size_t logicalIndex = 0;
                 logicalIndex < runtimeObjectElementCount(left);
                 ++logicalIndex) {
                const auto* leftElement =
                    runtimeObjectLogicalElement(left, logicalIndex);
                const auto* rightElement =
                    runtimeObjectLogicalElement(right, logicalIndex);
                if (!leftElement || !rightElement ||
                    !runtimeValuesEqualImpl(
                        *leftElement, *rightElement, comparedHandles,
                        comparedTables)) {
                    return false;
                }
            }
            return true;
        }
        const auto& leftFields = objectFields(left);
        const auto& rightFields = objectFields(right);
        if (left.className != right.className ||
            left.handleObject != right.handleObject ||
            leftFields.size() != rightFields.size()) {
            return false;
        }
        if (left.handleObject && left.sharedFields && right.sharedFields) {
            const auto identity = std::pair<const void*, const void*>{
                left.sharedFields.get(), right.sharedFields.get()};
            if (!comparedHandles.insert(identity).second) {
                return true;
            }
        }
        for (const auto& [name, value] : leftFields) {
            const auto other = rightFields.find(name);
            if (other == rightFields.end() ||
                !runtimeValuesEqualImpl(value, other->second,
                                        comparedHandles,
                                        comparedTables)) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

bool runtimeValuesEqual(const RuntimeValue& left,
                        const RuntimeValue& right) {
    ComparedHandleObjects comparedHandles;
    ComparedTables comparedTables;
    return runtimeValuesEqualImpl(
        left, right, comparedHandles, comparedTables);
}

std::pair<bool, std::string> compareVariables(
    const std::vector<RuntimeVariable>& left,
    std::string_view leftName,
    const std::vector<RuntimeVariable>& right,
    std::string_view rightName) {
    std::map<std::string_view, const RuntimeValue*> leftByName;
    std::map<std::string_view, const RuntimeValue*> rightByName;
    for (const auto& variable : left) {
        leftByName[variable.name] = &variable.value;
    }
    for (const auto& variable : right) {
        rightByName[variable.name] = &variable.value;
    }

    if (leftByName.size() != rightByName.size()) {
        return {false, std::string(leftName) + " and " +
                           std::string(rightName) +
                           " variable counts differ"};
    }

    for (const auto& [name, value] : leftByName) {
        const auto candidate = rightByName.find(name);
        if (candidate == rightByName.end()) {
            return {false, std::string(rightName) +
                               " output is missing variable: " +
                               std::string(name)};
        }
        if (!runtimeValuesEqual(*value, *candidate->second)) {
            return {false, std::string(leftName) + " and " +
                               std::string(rightName) +
                               " values differ for variable: " +
                               std::string(name)};
        }
    }
    return {true, std::string(leftName) + " and " +
                      std::string(rightName) + " outputs match"};
}

double elapsedMicroseconds(Clock::time_point begin,
                           Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

struct ExecutionTotals {
    size_t executedInstructionCount = 0;
    size_t typedRegionAttemptCount = 0;
    size_t typedRegionExecutionCount = 0;
    size_t typedRegionFallbackCount = 0;
    size_t typedNestedIterationCount = 0;
    size_t typedInstructionCount = 0;
    size_t typedKernelInstructionCount = 0;
};

void accumulateTypedExecutions(
    ExecutionTotals& totals, const BytecodeVmResult& runtime) {
    totals.executedInstructionCount += runtime.executedInstructionCount;
    for (const auto& execution : runtime.typedRegionExecutions) {
        totals.typedRegionAttemptCount += execution.attemptCount;
        totals.typedRegionExecutionCount += execution.executionCount;
        totals.typedRegionFallbackCount += execution.fallbackCount;
        totals.typedNestedIterationCount +=
            execution.nestedIterationCount;
        totals.typedInstructionCount += execution.executedInstructionCount;
        totals.typedKernelInstructionCount +=
            execution.executedKernelInstructionCount;
    }
}

RuntimeBenchmarkStatistics statistics(
    const std::vector<double>& samples,
    const ExecutionTotals& totals = {}) {
    RuntimeBenchmarkStatistics result;
    result.completedIterations = samples.size();
    if (samples.empty()) {
        return result;
    }

    result.totalMilliseconds =
        std::accumulate(samples.begin(), samples.end(), 0.0) / 1000.0;
    result.meanMicroseconds =
        result.totalMilliseconds * 1000.0 /
        static_cast<double>(samples.size());
    const auto [minimum, maximum] =
        std::minmax_element(samples.begin(), samples.end());
    result.minimumMicroseconds = *minimum;
    result.maximumMicroseconds = *maximum;

    std::vector<double> ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const size_t middle = ordered.size() / 2;
    result.medianMicroseconds =
        ordered.size() % 2 == 0
            ? (ordered[middle - 1] + ordered[middle]) / 2.0
            : ordered[middle];
    result.meanExecutedInstructionCount =
        static_cast<double>(totals.executedInstructionCount) /
        static_cast<double>(samples.size());
    result.meanTypedRegionAttemptCount =
        static_cast<double>(totals.typedRegionAttemptCount) /
        static_cast<double>(samples.size());
    result.meanTypedRegionExecutionCount =
        static_cast<double>(totals.typedRegionExecutionCount) /
        static_cast<double>(samples.size());
    result.meanTypedRegionFallbackCount =
        static_cast<double>(totals.typedRegionFallbackCount) /
        static_cast<double>(samples.size());
    result.meanTypedNestedIterationCount =
        static_cast<double>(totals.typedNestedIterationCount) /
        static_cast<double>(samples.size());
    result.meanTypedInstructionCount =
        static_cast<double>(totals.typedInstructionCount) /
        static_cast<double>(samples.size());
    result.meanTypedKernelInstructionCount =
        static_cast<double>(totals.typedKernelInstructionCount) /
        static_cast<double>(samples.size());
    return result;
}

bool hasDiagnostics(const InterpreterResult& result) {
    return hasErrorDiagnostics(result.diagnostics);
}

bool hasDiagnostics(const BytecodeVmResult& result) {
    return hasErrorDiagnostics(result.diagnostics);
}

} // namespace

RuntimeBenchmarkResult RuntimeBenchmarkRunner::run(
    const SemanticResult& semantic, const BytecodeProgram& bytecode,
    const RuntimeBenchmarkOptions& options) const {
    if (options.measuredIterations == 0) {
        throw std::invalid_argument(
            "runtime benchmark needs at least one measured iteration");
    }

    RuntimeBenchmarkResult result;
    result.options = options;

    auto validation =
        validateBytecodeProgram(bytecode, &semantic);
    if (!validation.succeeded) {
        result.lastProfiledBytecodeVmResult.diagnostics =
            std::move(validation.diagnostics);
        result.comparisonMessage =
            "benchmark bytecode validation failed";
        return result;
    }

    BytecodeVm profilingVm;
    result.lastProfiledBytecodeVmResult =
        profilingVm.runValidated(
            bytecode, semantic, BytecodeVmOptions{});
    if (hasDiagnostics(result.lastProfiledBytecodeVmResult)) {
        result.comparisonMessage =
            "benchmark profiling run reported diagnostics";
        return result;
    }
    BytecodeOptimizationPlanner planner;
    BytecodeTypedIrBuilder builder;
    const auto typedIr =
        builder.build(planner.plan(
            result.lastProfiledBytecodeVmResult.profile, bytecode,
            semantic.builtinRegistry));
    result.typedRegionCount = typedIr.regions.size();
    BytecodeVmOptions steadyOptions;
    steadyOptions.profiling = BytecodeVmProfilingMode::Disabled;
    steadyOptions.typedRegionBackend = options.typedRegionBackend;

    for (size_t iteration = 0; iteration < options.warmupIterations;
         ++iteration) {
        Interpreter interpreter;
        result.lastInterpreterResult = interpreter.run(semantic);
        BytecodeVm profiledVm;
        result.lastProfiledBytecodeVmResult =
            profiledVm.runValidated(
                bytecode, semantic, BytecodeVmOptions{});
        BytecodeVm steadyVm;
        result.lastBytecodeVmResult =
            steadyVm.runValidated(
                bytecode, semantic, steadyOptions);
        BytecodeVm typedVm;
        result.lastTypedBytecodeVmResult =
            typedVm.runValidated(
                bytecode, semantic, typedIr, steadyOptions);
        if (hasDiagnostics(result.lastInterpreterResult) ||
            hasDiagnostics(result.lastProfiledBytecodeVmResult) ||
            hasDiagnostics(result.lastBytecodeVmResult) ||
            hasDiagnostics(result.lastTypedBytecodeVmResult)) {
            result.comparisonMessage =
                "benchmark warmup stopped because a runtime reported diagnostics";
            return result;
        }
    }

    std::vector<double> interpreterSamples;
    std::vector<double> profiledBytecodeSamples;
    std::vector<double> bytecodeSamples;
    std::vector<double> typedBytecodeSamples;
    interpreterSamples.reserve(options.measuredIterations);
    profiledBytecodeSamples.reserve(options.measuredIterations);
    bytecodeSamples.reserve(options.measuredIterations);
    typedBytecodeSamples.reserve(options.measuredIterations);
    ExecutionTotals profiledBytecodeTotals;
    ExecutionTotals bytecodeTotals;
    ExecutionTotals typedBytecodeTotals;

    const auto runInterpreter = [&]() {
        Interpreter interpreter;
        const auto begin = Clock::now();
        result.lastInterpreterResult = interpreter.run(semantic);
        const auto end = Clock::now();
        interpreterSamples.push_back(elapsedMicroseconds(begin, end));
    };
    const auto runProfiledBytecodeVm = [&]() {
        BytecodeVm vm;
        const auto begin = Clock::now();
        result.lastProfiledBytecodeVmResult =
            vm.runValidated(
                bytecode, semantic, BytecodeVmOptions{});
        const auto end = Clock::now();
        profiledBytecodeSamples.push_back(
            elapsedMicroseconds(begin, end));
        profiledBytecodeTotals.executedInstructionCount +=
            result.lastProfiledBytecodeVmResult.executedInstructionCount;
    };
    const auto runBytecodeVm = [&]() {
        BytecodeVm vm;
        const auto begin = Clock::now();
        result.lastBytecodeVmResult =
            vm.runValidated(bytecode, semantic, steadyOptions);
        const auto end = Clock::now();
        bytecodeSamples.push_back(elapsedMicroseconds(begin, end));
        bytecodeTotals.executedInstructionCount +=
            result.lastBytecodeVmResult.executedInstructionCount;
    };
    const auto runTypedBytecodeVm = [&]() {
        BytecodeVm vm;
        const auto begin = Clock::now();
        result.lastTypedBytecodeVmResult =
            vm.runValidated(
                bytecode, semantic, typedIr, steadyOptions);
        const auto end = Clock::now();
        typedBytecodeSamples.push_back(elapsedMicroseconds(begin, end));
        accumulateTypedExecutions(typedBytecodeTotals,
                                  result.lastTypedBytecodeVmResult);
    };

    for (size_t iteration = 0; iteration < options.measuredIterations;
         ++iteration) {
        if (iteration % 4 == 0) {
            runInterpreter();
            runProfiledBytecodeVm();
            runBytecodeVm();
            runTypedBytecodeVm();
        } else if (iteration % 4 == 1) {
            runProfiledBytecodeVm();
            runBytecodeVm();
            runTypedBytecodeVm();
            runInterpreter();
        } else if (iteration % 4 == 2) {
            runBytecodeVm();
            runTypedBytecodeVm();
            runInterpreter();
            runProfiledBytecodeVm();
        } else {
            runTypedBytecodeVm();
            runInterpreter();
            runProfiledBytecodeVm();
            runBytecodeVm();
        }

        if (hasDiagnostics(result.lastInterpreterResult) ||
            hasDiagnostics(result.lastProfiledBytecodeVmResult) ||
            hasDiagnostics(result.lastBytecodeVmResult) ||
            hasDiagnostics(result.lastTypedBytecodeVmResult)) {
            break;
        }
    }

    result.interpreter = statistics(interpreterSamples);
    result.profiledBytecodeVm =
        statistics(profiledBytecodeSamples, profiledBytecodeTotals);
    result.bytecodeVm = statistics(bytecodeSamples, bytecodeTotals);
    result.typedBytecodeVm =
        statistics(typedBytecodeSamples, typedBytecodeTotals);
    if (hasDiagnostics(result.lastInterpreterResult) ||
        hasDiagnostics(result.lastProfiledBytecodeVmResult) ||
        hasDiagnostics(result.lastBytecodeVmResult) ||
        hasDiagnostics(result.lastTypedBytecodeVmResult)) {
        result.comparisonMessage =
            "benchmark stopped because a runtime reported diagnostics";
        return result;
    }

    result.outputsComparable =
        result.interpreter.completedIterations == options.measuredIterations &&
        result.profiledBytecodeVm.completedIterations ==
            options.measuredIterations &&
        result.bytecodeVm.completedIterations == options.measuredIterations &&
        result.typedBytecodeVm.completedIterations ==
            options.measuredIterations;
    if (!result.outputsComparable) {
        result.comparisonMessage =
            "benchmark did not complete all requested iterations";
        return result;
    }

    const auto interpreterComparison = compareVariables(
        result.lastInterpreterResult.variables, "HIR interpreter",
        result.lastProfiledBytecodeVmResult.variables,
        "profiled bytecode VM");
    result.interpreterProfiledBytecodeOutputsMatch =
        interpreterComparison.first;
    if (!interpreterComparison.first) {
        result.comparisonMessage = interpreterComparison.second;
        return result;
    }

    const auto steadyComparison = compareVariables(
        result.lastProfiledBytecodeVmResult.variables,
        "profiled bytecode VM", result.lastBytecodeVmResult.variables,
        "profile-off bytecode VM");
    result.profiledSteadyBytecodeOutputsMatch = steadyComparison.first;
    if (!steadyComparison.first) {
        result.comparisonMessage = steadyComparison.second;
        return result;
    }

    const auto typedComparison = compareVariables(
        result.lastBytecodeVmResult.variables, "profile-off bytecode VM",
        result.lastTypedBytecodeVmResult.variables,
        "profile-off typed bytecode VM");
    result.typedBytecodeOutputsMatch = typedComparison.first;
    result.outputsMatch = interpreterComparison.first &&
                          steadyComparison.first && typedComparison.first;
    result.comparisonMessage =
        result.outputsMatch ? "all runtime outputs match"
                            : typedComparison.second;
    return result;
}

} // namespace mparser
