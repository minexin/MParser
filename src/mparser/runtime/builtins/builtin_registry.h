#pragma once

#include "mparser/frontend/diagnostic.h"
#include "mparser/runtime/core/runtime_output.h"
#include "mparser/runtime/core/runtime_value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::uint32_t kBuiltinSourceContractMajor = 1;
inline constexpr std::uint32_t kBuiltinSourceContractMinor = 16;

struct RuntimeObjectArrayPolicy;
class RuntimeExecutionControl;
class RuntimeSystemContext;
class RuntimeWarningContext;
class BuiltinRegistry;

enum class BuiltinWorkspaceScope {
    Current,
    Caller,
    Base,
};

struct BuiltinWorkspaceAccess {
    RuntimeWorkspace* variables = nullptr;
    std::function<RuntimeWorkspace*(BuiltinWorkspaceScope)> resolveVariables;
    std::function<void()> clearVariables;
    std::function<bool(std::string_view)> eraseVariable;
    std::function<bool(std::string_view)> functionExists;
    std::function<bool(std::string_view)> classExists;
};

struct BuiltinDisplayFormatAccess {
    std::function<RuntimeDisplayFormat()> current;
    std::function<RuntimeDisplayFormat(RuntimeDisplayFormat)> replace;
};

struct BuiltinArity {
    size_t minimum = 0;
    std::optional<size_t> maximum;

    static BuiltinArity fixed(size_t count);
    static BuiltinArity range(size_t minimum, size_t maximum);
    static BuiltinArity variadic(size_t minimum = 0);

    bool accepts(size_t count) const;
    std::string describe() const;
};

enum class BuiltinImplementationKind {
    Shared,
    Context,
    Intrinsic,
    Unsupported,
};

enum class BuiltinPurity {
    Pure,
    ReadOnly,
    Contextual,
    Impure,
};

enum class BuiltinDeterminism {
    Deterministic,
    ContextDependent,
    Nondeterministic,
};

enum class BuiltinThreadSafety {
    Reentrant,
    ContextBound,
    Serialized,
};

enum class BuiltinValueConstraint {
    Any,
    Numeric,
    ScalarNumeric,
    Text,
    FunctionHandle,
};

enum class BuiltinShapeConstraint {
    Any,
    Scalar,
    DenseArray,
};

struct BuiltinArgumentConstraint {
    BuiltinValueConstraint value = BuiltinValueConstraint::Any;
    BuiltinShapeConstraint shape = BuiltinShapeConstraint::Any;
};

using BuiltinOutputConstraint = BuiltinArgumentConstraint;

enum class BuiltinSideEffect : std::uint32_t {
    None = 0,
    Workspace = 1U << 0U,
    Console = 1U << 1U,
    WarningState = 1U << 2U,
    Time = 1U << 3U,
    ObjectState = 1U << 4U,
    External = 1U << 5U,
    RandomState = 1U << 6U,
    DisplayState = 1U << 7U,
};

enum class BuiltinContextPermission : std::uint32_t {
    None = 0,
    Workspace = 1U << 0U,
    WarningState = 1U << 1U,
    ObjectArrayPolicy = 1U << 2U,
    DynamicCall = 1U << 3U,
    ExecutionControl = 1U << 4U,
    Output = 1U << 5U,
    SystemServices = 1U << 6U,
    DisplayFormat = 1U << 7U,
    SourceEvaluation = 1U << 8U,
};

BuiltinSideEffect operator|(BuiltinSideEffect left,
                            BuiltinSideEffect right);
BuiltinContextPermission operator|(BuiltinContextPermission left,
                                   BuiltinContextPermission right);

bool hasBuiltinSideEffect(BuiltinSideEffect value,
                          BuiltinSideEffect expected);
bool hasBuiltinContextPermission(BuiltinContextPermission value,
                                 BuiltinContextPermission expected);

enum class BuiltinTypedLowering {
    None,
    Absolute,
    ArcCosine,
    ArcSine,
    ArcTangent,
    Cosine,
    Exponential,
    Logarithm,
    Sine,
    SquareRoot,
    Tangent,
    Sum,
    Product,
    Mean,
};

bool builtinTypedLoweringIsElementwiseUnary(
    BuiltinTypedLowering lowering);
bool builtinTypedLoweringIsReduction(BuiltinTypedLowering lowering);

enum class BuiltinImplicitOutputPolicy {
    FirstAvailable,
    None,
    FirstWhenNoArguments,
};

struct BuiltinResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::vector<Diagnostic> diagnostics;

    static BuiltinResult success(
        std::vector<RuntimeValue> outputs = {},
        std::vector<Diagnostic> diagnostics = {});
    static BuiltinResult failure(SourceSpan span, std::string message,
                                 std::string identifier);
};

using BuiltinDynamicInvoker = std::function<BuiltinResult(
    const RuntimeValue& callable,
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount, SourceSpan span)>;

struct BuiltinSourceEvaluationRequest {
    std::string source;
    BuiltinWorkspaceScope workspace = BuiltinWorkspaceScope::Current;
    size_t requestedOutputCount = 0;
    bool captureOutput = false;
    SourceSpan span;
};

struct BuiltinSourceEvaluationResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::string capturedOutput;
    std::vector<Diagnostic> diagnostics;
};

using BuiltinSourceEvaluator = std::function<
    BuiltinSourceEvaluationResult(const BuiltinSourceEvaluationRequest&)>;

struct BuiltinCallContext {
    BuiltinWorkspaceAccess* workspace = nullptr;
    RuntimeWarningContext* warningContext = nullptr;
    const RuntimeObjectArrayPolicy* objectArrayPolicy = nullptr;
    RuntimeExecutionControl* executionControl = nullptr;
    RuntimeOutputSink* outputSink = nullptr;
    RuntimeSystemContext* systemContext = nullptr;
    BuiltinDisplayFormatAccess* displayFormat = nullptr;
    const BuiltinRegistry* registry = nullptr;
    BuiltinDynamicInvoker dynamicInvoker;
    BuiltinSourceEvaluator sourceEvaluator;
};

struct BuiltinCall {
    const std::vector<RuntimeValue>& arguments;
    size_t requestedOutputCount = 0;
    SourceSpan span;
    BuiltinCallContext* context = nullptr;
    std::optional<size_t> callerOutputCount = std::nullopt;

    size_t callerNargout() const;
};

using BuiltinHandler =
    std::function<BuiltinResult(const BuiltinCall& call)>;

struct BuiltinDescriptor {
    std::string name;
    std::vector<std::string> aliases;
    BuiltinArity inputs = BuiltinArity::variadic();
    BuiltinArity outputs = BuiltinArity::variadic();
    std::vector<BuiltinArgumentConstraint> argumentConstraints;
    std::vector<BuiltinOutputConstraint> outputConstraints;
    BuiltinImplementationKind implementation =
        BuiltinImplementationKind::Intrinsic;
    BuiltinPurity purity = BuiltinPurity::Contextual;
    BuiltinDeterminism determinism =
        BuiltinDeterminism::ContextDependent;
    BuiltinThreadSafety threadSafety =
        BuiltinThreadSafety::ContextBound;
    BuiltinSideEffect sideEffects = BuiltinSideEffect::None;
    BuiltinContextPermission contextPermissions =
        BuiltinContextPermission::None;
    BuiltinContextPermission requiredContext =
        BuiltinContextPermission::None;
    BuiltinTypedLowering typedLowering = BuiltinTypedLowering::None;
    BuiltinImplicitOutputPolicy implicitOutputPolicy =
        BuiltinImplicitOutputPolicy::FirstAvailable;
    std::string errorIdentifier = "MParser:InvalidBuiltinCall";
    std::string summary;
    BuiltinHandler handler;

    size_t implicitOutputCount(size_t suppliedInputCount) const;
};

struct BuiltinRegistrationResult {
    bool succeeded = false;
    std::string error;
};

class BuiltinRegistry {
public:
    BuiltinRegistrationResult registerBuiltin(
        BuiltinDescriptor descriptor);

    const BuiltinDescriptor* find(std::string_view name) const;
    bool contains(std::string_view name) const;
    std::vector<std::string> names() const;
    std::vector<std::reference_wrapper<const BuiltinDescriptor>>
    descriptors() const;

    BuiltinResult invoke(std::string_view name,
                         const BuiltinCall& call) const;

    void freeze();
    bool frozen() const;

private:
    std::map<std::string, BuiltinDescriptor, std::less<>>
        descriptors_;
    std::map<std::string, std::string, std::less<>> aliases_;
    bool frozen_ = false;
};

std::shared_ptr<BuiltinRegistry>
createBuiltinRegistryWithDefaults();

std::shared_ptr<const BuiltinRegistry>
defaultBuiltinRegistry();

} // namespace mparser
