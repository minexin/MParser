#include "mparser/embedding/compiled_module.h"
#include "mparser/runtime/core/session/runtime_call_frame.h"
#include "mparser/execution/runtime_fallback.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void assertNumber(const mparser::RuntimeValue& value, double expected) {
    assert(value.kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value.number - expected) < 1e-9);
}

void assertValid(const mparser::RuntimeValue& value) {
    const auto contract = mparser::validateRuntimeValueContract(value);
    if (!contract.valid) {
        std::cerr << contract.path << ": " << contract.error << "\n";
    }
    assert(contract.valid);
}

void assertInvalid(const mparser::RuntimeValue& value,
                   std::string_view expectedError) {
    const auto contract = mparser::validateRuntimeValueContract(value);
    assert(!contract.valid);
    assert(contract.error.find(expectedError) != std::string::npos);
}

void assertNoDiagnostics(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.message << "\n";
    }
    assert(diagnostics.empty());
}

void runValueFactoryContractSmoke() {
    auto matrix = mparser::makeRuntimeMatrixValue(
        2, 2, {1.0, 2.0, 3.0, 4.0});
    mparser::setRuntimeDimensions(matrix, {2, 1, 2});

    std::vector<mparser::RuntimeValue> values{
        mparser::makeRuntimeMissingValue(),
        mparser::makeRuntimeMissingArrayValue(),
        mparser::makeRuntimeNumberValue(3.0),
        mparser::makeRuntimeLogicalValue(true),
        mparser::makeRuntimeVectorValue({1.0, 2.0}),
        matrix,
        mparser::makeRuntimeCellValue(
            {mparser::makeRuntimeNumberValue(1.0),
             mparser::makeRuntimeCharacterVectorUtf8("value")}),
        mparser::makeRuntimeStructValue(
            {{"field", mparser::makeRuntimeNumberValue(4.0)}}),
        mparser::makeRuntimeCharacterVectorUtf8("text"),
        mparser::makeRuntimeStringScalarUtf8("text"),
    };
    for (const auto& value : values) {
        assertValid(value);
    }

    auto nameValue = mparser::makeRuntimeNameValueArgument(
        "Mode", mparser::makeRuntimeStringScalarUtf8("fast"));
    assertValid(nameValue);
    assert(!mparser::runtimeValueIsStorable(nameValue));

    mparser::RuntimeValue commaList;
    commaList.kind = mparser::RuntimeValueKind::CommaSeparatedList;
    commaList.cells = {mparser::makeRuntimeNumberValue(1.0),
                       mparser::makeRuntimeNumberValue(2.0)};
    mparser::setRuntimeDimensions(commaList, {1, 2});
    assertValid(commaList);
    assert(!mparser::runtimeValueIsStorable(commaList));

    assert(mparser::runtimeValueOwnership(values[0]) ==
           mparser::RuntimeValueOwnership::Transient);
    assert(mparser::runtimeValueOwnership(values[1]) ==
           mparser::RuntimeValueOwnership::Value);
    assert(mparser::runtimeDimensions(values[1]) ==
           std::vector<size_t>({1, 1}));
    assert(mparser::runtimeValueOwnership(values[2]) ==
           mparser::RuntimeValueOwnership::Immediate);
    assert(mparser::runtimeValueOwnership(values[6]) ==
           mparser::RuntimeValueOwnership::Value);
    assert(mparser::runtimeValueOwnershipName(
               mparser::RuntimeValueOwnership::SharedHandle) ==
           "shared-handle");
    assert(mparser::runtimeValueKindName(
               mparser::RuntimeValueKind::FunctionHandle) ==
           "function-handle");
}

void runDynamicMetadataContractSmoke() {
    auto descriptor = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::DynamicProperty, "dynamic-property/1");
    descriptor.opaqueId = 1;
    descriptor.sharedFields = std::make_shared<mparser::RuntimeWorkspace>();
    (*descriptor.sharedFields)["Name"] =
        mparser::makeRuntimeCharacterVectorUtf8("Value");
    assertValid(descriptor);
    assertValid(mparser::makeRuntimeCellValue({descriptor}));

    auto invalid = descriptor;
    invalid.handleObject = false;
    assertInvalid(invalid, "invalid handle identity or shape");
    invalid = descriptor;
    invalid.opaqueId = 0;
    assertInvalid(invalid, "invalid handle identity or shape");
    invalid = descriptor;
    invalid.fields["Name"] = mparser::makeRuntimeNumberValue(1);
    assertInvalid(invalid, "shared field storage");

    auto immutable = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::Class, "Example");
    immutable.sharedFields = descriptor.sharedFields;
    assertInvalid(immutable, "metadata object must not carry property storage");
}

void runCopyAndIdentityContractSmoke() {
    auto cell = mparser::makeRuntimeCellValue(
        {mparser::makeRuntimeStructValue(
            {{"value", mparser::makeRuntimeNumberValue(1.0)}})});
    auto cellCopy = cell;
    cellCopy.cells[0].structElements[0]["value"].number = 9.0;
    assertNumber(cell.cells[0].structElements[0].at("value"), 1.0);
    assertNumber(cellCopy.cells[0].structElements[0].at("value"), 9.0);

    auto valueObject = mparser::makeRuntimeObjectScalar(
        "ValueBox", {{"value", mparser::makeRuntimeNumberValue(2.0)}});
    auto valueObjectCopy = valueObject;
    mparser::runtimeObjectFields(valueObjectCopy)->at("value").number = 7.0;
    assertNumber(
        mparser::runtimeObjectFields(valueObject)->at("value"), 2.0);
    assertNumber(
        mparser::runtimeObjectFields(valueObjectCopy)->at("value"), 7.0);
    assert(mparser::runtimeValueOwnership(valueObject) ==
           mparser::RuntimeValueOwnership::Value);
    assertValid(valueObject);
    assertValid(valueObjectCopy);

    auto handleObject = mparser::makeRuntimeObjectScalar(
        "HandleBox", {{"value", mparser::makeRuntimeNumberValue(3.0)}},
        true);
    auto handleAlias = handleObject;
    assert(handleObject.sharedFields == handleAlias.sharedFields);
    mparser::runtimeObjectFields(handleAlias)->at("value").number = 8.0;
    assertNumber(
        mparser::runtimeObjectFields(handleObject)->at("value"), 8.0);
    assert(mparser::runtimeValueOwnership(handleObject) ==
           mparser::RuntimeValueOwnership::SharedHandle);
    assertValid(handleObject);

    auto cyclicHandle =
        mparser::makeRuntimeObjectScalar("HandleNode", {}, true);
    (*cyclicHandle.sharedFields)["self"] = cyclicHandle;
    assertValid(cyclicHandle);
    cyclicHandle.sharedFields->clear();

    mparser::RuntimeFunctionHandle descriptor;
    descriptor.kind = mparser::RuntimeFunctionHandleKind::Function;
    descriptor.backend =
        mparser::RuntimeFunctionHandleBackend::Independent;
    descriptor.display = "@contractTarget";
    descriptor.targetName = "contractTarget";
    auto functionHandle =
        mparser::makeRuntimeFunctionHandleValue(std::move(descriptor));
    auto functionAlias = functionHandle;

    mparser::RuntimeFunctionHandle secondDescriptor;
    secondDescriptor.kind =
        mparser::RuntimeFunctionHandleKind::Function;
    secondDescriptor.backend =
        mparser::RuntimeFunctionHandleBackend::Independent;
    secondDescriptor.display = "@contractTarget";
    secondDescriptor.targetName = "contractTarget";
    auto secondHandle =
        mparser::makeRuntimeFunctionHandleValue(
            std::move(secondDescriptor));

    assert(functionHandle.functionHandle == functionAlias.functionHandle);
    assert(functionHandle.opaqueId == functionAlias.opaqueId);
    assert(functionHandle.opaqueId != secondHandle.opaqueId);
    assert(mparser::runtimeValueOwnership(functionHandle) ==
           mparser::RuntimeValueOwnership::Callable);
    assert(mparser::runtimeValueIsStorable(functionHandle));
    assertValid(functionHandle);
    assertValid(secondHandle);
}

void runInvalidValueContractSmoke() {
    auto malformedShape =
        mparser::makeRuntimeVectorValue({1.0, 2.0});
    mparser::setRuntimeDimensions(malformedShape, {1, 3});
    assertInvalid(malformedShape, "payload count");

    auto malformedStruct = mparser::makeRuntimeStructValue(
        {{"field", mparser::makeRuntimeNumberValue(1.0)}});
    malformedStruct.fieldOrder.push_back("missing");
    assertInvalid(malformedStruct, "schema");

    mparser::RuntimeValue missingDescriptor;
    missingDescriptor.kind =
        mparser::RuntimeValueKind::FunctionHandle;
    mparser::setRuntimeDimensions(missingDescriptor, {1, 1});
    assertInvalid(missingDescriptor, "descriptor is missing");

    auto malformedHandle =
        mparser::makeRuntimeObjectScalar("HandleBox", {}, true);
    malformedHandle.fields["unexpected"] =
        mparser::makeRuntimeNumberValue(1.0);
    assertInvalid(malformedHandle, "shared field storage");
}

void runCallFrameContractSmoke() {
    auto script = mparser::makeRuntimeScriptFrame(
        {{"input", mparser::makeRuntimeNumberValue(4.0)}});
    assert(script.kind == mparser::RuntimeCallFrameKind::Script);
    assert(script.workspace.count("nargin") == 0);
    assert(mparser::runtimeCallFrameKindName(script.kind) == "script");

    auto function = mparser::makeRuntimeFunctionFrame(
        mparser::RuntimeCallFrameKind::Function, "contractFunction", {},
        2, 3, {{"input", mparser::makeRuntimeNumberValue(4.0)}});
    assert(function.callable == "contractFunction");
    assert(function.suppliedArgumentCount == 2);
    assert(function.requestedOutputCount == 3);
    assertNumber(function.workspace.at("nargin"), 2.0);
    assertNumber(function.workspace.at("nargout"), 3.0);
    assert(mparser::runtimeCallFrameKindName(function.kind) == "function");

    mparser::setRuntimeCallFrameArity(function, 4, 1);
    assertNumber(function.workspace.at("nargin"), 4.0);
    assertNumber(function.workspace.at("nargout"), 1.0);

    auto initializer =
        mparser::makeRuntimeInitializerFrame("ValueBox", {});
    assert(initializer.kind ==
           mparser::RuntimeCallFrameKind::Initializer);
    assert(initializer.workspace.count("nargin") == 0);
    assert(initializer.workspace.count("nargout") == 0);
    assert(mparser::runtimeCallFrameKindName(initializer.kind) ==
           "initializer");
}

void runFallbackContractSmoke() {
    const std::vector<std::pair<mparser::RuntimeFallbackKind,
                                std::string_view>>
        names{
            {mparser::RuntimeFallbackKind::None, "none"},
            {mparser::RuntimeFallbackKind::RegionUnavailable,
             "region-unavailable"},
            {mparser::RuntimeFallbackKind::RegionNotClosed,
             "region-not-closed"},
            {mparser::RuntimeFallbackKind::ContainsCall,
             "contains-call"},
            {mparser::RuntimeFallbackKind::UnsupportedMutation,
             "unsupported-mutation"},
            {mparser::RuntimeFallbackKind::UnsupportedControlFlow,
             "unsupported-control-flow"},
            {mparser::RuntimeFallbackKind::UnsupportedOperation,
             "unsupported-operation"},
            {mparser::RuntimeFallbackKind::InvalidContract,
             "invalid-contract"},
            {mparser::RuntimeFallbackKind::MissingStackValue,
             "missing-stack-value"},
            {mparser::RuntimeFallbackKind::UnsupportedRuntimeValue,
             "unsupported-runtime-value"},
            {mparser::RuntimeFallbackKind::UnsupportedRange,
             "unsupported-range"},
            {mparser::RuntimeFallbackKind::MissingInput,
             "missing-input"},
            {mparser::RuntimeFallbackKind::UnsupportedInput,
             "unsupported-input"},
            {mparser::RuntimeFallbackKind::KernelRejected,
             "kernel-rejected"},
            {mparser::RuntimeFallbackKind::BackendUnavailable,
             "backend-unavailable"},
            {mparser::RuntimeFallbackKind::BackendUnsupported,
             "backend-unsupported"},
            {mparser::RuntimeFallbackKind::CompilationFailed,
             "compilation-failed"},
            {mparser::RuntimeFallbackKind::RuntimeFailed,
             "runtime-failed"},
            {mparser::RuntimeFallbackKind::AdaptiveRetrainingRejected,
             "adaptive-retraining-rejected"},
        };
    for (const auto& [kind, name] : names) {
        assert(mparser::runtimeFallbackKindName(kind) == name);
    }
}

void runCompiledSessionStressSmoke() {
    const auto module = mparser::CompiledModule::compile(R"(
function [count, metadata] = accumulate(step)
persistent total
if isempty(total)
    total = 0;
end
total = total + step;
count = total;
metadata = nargin * 10 + nargout;
end
)");
    assert(module.valid());

    const auto invocationDiagnostics = module.validateInvocation(
        "accumulate", {mparser::makeRuntimeNumberValue(1.0)}, 2);
    assertNoDiagnostics(invocationDiagnostics);

    auto session = module.createSession();
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = "accumulate";
    options.arguments = {mparser::makeRuntimeNumberValue(1.0)};
    options.requestedOutputCount = 2;

    constexpr size_t invocationCount = 2048;
    for (size_t invocation = 0; invocation < invocationCount;
         ++invocation) {
        const auto result = session.invoke(options);
        assertNoDiagnostics(result.diagnostics);
        assert(result.outputs.size() == 2);
        assertValid(result.outputs[0]);
        assertValid(result.outputs[1]);
        assertNumber(result.outputs[0],
                     static_cast<double>(invocation + 1));
        assertNumber(result.outputs[1], 12.0);
    }

    const auto persistent = session.persistentVariables();
    assert(persistent.size() == 1);
    assert(persistent.front().function == "accumulate");
    assert(persistent.front().name == "total");
    assertValid(persistent.front().value);
    assertNumber(persistent.front().value,
                 static_cast<double>(invocationCount));
}

} // namespace

int main() {
    runDynamicMetadataContractSmoke();
    runValueFactoryContractSmoke();
    runCopyAndIdentityContractSmoke();
    runInvalidValueContractSmoke();
    runCallFrameContractSmoke();
    runFallbackContractSmoke();
    runCompiledSessionStressSmoke();
    std::cout << "runtime contract smoke tests passed\n";
    return 0;
}
