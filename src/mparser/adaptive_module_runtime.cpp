#include "mparser/adaptive_module_runtime.h"

#include <utility>

namespace mparser {
namespace {

size_t eventCount(const AdaptiveBytecodeVmSession& session,
                  AdaptiveBytecodeEventKind kind) {
    size_t count = 0;
    for (const auto& event : session.events()) {
        if (event.kind == kind) {
            ++count;
        }
    }
    return count;
}

} // namespace

AdaptiveModuleRuntime::AdaptiveModuleRuntime(
    const CompiledModule& module,
    const AdaptiveModuleRuntimeOptions& options)
    : module_(&module), options_(options) {}

AdaptiveModuleInvocationResult AdaptiveModuleRuntime::invoke(
    std::string entryFunction, std::vector<RuntimeValue> arguments,
    std::optional<size_t> requestedOutputCount) {
    AdaptiveModuleInvocationResult result;
    result.entryFunction = entryFunction;

    if (entryFunction.empty()) {
        result.adaptive.runtime.diagnostics.push_back(Diagnostic{
            SourceSpan{},
            "adaptive module runtime requires a named entry function"});
        return result;
    }

    const auto validation =
        module_->validateInvocation(entryFunction, arguments,
                                    requestedOutputCount);
    if (!validation.empty()) {
        result.adaptive.runtime.diagnostics = validation;
        return result;
    }

    auto found = sessions_.find(entryFunction);
    if (found == sessions_.end()) {
        AdaptiveBytecodeVmOptions sessionOptions;
        sessionOptions.hotLoopThreshold = options_.hotLoopThreshold;
        sessionOptions.fallbackInvalidationThreshold =
            options_.fallbackInvalidationThreshold;
        sessionOptions.preserveWorkspace = options_.preserveWorkspace;
        sessionOptions.sessionState =
            std::make_shared<RuntimeSessionState>(
                options_.systemContext);
        sessionOptions.initialWorkspace = options_.initialWorkspace;
        sessionOptions.typedRegionBackend = options_.typedRegionBackend;
        sessionOptions.entryFunction = entryFunction;
        sessionOptions.arguments = std::move(arguments);
        sessionOptions.requestedOutputCount = requestedOutputCount;
        auto session = std::make_unique<AdaptiveBytecodeVmSession>(
            module_->bytecode(), module_->semantic(), sessionOptions);
        found = sessions_.emplace(entryFunction, std::move(session)).first;
        result.sessionCreated = true;
    } else {
        found->second->setArguments(std::move(arguments));
        found->second->setRequestedOutputCount(requestedOutputCount);
    }

    result.adaptive = found->second->run();
    result.state = summarize(entryFunction, *found->second);
    return result;
}

bool AdaptiveModuleRuntime::hasFunctionState(
    std::string_view entryFunction) const {
    return sessions_.find(std::string(entryFunction)) != sessions_.end();
}

AdaptiveModuleFunctionState AdaptiveModuleRuntime::functionState(
    std::string_view entryFunction) const {
    const auto found = sessions_.find(std::string(entryFunction));
    if (found == sessions_.end()) {
        AdaptiveModuleFunctionState state;
        state.entryFunction = std::string(entryFunction);
        return state;
    }
    return summarize(entryFunction, *found->second);
}

std::vector<AdaptiveModuleFunctionState>
AdaptiveModuleRuntime::functionStates() const {
    std::vector<AdaptiveModuleFunctionState> states;
    states.reserve(sessions_.size());
    for (const auto& [entryFunction, session] : sessions_) {
        states.push_back(summarize(entryFunction, *session));
    }
    return states;
}

const AdaptiveBytecodeVmSession* AdaptiveModuleRuntime::session(
    std::string_view entryFunction) const {
    const auto found = sessions_.find(std::string(entryFunction));
    return found == sessions_.end() ? nullptr : found->second.get();
}

void AdaptiveModuleRuntime::resetFunction(
    std::string_view entryFunction) {
    sessions_.erase(std::string(entryFunction));
}

void AdaptiveModuleRuntime::resetAll() {
    sessions_.clear();
}

AdaptiveModuleFunctionState AdaptiveModuleRuntime::summarize(
    std::string_view entryFunction,
    const AdaptiveBytecodeVmSession& session) const {
    AdaptiveModuleFunctionState state;
    state.entryFunction = std::string(entryFunction);
    state.invocationCount = session.invocationCount();
    state.installedTier = session.hasTypedModule()
                              ? AdaptiveBytecodeTier::Typed
                              : AdaptiveBytecodeTier::Profiling;
    state.promotionCount =
        eventCount(session, AdaptiveBytecodeEventKind::Promotion);
    state.typedExecutionCount =
        eventCount(session, AdaptiveBytecodeEventKind::TypedExecution);
    state.typedFallbackCount =
        eventCount(session, AdaptiveBytecodeEventKind::TypedFallback);
    state.invalidationCount =
        eventCount(session, AdaptiveBytecodeEventKind::Invalidation);
    state.eventCount = session.events().size();
    return state;
}

} // namespace mparser
