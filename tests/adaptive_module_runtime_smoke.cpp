#include "mparser/adaptive_module_runtime.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

mparser::RuntimeValue vector(std::initializer_list<double> values) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Vector;
    result.elements = values;
    result.rows = 1;
    result.columns = result.elements.size();
    return result;
}

void assertOutput(const mparser::AdaptiveModuleInvocationResult& result,
                  double expected) {
    assert(result.adaptive.runtime.diagnostics.empty());
    assert(result.adaptive.runtime.outputs.size() == 1);
    const auto& output = result.adaptive.runtime.outputs.front();
    assert(output.kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(output.number - expected) < 1e-9);
}

const std::string kSource = R"(function y = module_demo()
y = -1;
end

function y = hot_a(seed)
y = 0;
for i = 1:6
    y = y + seed * i;
end
end

function y = hot_b(seed)
y = 1;
for j = 1:6
    y = y + seed * j;
end
end
)";

void runIndependentTieringSmoke() {
    auto module = mparser::CompiledModule::compile(kSource);
    assert(module.valid());
    mparser::AdaptiveModuleRuntime runtime(
        module, mparser::AdaptiveModuleRuntimeOptions{10, 2});

    const auto a1 = runtime.invoke("hot_a", {number(2)});
    const auto b1 = runtime.invoke("hot_b", {number(3)});
    const auto a2 = runtime.invoke("hot_a", {number(2)});
    const auto b2 = runtime.invoke("hot_b", {number(3)});
    assert(a1.sessionCreated);
    assert(b1.sessionCreated);
    assert(!a2.sessionCreated);
    assert(!b2.sessionCreated);
    assert(a2.adaptive.promotionOccurred);
    assert(b2.adaptive.promotionOccurred);
    assert(runtime.functionState("hot_a").installedTier ==
           mparser::AdaptiveBytecodeTier::Typed);
    assert(runtime.functionState("hot_b").installedTier ==
           mparser::AdaptiveBytecodeTier::Typed);

    const auto a3 = runtime.invoke("hot_a", {number(2)});
    const auto b3 = runtime.invoke("hot_b", {number(3)});
    assert(a3.adaptive.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(b3.adaptive.tier == mparser::AdaptiveBytecodeTier::Typed);
    assertOutput(a3, 42);
    assertOutput(b3, 64);

    const auto aFallback1 = runtime.invoke("hot_a", {vector({1, 2})});
    const auto aFallback2 = runtime.invoke("hot_a", {vector({1, 2})});
    assert(!aFallback1.adaptive.invalidationOccurred);
    assert(aFallback2.adaptive.invalidationOccurred);

    const auto aInvalidated = runtime.functionState("hot_a");
    const auto bUnaffected = runtime.functionState("hot_b");
    assert(aInvalidated.installedTier ==
           mparser::AdaptiveBytecodeTier::Profiling);
    assert(aInvalidated.invalidationCount == 1);
    assert(aInvalidated.typedFallbackCount == 2);
    assert(bUnaffected.installedTier ==
           mparser::AdaptiveBytecodeTier::Typed);
    assert(bUnaffected.invalidationCount == 0);

    const auto b4 = runtime.invoke("hot_b", {number(3)});
    assert(b4.adaptive.tier == mparser::AdaptiveBytecodeTier::Typed);
    assertOutput(b4, 64);
    assert(runtime.functionState("hot_b").typedExecutionCount >= 2);

    const auto retrain1 = runtime.invoke("hot_a", {number(4)});
    const auto retrain2 = runtime.invoke("hot_a", {number(4)});
    const auto retyped = runtime.invoke("hot_a", {number(4)});
    assert(!retrain1.adaptive.promotionOccurred);
    assert(retrain2.adaptive.promotionOccurred);
    assert(retyped.adaptive.tier == mparser::AdaptiveBytecodeTier::Typed);
    assertOutput(retyped, 84);

    const auto finalA = runtime.functionState("hot_a");
    const auto finalB = runtime.functionState("hot_b");
    assert(finalA.promotionCount == 2);
    assert(finalA.invalidationCount == 1);
    assert(finalB.promotionCount == 1);
    assert(finalB.invalidationCount == 0);
    assert(runtime.functionStates().size() == 2);
}

void runValidationAndResetSmoke() {
    auto module = mparser::CompiledModule::compile(kSource);
    mparser::AdaptiveModuleRuntime runtime(module);

    const auto unnamed = runtime.invoke("", {});
    assert(unnamed.adaptive.runtime.diagnostics.size() == 1);
    assert(!runtime.hasFunctionState(""));

    const auto missing = runtime.invoke("missing", {});
    assert(missing.adaptive.runtime.diagnostics.size() == 1);
    assert(!runtime.hasFunctionState("missing"));

    const auto wrongArity = runtime.invoke("hot_a", {});
    assert(wrongArity.adaptive.runtime.diagnostics.size() == 1);
    assert(!runtime.hasFunctionState("hot_a"));

    runtime.invoke("hot_a", {number(1)});
    runtime.invoke("hot_b", {number(1)});
    assert(runtime.hasFunctionState("hot_a"));
    assert(runtime.hasFunctionState("hot_b"));
    assert(runtime.session("hot_a") != nullptr);

    runtime.resetFunction("hot_a");
    assert(!runtime.hasFunctionState("hot_a"));
    assert(runtime.hasFunctionState("hot_b"));
    runtime.resetAll();
    assert(runtime.functionStates().empty());
}

void runInvalidModuleSmoke() {
    auto module = mparser::CompiledModule::compile(
        "function y = broken(\ny = 1;\n");
    assert(!module.valid());
    mparser::AdaptiveModuleRuntime runtime(module);
    const auto result = runtime.invoke("broken", {});
    assert(!result.adaptive.runtime.diagnostics.empty());
    assert(!runtime.hasFunctionState("broken"));
}

} // namespace

int main() {
    runIndependentTieringSmoke();
    runValidationAndResetSmoke();
    runInvalidModuleSmoke();
    std::cout << "adaptive module runtime smoke tests passed\n";
    return 0;
}
