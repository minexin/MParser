#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/bytecode.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

struct ProgramFixture {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

ProgramFixture lower(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    return ProgramFixture{std::move(semantic), std::move(bytecode)};
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const mparser::RuntimeValue* findWorkspaceVariable(
    const mparser::AdaptiveBytecodeVmSession& session,
    std::string_view name) {
    for (const auto& variable : session.workspace()) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const mparser::BytecodeLoopProfile* findLoop(
    const mparser::BytecodeVmProfile& profile, std::string_view variable) {
    for (const auto& loop : profile.loops) {
        if (loop.variable == variable) {
            return &loop;
        }
    }
    return nullptr;
}

const mparser::BytecodeTypedRegionExecutionProfile* findExecution(
    const mparser::BytecodeVmResult& result, std::string_view target) {
    for (const auto& execution : result.typedRegionExecutions) {
        if (execution.kind == "scalar-loop" &&
            execution.target == target) {
            return &execution;
        }
    }
    return nullptr;
}

const mparser::BytecodeCallSiteProfile* findCallSite(
    const mparser::BytecodeVmProfile& profile, std::string_view target) {
    for (const auto& site : profile.callSites) {
        if (site.target == target) {
            return &site;
        }
    }
    return nullptr;
}

size_t eventCount(
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

void runAdaptivePromotionSmoke() {
    auto fixture = lower(R"(function y = main()
y = 0;
for i = 1:6
    y = y + i * i;
end
end
)");

    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic,
        mparser::AdaptiveBytecodeVmOptions{10});

    const auto cold = session.run();
    assert(cold.invocation == 1);
    assert(cold.tier == mparser::AdaptiveBytecodeTier::Profiling);
    assert(cold.runtime.profile.collected);
    assert(!cold.promotionOccurred);
    assert(cold.accumulatedLoopIterations == 6);
    assert(cold.hotLoopCount == 0);
    assert(!session.hasTypedModule());
    const auto* coldLoop = findLoop(session.accumulatedProfile(), "i");
    assert(coldLoop != nullptr);
    assert(coldLoop->iterationCount == 6);
    assert(!coldLoop->hot);

    const auto warming = session.run();
    assert(warming.invocation == 2);
    assert(warming.tier == mparser::AdaptiveBytecodeTier::Profiling);
    assert(warming.runtime.profile.collected);
    assert(warming.promotionOccurred);
    assert(warming.installedRegionCount > 0);
    assert(warming.executableRegionCount == 1);
    assert(warming.accumulatedLoopIterations == 12);
    assert(warming.hotLoopCount == 1);
    assert(session.hasTypedModule());
    const auto* hotLoop = findLoop(session.accumulatedProfile(), "i");
    assert(hotLoop != nullptr);
    assert(hotLoop->entryCount == 2);
    assert(hotLoop->iterationCount == 12);
    assert(hotLoop->hot);

    const auto steady = session.run();
    assert(steady.invocation == 3);
    assert(steady.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(!steady.runtime.profile.collected);
    assert(!steady.promotionOccurred);
    assert(steady.accumulatedLoopIterations == 12);
    assert(steady.hotLoopCount == 1);
    assert(steady.runtime.executedInstructionCount <
           warming.runtime.executedInstructionCount);
    const auto* execution = findExecution(steady.runtime, "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Promotion) == 1);
    assert(eventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::TypedExecution) == 1);

    const auto* output = findVariable(steady.runtime, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(output->number - 91.0) < 1e-9);

    session.reset();
    assert(session.invocationCount() == 0);
    assert(!session.hasTypedModule());
    assert(!session.accumulatedProfile().collected);
    assert(session.events().empty());
}

void runAdaptiveFallbackSmoke() {
    auto fixture = lower(R"(function y = main()
y = [0 0];
for i = 1:6
    y = y + i * i;
end
end
)");

    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic,
        mparser::AdaptiveBytecodeVmOptions{10, 2});
    (void)session.run();
    const auto promotion = session.run();
    assert(promotion.promotionOccurred);

    const auto fallback = session.run();
    assert(fallback.tier == mparser::AdaptiveBytecodeTier::Typed);
    const auto* execution = findExecution(fallback.runtime, "i");
    assert(execution != nullptr);
    assert(execution->attemptCount == 1);
    assert(execution->executionCount == 0);
    assert(execution->fallbackCount == 1);
    assert(execution->lastReason ==
           "typed region input is not scalar numeric: y");

    const auto* output = findVariable(fallback.runtime, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Vector);
    assert(output->elements.size() == 2);
    assert(output->elements[0] == 91.0);
    assert(output->elements[1] == 91.0);

    const auto invalidation = session.run();
    assert(invalidation.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(invalidation.invalidationOccurred);
    assert(invalidation.installedRegionCount == 0);
    assert(invalidation.executableRegionCount == 0);
    assert(invalidation.promotionCount == 1);
    assert(invalidation.invalidationCount == 1);
    assert(!session.hasTypedModule());
    assert(!session.accumulatedProfile().collected);

    const auto retrainingCold = session.run();
    assert(retrainingCold.tier ==
           mparser::AdaptiveBytecodeTier::Profiling);
    assert(!retrainingCold.promotionOccurred);
    assert(retrainingCold.accumulatedLoopIterations == 6);

    const auto retrainingHot = session.run();
    assert(retrainingHot.tier ==
           mparser::AdaptiveBytecodeTier::Profiling);
    assert(!retrainingHot.promotionOccurred);
    assert(retrainingHot.accumulatedLoopIterations == 12);
    assert(retrainingHot.hotLoopCount == 1);
    assert(!session.hasTypedModule());
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Promotion) == 1);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::TypedFallback) == 2);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Invalidation) == 1);
    assert(eventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::RetrainingRejected) == 1);
}

void runAdaptiveStaticRejectionSmoke() {
    auto fixture = lower(R"(function y = main()
y = 0;
for i = 1:6
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * 2;
end
)");

    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic,
        mparser::AdaptiveBytecodeVmOptions{10});
    const auto cold = session.run();
    const auto hot = session.run();
    const auto stillProfiling = session.run();

    assert(!cold.promotionOccurred);
    assert(!hot.promotionOccurred);
    assert(!stillProfiling.promotionOccurred);
    assert(stillProfiling.tier ==
           mparser::AdaptiveBytecodeTier::Profiling);
    assert(!session.hasTypedModule());
    const auto* loop = findLoop(session.accumulatedProfile(), "i");
    assert(loop != nullptr);
    assert(loop->hot);
    assert(loop->iterationCount == 18);
    const auto* call = findCallSite(session.accumulatedProfile(), "kernel");
    assert(call != nullptr);
    assert(call->executionCount == 18);

    const auto* output = findVariable(stillProfiling.runtime, "y");
    assert(output != nullptr);
    assert(output->number == 42.0);
}

void runPersistentWorkspaceRetrainingSmoke() {
    auto fixture = lower(R"(phase = phase + 1;
if phase <= 2
    y = 0;
elseif phase <= 4
    y = [0 0];
else
    y = 0;
end
for i = 1:6
    y = y + i * i;
end
)");

    mparser::RuntimeValue phase;
    phase.kind = mparser::RuntimeValueKind::Number;
    phase.number = 0.0;
    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 10;
    options.fallbackInvalidationThreshold = 2;
    options.preserveWorkspace = true;
    options.initialWorkspace.push_back(
        mparser::RuntimeVariable{"phase", phase});
    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic, options);

    const auto cold = session.run();
    assert(cold.tier == mparser::AdaptiveBytecodeTier::Profiling);
    assert(!cold.promotionOccurred);
    assert(findVariable(cold.runtime, "phase")->number == 1.0);

    const auto firstPromotion = session.run();
    assert(firstPromotion.promotionOccurred);
    assert(firstPromotion.promotionCount == 1);
    assert(findVariable(firstPromotion.runtime, "phase")->number == 2.0);

    const auto firstFallback = session.run();
    assert(firstFallback.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(!firstFallback.invalidationOccurred);
    const auto* firstExecution = findExecution(firstFallback.runtime, "i");
    assert(firstExecution != nullptr);
    assert(firstExecution->fallbackCount == 1);
    assert(findVariable(firstFallback.runtime, "phase")->number == 3.0);

    const auto invalidation = session.run();
    assert(invalidation.invalidationOccurred);
    assert(invalidation.invalidationCount == 1);
    assert(!session.hasTypedModule());
    assert(findVariable(invalidation.runtime, "phase")->number == 4.0);

    const auto retraining = session.run();
    assert(retraining.tier == mparser::AdaptiveBytecodeTier::Profiling);
    assert(!retraining.promotionOccurred);
    assert(findVariable(retraining.runtime, "phase")->number == 5.0);

    const auto secondPromotion = session.run();
    assert(secondPromotion.tier ==
           mparser::AdaptiveBytecodeTier::Profiling);
    assert(secondPromotion.promotionOccurred);
    assert(secondPromotion.promotionCount == 2);
    assert(secondPromotion.invalidationCount == 1);
    assert(findVariable(secondPromotion.runtime, "phase")->number == 6.0);

    const auto stable = session.run();
    assert(stable.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(!stable.invalidationOccurred);
    const auto* stableExecution = findExecution(stable.runtime, "i");
    assert(stableExecution != nullptr);
    assert(stableExecution->executionCount == 1);
    assert(stableExecution->fallbackCount == 0);
    assert(findVariable(stable.runtime, "phase")->number == 7.0);
    const auto* output = findVariable(stable.runtime, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(output->number == 91.0);

    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Promotion) == 2);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::TypedFallback) == 2);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Invalidation) == 1);
    assert(eventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::RetrainingRejected) == 0);
    assert(eventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::TypedExecution) == 1);
    const auto* workspacePhase = findWorkspaceVariable(session, "phase");
    assert(workspacePhase != nullptr);
    assert(workspacePhase->number == 7.0);
}

void runFunctionArgumentRetrainingSmoke() {
    auto fixture = lower(R"(function y = kernel(seed)
y = 0;
for i = 1:6
    y = y + seed * i;
end
end
)");

    mparser::RuntimeValue scalar;
    scalar.kind = mparser::RuntimeValueKind::Number;
    scalar.number = 2.0;
    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 10;
    options.fallbackInvalidationThreshold = 2;
    options.entryFunction = "kernel";
    options.arguments = {scalar};
    mparser::AdaptiveBytecodeVmSession session(
        fixture.bytecode, fixture.semantic, options);

    const auto cold = session.run();
    assert(!cold.promotionOccurred);
    const auto firstPromotion = session.run();
    assert(firstPromotion.promotionOccurred);
    assert(firstPromotion.runtime.outputs.size() == 1);
    assert(firstPromotion.runtime.outputs[0].number == 42.0);

    mparser::RuntimeValue vector;
    vector.kind = mparser::RuntimeValueKind::Vector;
    vector.elements = {1.0, 1.0};
    vector.rows = 1;
    vector.columns = 2;
    session.setArguments({vector});
    const auto fallback = session.run();
    assert(findExecution(fallback.runtime, "i")->fallbackCount == 1);
    const auto invalidation = session.run();
    assert(invalidation.invalidationOccurred);
    assert(!session.hasTypedModule());

    scalar.number = 3.0;
    session.setArguments({scalar});
    const auto retraining = session.run();
    assert(!retraining.promotionOccurred);
    assert(retraining.runtime.profile.functionEntries.size() == 1);
    assert(retraining.runtime.profile.functionEntries[0]
               .argumentObservations[0]
               .kind == "number");
    const auto replacement = session.run();
    assert(replacement.promotionOccurred);
    assert(replacement.promotionCount == 2);
    assert(replacement.invalidationCount == 1);

    const auto stable = session.run();
    assert(stable.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(findExecution(stable.runtime, "i")->executionCount == 1);
    assert(stable.runtime.entryFunction == "kernel");
    assert(stable.runtime.outputs.size() == 1);
    assert(stable.runtime.outputs[0].kind ==
           mparser::RuntimeValueKind::Number);
    assert(stable.runtime.outputs[0].number == 63.0);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Promotion) == 2);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::TypedFallback) == 2);
    assert(eventCount(session,
                      mparser::AdaptiveBytecodeEventKind::Invalidation) == 1);
    assert(eventCount(
               session,
               mparser::AdaptiveBytecodeEventKind::TypedExecution) == 1);
}

} // namespace

int main() {
    try {
        runAdaptivePromotionSmoke();
        runAdaptiveFallbackSmoke();
        runAdaptiveStaticRejectionSmoke();
        runPersistentWorkspaceRetrainingSmoke();
        runFunctionArgumentRetrainingSmoke();
        std::cout << "adaptive bytecode VM smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
