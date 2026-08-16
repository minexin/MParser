#pragma once

#include "mparser/compiled_module.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct AdaptiveModuleRuntimeOptions {
    size_t hotLoopThreshold = 10;
    size_t fallbackInvalidationThreshold = 3;
    bool preserveWorkspace = false;
    std::vector<RuntimeVariable> initialWorkspace = {};
    TypedRegionBackend typedRegionBackend = TypedRegionBackend::Auto;
    std::shared_ptr<RuntimeSystemContext> systemContext = {};
};

struct AdaptiveModuleFunctionState {
    std::string entryFunction;
    size_t invocationCount = 0;
    AdaptiveBytecodeTier installedTier =
        AdaptiveBytecodeTier::Profiling;
    size_t promotionCount = 0;
    size_t typedExecutionCount = 0;
    size_t typedFallbackCount = 0;
    size_t invalidationCount = 0;
    size_t eventCount = 0;
};

struct AdaptiveModuleInvocationResult {
    std::string entryFunction;
    bool sessionCreated = false;
    AdaptiveBytecodeVmRunResult adaptive;
    AdaptiveModuleFunctionState state;
};

class AdaptiveModuleRuntime {
public:
    explicit AdaptiveModuleRuntime(
        const CompiledModule& module,
        const AdaptiveModuleRuntimeOptions& options = {});

    AdaptiveModuleInvocationResult invoke(
        std::string entryFunction,
        std::vector<RuntimeValue> arguments = {},
        std::optional<size_t> requestedOutputCount = std::nullopt);

    bool hasFunctionState(std::string_view entryFunction) const;
    AdaptiveModuleFunctionState functionState(
        std::string_view entryFunction) const;
    std::vector<AdaptiveModuleFunctionState> functionStates() const;
    const AdaptiveBytecodeVmSession* session(
        std::string_view entryFunction) const;
    void resetFunction(std::string_view entryFunction);
    void resetAll();

private:
    AdaptiveModuleFunctionState summarize(
        std::string_view entryFunction,
        const AdaptiveBytecodeVmSession& session) const;

    const CompiledModule* module_ = nullptr;
    AdaptiveModuleRuntimeOptions options_;
    std::map<std::string, std::unique_ptr<AdaptiveBytecodeVmSession>>
        sessions_;
};

} // namespace mparser
