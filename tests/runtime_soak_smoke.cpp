#include "mparser/execution/bytecode/adaptive_bytecode_vm.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/runtime/core/session/runtime_session_state.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef assert
#undef assert
#endif

#define assert(condition)                                                   \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error("requirement failed: " #condition);   \
        }                                                                   \
    } while (false)

namespace {

constexpr size_t kCompiledModuleInvocations = 5000;
constexpr size_t kRuntimeSessionInvocations = 2500;
constexpr size_t kAdaptiveInvocations = 512;

const std::string kSoakModuleSource = R"(
function y = statelessKernel(x)
y = x * 3 + 7;
end

function out = sessionKernel(step)
persistent count
global soak_shared
if isempty(count)
    count = 0;
end
if isempty(soak_shared)
    soak_shared = 0;
end
count = count + step;
soak_shared = soak_shared + step * 2;
out = count * 100000 + soak_shared;
end

function y = adaptiveKernel(seed)
y = 0;
for i = 1:6
    y = y + seed * i;
end
end
)";

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    result.dimensions = {1, 1};
    return result;
}

void assertNumber(const mparser::RuntimeValue& value, double expected) {
    assert(value.kind == mparser::RuntimeValueKind::Number);
    assert(value.number == expected);
}

void assertNoDiagnostics(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.message << "\n";
    }
    assert(diagnostics.empty());
}

size_t adaptiveEventCount(
    const mparser::AdaptiveBytecodeVmSession& session,
    mparser::AdaptiveBytecodeEventKind kind) {
    size_t count = 0;
    for (const auto& event : session.events()) {
        if (event.kind == kind) {
            ++count;
        }
    }
    return count;
}

std::int64_t runCompiledModuleSoak(
    const mparser::CompiledModule& module) {
    mparser::BytecodeVmOptions options;
    options.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = "statelessKernel";
    options.requestedOutputCount = 1;

    std::int64_t checksum = 0;
    for (size_t invocation = 0;
         invocation < kCompiledModuleInvocations; ++invocation) {
        const auto input =
            static_cast<std::int64_t>(invocation % 97) - 48;
        const auto expected = input * 3 + 7;
        options.arguments = {number(static_cast<double>(input))};

        const auto result = module.invoke(options);
        assertNoDiagnostics(result.diagnostics);
        assert(result.entryFunction == "statelessKernel");
        assert(result.outputNames.size() == 1);
        assert(result.outputNames.front() == "y");
        assert(result.outputs.size() == 1);
        assertNumber(result.outputs.front(),
                     static_cast<double>(expected));
        checksum += expected;
    }
    return checksum;
}

std::int64_t runRuntimeSessionSoak(
    const mparser::CompiledModule& module) {
    auto state = std::make_shared<mparser::RuntimeSessionState>();
    auto session = module.createSession(state);
    assert(session.state() == state);

    mparser::BytecodeVmOptions options;
    options.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = "sessionKernel";
    options.requestedOutputCount = 1;

    std::int64_t accumulated = 0;
    std::int64_t checksum = 0;
    for (size_t invocation = 0;
         invocation < kRuntimeSessionInvocations; ++invocation) {
        const auto step =
            static_cast<std::int64_t>(1 + invocation % 3);
        accumulated += step;
        options.arguments = {number(static_cast<double>(step))};

        const auto result = session.invoke(options);
        const auto expected = accumulated * 100000 +
                              accumulated * 2;
        assertNoDiagnostics(result.diagnostics);
        assert(result.outputs.size() == 1);
        assertNumber(result.outputs.front(),
                     static_cast<double>(expected));
        checksum += expected;
    }

    const auto global = state->findGlobal("soak_shared");
    assert(global.has_value());
    assertNumber(*global, static_cast<double>(accumulated * 2));
    assert(state->globals().size() == 1);

    const auto persistent = session.persistentVariables();
    assert(persistent.size() == 1);
    assert(persistent.front().contextIdentity != 0);
    assert(persistent.front().function == "sessionKernel");
    assert(persistent.front().name == "count");
    assertNumber(persistent.front().value,
                 static_cast<double>(accumulated));
    assert(state->persistentVariables().size() == 1);
    return checksum;
}

std::int64_t runAdaptiveSessionSoak(
    const mparser::CompiledModule& module) {
    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 10;
    options.entryFunction = "adaptiveKernel";
    options.arguments = {number(1)};
    options.requestedOutputCount = 1;
    auto session = module.createAdaptiveSession(options);

    size_t promotions = 0;
    std::int64_t checksum = 0;
    for (size_t invocation = 0;
         invocation < kAdaptiveInvocations; ++invocation) {
        const auto seed =
            static_cast<std::int64_t>(1 + invocation % 17);
        session.setArguments(
            {number(static_cast<double>(seed))});

        const auto result = session.run();
        assert(result.invocation == invocation + 1);
        assertNoDiagnostics(result.runtime.diagnostics);
        assert(result.runtime.entryFunction == "adaptiveKernel");
        assert(result.runtime.outputs.size() == 1);
        assertNumber(result.runtime.outputs.front(),
                     static_cast<double>(seed * 21));
        assert(!result.invalidationOccurred);

        if (result.promotionOccurred) {
            ++promotions;
        }
        if (invocation == 0) {
            assert(result.tier ==
                   mparser::AdaptiveBytecodeTier::Profiling);
            assert(!result.promotionOccurred);
        } else if (invocation == 1) {
            assert(result.tier ==
                   mparser::AdaptiveBytecodeTier::Profiling);
            assert(result.promotionOccurred);
        } else {
            assert(result.tier ==
                   mparser::AdaptiveBytecodeTier::Typed);
            assert(!result.promotionOccurred);
            assert(session.hasTypedModule());
        }
        checksum += seed * 21;
    }

    assert(session.invocationCount() == kAdaptiveInvocations);
    assert(session.hasTypedModule());
    assert(promotions == 1);
    assert(adaptiveEventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::Promotion) == 1);
    assert(adaptiveEventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::TypedExecution) ==
           kAdaptiveInvocations - 2);
    assert(adaptiveEventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::TypedFallback) == 0);
    assert(adaptiveEventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::Invalidation) == 0);
    return checksum;
}

} // namespace

int main() {
    try {
        const auto module =
            mparser::CompiledModule::compile(kSoakModuleSource);
        assert(module.valid());
        assertNoDiagnostics(module.diagnostics());
        assert(module.findFunction("statelessKernel") != nullptr);
        assert(module.findFunction("sessionKernel") != nullptr);
        assert(module.findFunction("adaptiveKernel") != nullptr);

        const auto compiledChecksum =
            runCompiledModuleSoak(module);
        const auto sessionChecksum =
            runRuntimeSessionSoak(module);
        const auto adaptiveChecksum =
            runAdaptiveSessionSoak(module);

        std::cout << "runtime soak smoke tests passed: "
                  << "compiled-module="
                  << kCompiledModuleInvocations
                  << ", runtime-session="
                  << kRuntimeSessionInvocations
                  << ", adaptive=" << kAdaptiveInvocations
                  << ", total="
                  << (kCompiledModuleInvocations +
                      kRuntimeSessionInvocations +
                      kAdaptiveInvocations)
                  << ", checksums=" << compiledChecksum << "/"
                  << sessionChecksum << "/" << adaptiveChecksum
                  << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
