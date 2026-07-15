#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/adaptive_module_runtime.h"
#include "mparser/bytecode.h"
#include "mparser/bytecode_dump.h"
#include "mparser/bytecode_vm.h"
#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/optimization_plan.h"
#include "mparser/parser.h"
#include "mparser/runtime_benchmark.h"
#include "mparser/runtime_shape.h"
#include "mparser/semantic.h"
#include "mparser/semantic_dump.h"
#include "mparser/source_loader.h"
#include "mparser/syntax_dump.h"
#include "mparser/typed_ir.h"
#include "mparser/token.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MPARSER_VERSION
#define MPARSER_VERSION "0.51.0"
#endif

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open input file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void printUsage() {
    std::cerr << "usage: mparser [--version] "
                 "[--tokens | --hir | --bytecode | --run | --run-bytecode | "
                 "--module-info | "
                 "--profile-bytecode | --plan-bytecode | "
                 "--typed-ir-bytecode | --check-typed-ir-bytecode | "
                 "--run-typed-bytecode | --run-adaptive-bytecode | "
                 "--run-module-runtime | "
                 "--benchmark-runtime] "
                 "[--benchmark-warmup=N] [--benchmark-iterations=N] "
                 "[--typed-backend=auto|portable|native] "
                 "[--adaptive-runs=N] [--adaptive-hot-loop=N] "
                 "[--adaptive-fallback-limit=N] "
                 "[--adaptive-persist-workspace] "
                 "[--adaptive-workspace=name=value] "
                 "[--entry-function=name] [--argument=value] "
                 "[--outputs=N] "
                 "[--module-call=name[:value...]] "
                 "[--path=DIR] [--class-path=DIR] "
                 "<file.m>\n";
}

void printDiagnostics(
    std::string_view header,
    const std::vector<mparser::Diagnostic>& diagnostics,
    const mparser::CompiledModule* module = nullptr,
    std::string_view fallbackSource = {}) {
    if (diagnostics.empty()) {
        return;
    }

    std::cerr << "\n" << header << ":\n";
    for (const auto& diagnostic : diagnostics) {
        std::string_view sourceName = fallbackSource;
        if (module != nullptr) {
            const auto moduleSourceName =
                module->sourceName(diagnostic.span);
            if (!moduleSourceName.empty()) {
                sourceName = moduleSourceName;
            }
        }
        if (!sourceName.empty()) {
            std::cerr << sourceName << ":";
        }
        std::cerr << diagnostic.span.begin.line << ":"
                  << diagnostic.span.begin.column << ": "
                  << diagnostic.message << "\n";
    }
}

size_t parseCountOption(const std::string& argument,
                        std::string_view prefix, bool allowZero) {
    const std::string valueText = argument.substr(prefix.size());
    if (valueText.empty()) {
        throw std::invalid_argument("missing count for option: " +
                                    std::string(prefix));
    }
    if (!std::all_of(valueText.begin(), valueText.end(),
                     [](unsigned char character) {
                         return character >= '0' && character <= '9';
                     })) {
        throw std::invalid_argument("invalid count for option: " + argument);
    }

    size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(valueText, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid count for option: " + argument);
    }
    if (consumed != valueText.size() || (!allowZero && value == 0) ||
        value > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("invalid count for option: " + argument);
    }
    return static_cast<size_t>(value);
}

mparser::TypedRegionBackend parseTypedRegionBackendOption(
    const std::string& argument) {
    constexpr std::string_view prefix = "--typed-backend=";
    const std::string value = argument.substr(prefix.size());
    if (value == "auto") {
        return mparser::TypedRegionBackend::Auto;
    }
    if (value == "portable") {
        return mparser::TypedRegionBackend::Portable;
    }
    if (value == "native") {
        return mparser::TypedRegionBackend::Native;
    }
    throw std::invalid_argument(
        "typed backend must be auto, portable, or native");
}

mparser::RuntimeValue parseRuntimeValue(const std::string& valueText) {
    mparser::RuntimeValue value;
    if (valueText.size() >= 2 && valueText.front() == '[' &&
        valueText.back() == ']') {
        std::string elementsText =
            valueText.substr(1, valueText.size() - 2);
        std::replace(elementsText.begin(), elementsText.end(), ',', ' ');
        std::istringstream input(elementsText);
        double element = 0.0;
        while (input >> element) {
            value.elements.push_back(element);
        }
        if (value.elements.empty() || !input.eof()) {
            throw std::invalid_argument(
                "invalid numeric runtime vector: " + valueText);
        }
        value.kind = mparser::RuntimeValueKind::Vector;
        mparser::setRuntimeDimensions(value, {1, value.elements.size()});
    } else if (valueText.size() >= 2 && valueText.front() == '"' &&
               valueText.back() == '"') {
        value.kind = mparser::RuntimeValueKind::String;
        value.text = valueText.substr(1, valueText.size() - 2);
        mparser::setRuntimeDimensions(value, {1, value.text.size()});
    } else {
        size_t consumed = 0;
        try {
            value.number = std::stod(valueText, &consumed);
        } catch (const std::exception&) {
            throw std::invalid_argument(
                "invalid numeric runtime value: " + valueText);
        }
        if (consumed != valueText.size()) {
            throw std::invalid_argument(
                "invalid numeric runtime value: " + valueText);
        }
        value.kind = mparser::RuntimeValueKind::Number;
        mparser::setRuntimeDimensions(value, {1, 1});
    }
    return value;
}

struct ModuleCall {
    std::string entryFunction;
    std::vector<mparser::RuntimeValue> arguments;
};

ModuleCall parseModuleCallOption(const std::string& argument) {
    constexpr std::string_view prefix = "--module-call=";
    const std::string payload = argument.substr(prefix.size());
    if (payload.empty()) {
        throw std::invalid_argument("module call cannot be empty");
    }

    ModuleCall call;
    size_t begin = 0;
    size_t separator = payload.find(':');
    call.entryFunction = payload.substr(0, separator);
    if (call.entryFunction.empty()) {
        throw std::invalid_argument(
            "module call entry function cannot be empty");
    }

    while (separator != std::string::npos) {
        begin = separator + 1;
        separator = payload.find(':', begin);
        const std::string valueText =
            payload.substr(begin, separator - begin);
        if (valueText.empty()) {
            throw std::invalid_argument(
                "module call argument cannot be empty: " + argument);
        }
        call.arguments.push_back(parseRuntimeValue(valueText));
    }
    return call;
}

mparser::RuntimeVariable parseWorkspaceOption(
    const std::string& argument, std::string_view prefix) {
    const std::string payload = argument.substr(prefix.size());
    const size_t separator = payload.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= payload.size()) {
        throw std::invalid_argument(
            "workspace option must use name=value: " + argument);
    }

    const std::string name = payload.substr(0, separator);
    const bool validName =
        (name.front() == '_' ||
         (name.front() >= 'A' && name.front() <= 'Z') ||
         (name.front() >= 'a' && name.front() <= 'z')) &&
        std::all_of(name.begin() + 1, name.end(), [](char character) {
            return character == '_' ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9');
        });
    if (!validName) {
        throw std::invalid_argument(
            "invalid workspace variable name: " + name);
    }

    auto value = parseRuntimeValue(payload.substr(separator + 1));
    return mparser::RuntimeVariable{name, std::move(value)};
}

void upsertWorkspaceVariable(
    std::vector<mparser::RuntimeVariable>& workspace,
    mparser::RuntimeVariable variable) {
    const auto existing = std::find_if(
        workspace.begin(), workspace.end(), [&](const auto& candidate) {
            return candidate.name == variable.name;
        });
    if (existing == workspace.end()) {
        workspace.push_back(std::move(variable));
    } else {
        *existing = std::move(variable);
    }
}

std::string observationToString(
    const mparser::BytecodeValueObservation& observation) {
    if (observation.observationCount == 0) {
        return "<none>";
    }

    std::ostringstream output;
    if (observation.stable) {
        const std::vector<size_t> dimensions =
            observation.dimensions.empty()
                ? std::vector<size_t>{observation.rows,
                                      observation.columns}
                : observation.dimensions;
        output << observation.kind;
        if (!observation.numericClass.empty()) {
            output << "<" << observation.numericClass << ">";
        }
        output << "(";
        for (size_t index = 0; index < dimensions.size(); ++index) {
            if (index != 0) {
                output << "x";
            }
            output << dimensions[index];
        }
        output << ")";
    } else {
        output << "mixed";
    }
    output << " x" << observation.observationCount;
    return output.str();
}

std::string observationListToString(
    const std::vector<mparser::BytecodeValueObservation>& observations) {
    if (observations.empty()) {
        return "[]";
    }

    std::ostringstream output;
    output << "[";
    for (size_t index = 0; index < observations.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << observationToString(observations[index]);
    }
    output << "]";
    return output.str();
}

std::string stringListToString(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "[]";
    }

    std::ostringstream output;
    output << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << values[index];
    }
    output << "]";
    return output.str();
}

bool runtimeValueEqual(const mparser::RuntimeValue& left,
                       const mparser::RuntimeValue& right) {
    return left.kind == right.kind &&
           left.numericClass == right.numericClass &&
           left.number == right.number &&
           left.text == right.text && left.elements == right.elements &&
           mparser::runtimeDimensions(left) ==
               mparser::runtimeDimensions(right);
}

std::string dimensionListToString(const std::vector<size_t>& dimensions,
                                  size_t rows, size_t columns) {
    const std::vector<size_t> shape =
        dimensions.empty() ? std::vector<size_t>{rows, columns} : dimensions;
    std::ostringstream output;
    for (size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) {
            output << "x";
        }
        output << shape[index];
    }
    return output.str();
}

bool runtimeVariablesEqual(
    const std::vector<mparser::RuntimeVariable>& left,
    const std::vector<mparser::RuntimeVariable>& right,
    const std::vector<std::string>& ignoredNames = {}) {
    if (left.size() != right.size()) {
        return false;
    }
    for (const auto& variable : left) {
        if (std::find(ignoredNames.begin(), ignoredNames.end(),
                      variable.name) != ignoredNames.end()) {
            continue;
        }
        const auto candidate = std::find_if(
            right.begin(), right.end(), [&](const auto& value) {
                return value.name == variable.name;
            });
        if (candidate == right.end() ||
            !runtimeValueEqual(variable.value, candidate->value)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> nondeterministicAssignmentTargets(
    const mparser::BytecodeProgram& program) {
    std::vector<std::string> result;
    for (const auto& instruction : program.instructions) {
        if (!instruction.nondeterministicAssignment ||
            instruction.operand.empty() ||
            std::find(result.begin(), result.end(), instruction.operand) !=
                result.end()) {
            continue;
        }
        result.push_back(instruction.operand);
    }
    return result;
}

void printBytecodeProfile(const mparser::BytecodeVmResult& runtime) {
    std::cout << "Bytecode profile:\n";
    std::cout << "  instructions executed: "
              << runtime.executedInstructionCount << "\n";
    std::cout << "  hot loop threshold: "
              << runtime.profile.hotLoopThreshold << "\n";

    std::cout << "  workspace inputs:\n";
    for (const auto& input : runtime.profile.workspaceInputs) {
        std::cout << "    " << input.name << "="
                  << observationToString(input.valueObservation) << "\n";
    }

    std::cout << "  function entries:\n";
    for (const auto& entry : runtime.profile.functionEntries) {
        std::cout << "    " << entry.name
                  << ": invocations=" << entry.invocationCount
                  << ", args="
                  << observationListToString(entry.argumentObservations)
                  << ", outputs="
                  << observationListToString(entry.resultObservations)
                  << "\n";
    }

    std::cout << "  functions:\n";
    for (const auto& function : runtime.profile.functions) {
        std::cout << "    " << function.name << ": calls="
                  << function.callCount << ", instructions="
                  << function.executedInstructionCount << "\n";
    }

    std::cout << "  loops:\n";
    for (const auto& loop : runtime.profile.loops) {
        const std::string variable =
            loop.variable.empty() ? "<unknown>" : loop.variable;
        std::cout << "    pc=" << loop.headerPc << ", variable=" << variable
                  << ", entries=" << loop.entryCount
                  << ", iterations=" << loop.iterationCount
                  << ", backedges=" << loop.backedgeCount
                  << ", completions=" << loop.completionCount
                  << ", breaks=" << loop.breakCount
                  << ", continues=" << loop.continueCount
                  << ", variableValue="
                  << observationToString(loop.variableObservation)
                  << ", hot=" << (loop.hot ? "yes" : "no") << "\n";
    }

    std::cout << "  call/index sites:\n";
    for (const auto& site : runtime.profile.callSites) {
        const std::string target =
            site.target.empty() ? "<runtime>" : site.target;
        std::cout << "    pc=" << site.pc << ", kind=" << site.kind
                  << ", target=" << target
                  << ", results=" << site.resultCount
                  << ", executions=" << site.executionCount;
        if (site.hasReceiverObservation) {
            std::cout << ", receiver="
                      << observationToString(site.receiverObservation);
        }
        std::cout << ", args="
                  << observationListToString(site.argumentObservations)
                  << ", outputs="
                  << observationListToString(site.resultObservations)
                  << "\n";
    }

    std::cout << "  assignments:\n";
    for (const auto& assignment : runtime.profile.assignments) {
        const std::string target =
            assignment.target.empty() ? "<unknown>" : assignment.target;
        std::cout << "    pc=" << assignment.pc
                  << ", kind=" << assignment.kind
                  << ", target=" << target
                  << ", executions=" << assignment.executionCount
                  << ", value="
                  << observationToString(assignment.valueObservation)
                  << ", inLoop="
                  << (assignment.inLoop ? "yes" : "no");
        if (assignment.inLoop) {
            std::cout << ", loopPc=" << assignment.loopHeaderPc;
        }
        std::cout << "\n";
    }
}

void printBytecodeOptimizationPlan(
    const mparser::BytecodeOptimizationPlan& plan) {
    std::cout << "Bytecode optimization plan:\n";
    std::cout << "  hot loop threshold: " << plan.hotLoopThreshold << "\n";
    std::cout << "  candidates: " << plan.candidates.size() << "\n";
    for (const auto& candidate : plan.candidates) {
        std::cout << "    pc=" << candidate.pc
                  << ", kind=" << candidate.kind
                  << ", target=" << candidate.target
                  << ", executions=" << candidate.executionCount
                  << ", reason=" << candidate.reason << "\n";
        const auto& region = candidate.region;
        std::cout << "      region pc=[" << region.beginPc << ","
                  << region.endPc << "), body=[" << region.bodyBeginPc
                  << "," << region.bodyEndPc << "), closed="
                  << (region.closed ? "yes" : "no")
                  << ", typedEligible="
                  << (region.eligibleForTypedExecution ? "yes" : "no")
                  << ", nestedLoops=" << region.nestedLoopCount
                  << ", maxLoopDepth=" << region.maxLoopDepth
                  << ", stack=" << region.stackInputCount << "->"
                  << region.stackOutputCount << "\n";
        std::cout << "      inputs="
                  << stringListToString(region.inputs)
                  << ", outputs="
                  << stringListToString(region.outputs)
                  << ", writes=" << stringListToString(region.writes)
                  << ", calls="
                  << stringListToString(region.callTargets) << "\n";
        std::cout << "      regionReason=" << region.reason << "\n";
        for (const auto& guard : candidate.guards) {
            std::cout << "      guard source=" << guard.source
                      << ", role=" << guard.role
                      << ", kind=" << guard.kind
                      << ", shape="
                      << dimensionListToString(guard.dimensions, guard.rows,
                                               guard.columns)
                      << ", observations=" << guard.observationCount
                      << "\n";
        }
    }
}

void printBytecodeTypedIr(const mparser::BytecodeTypedIrModule& module) {
    std::cout << "Bytecode typed IR:\n";
    std::cout << "  regions: " << module.regions.size() << "\n";
    for (const auto& region : module.regions) {
        std::cout << "    region=" << region.id
                  << ", kind=" << region.kind
                  << ", sourcePc=" << region.sourcePc
                  << ", target=" << region.target
                  << ", executions=" << region.executionCount << "\n";
        std::cout << "      bytecode pc=[" << region.region.beginPc << ","
                  << region.region.endPc << "), body=["
                  << region.region.bodyBeginPc << ","
                  << region.region.bodyEndPc << "), typedEligible="
                  << (region.region.eligibleForTypedExecution ? "yes" : "no")
                  << ", nestedLoops="
                  << region.region.nestedLoopCount
                  << ", maxLoopDepth=" << region.region.maxLoopDepth
                  << "\n";
        std::cout << "      inputs="
                  << stringListToString(region.region.inputs)
                  << ", outputs="
                  << stringListToString(region.region.outputs)
                  << ", writes="
                  << stringListToString(region.region.writes)
                  << ", calls="
                  << stringListToString(region.region.callTargets) << "\n";
        std::cout << "      regionReason=" << region.region.reason << "\n";
        for (const auto& guard : region.guards) {
            std::cout << "      guard source=" << guard.source
                      << ", role=" << guard.role
                      << ", kind=" << guard.value.kind
                      << ", shape=" << dimensionListToString(
                             guard.value.dimensions, guard.value.rows,
                             guard.value.columns)
                      << ", observations=" << guard.observationCount
                      << "\n";
        }
        for (const auto& operation : region.operations) {
            std::cout << "      op " << operation.opcode;
            if (!operation.operand.empty()) {
                std::cout << " " << operation.operand;
            }
            std::cout << "\n";
        }
    }
}

void printBytecodeTypedIrEvaluation(
    const mparser::BytecodeTypedIrEvaluation& evaluation) {
    std::cout << "Bytecode typed IR guard checks:\n";
    std::cout << "  regions: " << evaluation.regions.size() << "\n";
    for (const auto& region : evaluation.regions) {
        std::cout << "    region=" << region.regionId
                  << ", kind=" << region.kind
                  << ", target=" << region.target
                  << ", checked=" << region.checkedCount
                  << ", passed=" << region.passedCount
                  << ", failed=" << region.failedCount
                  << ", skipped=" << region.skippedCount
                  << ", regionEligible="
                  << (region.regionEligible ? "yes" : "no")
                  << ", enterTypedPath="
                  << (region.canEnterTypedPath ? "yes" : "no") << "\n";
        std::cout << "      regionReason=" << region.regionReason << "\n";
        for (const auto& check : region.checks) {
            std::cout << "      "
                      << (check.checked
                              ? (check.passed ? "pass" : "fail")
                              : "skip")
                      << " source=" << check.source
                      << ", role=" << check.role
                      << ", reason=" << check.reason << "\n";
        }
    }
}

void printBenchmarkStatistics(
    std::string_view name,
    const mparser::RuntimeBenchmarkStatistics& statistics) {
    std::cout << "  " << name
              << ": completed=" << statistics.completedIterations
              << ", totalMs=" << statistics.totalMilliseconds
              << ", meanUs=" << statistics.meanMicroseconds
              << ", medianUs=" << statistics.medianMicroseconds
              << ", minUs=" << statistics.minimumMicroseconds
              << ", maxUs=" << statistics.maximumMicroseconds;
    if (statistics.meanExecutedInstructionCount > 0.0) {
        std::cout << ", instructionsPerRun="
                  << statistics.meanExecutedInstructionCount;
    }
    if (statistics.meanTypedRegionAttemptCount > 0.0 ||
        statistics.meanTypedRegionExecutionCount > 0.0 ||
        statistics.meanTypedRegionFallbackCount > 0.0) {
        std::cout << ", typedAttempts="
                  << statistics.meanTypedRegionAttemptCount
                  << ", typedExecutions="
                  << statistics.meanTypedRegionExecutionCount
                  << ", typedFallbacks="
                  << statistics.meanTypedRegionFallbackCount
                  << ", nestedIterations="
                  << statistics.meanTypedNestedIterationCount
                  << ", typedInstructions="
                  << statistics.meanTypedInstructionCount
                  << ", kernelInstructions="
                  << statistics.meanTypedKernelInstructionCount;
    }
    std::cout << "\n";
}

void printRuntimeBenchmark(
    const mparser::RuntimeBenchmarkResult& benchmark) {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Runtime benchmark (parse and lowering excluded):\n";
    std::cout << "  warmup iterations: "
              << benchmark.options.warmupIterations << "\n";
    std::cout << "  measured iterations: "
              << benchmark.options.measuredIterations << "\n";
    std::cout << "  typed backend: "
              << mparser::typedRegionBackendName(
                     benchmark.options.typedRegionBackend)
              << "\n";
    std::cout << "  typed IR regions: " << benchmark.typedRegionCount
              << "\n";
    printBenchmarkStatistics("HIR interpreter", benchmark.interpreter);
    printBenchmarkStatistics("profiled bytecode VM",
                             benchmark.profiledBytecodeVm);
    printBenchmarkStatistics("profile-off bytecode VM",
                             benchmark.bytecodeVm);
    printBenchmarkStatistics("profile-off typed bytecode VM",
                             benchmark.typedBytecodeVm);
    if (benchmark.profiledBytecodeVm.meanMicroseconds > 0.0) {
        std::cout << "  profiled VM speed relative to interpreter: "
                  << benchmark.interpreter.meanMicroseconds /
                         benchmark.profiledBytecodeVm.meanMicroseconds
                  << "x\n";
    }
    if (benchmark.bytecodeVm.meanMicroseconds > 0.0) {
        std::cout << "  profile-off speed relative to profiled VM: "
                  << benchmark.profiledBytecodeVm.meanMicroseconds /
                         benchmark.bytecodeVm.meanMicroseconds
                  << "x\n";
    }
    if (benchmark.typedBytecodeVm.meanMicroseconds > 0.0) {
        std::cout << "  typed VM speed relative to profile-off VM: "
                  << benchmark.bytecodeVm.meanMicroseconds /
                         benchmark.typedBytecodeVm.meanMicroseconds
                  << "x\n";
    }
    std::cout << "  outputs: "
              << (benchmark.outputsComparable
                      ? (benchmark.outputsMatch ? "match" : "mismatch")
                      : "not comparable")
              << " (" << benchmark.comparisonMessage << ")\n";
}

void printTypedRegionExecutions(
    const mparser::BytecodeVmResult& runtime, bool outputsMatch,
    size_t ignoredOutputCount) {
    std::cout << "Typed region execution:\n";
    std::cout << "  regions: " << runtime.typedRegionExecutions.size()
              << "\n";
    for (const auto& execution : runtime.typedRegionExecutions) {
        std::cout << "    region=" << execution.regionId
                  << ", kind=" << execution.kind
                  << ", sourcePc=" << execution.sourcePc
                  << ", target=" << execution.target
                  << ", eligible="
                  << (execution.eligible ? "yes" : "no")
                  << ", attempts=" << execution.attemptCount
                  << ", executions=" << execution.executionCount
                  << ", fallbacks=" << execution.fallbackCount
                  << ", iterations=" << execution.iterationCount
                  << ", nestedIterations="
                  << execution.nestedIterationCount
                  << ", typedInstructions="
                  << execution.executedInstructionCount
                  << ", kernelInstructions="
                  << execution.executedKernelInstructionCount
                  << ", backend=" << execution.backend;
        if (execution.nativeCompilationCount != 0 ||
            execution.nativeCacheHitCount != 0 ||
            execution.nativeCodeSize != 0) {
            std::cout << ", nativeCompilations="
                      << execution.nativeCompilationCount
                      << ", nativeCacheHits="
                      << execution.nativeCacheHitCount
                      << ", nativeCodeBytes="
                      << execution.nativeCodeSize
                      << ", nativePlatform="
                      << execution.nativePlatform;
        }
        std::cout << "\n";
        if (!execution.nativeFallbackReason.empty()) {
            std::cout << "      nativeFallback="
                      << execution.nativeFallbackReason << "\n";
        }
        std::cout << "      reason=" << execution.lastReason << "\n";
    }
    std::cout << "  baseline outputs: "
              << (outputsMatch ? "match" : "mismatch");
    if (ignoredOutputCount != 0) {
        std::cout << " (excluding " << ignoredOutputCount
                  << " nondeterministic timing assignment"
                  << (ignoredOutputCount == 1 ? "" : "s") << ")";
    }
    std::cout << "\n";
}

void printAdaptiveRun(
    const mparser::AdaptiveBytecodeVmRunResult& result) {
    size_t typedAttempts = 0;
    size_t typedExecutions = 0;
    size_t typedFallbacks = 0;
    size_t nativeCompilations = 0;
    size_t nativeCacheHits = 0;
    size_t nativeCodeBytes = 0;
    std::string typedBackend;
    std::string nativePlatform;
    for (const auto& region : result.runtime.typedRegionExecutions) {
        typedAttempts += region.attemptCount;
        typedExecutions += region.executionCount;
        typedFallbacks += region.fallbackCount;
        nativeCompilations += region.nativeCompilationCount;
        nativeCacheHits += region.nativeCacheHitCount;
        nativeCodeBytes += region.nativeCodeSize;
        if (region.attemptCount != 0 && !region.backend.empty()) {
            if (typedBackend.empty()) {
                typedBackend = region.backend;
            } else if (typedBackend != region.backend) {
                typedBackend = "mixed";
            }
        }
        if (nativePlatform.empty() && !region.nativePlatform.empty()) {
            nativePlatform = region.nativePlatform;
        }
    }

    std::cout << "  invocation=" << result.invocation
              << ", tier=" << mparser::adaptiveBytecodeTierName(result.tier)
              << ", profile="
              << (result.runtime.profile.collected ? "full" : "off")
              << ", dispatched="
              << result.runtime.executedInstructionCount
              << ", accumulatedLoopIterations="
              << result.accumulatedLoopIterations
              << ", hotLoops=" << result.hotLoopCount
              << ", promoted="
              << (result.promotionOccurred ? "yes" : "no")
              << ", invalidated="
              << (result.invalidationOccurred ? "yes" : "no")
              << ", installedRegions=" << result.installedRegionCount
              << ", executableRegions=" << result.executableRegionCount
              << ", promotions=" << result.promotionCount
              << ", invalidations=" << result.invalidationCount;
    if (!result.runtime.typedRegionExecutions.empty()) {
        std::cout << ", typedAttempts=" << typedAttempts
                  << ", typedExecutions=" << typedExecutions
                  << ", typedFallbacks=" << typedFallbacks;
        if (!typedBackend.empty()) {
            std::cout << ", backend=" << typedBackend;
        }
        if (nativeCompilations != 0 || nativeCacheHits != 0 ||
            nativeCodeBytes != 0) {
            std::cout << ", nativeCompilations=" << nativeCompilations
                      << ", nativeCacheHits=" << nativeCacheHits
                      << ", nativeCodeBytes=" << nativeCodeBytes;
            if (!nativePlatform.empty()) {
                std::cout << ", nativePlatform=" << nativePlatform;
            }
        }
    }
    std::cout << "\n";
}

void printAdaptiveEvents(
    const std::vector<mparser::AdaptiveBytecodeEvent>& events) {
    std::cout << "Adaptive tiering events:\n";
    for (const auto& event : events) {
        std::cout << "  invocation=" << event.invocation
                  << ", kind="
                  << mparser::adaptiveBytecodeEventKindName(event.kind)
                  << ", region=" << event.regionId
                  << ", sourcePc=" << event.sourcePc
                  << ", target=" << event.target
                  << ", reason=" << event.reason << "\n";
    }
}

void printFunctionOutputs(const mparser::BytecodeVmResult& runtime) {
    if (runtime.entryFunction.empty()) {
        return;
    }
    std::cout << "Function outputs (" << runtime.entryFunction << "):\n";
    for (size_t index = 0; index < runtime.outputs.size(); ++index) {
        const std::string name =
            index < runtime.outputNames.size()
                ? runtime.outputNames[index]
                : "output" + std::to_string(index + 1);
        std::cout << "  " << name << " = "
                  << mparser::runtimeValueToString(runtime.outputs[index])
                  << "\n";
    }
}

void printNameList(const std::vector<std::string>& names,
                   std::string_view open, std::string_view close) {
    std::cout << open;
    for (size_t index = 0; index < names.size(); ++index) {
        if (index > 0) {
            std::cout << ", ";
        }
        std::cout << names[index];
    }
    std::cout << close;
}

void printCompiledModuleInfo(const mparser::CompiledModule& module) {
    size_t sourceBytes = 0;
    for (const auto& source : module.sources()) {
        sourceBytes += source.content.size();
    }

    std::cout << "Compiled module:\n"
              << "  status: " << (module.valid() ? "valid" : "invalid")
              << "\n"
              << "  source files: " << module.sources().size() << "\n"
              << "  source bytes: " << sourceBytes << "\n"
              << "  bytecode instructions: "
              << module.bytecode().instructions.size() << "\n"
              << "  invocable functions: " << module.functions().size()
              << "\n";

    std::cout << "Sources:\n";
    for (size_t sourceId = 0; sourceId < module.sources().size();
         ++sourceId) {
        const auto& source = module.sources()[sourceId];
        std::cout << "  [" << sourceId << "] " << source.name << " ("
                  << source.content.size() << " bytes)";
        if (!source.namespaceName.empty()) {
            std::cout << " namespace=" << source.namespaceName;
        }
        if (!source.primaryFunctionIdentity.empty()) {
            std::cout << " function="
                      << source.primaryFunctionIdentity;
        }
        if (!source.classMethodOwner.empty()) {
            std::cout << " method-of=" << source.classMethodOwner;
        }
        if (!source.classPrivateFunctionOwner.empty()) {
            std::cout << " private-of="
                      << source.classPrivateFunctionOwner;
        }
        std::cout << "\n";
        for (const auto& binding : source.functionBindings) {
            std::cout << "      bind " << binding.alias << " -> "
                      << binding.target << "\n";
        }
    }

    if (module.functions().empty()) {
        return;
    }

    std::cout << "Functions:\n";
    for (const auto& function : module.functions()) {
        std::cout << "  " << function.name;
        printNameList(function.signature.parameters, "(", ")");
        std::cout << " -> ";
        printNameList(function.signature.outputs, "[", "]");
        const auto sourceName = module.sourceName(function.span);
        std::cout << " @ ";
        if (!sourceName.empty()) {
            std::cout << sourceName << ":";
        }
        std::cout << function.span.begin.line << ":"
                  << function.span.begin.column << "\n";
    }
}

void printAdaptiveModuleState(
    const mparser::AdaptiveModuleFunctionState& state) {
    std::cout << "  " << state.entryFunction
              << ": invocations=" << state.invocationCount
              << ", installedTier="
              << mparser::adaptiveBytecodeTierName(state.installedTier)
              << ", promotions=" << state.promotionCount
              << ", typedExecutions=" << state.typedExecutionCount
              << ", typedFallbacks=" << state.typedFallbackCount
              << ", invalidations=" << state.invalidationCount
              << ", events=" << state.eventCount << "\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool printTokens = false;
        bool printHir = false;
        bool printBytecode = false;
        bool printModuleInfo = false;
        bool runProgram = false;
        bool runBytecode = false;
        bool profileBytecode = false;
        bool planBytecode = false;
        bool typedIrBytecode = false;
        bool checkTypedIrBytecode = false;
        bool runTypedBytecode = false;
        bool runAdaptiveBytecode = false;
        bool runModuleRuntime = false;
        bool benchmarkRuntime = false;
        size_t benchmarkWarmup = 3;
        size_t benchmarkIterations = 20;
        size_t adaptiveRuns = 3;
        size_t adaptiveHotLoopThreshold = 10;
        size_t adaptiveFallbackLimit = 3;
        mparser::TypedRegionBackend typedRegionBackend =
            mparser::TypedRegionBackend::Auto;
        bool adaptivePersistWorkspace = false;
        std::vector<mparser::RuntimeVariable> adaptiveInitialWorkspace;
        std::string entryFunction;
        std::vector<mparser::RuntimeValue> entryArguments;
        std::optional<size_t> requestedOutputCount;
        std::vector<ModuleCall> moduleCalls;
        std::vector<std::filesystem::path> searchPaths;
        std::string path;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--version") {
                std::cout << "MParser " << MPARSER_VERSION << "\n";
                return 0;
            } else if (arg == "--tokens") {
                printTokens = true;
            } else if (arg == "--hir") {
                printHir = true;
            } else if (arg == "--bytecode") {
                printBytecode = true;
            } else if (arg == "--module-info") {
                printModuleInfo = true;
            } else if (arg == "--run") {
                runProgram = true;
            } else if (arg == "--run-bytecode") {
                runBytecode = true;
            } else if (arg == "--profile-bytecode") {
                profileBytecode = true;
            } else if (arg == "--plan-bytecode") {
                planBytecode = true;
            } else if (arg == "--typed-ir-bytecode") {
                typedIrBytecode = true;
            } else if (arg == "--check-typed-ir-bytecode") {
                checkTypedIrBytecode = true;
            } else if (arg == "--run-typed-bytecode") {
                runTypedBytecode = true;
            } else if (arg == "--run-adaptive-bytecode") {
                runAdaptiveBytecode = true;
            } else if (arg == "--run-module-runtime") {
                runModuleRuntime = true;
            } else if (arg == "--benchmark-runtime") {
                benchmarkRuntime = true;
            } else if (arg.starts_with("--benchmark-warmup=")) {
                benchmarkWarmup = parseCountOption(
                    arg, "--benchmark-warmup=", true);
            } else if (arg.starts_with("--benchmark-iterations=")) {
                benchmarkIterations = parseCountOption(
                    arg, "--benchmark-iterations=", false);
            } else if (arg.starts_with("--typed-backend=")) {
                typedRegionBackend =
                    parseTypedRegionBackendOption(arg);
            } else if (arg.starts_with("--adaptive-runs=")) {
                adaptiveRuns = parseCountOption(
                    arg, "--adaptive-runs=", false);
            } else if (arg.starts_with("--adaptive-hot-loop=")) {
                adaptiveHotLoopThreshold = parseCountOption(
                    arg, "--adaptive-hot-loop=", false);
            } else if (arg.starts_with("--adaptive-fallback-limit=")) {
                adaptiveFallbackLimit = parseCountOption(
                    arg, "--adaptive-fallback-limit=", false);
            } else if (arg == "--adaptive-persist-workspace") {
                adaptivePersistWorkspace = true;
            } else if (arg.starts_with("--adaptive-workspace=")) {
                upsertWorkspaceVariable(
                    adaptiveInitialWorkspace,
                    parseWorkspaceOption(arg, "--adaptive-workspace="));
            } else if (arg.starts_with("--entry-function=")) {
                entryFunction =
                    arg.substr(std::string("--entry-function=").size());
                if (entryFunction.empty()) {
                    throw std::invalid_argument(
                        "entry function name cannot be empty");
                }
            } else if (arg.starts_with("--argument=")) {
                entryArguments.push_back(parseRuntimeValue(
                    arg.substr(std::string("--argument=").size())));
            } else if (arg.starts_with("--outputs=")) {
                requestedOutputCount =
                    parseCountOption(arg, "--outputs=", true);
            } else if (arg.starts_with("--module-call=")) {
                moduleCalls.push_back(parseModuleCallOption(arg));
            } else if (arg.starts_with("--path=") ||
                       arg.starts_with("--class-path=")) {
                const size_t prefixLength =
                    arg.starts_with("--path=")
                        ? std::string("--path=").size()
                        : std::string("--class-path=").size();
                const auto searchPath = arg.substr(prefixLength);
                if (searchPath.empty()) {
                    throw std::invalid_argument(
                        "search path cannot be empty");
                }
                searchPaths.emplace_back(searchPath);
            } else if (path.empty()) {
                path = arg;
            } else {
                printUsage();
                return 2;
            }
        }

        if (path.empty()) {
            printUsage();
            return 2;
        }

        const auto compileSourceGraph = [&]() {
            mparser::SourceLoaderOptions loaderOptions;
            loaderOptions.searchPaths = searchPaths;
            mparser::SourceLoader loader;
            auto loaded = loader.load(path, loaderOptions);
            return mparser::CompiledModule::compile(
                std::move(loaded.sources));
        };

        if (printModuleInfo) {
            const auto module = compileSourceGraph();
            printCompiledModuleInfo(module);
            if (!module.diagnostics().empty()) {
                printDiagnostics("Module diagnostics",
                                 module.diagnostics(), &module, path);
                return 1;
            }

            const auto validation = module.validateInvocation(
                entryFunction, entryArguments.size(), requestedOutputCount);
            if (!entryFunction.empty()) {
                std::cout << "Entry validation:\n"
                          << "  function: " << entryFunction << "\n"
                          << "  arguments: " << entryArguments.size()
                          << "\n"
                          << "  status: "
                          << (validation.empty() ? "valid" : "invalid")
                          << "\n";
            }
            if (!validation.empty()) {
                printDiagnostics("Entry diagnostics", validation,
                                 &module, path);
                return 1;
            }
            return 0;
        }

        if (runModuleRuntime) {
            const auto module = compileSourceGraph();
            if (!module.valid()) {
                printDiagnostics("Module diagnostics",
                                 module.diagnostics(), &module, path);
                return 1;
            }
            if (moduleCalls.empty()) {
                throw std::invalid_argument(
                    "--run-module-runtime requires at least one "
                    "--module-call");
            }

            mparser::AdaptiveModuleRuntimeOptions runtimeOptions;
            runtimeOptions.hotLoopThreshold = adaptiveHotLoopThreshold;
            runtimeOptions.fallbackInvalidationThreshold =
                adaptiveFallbackLimit;
            runtimeOptions.preserveWorkspace = adaptivePersistWorkspace;
            runtimeOptions.initialWorkspace = adaptiveInitialWorkspace;
            runtimeOptions.typedRegionBackend = typedRegionBackend;
            mparser::AdaptiveModuleRuntime runtime(module, runtimeOptions);

            std::cout << "Adaptive module runtime:\n";
            for (size_t index = 0; index < moduleCalls.size(); ++index) {
                auto result = runtime.invoke(
                    moduleCalls[index].entryFunction,
                    moduleCalls[index].arguments, requestedOutputCount);
                std::cout << "Call " << index + 1 << ": entry="
                          << result.entryFunction
                          << ", session="
                          << (result.sessionCreated ? "created" : "reused")
                          << "\n";
                printAdaptiveRun(result.adaptive);
                if (!result.adaptive.runtime.diagnostics.empty()) {
                    printDiagnostics(
                        "Module runtime diagnostics",
                        result.adaptive.runtime.diagnostics, &module,
                        path);
                    return 1;
                }
                printFunctionOutputs(result.adaptive.runtime);
            }

            std::cout << "Function tier states:\n";
            for (const auto& state : runtime.functionStates()) {
                printAdaptiveModuleState(state);
            }
            return 0;
        }

        const std::string source = readFile(path);
        mparser::Lexer lexer(source);
        auto tokens = lexer.lex();

        if (printTokens) {
            for (const auto& token : tokens) {
                std::cout << mparser::tokenKindName(token.kind) << " '"
                          << token.text << "' @ " << token.span.begin.line
                          << ":" << token.span.begin.column << "\n";
            }
            return 0;
        }

        if (printHir || printBytecode || runProgram || runBytecode ||
            profileBytecode || planBytecode || typedIrBytecode ||
            checkTypedIrBytecode || runTypedBytecode ||
            runAdaptiveBytecode || benchmarkRuntime) {
            const auto module = compileSourceGraph();
            if (!module.valid()) {
                printDiagnostics("Diagnostics", module.diagnostics(),
                                 &module, path);
                return 1;
            }

            const auto& semantic = module.semantic();
            const auto& bytecode = module.bytecode();
            if (printHir) {
                mparser::dumpSemanticTree(std::cout, semantic);
            } else if (printBytecode) {
                mparser::dumpBytecode(std::cout, bytecode, semantic);
            } else if (runBytecode || profileBytecode || planBytecode ||
                       typedIrBytecode || checkTypedIrBytecode ||
                       runTypedBytecode || runAdaptiveBytecode ||
                       benchmarkRuntime) {
                if (runAdaptiveBytecode) {
                    mparser::AdaptiveBytecodeVmOptions adaptiveOptions;
                    adaptiveOptions.hotLoopThreshold =
                        adaptiveHotLoopThreshold;
                    adaptiveOptions.fallbackInvalidationThreshold =
                        adaptiveFallbackLimit;
                    adaptiveOptions.preserveWorkspace =
                        adaptivePersistWorkspace;
                    adaptiveOptions.initialWorkspace =
                        adaptiveInitialWorkspace;
                    adaptiveOptions.entryFunction = entryFunction;
                    adaptiveOptions.arguments = entryArguments;
                    adaptiveOptions.requestedOutputCount =
                        requestedOutputCount;
                    adaptiveOptions.typedRegionBackend =
                        typedRegionBackend;
                    mparser::AdaptiveBytecodeVmSession session(
                        bytecode, semantic, adaptiveOptions);
                    mparser::BytecodeVmResult lastRuntime;
                    std::cout << "Adaptive bytecode session:\n";
                    for (size_t invocation = 0; invocation < adaptiveRuns;
                         ++invocation) {
                        auto adaptive = session.run();
                        printAdaptiveRun(adaptive);
                        if (!adaptive.runtime.diagnostics.empty()) {
                            printDiagnostics(
                                "Adaptive bytecode VM diagnostics",
                                adaptive.runtime.diagnostics, &module,
                                path);
                            return 1;
                        }
                        lastRuntime = std::move(adaptive.runtime);
                    }
                    printAdaptiveEvents(session.events());
                    std::cout << "Variables:\n";
                    for (const auto& variable : lastRuntime.variables) {
                        std::cout << "  " << variable.name << " = "
                                  << mparser::runtimeValueToString(
                                         variable.value)
                                  << "\n";
                    }
                    printFunctionOutputs(lastRuntime);
                    return semantic.diagnostics.empty() ? 0 : 1;
                }
                if (benchmarkRuntime) {
                    mparser::RuntimeBenchmarkRunner benchmarkRunner;
                    const auto benchmark = benchmarkRunner.run(
                        semantic, bytecode,
                        mparser::RuntimeBenchmarkOptions{
                            benchmarkWarmup, benchmarkIterations,
                            typedRegionBackend});
                    printRuntimeBenchmark(benchmark);
                    if (!benchmark.lastInterpreterResult.diagnostics.empty()) {
                        printDiagnostics(
                            "Interpreter diagnostics",
                            benchmark.lastInterpreterResult.diagnostics,
                            &module, path);
                    }
                    if (!benchmark.lastProfiledBytecodeVmResult.diagnostics
                             .empty()) {
                        printDiagnostics(
                            "Profiled bytecode VM diagnostics",
                            benchmark.lastProfiledBytecodeVmResult
                                .diagnostics,
                            &module, path);
                    }
                    if (!benchmark.lastBytecodeVmResult.diagnostics.empty()) {
                        printDiagnostics(
                            "Profile-off bytecode VM diagnostics",
                            benchmark.lastBytecodeVmResult.diagnostics,
                            &module, path);
                    }
                    if (!benchmark.lastTypedBytecodeVmResult.diagnostics
                             .empty()) {
                        printDiagnostics(
                            "Profile-off typed bytecode VM diagnostics",
                            benchmark.lastTypedBytecodeVmResult.diagnostics,
                            &module, path);
                    }
                    return semantic.diagnostics.empty() &&
                                   benchmark.outputsComparable &&
                                   benchmark.outputsMatch
                               ? 0
                               : 1;
                }
                mparser::BytecodeVm vm;
                mparser::BytecodeVmOptions vmOptions;
                vmOptions.entryFunction = entryFunction;
                vmOptions.arguments = entryArguments;
                vmOptions.requestedOutputCount = requestedOutputCount;
                vmOptions.typedRegionBackend = typedRegionBackend;
                const bool needsProfile =
                    profileBytecode || planBytecode || typedIrBytecode ||
                    checkTypedIrBytecode || runTypedBytecode;
                if (!needsProfile) {
                    vmOptions.profiling =
                        mparser::BytecodeVmProfilingMode::Disabled;
                }
                auto runtime = vm.run(bytecode, semantic, vmOptions);
                std::vector<mparser::RuntimeVariable> baselineVariables;
                std::vector<std::string> nondeterministicTargets;
                bool typedOutputsMatch = true;
                if (runTypedBytecode && runtime.diagnostics.empty()) {
                    baselineVariables = runtime.variables;
                    nondeterministicTargets =
                        nondeterministicAssignmentTargets(bytecode);
                    mparser::BytecodeOptimizationPlanner planner;
                    mparser::BytecodeTypedIrBuilder builder;
                    const auto module = builder.build(
                        planner.plan(runtime.profile, bytecode));
                    mparser::BytecodeVm typedVm;
                    mparser::BytecodeVmOptions steadyOptions = vmOptions;
                    steadyOptions.profiling =
                        mparser::BytecodeVmProfilingMode::Disabled;
                    auto typedRuntime = typedVm.run(
                        bytecode, semantic, module, steadyOptions);
                    runtime = std::move(typedRuntime);
                    typedOutputsMatch = runtimeVariablesEqual(
                        baselineVariables, runtime.variables,
                        nondeterministicTargets);
                }
                std::cout << "Variables:\n";
                for (const auto& variable : runtime.variables) {
                    std::cout << "  " << variable.name << " = "
                              << mparser::runtimeValueToString(variable.value)
                              << "\n";
                }
                printFunctionOutputs(runtime);
                if (!runtime.diagnostics.empty()) {
                    printDiagnostics("Bytecode VM diagnostics",
                                     runtime.diagnostics, &module, path);
                    return 1;
                }
                if (profileBytecode) {
                    printBytecodeProfile(runtime);
                }
                if (planBytecode) {
                    mparser::BytecodeOptimizationPlanner planner;
                    printBytecodeOptimizationPlan(
                        planner.plan(runtime.profile, bytecode));
                }
                if (typedIrBytecode) {
                    mparser::BytecodeOptimizationPlanner planner;
                    mparser::BytecodeTypedIrBuilder builder;
                    printBytecodeTypedIr(
                        builder.build(
                            planner.plan(runtime.profile, bytecode)));
                }
                if (checkTypedIrBytecode) {
                    mparser::BytecodeOptimizationPlanner planner;
                    mparser::BytecodeTypedIrBuilder builder;
                    mparser::BytecodeTypedIrGuardEvaluator evaluator;
                    const auto typedModule =
                        builder.build(
                            planner.plan(runtime.profile, bytecode));
                    printBytecodeTypedIrEvaluation(
                        evaluator.evaluate(typedModule,
                                           runtime.variables));
                }
                if (runTypedBytecode) {
                    printTypedRegionExecutions(
                        runtime, typedOutputsMatch,
                        nondeterministicTargets.size());
                    if (!typedOutputsMatch) {
                        return 1;
                    }
                }
            } else {
                mparser::Interpreter interpreter;
                const auto runtime = interpreter.run(semantic);
                std::cout << "Variables:\n";
                for (const auto& variable : runtime.variables) {
                    std::cout << "  " << variable.name << " = "
                              << mparser::runtimeValueToString(variable.value)
                              << "\n";
                }
                if (!runtime.diagnostics.empty()) {
                    printDiagnostics("Runtime diagnostics",
                                     runtime.diagnostics, &module, path);
                    return 1;
                }
            }
            return semantic.diagnostics.empty() ? 0 : 1;
        }

        mparser::Parser parser(tokens);
        auto result = parser.parse();
        mparser::dumpSyntaxTree(std::cout, *result.root);

        if (!result.diagnostics.empty()) {
            printDiagnostics("Diagnostics", result.diagnostics,
                             nullptr, path);
            return 1;
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
