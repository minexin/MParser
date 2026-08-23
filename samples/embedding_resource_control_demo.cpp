#include "mparser/embedding/compiled_module.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_value.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr const char* kSource = R"(
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

function out = identity(value)
out = value;
end
)";

mparser::ModuleInvocationRequest requestFor(
    std::string entry) {
    mparser::ModuleInvocationRequest request;
    request.entryFunction = std::move(entry);
    request.requestedOutputCount = 1;
    return request;
}

} // namespace

int main() {
    try {
        const auto module =
            mparser::CompiledModule::compile(kSource);
        if (!module.valid()) {
            throw std::runtime_error(
                "resource demo module did not compile");
        }

        auto session = module.createSession();
        auto limited = requestFor("spin");
        limited.limits.maxInstructionCount = 64;
        const auto limitedResult = session.execute(limited);
        if (limitedResult.execution.stopReason !=
                mparser::RuntimeExecutionStopReason::
                    InstructionLimit ||
            limitedResult.execution.executedInstructionCount !=
                64 ||
            !limitedResult.execution
                 .optimizedExecutionSuppressed) {
            throw std::runtime_error(
                "instruction limit was not enforced");
        }

        mparser::RuntimeCancellationToken cancellation;
        cancellation.requestCancellation();
        auto cancelled = requestFor("identity");
        cancelled.arguments = {
            mparser::makeRuntimeNumberValue(7)};
        cancelled.cancellationToken = cancellation;
        const auto cancelledResult = module.execute(cancelled);
        if (cancelledResult.execution.stopReason !=
            mparser::RuntimeExecutionStopReason::Cancelled) {
            throw std::runtime_error(
                "cancellation was not enforced");
        }

        auto recovered = requestFor("identity");
        recovered.arguments = {
            mparser::makeRuntimeNumberValue(42)};
        const auto recoveredResult = session.execute(recovered);
        if (!recoveredResult.succeeded() ||
            recoveredResult.outputs.size() != 1 ||
            recoveredResult.outputs.front().kind !=
                mparser::RuntimeValueKind::Number ||
            std::fabs(
                recoveredResult.outputs.front().number - 42.0) >
                1e-9) {
            throw std::runtime_error(
                "session did not recover after a resource stop");
        }

        std::cout << "instruction_status = "
                  << mparser::moduleInvocationStatusName(
                         limitedResult.status)
                  << "\n";
        std::cout << "instruction_stop = "
                  << mparser::runtimeExecutionStopReasonName(
                         limitedResult.execution.stopReason)
                  << "\n";
        std::cout << "instruction_count = "
                  << limitedResult.execution.executedInstructionCount
                  << "\n";
        std::cout << "optimized_suppressed = "
                  << (limitedResult.execution
                              .optimizedExecutionSuppressed
                          ? "true"
                          : "false")
                  << "\n";
        std::cout << "cancellation_stop = "
                  << mparser::runtimeExecutionStopReasonName(
                         cancelledResult.execution.stopReason)
                  << "\n";
        std::cout << "recovered = "
                  << recoveredResult.outputs.front().number
                  << "\n";
        std::cout << "summary = instruction-limit,cancelled,42\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "resource control demo failed: "
                  << exception.what() << "\n";
        return 1;
    }
}
