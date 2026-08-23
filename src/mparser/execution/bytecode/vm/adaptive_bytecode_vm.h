#pragma once

#include "mparser/execution/bytecode/vm/bytecode_vm.h"
#include "mparser/execution/jit/typed_ir.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {

enum class AdaptiveBytecodeTier {
    Profiling,
    Typed,
};

std::string_view adaptiveBytecodeTierName(AdaptiveBytecodeTier tier);

enum class AdaptiveBytecodeEventKind {
    Promotion,
    TypedExecution,
    TypedFallback,
    Invalidation,
    RetrainingRejected,
};

std::string_view adaptiveBytecodeEventKindName(
    AdaptiveBytecodeEventKind kind);

struct AdaptiveBytecodeEvent {
    AdaptiveBytecodeEventKind kind =
        AdaptiveBytecodeEventKind::Promotion;
    size_t invocation = 0;
    size_t regionId = 0;
    size_t sourcePc = 0;
    std::string target;
    std::string reason;
    RuntimeFallbackKind fallbackKind =
        RuntimeFallbackKind::None;
    RuntimeFallbackKind nativeFallbackKind =
        RuntimeFallbackKind::None;
};

struct AdaptiveBytecodeVmOptions {
    size_t hotLoopThreshold = 10;
    size_t fallbackInvalidationThreshold = 3;
    bool preserveWorkspace = false;
    std::shared_ptr<RuntimeCallableContext> callableContext = {};
    std::shared_ptr<RuntimeSessionState> sessionState = {};
    std::vector<RuntimeVariable> initialWorkspace = {};
    std::string entryFunction = {};
    std::vector<RuntimeValue> arguments = {};
    std::optional<size_t> requestedOutputCount = {};
    TypedRegionBackend typedRegionBackend = TypedRegionBackend::Auto;
};

struct AdaptiveBytecodeVmRunResult {
    size_t invocation = 0;
    AdaptiveBytecodeTier tier = AdaptiveBytecodeTier::Profiling;
    BytecodeVmResult runtime;
    bool promotionOccurred = false;
    bool invalidationOccurred = false;
    size_t installedRegionCount = 0;
    size_t executableRegionCount = 0;
    size_t accumulatedLoopIterations = 0;
    size_t hotLoopCount = 0;
    size_t promotionCount = 0;
    size_t invalidationCount = 0;
};

class AdaptiveBytecodeVmSession {
public:
    AdaptiveBytecodeVmSession(
        const BytecodeProgram& program, const SemanticResult& semantic,
        const AdaptiveBytecodeVmOptions& options = {});

    AdaptiveBytecodeVmRunResult run();
    void reset();

    size_t invocationCount() const;
    bool hasTypedModule() const;
    const BytecodeVmProfile& accumulatedProfile() const;
    const BytecodeTypedIrModule* typedModule() const;
    const std::vector<AdaptiveBytecodeEvent>& events() const;
    const std::vector<RuntimeVariable>& workspace() const;
    void setWorkspace(std::vector<RuntimeVariable> workspace);
    const std::vector<RuntimeValue>& arguments() const;
    void setArguments(std::vector<RuntimeValue> arguments);
    std::optional<size_t> requestedOutputCount() const;
    void setRequestedOutputCount(std::optional<size_t> count);

private:
    void mergeProfile(const BytecodeVmProfile& profile);
    size_t executableRegionCount(
        const BytecodeTypedIrModule& module) const;
    void populateSessionState(AdaptiveBytecodeVmRunResult& result) const;
    void processTypedExecutions(AdaptiveBytecodeVmRunResult& result);
    void applyRetrainingRequirements(BytecodeTypedIrModule& module);
    std::pair<bool, std::string> hasRetrainingEvidence(
        const BytecodeTypedIrRegion& region) const;
    void resetAccumulatedProfile();
    const BytecodeTypedIrRegion* findTypedRegion(size_t regionId) const;
    void appendEvent(AdaptiveBytecodeEventKind kind, size_t regionId,
                     size_t sourcePc, std::string target,
                     std::string reason,
                     RuntimeFallbackKind fallbackKind =
                         RuntimeFallbackKind::None,
                     RuntimeFallbackKind nativeFallbackKind =
                         RuntimeFallbackKind::None);

    BytecodeProgram program_;
    SemanticResult semantic_;
    AdaptiveBytecodeVmOptions options_;
    size_t invocationCount_ = 0;
    BytecodeVmProfile accumulatedProfile_;
    BytecodeTypedIrModule typedModule_;
    bool bytecodeValidated_ = false;
    bool hasTypedModule_ = false;
    size_t promotionCount_ = 0;
    size_t invalidationCount_ = 0;
    std::map<size_t, size_t> consecutiveFallbacks_;
    std::set<size_t> retrainingSources_;
    std::set<size_t> reportedRetrainingRejections_;
    std::vector<AdaptiveBytecodeEvent> events_;
    std::vector<RuntimeVariable> workspace_;
    std::vector<RuntimeValue> arguments_;
};

} // namespace mparser
