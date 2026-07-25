#include "mparser/compiled_module.h"
#include "mparser/runtime_value.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kSource = R"(function total = sumTo(limit)
total = 0;
for i = 1:limit
    total = total + i;
end
end

function value = nextValue(step)
persistent total
if isempty(total)
    total = 0;
end
total = total + step;
value = total;
end
)";

double scalarOutput(
    const mparser::ModuleInvocationResult& result) {
    if (!result.succeeded() || result.outputs.size() != 1 ||
        result.outputs.front().kind !=
            mparser::RuntimeValueKind::Number) {
        throw std::runtime_error("expected one scalar output");
    }
    return result.outputs.front().number;
}

} // namespace

int main() {
    try {
        const auto module =
            mparser::CompiledModule::compile(kSource);
        if (!module.valid()) {
            std::cerr << "module compilation failed\n";
            return 1;
        }

        mparser::ModuleInvocationRequest sumRequest;
        sumRequest.entryFunction = "sumTo";
        sumRequest.arguments = {
            mparser::makeRuntimeNumberValue(100)};
        sumRequest.requestedOutputCount = 1;
        const auto sumResult = module.execute(sumRequest);
        const double sum = scalarOutput(sumResult);

        auto session = module.createSession();
        mparser::ModuleInvocationRequest nextRequest;
        nextRequest.entryFunction = "nextValue";
        nextRequest.requestedOutputCount = 1;
        nextRequest.backend =
            mparser::ModuleExecutionBackend::Bytecode;
        nextRequest.arguments = {
            mparser::makeRuntimeNumberValue(2)};
        const double first =
            scalarOutput(session.execute(nextRequest));
        nextRequest.arguments = {
            mparser::makeRuntimeNumberValue(3)};
        const double second =
            scalarOutput(session.execute(nextRequest));

        std::cout << "status = "
                  << mparser::moduleInvocationStatusName(
                         sumResult.status)
                  << "\n";
        std::cout << "requested_backend = "
                  << mparser::moduleExecutionBackendName(
                         sumResult.execution.requestedBackend)
                  << "\n";
        std::cout << "effective_tier = "
                  << mparser::moduleExecutionTierName(
                         sumResult.execution.effectiveTier)
                  << "\n";
        std::cout << "summary = "
                  << sum + first * 10 + second << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "embedding demo failed: "
                  << exception.what() << "\n";
        return 1;
    }
}
