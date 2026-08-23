#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_value.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

mparser::RuntimeValue number(double value) {
    return mparser::makeRuntimeNumberValue(value);
}

const mparser::ModuleDiagnostic* findDiagnostic(
    const mparser::ModuleInvocationResult& result,
    std::string_view identifier) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier == identifier) {
            return &diagnostic;
        }
    }
    return nullptr;
}

void requireStop(
    const mparser::ModuleInvocationResult& result,
    mparser::RuntimeExecutionStopReason reason,
    std::string_view identifier) {
    require(result.status ==
                mparser::ModuleInvocationStatus::RuntimeFailed,
            "resource stop did not fail the invocation");
    require(result.execution.stopReason == reason,
            "resource stop reason is incorrect");
    require(findDiagnostic(result, identifier) != nullptr,
            "resource stop diagnostic identifier is missing");
}

mparser::ModuleInvocationRequest functionRequest(
    std::string entry,
    std::vector<mparser::RuntimeValue> arguments = {}) {
    mparser::ModuleInvocationRequest request;
    request.entryFunction = std::move(entry);
    request.arguments = std::move(arguments);
    request.requestedOutputCount = 1;
    request.backend =
        mparser::ModuleExecutionBackend::Automatic;
    return request;
}

const std::string kResourceSource = R"(
function total = sumTo(limit)
total = 0;
for i = 1:limit
    total = total + i;
end
end

function out = spin()
out = 0;
while 1
    out = out + 1;
end
end

function out = guardedSpin()
out = 0;
try
    while 1
        out = out + 1;
    end
catch
    out = -1;
end
end

function out = descend(value)
if value <= 0
    out = 0;
else
    out = 1 + descend(value - 1);
end
end

function out = makeLargeArray()
out = zeros(1, 64);
end

function out = warnMany()
out = 0;
for i = 1:20
    warning("Resource:Notice", "notice");
    out = out + 1;
end
end

function out = identity(value)
out = value;
end
)";

void runArrayByteAccountingSmoke() {
    const auto matrix =
        mparser::makeRuntimeMatrixValue(
            2, 3, {1, 2, 3, 4, 5, 6});
    const auto matrixBytes =
        mparser::runtimeValueArrayBytes(matrix);
    require(matrixBytes && *matrixBytes == 6 * sizeof(double),
            "numeric array byte accounting is incorrect");

    const auto cell = mparser::makeRuntimeCellValue(
        {matrix, number(7)});
    const auto cellBytes =
        mparser::runtimeValueArrayBytes(cell);
    require(cellBytes &&
                *cellBytes == 7 * sizeof(double),
            "recursive cell byte accounting is incorrect");
}

void runInstructionAndCatchSmoke(
    const mparser::CompiledModule& module) {
    auto request = functionRequest("spin");
    request.limits.maxInstructionCount = 64;
    const auto result = module.execute(request);
    requireStop(
        result,
        mparser::RuntimeExecutionStopReason::InstructionLimit,
        "MParser:InstructionLimitExceeded");
    require(result.execution.executedInstructionCount == 64,
            "instruction limit was not exact");
    require(result.execution.resourceControlsActive,
            "resource controls were not reported as active");
    require(result.execution.optimizedExecutionSuppressed,
            "strict instruction control did not suppress optimized regions");
    require(result.execution.effectiveTier ==
                mparser::ModuleExecutionTier::Bytecode,
            "strict instruction control did not use bytecode");
    require(result.execution.fallbackOccurred,
            "policy fallback was not reported");

    request = functionRequest("guardedSpin");
    request.limits.maxInstructionCount = 64;
    const auto guarded = module.execute(request);
    requireStop(
        guarded,
        mparser::RuntimeExecutionStopReason::InstructionLimit,
        "MParser:InstructionLimitExceeded");
    require(guarded.outputs.empty() ||
                guarded.outputs.front().kind ==
                    mparser::RuntimeValueKind::Missing ||
                std::fabs(guarded.outputs.front().number + 1.0) >
                    1e-9,
            "language try/catch intercepted a host resource stop");
}

void runCancellationAndDeadlineSmoke(
    const mparser::CompiledModule& module) {
    mparser::RuntimeCancellationToken cancellation;
    cancellation.requestCancellation();
    auto cancelled = functionRequest("identity", {number(42)});
    cancelled.cancellationToken = cancellation;
    const auto cancelledResult = module.execute(cancelled);
    requireStop(
        cancelledResult,
        mparser::RuntimeExecutionStopReason::Cancelled,
        "MParser:ExecutionCancelled");
    require(cancelledResult.execution.executedInstructionCount == 0,
            "pre-cancelled invocation executed bytecode");

    mparser::RuntimeCancellationToken liveCancellation;
    auto live = functionRequest("spin");
    live.cancellationToken = liveCancellation;
    live.limits.maxWallTime = std::chrono::seconds(2);
    mparser::ModuleInvocationResult liveResult;
    std::thread worker([&]() {
        liveResult = module.execute(live);
    });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(5));
    liveCancellation.requestCancellation();
    worker.join();
    requireStop(
        liveResult,
        mparser::RuntimeExecutionStopReason::Cancelled,
        "MParser:ExecutionCancelled");

    auto deadline = functionRequest("spin");
    deadline.limits.maxWallTime =
        std::chrono::nanoseconds(1);
    const auto deadlineResult = module.execute(deadline);
    requireStop(
        deadlineResult,
        mparser::RuntimeExecutionStopReason::WallTimeLimit,
        "MParser:WallTimeLimitExceeded");
    require(deadlineResult.execution.optimizedExecutionSuppressed,
            "deadline did not suppress optimized execution");
}

void runCallDepthSmoke(const mparser::CompiledModule& module) {
    auto request = functionRequest("descend", {number(20)});
    request.limits.maxCallDepth = 4;
    const auto result = module.execute(request);
    requireStop(
        result,
        mparser::RuntimeExecutionStopReason::CallDepthLimit,
        "MParser:CallDepthLimitExceeded");
    require(result.execution.maximumCallDepth == 4,
            "maximum call depth is not exact");
    require(!result.execution.optimizedExecutionSuppressed,
            "call-depth-only control unnecessarily suppressed typed regions");
}

void runArrayLimitSmoke(const mparser::CompiledModule& module) {
    auto request = functionRequest("makeLargeArray");
    request.limits.maxArrayBytes = 128;
    const auto result = module.execute(request);
    requireStop(
        result,
        mparser::RuntimeExecutionStopReason::ArrayByteLimit,
        "MParser:ArrayByteLimitExceeded");
    require(result.execution.maximumArrayBytes >=
                64 * sizeof(double),
            "large array payload was not observed");

    request = functionRequest(
        "identity",
        {mparser::makeRuntimeMatrixValue(
            1, 32, std::vector<double>(32, 1.0))});
    request.limits.maxArrayBytes = 128;
    const auto rejected = module.execute(request);
    require(rejected.status ==
                mparser::ModuleInvocationStatus::RequestRejected,
            "oversized host argument was not rejected");
    require(findDiagnostic(
                rejected,
                "MParser:ArrayByteLimitExceeded") != nullptr,
            "oversized host argument diagnostic is missing");
}

void runDiagnosticLimitSmoke(
    const mparser::CompiledModule& module) {
    auto request = functionRequest("warnMany");
    request.backend =
        mparser::ModuleExecutionBackend::Bytecode;
    request.limits.maxDiagnosticCount = 3;
    const auto result = module.execute(request);
    requireStop(
        result,
        mparser::RuntimeExecutionStopReason::DiagnosticLimit,
        "MParser:DiagnosticLimitExceeded");
    require(result.execution.maximumDiagnosticCount == 4,
            "diagnostic high-water mark is incorrect");
    require(result.diagnostics.size() == 4,
            "retained diagnostics plus terminal diagnostic are incorrect");
    size_t warningCount = 0;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier == "Resource:Notice") {
            ++warningCount;
        }
    }
    require(warningCount == 3,
            "diagnostic limit did not retain the configured warning count");
}

void runOptimizedControlSmoke(
    const mparser::CompiledModule& module) {
    auto request = functionRequest("sumTo", {number(100)});
    request.limits.maxCallDepth = 8;
    request.limits.maxArrayBytes = 1024;
    request.limits.maxDiagnosticCount = 8;
    const auto result = module.execute(request);
    require(result.succeeded(),
            "non-checkpoint controls broke optimized execution");
    require(!result.execution.optimizedExecutionSuppressed,
            "non-checkpoint controls suppressed optimized execution");
    require(result.execution.effectiveTier ==
                (mparser::nativeScalarJitAvailable()
                     ? mparser::ModuleExecutionTier::Native
                     : mparser::ModuleExecutionTier::Portable),
            "optimized execution used an unexpected tier");
    require(result.outputs.size() == 1 &&
                std::fabs(result.outputs.front().number - 5050.0) <
                    1e-9,
            "optimized controlled result is incorrect");
}

void runValidationAndSessionSmoke(
    const mparser::CompiledModule& module) {
    auto invalid = functionRequest("identity", {number(1)});
    invalid.limits.maxWallTime =
        std::chrono::nanoseconds(-1);
    const auto rejected = module.execute(invalid);
    require(rejected.status ==
                mparser::ModuleInvocationStatus::RequestRejected,
            "negative wall-time limit was not rejected");
    require(findDiagnostic(
                rejected,
                "MParser:InvalidExecutionLimits") != nullptr,
            "invalid limit diagnostic is missing");

    auto session = module.createSession();
    auto limited = functionRequest("spin");
    limited.limits.maxInstructionCount = 32;
    require(!session.execute(limited).succeeded(),
            "session resource stop unexpectedly succeeded");

    const auto recovered =
        session.execute(functionRequest("identity", {number(42)}));
    require(recovered.succeeded() &&
                recovered.outputs.size() == 1 &&
                std::fabs(recovered.outputs.front().number - 42.0) <
                    1e-9,
            "session was not reusable after a resource stop");
}

void runBuiltinBoundarySmoke() {
    auto registry =
        mparser::createBuiltinRegistryWithDefaults();

    mparser::BuiltinDescriptor control;
    control.name = "resource_control_active";
    control.inputs = mparser::BuiltinArity::fixed(0);
    control.outputs = mparser::BuiltinArity::fixed(1);
    control.implementation =
        mparser::BuiltinImplementationKind::Context;
    control.contextPermissions =
        mparser::BuiltinContextPermission::ExecutionControl;
    control.requiredContext =
        mparser::BuiltinContextPermission::ExecutionControl;
    control.handler = [](const mparser::BuiltinCall& call) {
        const bool active =
            call.context &&
            call.context->executionControl &&
            call.context->executionControl->active();
        return mparser::BuiltinResult::success(
            {mparser::makeRuntimeLogicalValue(active)});
    };
    auto registration =
        registry->registerBuiltin(std::move(control));
    require(registration.succeeded, registration.error);

    mparser::BuiltinDescriptor allocation;
    allocation.name = "resource_bad_alloc";
    allocation.inputs = mparser::BuiltinArity::fixed(0);
    allocation.outputs = mparser::BuiltinArity::fixed(1);
    allocation.implementation =
        mparser::BuiltinImplementationKind::Shared;
    allocation.handler = [](const mparser::BuiltinCall&)
        -> mparser::BuiltinResult {
        throw std::bad_alloc();
    };
    registration =
        registry->registerBuiltin(std::move(allocation));
    require(registration.succeeded, registration.error);
    registry->freeze();

    const std::vector<mparser::RuntimeValue> noArguments;
    const auto missingControl = registry->invoke(
        "resource_control_active",
        mparser::BuiltinCall{
            noArguments, 1, mparser::SourceSpan{}, nullptr});
    require(!missingControl.succeeded &&
                !missingControl.diagnostics.empty() &&
                missingControl.diagnostics.front().identifier ==
                    "MParser:MissingBuiltinContext",
            "required execution control was not enforced");

    const std::string source = R"(
function out = controlVisible()
out = resource_control_active();
end

function out = allocationFailure()
out = resource_bad_alloc();
end
)";
    mparser::CompiledModuleCompileOptions options;
    options.builtinRegistry = registry;
    const auto module =
        mparser::CompiledModule::compile(source, options);
    require(module.valid(),
            "custom resource module did not compile");

    auto visible = functionRequest("controlVisible");
    visible.limits.maxCallDepth = 4;
    const auto visibleResult = module.execute(visible);
    require(visibleResult.succeeded() &&
                visibleResult.outputs.size() == 1 &&
                visibleResult.outputs.front().number == 1.0,
            "custom builtin did not receive execution control");

    bool badAllocationEscaped = false;
    try {
        (void)module.execute(
            functionRequest("allocationFailure"));
    } catch (const std::bad_alloc&) {
        badAllocationEscaped = true;
    }
    require(badAllocationEscaped,
            "std::bad_alloc was incorrectly converted to a language error");
}

void runNameSmoke() {
    require(mparser::runtimeExecutionStopReasonName(
                mparser::RuntimeExecutionStopReason::None) == "none",
            "none stop reason name is unstable");
    require(mparser::runtimeExecutionStopReasonName(
                mparser::RuntimeExecutionStopReason::DiagnosticLimit) ==
                "diagnostic-limit",
            "diagnostic stop reason name is unstable");
}

} // namespace

int main() {
    try {
        runArrayByteAccountingSmoke();
        const auto module =
            mparser::CompiledModule::compile(kResourceSource);
        require(module.valid(),
                "resource control module did not compile");
        runInstructionAndCatchSmoke(module);
        runCancellationAndDeadlineSmoke(module);
        runCallDepthSmoke(module);
        runArrayLimitSmoke(module);
        runDiagnosticLimitSmoke(module);
        runOptimizedControlSmoke(module);
        runValidationAndSessionSmoke(module);
        runBuiltinBoundarySmoke();
        runNameSmoke();
        std::cout << "module resource control smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "module resource control smoke tests failed: "
                  << exception.what() << "\n";
        return 1;
    }
}
