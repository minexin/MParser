#include "mparser/machine_protocol.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readGolden(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input),
            "failed to open machine-protocol golden file: " + path);
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

void writeGolden(const std::string& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output),
            "failed to open machine-protocol golden file for writing: " +
                path);
    output << text << '\n';
    require(static_cast<bool>(output),
            "failed to write machine-protocol golden file: " + path);
}

mparser::RuntimeValue makeCommaSeparatedList(
    std::vector<mparser::RuntimeValue> values) {
    mparser::RuntimeValue result;
    result.kind =
        mparser::RuntimeValueKind::CommaSeparatedList;
    result.cells = std::move(values);
    mparser::setRuntimeDimensions(
        result, {1, result.cells.size()});
    return result;
}

mparser::RuntimeValue makeOpaqueObject() {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Object;
    result.className = "DemoObject";
    result.enumerationMemberName = "Ready";
    result.fields.emplace(
        "hidden", mparser::makeRuntimeNumberValue(99));
    mparser::setRuntimeDimensions(result, {1, 1});
    return result;
}

mparser::RuntimeValue makeComplexSingle() {
    using mparser::RuntimeNumericClass;
    using mparser::RuntimeNumericElementValue;
    std::vector<RuntimeNumericElementValue> elements(2);
    elements[0] = {RuntimeNumericClass::Single, 1.0, 2.0, 0, 0,
                   true};
    elements[1] = {RuntimeNumericClass::Single, 3.0, -4.0, 0, 0,
                   true};
    auto result = mparser::runtimeNumericValueFromElements(
        {1, 2}, std::move(elements), RuntimeNumericClass::Single);
    require(result.has_value(),
            "failed to construct complex machine-protocol fixture");
    return std::move(*result);
}

mparser::RuntimeValue makeExactUint64() {
    using mparser::RuntimeNumericClass;
    using mparser::RuntimeNumericElementValue;
    std::vector<RuntimeNumericElementValue> elements(2);
    elements[0].numericClass = RuntimeNumericClass::UInt64;
    elements[0].integerRealBits = 9007199254740993ULL;
    elements[1].numericClass = RuntimeNumericClass::UInt64;
    elements[1].integerRealBits =
        std::numeric_limits<std::uint64_t>::max();
    auto result = mparser::runtimeNumericValueFromElements(
        {1, 2}, std::move(elements), RuntimeNumericClass::UInt64);
    require(result.has_value(),
            "failed to construct uint64 machine-protocol fixture");
    return std::move(*result);
}

mparser::ModuleInvocationResult makeResult() {
    using mparser::RuntimeNumericClass;
    using mparser::RuntimeStringElement;

    mparser::ModuleInvocationResult result;
    result.status =
        mparser::ModuleInvocationStatus::RuntimeFailed;
    result.entryFunction = "entry\n\"quoted\"";
    result.requestedOutputCount = 2;
    result.outputNames = {"primary"};
    result.outputs = {
        mparser::makeRuntimeNumberValue(3.5),
        mparser::makeRuntimeMissingValue(),
    };

    result.variables.push_back({
        "matrix",
        mparser::makeRuntimeMatrixValue(
            2, 2, {1, 2, 3, 4}),
    });
    result.variables.push_back({
        "logical",
        mparser::makeRuntimeMatrixValue(
            2, 2, {0, 1, 1, 0},
            RuntimeNumericClass::Logical),
    });
    result.variables.push_back({
        "nonfinite",
        mparser::makeRuntimeVectorValue(
            {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(), -0.0}),
    });
    result.variables.push_back({"complex_single", makeComplexSingle()});
    result.variables.push_back({"uint64_exact", makeExactUint64()});
    result.variables.push_back({
        "character",
        mparser::makeRuntimeCharacterArray({2, 2}, u"ABCD"),
    });
    result.variables.push_back({
        "string",
        mparser::makeRuntimeStringArray(
            {2, 2},
            {RuntimeStringElement{u"one", false},
             RuntimeStringElement{u"", true},
             RuntimeStringElement{u"\u4e09", false},
             RuntimeStringElement{u"four", false}}),
    });
    result.variables.push_back({
        "cell",
        mparser::makeRuntimeCellValue(
            {2, 2},
            {mparser::makeRuntimeNumberValue(1),
             mparser::makeRuntimeNumberValue(2),
             mparser::makeRuntimeNumberValue(3),
             mparser::makeRuntimeNumberValue(4)}),
    });

    std::vector<mparser::RuntimeStructElement> elements;
    for (int index = 0; index < 4; ++index) {
        elements.push_back({
            {"z", mparser::makeRuntimeNumberValue(10 + index)},
            {"a", mparser::makeRuntimeStringScalarUtf8(
                      "item-" + std::to_string(index))},
        });
    }
    result.variables.push_back({
        "structure",
        mparser::makeRuntimeStructArrayValue(
            {"z", "a"}, std::move(elements), {2, 2}),
    });

    mparser::RuntimeFunctionHandle handle;
    handle.kind = mparser::RuntimeFunctionHandleKind::Method;
    handle.backend =
        mparser::RuntimeFunctionHandleBackend::Bytecode;
    handle.context = mparser::makeRuntimeCallableContext();
    handle.display = "@DemoObject.scale";
    handle.targetName = "scale";
    handle.className = "DemoObject";
    handle.methodName = "scale";
    handle.declaringClass = "BaseObject";
    result.variables.push_back({
        "function",
        mparser::makeRuntimeFunctionHandleValue(std::move(handle)),
    });
    result.variables.push_back({"object", makeOpaqueObject()});
    result.variables.push_back({
        "comma",
        makeCommaSeparatedList(
            {mparser::makeRuntimeNumberValue(7),
             mparser::makeRuntimeStringScalarUtf8("eight")}),
    });
    result.variables.push_back({
        "name_value",
        mparser::makeRuntimeNameValueArgument(
            "Scale", mparser::makeRuntimeNumberValue(4)),
    });
    std::string malformedName = "bad";
    malformedName.push_back(static_cast<char>(0xff));
    result.variables.push_back({
        std::move(malformedName),
        mparser::makeRuntimeNumberValue(5),
    });

    mparser::ModuleDiagnostic diagnostic;
    diagnostic.phase =
        mparser::ModuleDiagnosticPhase::Execution;
    diagnostic.severity =
        mparser::ModuleDiagnosticSeverity::Error;
    diagnostic.identifier = "MParserTest:Failure";
    diagnostic.message = "bad \"value\"\nnext\tline";
    diagnostic.source.available = true;
    diagnostic.source.sourceName = "demo\".m";
    diagnostic.source.begin = {12, 2, 3};
    diagnostic.source.end = {18, 2, 9};
    diagnostic.stack.push_back({"demo.m", "entry", 2});
    mparser::ModuleDiagnosticCause nested;
    nested.identifier = "MParserTest:Nested";
    nested.message = "nested";
    mparser::ModuleDiagnosticCause cause;
    cause.identifier = "MParserTest:Cause";
    cause.message = "cause";
    cause.stack.push_back({"helper.m", "helper", 7});
    cause.causes.push_back(std::move(nested));
    diagnostic.causes.push_back(std::move(cause));
    result.diagnostics.push_back(std::move(diagnostic));

    mparser::ModuleDiagnostic warning;
    warning.phase =
        mparser::ModuleDiagnosticPhase::Validation;
    warning.severity =
        mparser::ModuleDiagnosticSeverity::Warning;
    warning.identifier = "MParserTest:Warning";
    warning.message = "warning";
    result.diagnostics.push_back(std::move(warning));

    auto& execution = result.execution;
    execution.requestedBackend =
        mparser::ModuleExecutionBackend::Native;
    execution.effectiveTier =
        mparser::ModuleExecutionTier::Mixed;
    execution.profilingCollected = true;
    execution.fallbackOccurred = true;
    execution.resourceControlsActive = true;
    execution.optimizedExecutionSuppressed = true;
    execution.stopReason =
        mparser::RuntimeExecutionStopReason::WallTimeLimit;
    execution.executedInstructionCount =
        std::numeric_limits<std::uint64_t>::max();
    execution.typedRegionCount = 2;
    execution.typedRegionAttemptCount = 3;
    execution.typedRegionExecutionCount = 4;
    execution.typedRegionFallbackCount = 5;
    execution.nativeCompilationCount = 6;
    execution.nativeCacheHitCount = 7;
    execution.maximumCallDepth = 8;
    execution.maximumArrayBytes = 9;
    execution.maximumDiagnosticCount = 10;
    execution.elapsedNanoseconds = 11;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3 ||
                    (argc == 4 &&
                     std::string_view(argv[3]) == "--update-golden"),
                "machine_protocol_smoke expects normal and emergency "
                "golden-file paths and optional --update-golden");
        const auto result = makeResult();
        const std::string actual =
            mparser::serializeMachineResultJsonV1(
                result, "0.86.0-test");
        const std::string second =
            mparser::serializeMachineResultJsonV1(
                result, "0.86.0-test");
        require(actual == second,
                "machine protocol must be deterministic");
        if (argc == 4) {
            writeGolden(argv[1], actual);
            writeGolden(argv[2],
                        mparser::machineProtocolEmergencyJsonV1());
            return EXIT_SUCCESS;
        }
        const std::string expected = readGolden(argv[1]);
        if (actual != expected) {
            std::cerr << "machine protocol golden mismatch\n"
                      << "actual:\n"
                      << actual << "\n";
            return EXIT_FAILURE;
        }
        const std::string emergencyExpected = readGolden(argv[2]);
        const std::string emergencyActual(
            mparser::machineProtocolEmergencyJsonV1());
        require(
            emergencyActual == emergencyExpected,
            "machine protocol emergency golden mismatch");

        require(
            mparser::machineResultExitCode(
                mparser::ModuleInvocationStatus::Succeeded) == 0 &&
                mparser::machineResultExitCode(
                    mparser::ModuleInvocationStatus::CompilationFailed) ==
                    1 &&
                mparser::machineResultExitCode(
                    mparser::ModuleInvocationStatus::RequestRejected) ==
                    2 &&
                mparser::machineResultExitCode(
                    mparser::ModuleInvocationStatus::RuntimeFailed) == 3,
            "machine protocol exit codes changed");
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return EXIT_FAILURE;
    }
}
