#include "mparser/c_api.h"

#include "mparser/embedding/c_api_test_hooks.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/runtime/io/filesystem_utf8.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_timetable.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/io/runtime_system.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/frontend/source_loader.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mparser::c_api_test {

#if defined(MPARSER_C_API_TEST_FAULTS)
namespace {

struct UnknownFault final {};

struct FaultState {
    FaultPoint point = FaultPoint::None;
    ExceptionKind kind = ExceptionKind::BadAllocation;
    std::size_t matchingCallsBeforeFailure = 0;
};

thread_local FaultState faultState;

} // namespace

void arm(FaultPoint point, ExceptionKind kind,
         std::size_t matchingCallsBeforeFailure) noexcept {
    faultState = FaultState{
        point, kind, matchingCallsBeforeFailure};
}

void clear() noexcept {
    faultState = {};
}

void inject(FaultPoint point) {
    if (faultState.point != point) {
        return;
    }
    if (faultState.matchingCallsBeforeFailure != 0) {
        --faultState.matchingCallsBeforeFailure;
        return;
    }
    const auto kind = faultState.kind;
    clear();
    if (kind == ExceptionKind::BadAllocation) {
        throw std::bad_alloc();
    }
    throw UnknownFault{};
}
#else
void inject(FaultPoint) {}
#endif

} // namespace mparser::c_api_test

#ifndef MPARSER_VERSION_MAJOR
#define MPARSER_VERSION_MAJOR 1
#endif
#ifndef MPARSER_VERSION_MINOR
#define MPARSER_VERSION_MINOR 3
#endif
#ifndef MPARSER_VERSION_PATCH
#define MPARSER_VERSION_PATCH 0
#endif

static_assert(
    MPARSER_C_ABI_GENERATION ==
    MPARSER_C_ABI_GENERATION_EXPECTED);
static_assert(
    MPARSER_C_ABI_REVISION ==
    MPARSER_C_ABI_REVISION_EXPECTED);
static_assert(
    MPARSER_C_API_VERSION_MAJOR == MPARSER_VERSION_MAJOR &&
    MPARSER_C_API_VERSION_MINOR == MPARSER_VERSION_MINOR &&
    MPARSER_C_API_VERSION_PATCH == MPARSER_VERSION_PATCH);

namespace mparser_c_detail {

struct DiagnosticFrame {
    std::string source;
    std::string function;
    int32_t line = 1;
};

struct ModuleState;
struct RuntimeState;

using NumericColumnMajorStorage = std::variant<
    std::monostate,
    std::vector<double>,
    std::vector<float>,
    std::vector<int8_t>,
    std::vector<uint8_t>,
    std::vector<int16_t>,
    std::vector<uint16_t>,
    std::vector<int32_t>,
    std::vector<uint32_t>,
    std::vector<int64_t>,
    std::vector<uint64_t>>;

struct ValueState {
    mparser::RuntimeValue value;
    std::shared_ptr<ModuleState> owner;
    std::shared_ptr<RuntimeState> runtimeOwner;
    std::vector<size_t> dimensions;
    size_t elementCount = 0;
    NumericColumnMajorStorage numericRealColumnMajor;
    NumericColumnMajorStorage numericImaginaryColumnMajor;
    std::vector<uint16_t> characterColumnMajor;
    std::vector<std::vector<uint16_t>> stringColumnMajor;
    std::vector<std::string> structFieldNames;
    std::string functionText;
};

} // namespace mparser_c_detail

struct mparser_diagnostic {
    mparser_diagnostic_phase phase =
        MPARSER_DIAGNOSTIC_EXECUTION;
    mparser_diagnostic_severity severity =
        MPARSER_DIAGNOSTIC_ERROR;
    std::string identifier;
    std::string message;
    bool sourceAvailable = false;
    std::string sourceName;
    mparser_source_position begin{};
    mparser_source_position end{};
    std::vector<mparser_c_detail::DiagnosticFrame> stack;
    std::vector<mparser_diagnostic> causes;
};

namespace mparser_c_detail {

struct ModuleState {
    explicit ModuleState(mparser::CompiledModule compiled)
        : module(std::move(compiled)) {}

    mparser::CompiledModule module;
    std::vector<mparser_diagnostic> diagnostics;
    std::recursive_mutex sharedGraphMutex;
};

struct RuntimeState final
    : std::enable_shared_from_this<RuntimeState> {
    explicit RuntimeState(
        std::shared_ptr<mparser::RuntimeSystemContext> systemContext)
        : session(std::make_shared<mparser::RuntimeSessionState>(
              std::move(systemContext))) {}

    mparser::RuntimeSourceCallableInvoker callableInvoker(
        mparser::ModuleExecutionBackend backend,
        std::shared_ptr<mparser::RuntimeExecutionControl> executionControl);

    std::shared_ptr<mparser::RuntimeSessionState> session;
    std::map<size_t, std::shared_ptr<ModuleState>> modules;
    std::recursive_mutex mutex;
};

struct SessionState {
    SessionState(std::shared_ptr<ModuleState> owner,
                 mparser::CompiledModuleSession created)
        : module(std::move(owner)),
          session(std::move(created)) {}

    std::shared_ptr<ModuleState> module;
    mparser::CompiledModuleSession session;
    std::mutex mutex;
};

} // namespace mparser_c_detail

struct mparser_module {
    std::atomic_size_t references{1};
    std::shared_ptr<mparser_c_detail::ModuleState> state;
};

struct mparser_session {
    std::atomic_size_t references{1};
    std::shared_ptr<mparser_c_detail::SessionState> state;
};

struct mparser_runtime {
    std::atomic_size_t references{1};
    std::shared_ptr<mparser_c_detail::RuntimeState> state;
};

struct mparser_system_context {
    std::atomic_size_t references{1};
    std::shared_ptr<mparser::RuntimeSystemContext> state;
};

struct mparser_result {
    std::atomic_size_t references{1};
    mparser::ModuleInvocationResult value;
    std::shared_ptr<mparser_c_detail::ModuleState> owner;
    std::shared_ptr<mparser_c_detail::RuntimeState> runtimeOwner;
    std::vector<mparser_diagnostic> diagnostics;
};

struct mparser_value {
    std::atomic_size_t references{1};
    std::shared_ptr<mparser_c_detail::ValueState> state;
};

struct mparser_cancel_token {
    std::atomic_size_t references{1};
    mparser::RuntimeCancellationToken token;
};

namespace mparser_c_detail {

mparser::RuntimeSourceCallableInvoker RuntimeState::callableInvoker(
    mparser::ModuleExecutionBackend backend,
    std::shared_ptr<mparser::RuntimeExecutionControl> executionControl) {
    const std::weak_ptr<RuntimeState> weakRuntime = weak_from_this();
    return [weakRuntime, backend,
            executionControl = std::move(executionControl)](
               const mparser::RuntimeValue& callable,
               const std::vector<mparser::RuntimeValue>& arguments,
               size_t requestedOutputCount, mparser::SourceSpan span,
               mparser::RuntimeWorkspace*) {
        mparser::RuntimeSourceCallableInvocationResult result;
        const auto runtime = weakRuntime.lock();
        if (!runtime) {
            result.diagnostics.push_back(mparser::Diagnostic{
                span, "shared runtime is no longer available",
                "MParser:RuntimeUnavailable"});
            return result;
        }
        if (!callable.functionHandle ||
            callable.functionHandle->backend !=
                mparser::RuntimeFunctionHandleBackend::Bytecode ||
            !callable.functionHandle->context) {
            result.diagnostics.push_back(mparser::Diagnostic{
                span, "cross-module callable is not bytecode-backed",
                "MParser:InvalidRuntimeCallable"});
            return result;
        }

        std::lock_guard lock(runtime->mutex);
        const auto target = runtime->modules.find(
            callable.functionHandle->context->identity);
        if (target == runtime->modules.end() || !target->second) {
            result.diagnostics.push_back(mparser::Diagnostic{
                span, "callable owner is not registered in this runtime",
                "MParser:RuntimeOwnerUnavailable"});
            return result;
        }
        return target->second->module.invokeCallable(
            callable, arguments, requestedOutputCount, runtime->session,
            executionControl, backend,
            runtime->callableInvoker(backend, executionControl));
    };
}

namespace {

template <typename Handle>
void retainHandle(Handle* handle) noexcept {
    if (handle) {
        handle->references.fetch_add(1, std::memory_order_relaxed);
    }
}

template <typename Handle>
void releaseHandle(Handle* handle) noexcept {
    if (handle &&
        handle->references.fetch_sub(
            1, std::memory_order_acq_rel) == 1) {
        delete handle;
    }
}

mparser_utf8_view utf8View(std::string_view value) noexcept {
    return mparser_utf8_view{value.data(), value.size()};
}

mparser_utf8_view emptyUtf8View() noexcept {
    return mparser_utf8_view{nullptr, 0};
}

mparser_source_position sourcePosition(
    const mparser::ModuleSourcePosition& position) noexcept {
    return mparser_source_position{
        static_cast<uint64_t>(position.offset),
        static_cast<int32_t>(position.line),
        static_cast<int32_t>(position.column)};
}

mparser_source_position sourcePosition(
    const mparser::SourcePosition& position) noexcept {
    return mparser_source_position{
        static_cast<uint64_t>(position.offset),
        static_cast<int32_t>(position.line),
        static_cast<int32_t>(position.column)};
}

DiagnosticFrame diagnosticFrame(
    const mparser::ModuleDiagnosticFrame& frame) {
    return DiagnosticFrame{
        frame.sourceName, frame.functionName,
        static_cast<int32_t>(frame.line)};
}

DiagnosticFrame diagnosticFrame(
    const mparser::DiagnosticFrame& frame) {
    return DiagnosticFrame{
        frame.file, frame.name,
        static_cast<int32_t>(frame.line)};
}

mparser_diagnostic diagnosticCause(
    const mparser::ModuleDiagnosticCause& cause,
    mparser_diagnostic_phase phase) {
    mparser_diagnostic result;
    result.phase = phase;
    result.identifier = cause.identifier;
    result.message = cause.message;
    result.stack.reserve(cause.stack.size());
    for (const auto& frame : cause.stack) {
        result.stack.push_back(diagnosticFrame(frame));
    }
    result.causes.reserve(cause.causes.size());
    for (const auto& nested : cause.causes) {
        result.causes.push_back(
            diagnosticCause(nested, phase));
    }
    return result;
}

mparser_diagnostic diagnosticCause(
    const mparser::DiagnosticCause& cause,
    mparser_diagnostic_phase phase) {
    mparser_diagnostic result;
    result.phase = phase;
    result.identifier = cause.identifier;
    result.message = cause.message;
    result.stack.reserve(cause.stack.size());
    for (const auto& frame : cause.stack) {
        result.stack.push_back(diagnosticFrame(frame));
    }
    result.causes.reserve(cause.causes.size());
    for (const auto& nested : cause.causes) {
        result.causes.push_back(
            diagnosticCause(nested, phase));
    }
    return result;
}

mparser_diagnostic externalDiagnostic(
    const mparser::ModuleDiagnostic& diagnostic) {
    mparser_diagnostic result;
    result.phase =
        diagnostic.phase ==
                mparser::ModuleDiagnosticPhase::Compilation
            ? MPARSER_DIAGNOSTIC_COMPILATION
            : diagnostic.phase ==
                      mparser::ModuleDiagnosticPhase::Validation
                  ? MPARSER_DIAGNOSTIC_VALIDATION
                  : MPARSER_DIAGNOSTIC_EXECUTION;
    result.severity =
        diagnostic.severity ==
                mparser::ModuleDiagnosticSeverity::Warning
            ? MPARSER_DIAGNOSTIC_WARNING
            : MPARSER_DIAGNOSTIC_ERROR;
    result.identifier = diagnostic.identifier;
    result.message = diagnostic.message;
    result.sourceAvailable = diagnostic.source.available;
    result.sourceName = diagnostic.source.sourceName;
    result.begin = sourcePosition(diagnostic.source.begin);
    result.end = sourcePosition(diagnostic.source.end);
    result.stack.reserve(diagnostic.stack.size());
    for (const auto& frame : diagnostic.stack) {
        result.stack.push_back(diagnosticFrame(frame));
    }
    result.causes.reserve(diagnostic.causes.size());
    for (const auto& cause : diagnostic.causes) {
        result.causes.push_back(
            diagnosticCause(cause, result.phase));
    }
    return result;
}

mparser_diagnostic compilationDiagnostic(
    const mparser::Diagnostic& diagnostic,
    const mparser::CompiledModule& module) {
    mparser_diagnostic result;
    result.phase = MPARSER_DIAGNOSTIC_COMPILATION;
    result.severity =
        diagnostic.severity == mparser::DiagnosticSeverity::Warning
            ? MPARSER_DIAGNOSTIC_WARNING
            : MPARSER_DIAGNOSTIC_ERROR;
    result.identifier =
        diagnostic.identifier.empty()
            ? "MParser:CompilationFailed"
            : diagnostic.identifier;
    result.message = diagnostic.message;
    if (diagnostic.span.begin.sourceId !=
            mparser::kInvalidSourceId &&
        diagnostic.span.begin.sourceId <
            module.sources().size()) {
        result.sourceAvailable = true;
        result.sourceName =
            std::string(module.sourceName(diagnostic.span));
        result.begin = sourcePosition(diagnostic.span.begin);
        result.end = sourcePosition(diagnostic.span.end);
    }
    result.stack.reserve(diagnostic.stack.size());
    for (const auto& frame : diagnostic.stack) {
        result.stack.push_back(diagnosticFrame(frame));
    }
    result.causes.reserve(diagnostic.causes.size());
    for (const auto& cause : diagnostic.causes) {
        result.causes.push_back(diagnosticCause(
            cause, MPARSER_DIAGNOSTIC_COMPILATION));
    }
    return result;
}

std::optional<std::string> copyBytes(
    const char* data, size_t size) {
    if (!data && size != 0) {
        return std::nullopt;
    }
    return std::string(data ? data : "", size);
}

bool validPathText(std::string_view value) noexcept {
    return !value.empty() &&
           value.find('\0') == std::string_view::npos &&
           mparser::isValidUtf8(value);
}

mparser_api_status copySourceLoadOptions(
    const mparser_source_load_options* options,
    mparser::SourceLoaderOptions& loaderOptions) {
    loaderOptions = {};
    if (!options) {
        return MPARSER_API_STATUS_OK;
    }
    if (options->struct_size < MPARSER_SOURCE_LOAD_OPTIONS_SIZE ||
        options->abi_generation != MPARSER_C_ABI_GENERATION) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    if (!options->search_paths && options->search_path_count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }

    loaderOptions.searchPaths.reserve(options->search_path_count);
    for (size_t index = 0; index < options->search_path_count; ++index) {
        const auto path = copyBytes(options->search_paths[index].data,
                                    options->search_paths[index].size);
        if (!path || !validPathText(*path)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        loaderOptions.searchPaths.push_back(mparser::pathFromUtf8(*path));
    }
    return MPARSER_API_STATUS_OK;
}

mparser_api_status publishCompiledModule(
    mparser::CompiledModule compiled,
    mparser_module** out_module) {
    mparser::c_api_test::inject(
        mparser::c_api_test::FaultPoint::ModulePublish);
    auto state =
        std::make_shared<ModuleState>(std::move(compiled));
    state->diagnostics.reserve(
        state->module.diagnostics().size());
    for (const auto& diagnostic :
         state->module.diagnostics()) {
        state->diagnostics.push_back(
            compilationDiagnostic(
                diagnostic, state->module));
    }
    auto* handle = new mparser_module;
    handle->state = std::move(state);
    *out_module = handle;
    return handle->state->module.valid()
               ? MPARSER_API_STATUS_OK
               : MPARSER_API_STATUS_COMPILATION_FAILED;
}

mparser_api_status publishSourceLoadFailure(
    std::string sourceName,
    std::string message,
    mparser_module** out_module) {
    auto state = std::make_shared<ModuleState>(
        mparser::CompiledModule::compile(
            std::vector<mparser::SourceUnit>{}));
    mparser_diagnostic diagnostic;
    diagnostic.phase = MPARSER_DIAGNOSTIC_COMPILATION;
    diagnostic.severity = MPARSER_DIAGNOSTIC_ERROR;
    diagnostic.identifier = "MParser:SourceLoadFailed";
    diagnostic.message = std::move(message);
    diagnostic.sourceAvailable = !sourceName.empty();
    diagnostic.sourceName = std::move(sourceName);
    diagnostic.begin = mparser_source_position{0, 1, 1};
    diagnostic.end = diagnostic.begin;
    state->diagnostics.push_back(std::move(diagnostic));

    auto* handle = new mparser_module;
    handle->state = std::move(state);
    *out_module = handle;
    return MPARSER_API_STATUS_SOURCE_LOAD_FAILED;
}

bool uint64ToSize(uint64_t value, size_t& result) noexcept {
    if (value >
        static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        return false;
    }
    result = static_cast<size_t>(value);
    return true;
}

uint64_t sizeToUint64(size_t value) noexcept {
    return static_cast<uint64_t>(value);
}

mparser_api_status copySystemContextOptions(
    const mparser_system_context_options* options,
    mparser::RuntimeRootedSystemContextOptions& copied) {
    if (!options) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (options->struct_size < MPARSER_SYSTEM_CONTEXT_OPTIONS_SIZE ||
        options->abi_generation != MPARSER_C_ABI_GENERATION) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    constexpr mparser_system_capability knownCapabilities =
        MPARSER_SYSTEM_CAPABILITY_CURRENT_DIRECTORY |
        MPARSER_SYSTEM_CAPABILITY_SEARCH_PATHS |
        MPARSER_SYSTEM_CAPABILITY_ENVIRONMENT_READ |
        MPARSER_SYSTEM_CAPABILITY_FILESYSTEM_READ |
        MPARSER_SYSTEM_CAPABILITY_PROCESS |
        MPARSER_SYSTEM_CAPABILITY_CLOCK |
        MPARSER_SYSTEM_CAPABILITY_SLEEP |
        MPARSER_SYSTEM_CAPABILITY_RANDOM |
        MPARSER_SYSTEM_CAPABILITY_DYNAMIC_EVALUATION |
        MPARSER_SYSTEM_CAPABILITY_FILESYSTEM_WRITE;
    if ((options->capabilities & ~knownCapabilities) != 0 ||
        (!options->search_paths && options->search_path_count != 0)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }

    const auto root = copyBytes(
        options->root_directory.data,
        options->root_directory.size);
    const auto current = copyBytes(
        options->current_directory.data,
        options->current_directory.size);
    const auto temporary = copyBytes(
        options->temporary_directory.data,
        options->temporary_directory.size);
    if (!root || !validPathText(*root) || !current || !temporary ||
        (!current->empty() && !validPathText(*current)) ||
        (!temporary->empty() && !validPathText(*temporary))) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }

    copied = {};
    copied.capabilities = static_cast<mparser::RuntimeSystemCapability>(
        options->capabilities);
    copied.rootDirectory = mparser::pathFromUtf8(*root);
    if (!current->empty()) {
        copied.currentDirectory = mparser::pathFromUtf8(*current);
    }
    if (!temporary->empty()) {
        copied.temporaryDirectory = mparser::pathFromUtf8(*temporary);
    }
    copied.searchPaths.reserve(options->search_path_count);
    for (size_t index = 0; index < options->search_path_count; ++index) {
        const auto path = copyBytes(
            options->search_paths[index].data,
            options->search_paths[index].size);
        if (!path || !validPathText(*path)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        copied.searchPaths.push_back(mparser::pathFromUtf8(*path));
    }
    copied.randomSeed = options->random_seed;
    if (!uint64ToSize(options->maximum_open_files,
                      copied.maximumOpenFiles) ||
        !uint64ToSize(options->maximum_file_read_bytes,
                      copied.maximumFileReadBytes)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    return MPARSER_API_STATUS_OK;
}

bool runtimeValueRequiresModule(
    const mparser::RuntimeValue& value,
    std::set<const void*>& functionHandles,
    std::set<const mparser::RuntimeTableStorage*>& tableStorages) {
    switch (value.kind) {
    case mparser::RuntimeValueKind::FunctionHandle:
        if (!value.functionHandle) {
            return false;
        }
        if (value.functionHandle->backend !=
            mparser::RuntimeFunctionHandleBackend::Independent) {
            return true;
        }
        if (!functionHandles.insert(
                 value.functionHandle.get()).second) {
            return false;
        }
        if (value.functionHandle->receiver &&
            runtimeValueRequiresModule(
                *value.functionHandle->receiver,
                functionHandles, tableStorages)) {
            return true;
        }
        for (const auto& [name, captured] :
             value.functionHandle->capturedVariables) {
            (void)name;
            if (runtimeValueRequiresModule(
                    captured, functionHandles, tableStorages)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Object:
        if (mparser::isRuntimeTabularValue(value)) {
            const auto* storage = mparser::runtimeTabularStorage(value);
            if (!storage) {
                return false;
            }
            if (!tableStorages.insert(storage).second) {
                return false;
            }
            if (mparser::isRuntimeTimetableValue(value) &&
                runtimeValueRequiresModule(
                    storage->rowTimes, functionHandles,
                    tableStorages)) {
                return true;
            }
            for (const auto& variable : storage->variables) {
                if (runtimeValueRequiresModule(
                        variable.value, functionHandles,
                        tableStorages)) {
                    return true;
                }
            }
            return runtimeValueRequiresModule(
                storage->userData, functionHandles, tableStorages);
        }
        if (mparser::isRuntimeCategoricalValue(value)) {
            // Dictionary/codes storage is immutable, value-owned, and opaque
            // to the C ABI, so it does not retain a compiled module graph.
            return false;
        }
        if (mparser::isRuntimeSparseValue(value)) {
            // CSC storage is immutable/value-owned and does not retain a
            // module graph; the C API intentionally keeps it opaque.
            return false;
        }
        if (mparser::isRuntimeTemporalValue(value)) {
            // datetime and duration are immutable value objects. They do not
            // retain a module graph and can cross a C API module boundary.
            return false;
        }
        if (mparser::isRuntimeMetadataObject(value)) {
            for (const auto& element : value.cells) {
                if (runtimeValueRequiresModule(element,
                                                functionHandles,
                                                tableStorages)) {
                    return true;
                }
            }
            return false;
        }
        return true;
    case mparser::RuntimeValueKind::Cell:
    case mparser::RuntimeValueKind::CommaSeparatedList:
    case mparser::RuntimeValueKind::NameValueArgument:
        for (const auto& element : value.cells) {
            if (runtimeValueRequiresModule(
                    element, functionHandles, tableStorages)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Struct:
        for (const auto& element : value.structElements) {
            for (const auto& [name, field] : element) {
                (void)name;
                if (runtimeValueRequiresModule(
                        field, functionHandles, tableStorages)) {
                    return true;
                }
            }
        }
        for (const auto& [name, field] : value.fields) {
            (void)name;
            if (runtimeValueRequiresModule(
                    field, functionHandles, tableStorages)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Missing:
    case mparser::RuntimeValueKind::MissingArray:
    case mparser::RuntimeValueKind::Number:
    case mparser::RuntimeValueKind::CharacterArray:
    case mparser::RuntimeValueKind::StringArray:
    case mparser::RuntimeValueKind::Vector:
    case mparser::RuntimeValueKind::Matrix:
        return false;
    }
    return false;
}

bool runtimeValueRequiresModule(
    const mparser::RuntimeValue& value) {
    std::set<const void*> functionHandles;
    std::set<const mparser::RuntimeTableStorage*> tableStorages;
    return runtimeValueRequiresModule(
        value, functionHandles, tableStorages);
}

bool externallyTransportable(
    const mparser::RuntimeValue& value) {
    if (mparser::isRuntimeMetadataObject(value)) {
        return true;
    }
    if (value.kind == mparser::RuntimeValueKind::Missing) {
        return true;
    }
    return mparser::runtimeValueIsStorable(value) &&
           value.kind !=
               mparser::RuntimeValueKind::CommaSeparatedList &&
           value.kind !=
               mparser::RuntimeValueKind::NameValueArgument;
}

std::optional<size_t> logicalStorageOffset(
    const std::vector<size_t>& dimensions,
    size_t index) noexcept {
    const auto count =
        mparser::checkedRuntimeDimensionProduct(dimensions);
    if (!count || index >= *count) {
        return std::nullopt;
    }

    size_t remaining = index;
    size_t offset = 0;
    for (size_t dimensionIndex = 0;
         dimensionIndex < dimensions.size();
         ++dimensionIndex) {
        const size_t dimension =
            dimensions[dimensionIndex];
        if (dimension == 0) {
            return std::nullopt;
        }
        const size_t coordinate =
            remaining % dimension;
        remaining /= dimension;

        size_t stride = 1;
        for (size_t suffix = dimensionIndex + 1;
             suffix < dimensions.size(); ++suffix) {
            if (dimensions[suffix] != 0 &&
                stride >
                    std::numeric_limits<size_t>::max() /
                        dimensions[suffix]) {
                return std::nullopt;
            }
            stride *= dimensions[suffix];
        }
        if (coordinate != 0 &&
            stride >
                (std::numeric_limits<size_t>::max() -
                 offset) /
                    coordinate) {
            return std::nullopt;
        }
        offset += coordinate * stride;
    }
    return remaining == 0
               ? std::optional<size_t>{offset}
               : std::nullopt;
}

std::vector<std::string> structFieldNames(
    const mparser::RuntimeValue& value) {
    if (!value.fieldOrder.empty()) {
        return value.fieldOrder;
    }
    std::vector<std::string> result;
    const mparser::RuntimeWorkspace* fields = nullptr;
    if (!value.structElements.empty()) {
        fields = &value.structElements.front();
    } else if (!value.fields.empty()) {
        fields = &value.fields;
    }
    if (fields) {
        result.reserve(fields->size());
        for (const auto& [name, field] : *fields) {
            (void)field;
            result.push_back(name);
        }
    }
    return result;
}

template <typename Element>
bool buildNumericComponentCache(
    NumericColumnMajorStorage& storage,
    const mparser::RuntimeValue& value,
    size_t count, bool imaginary) {
    std::vector<Element> elements;
    elements.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto source =
            mparser::runtimeNumericElementValue(value, index);
        if (!source) {
            return false;
        }
        if (value.numericClass ==
            mparser::RuntimeNumericClass::Logical) {
            const double component = imaginary
                                         ? source->imaginary
                                         : source->real;
            elements.push_back(
                static_cast<Element>(component != 0.0));
        } else if (mparser::runtimeNumericClassIsInteger(
                       value.numericClass)) {
            const uint64_t bits = imaginary
                                      ? source->integerImaginaryBits
                                      : source->integerRealBits;
            if (mparser::runtimeNumericClassIsSignedInteger(
                    value.numericClass)) {
                elements.push_back(static_cast<Element>(
                    std::bit_cast<int64_t>(bits)));
            } else {
                elements.push_back(static_cast<Element>(bits));
            }
        } else {
            elements.push_back(static_cast<Element>(
                imaginary ? source->imaginary : source->real));
        }
    }
    storage = std::move(elements);
    return true;
}

bool buildNumericComponentCache(
    NumericColumnMajorStorage& storage,
    const mparser::RuntimeValue& value,
    size_t count, bool imaginary) {
    switch (value.numericClass) {
    case mparser::RuntimeNumericClass::Double:
        return buildNumericComponentCache<double>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Logical:
    case mparser::RuntimeNumericClass::UInt8:
        return buildNumericComponentCache<uint8_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Single:
        return buildNumericComponentCache<float>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Int8:
        return buildNumericComponentCache<int8_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Int16:
        return buildNumericComponentCache<int16_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::UInt16:
        return buildNumericComponentCache<uint16_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Int32:
        return buildNumericComponentCache<int32_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::UInt32:
        return buildNumericComponentCache<uint32_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::Int64:
        return buildNumericComponentCache<int64_t>(
            storage, value, count, imaginary);
    case mparser::RuntimeNumericClass::UInt64:
        return buildNumericComponentCache<uint64_t>(
            storage, value, count, imaginary);
    }
    return false;
}

const void* numericStorageData(
    const NumericColumnMajorStorage& storage) {
    return std::visit(
        [](const auto& elements) -> const void* {
            using Storage = std::decay_t<decltype(elements)>;
            if constexpr (std::is_same_v<Storage, std::monostate>) {
                return nullptr;
            } else {
                return elements.data();
            }
        },
        storage);
}

bool buildExternalCaches(ValueState& state) {
    const auto& value = state.value;
    state.dimensions = mparser::runtimeDimensions(value);
    const auto count = mparser::checkedRuntimeDimensionProduct(
        state.dimensions);
    if (!count) {
        return false;
    }
    state.elementCount = *count;
    if (mparser::isRuntimeNumericValue(value)) {
        if (!buildNumericComponentCache(
                state.numericRealColumnMajor, value, *count, false)) {
            return false;
        }
        if (value.numericComplex &&
            !buildNumericComponentCache(
                state.numericImaginaryColumnMajor, value, *count, true)) {
            return false;
        }
    } else if (
        value.kind ==
        mparser::RuntimeValueKind::CharacterArray) {
        state.characterColumnMajor.resize(*count);
        for (size_t index = 0; index < *count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    state.dimensions, index);
            if (!offset ||
                *offset >= value.characterElements.size()) {
                return false;
            }
            state.characterColumnMajor[index] =
                static_cast<uint16_t>(
                    value.characterElements[*offset]);
        }
    } else if (
        value.kind == mparser::RuntimeValueKind::StringArray) {
        state.stringColumnMajor.resize(*count);
        for (size_t index = 0; index < *count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    state.dimensions, index);
            if (!offset ||
                *offset >= value.stringElements.size()) {
                return false;
            }
            const auto& source =
                value.stringElements[*offset].value;
            auto& destination =
                state.stringColumnMajor[index];
            destination.reserve(source.size());
            for (const char16_t codeUnit : source) {
                destination.push_back(
                    static_cast<uint16_t>(codeUnit));
            }
        }
    } else if (
        value.kind == mparser::RuntimeValueKind::Struct) {
        state.structFieldNames = structFieldNames(value);
    } else if (
        value.kind ==
        mparser::RuntimeValueKind::FunctionHandle) {
        state.functionText =
            mparser::runtimeFunctionHandleText(value);
    }
    return true;
}

template <typename RuntimeValue>
mparser_api_status makeValueHandle(
    RuntimeValue&& value,
    std::shared_ptr<ModuleState> owner,
    mparser_value** outValue,
    std::shared_ptr<RuntimeState> runtimeOwner = {}) {
    if (!outValue) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outValue = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ValuePublish);
        std::unique_lock<std::recursive_mutex> sharedGraphLock;
        std::unique_lock<std::recursive_mutex> runtimeLock;
        const bool requiresModule =
            (owner || runtimeOwner) && runtimeValueRequiresModule(value);
        if (requiresModule && runtimeOwner) {
            runtimeLock = std::unique_lock<std::recursive_mutex>(
                runtimeOwner->mutex);
        } else if (requiresModule) {
            sharedGraphLock =
                std::unique_lock<std::recursive_mutex>(
                    owner->sharedGraphMutex);
        }
        if (!externallyTransportable(value)) {
            return MPARSER_API_STATUS_TYPE_MISMATCH;
        }
        const auto contract =
            mparser::validateRuntimeValueContract(value);
        if (!contract.valid) {
            return MPARSER_API_STATUS_INTERNAL_ERROR;
        }
        auto state = std::make_shared<ValueState>();
        state->value =
            std::forward<RuntimeValue>(value);
        if (requiresModule) {
            state->owner = std::move(owner);
            state->runtimeOwner = std::move(runtimeOwner);
        }
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExternalValueCaches);
        if (!buildExternalCaches(*state)) {
            return MPARSER_API_STATUS_INTERNAL_ERROR;
        }
        auto handle = std::make_unique<mparser_value>();
        handle->state = std::move(state);
        *outValue = handle.release();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status copyDimensions(
    const size_t* dimensions,
    size_t rank,
    size_t elementCount,
    std::vector<size_t>& result) {
    if (!dimensions || rank < 2) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    result.assign(dimensions, dimensions + rank);
    const auto product =
        mparser::checkedRuntimeDimensionProduct(result);
    if (!product || *product != elementCount) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    return MPARSER_API_STATUS_OK;
}

bool numericClass(
    mparser_numeric_class value,
    mparser::RuntimeNumericClass& result) noexcept {
    if (value == MPARSER_NUMERIC_DOUBLE) {
        result = mparser::RuntimeNumericClass::Double;
        return true;
    }
    if (value == MPARSER_NUMERIC_LOGICAL) {
        result = mparser::RuntimeNumericClass::Logical;
        return true;
    }
    if (value == MPARSER_NUMERIC_SINGLE) {
        result = mparser::RuntimeNumericClass::Single;
        return true;
    }
    if (value == MPARSER_NUMERIC_INT8) {
        result = mparser::RuntimeNumericClass::Int8;
        return true;
    }
    if (value == MPARSER_NUMERIC_UINT8) {
        result = mparser::RuntimeNumericClass::UInt8;
        return true;
    }
    if (value == MPARSER_NUMERIC_INT16) {
        result = mparser::RuntimeNumericClass::Int16;
        return true;
    }
    if (value == MPARSER_NUMERIC_UINT16) {
        result = mparser::RuntimeNumericClass::UInt16;
        return true;
    }
    if (value == MPARSER_NUMERIC_INT32) {
        result = mparser::RuntimeNumericClass::Int32;
        return true;
    }
    if (value == MPARSER_NUMERIC_UINT32) {
        result = mparser::RuntimeNumericClass::UInt32;
        return true;
    }
    if (value == MPARSER_NUMERIC_INT64) {
        result = mparser::RuntimeNumericClass::Int64;
        return true;
    }
    if (value == MPARSER_NUMERIC_UINT64) {
        result = mparser::RuntimeNumericClass::UInt64;
        return true;
    }
    return false;
}

template <typename Element>
std::vector<mparser::RuntimeNumericElementValue>
copyNumericBufferElements(
    const mparser_numeric_buffer& buffer,
    mparser::RuntimeNumericClass numericClass) {
    const auto* real = static_cast<const Element*>(buffer.real_data);
    const auto* imaginary =
        static_cast<const Element*>(buffer.imaginary_data);
    std::vector<mparser::RuntimeNumericElementValue> elements;
    elements.reserve(buffer.element_count);
    for (size_t index = 0; index < buffer.element_count; ++index) {
        mparser::RuntimeNumericElementValue element;
        element.numericClass = numericClass;
        element.complex = buffer.is_complex != 0;
        if (numericClass == mparser::RuntimeNumericClass::Logical) {
            element.real = real[index] != 0 ? 1.0 : 0.0;
            element.imaginary =
                element.complex && imaginary[index] != 0 ? 1.0 : 0.0;
        } else if (mparser::runtimeNumericClassIsInteger(numericClass)) {
            if (mparser::runtimeNumericClassIsSignedInteger(numericClass)) {
                element.integerRealBits = std::bit_cast<uint64_t>(
                    static_cast<int64_t>(real[index]));
                if (element.complex) {
                    element.integerImaginaryBits = std::bit_cast<uint64_t>(
                        static_cast<int64_t>(imaginary[index]));
                }
            } else {
                element.integerRealBits =
                    static_cast<uint64_t>(real[index]);
                if (element.complex) {
                    element.integerImaginaryBits =
                        static_cast<uint64_t>(imaginary[index]);
                }
            }
        } else {
            element.real = static_cast<double>(real[index]);
            if (element.complex) {
                element.imaginary =
                    static_cast<double>(imaginary[index]);
            }
        }
        elements.push_back(element);
    }
    return elements;
}

std::optional<std::vector<mparser::RuntimeNumericElementValue>>
copyNumericBufferElements(
    const mparser_numeric_buffer& buffer,
    mparser::RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case mparser::RuntimeNumericClass::Double:
        return copyNumericBufferElements<double>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Logical:
    case mparser::RuntimeNumericClass::UInt8:
        return copyNumericBufferElements<uint8_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Single:
        return copyNumericBufferElements<float>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Int8:
        return copyNumericBufferElements<int8_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Int16:
        return copyNumericBufferElements<int16_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::UInt16:
        return copyNumericBufferElements<uint16_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Int32:
        return copyNumericBufferElements<int32_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::UInt32:
        return copyNumericBufferElements<uint32_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::Int64:
        return copyNumericBufferElements<int64_t>(buffer, numericClass);
    case mparser::RuntimeNumericClass::UInt64:
        return copyNumericBufferElements<uint64_t>(buffer, numericClass);
    }
    return std::nullopt;
}

mparser_api_status ownerForComposedValues(
    const mparser_value* const* values,
    size_t count,
    std::shared_ptr<ModuleState>& owner,
    std::shared_ptr<RuntimeState>& runtimeOwner) {
    owner.reset();
    runtimeOwner.reset();
    if (!values && count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!values[index] || !values[index]->state) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        const auto& candidateOwner = values[index]->state->owner;
        const auto& candidateRuntime =
            values[index]->state->runtimeOwner;
        if (candidateOwner) {
            if (runtimeOwner ||
                (owner && owner != candidateOwner)) {
                return MPARSER_API_STATUS_OWNER_MISMATCH;
            }
            owner = candidateOwner;
        }
        if (candidateRuntime) {
            if (owner ||
                (runtimeOwner && runtimeOwner != candidateRuntime)) {
                return MPARSER_API_STATUS_OWNER_MISMATCH;
            }
            runtimeOwner = candidateRuntime;
        }
    }
    return MPARSER_API_STATUS_OK;
}

bool ownerCompatible(
    const mparser_value* value,
    const std::shared_ptr<ModuleState>& module) noexcept {
    return value && value->state &&
           !value->state->runtimeOwner &&
           (!value->state->owner ||
            value->state->owner == module);
}

bool ownerCompatible(
    const mparser_value* value,
    const std::shared_ptr<RuntimeState>& runtime) noexcept {
    return value && value->state && !value->state->owner &&
           (!value->state->runtimeOwner ||
            value->state->runtimeOwner == runtime);
}

std::optional<mparser::ModuleExecutionBackend> backend(
    mparser_backend value) noexcept {
    switch (value) {
    case MPARSER_BACKEND_AUTOMATIC:
        return mparser::ModuleExecutionBackend::Automatic;
    case MPARSER_BACKEND_BYTECODE:
        return mparser::ModuleExecutionBackend::Bytecode;
    case MPARSER_BACKEND_PORTABLE:
        return mparser::ModuleExecutionBackend::Portable;
    case MPARSER_BACKEND_NATIVE:
        return mparser::ModuleExecutionBackend::Native;
    default:
        return std::nullopt;
    }
}

mparser_source_kind externalSourceKind(
    mparser::CompiledSourceKind value) noexcept {
    switch (value) {
    case mparser::CompiledSourceKind::Unknown:
        return MPARSER_SOURCE_UNKNOWN;
    case mparser::CompiledSourceKind::Script:
        return MPARSER_SOURCE_SCRIPT;
    case mparser::CompiledSourceKind::Function:
        return MPARSER_SOURCE_FUNCTION;
    case mparser::CompiledSourceKind::Class:
        return MPARSER_SOURCE_CLASS;
    }
    return MPARSER_SOURCE_UNKNOWN;
}

mparser_output_kind externalOutputKind(
    mparser::ModuleOutputKind value) noexcept {
    return value == mparser::ModuleOutputKind::Display
               ? MPARSER_OUTPUT_DISPLAY
               : MPARSER_OUTPUT_STANDARD;
}

template <typename OwnerState>
mparser_api_status buildRequest(
    const mparser_invocation_options* options,
    const std::shared_ptr<OwnerState>& owner,
    mparser::ModuleInvocationRequest& request,
    bool& usesSharedGraph) {
    usesSharedGraph = false;
    if (!options) {
        return MPARSER_API_STATUS_OK;
    }
    if (options->struct_size <
            MPARSER_INVOCATION_OPTIONS_SIZE ||
        options->abi_generation != MPARSER_C_ABI_GENERATION) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    if (options->has_requested_output_count > 1 ||
        options->collect_profile > 1 ||
        (!options->arguments &&
         options->argument_count != 0) ||
        (!options->initial_workspace &&
         options->initial_workspace_count != 0)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    const auto entry = copyBytes(
        options->entry_name, options->entry_name_size);
    if (!entry) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    const auto selectedBackend = backend(options->backend);
    if (!selectedBackend) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    request.entryFunction = *entry;
    request.backend = *selectedBackend;
    request.collectProfile = options->collect_profile != 0;
    if (options->has_requested_output_count != 0) {
        request.requestedOutputCount =
            options->requested_output_count;
    }
    request.arguments.reserve(options->argument_count);
    for (size_t index = 0;
         index < options->argument_count; ++index) {
        const auto* value = options->arguments[index];
        if (!ownerCompatible(value, owner)) {
            return value && value->state
                       ? MPARSER_API_STATUS_OWNER_MISMATCH
                       : MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        usesSharedGraph =
            usesSharedGraph ||
            static_cast<bool>(value->state->owner) ||
            static_cast<bool>(value->state->runtimeOwner);
        request.arguments.push_back(value->state->value);
    }
    request.initialWorkspace.reserve(
        options->initial_workspace_count);
    for (size_t index = 0;
         index < options->initial_workspace_count; ++index) {
        const auto& variable =
            options->initial_workspace[index];
        const auto name =
            copyBytes(variable.name, variable.name_size);
        if (!name || name->empty() ||
            !ownerCompatible(variable.value, owner)) {
            if (variable.value && variable.value->state) {
                return MPARSER_API_STATUS_OWNER_MISMATCH;
            }
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        usesSharedGraph =
            usesSharedGraph ||
            static_cast<bool>(variable.value->state->owner) ||
            static_cast<bool>(variable.value->state->runtimeOwner);
        request.initialWorkspace.push_back(
            mparser::RuntimeVariable{
                *name, variable.value->state->value});
    }

    if (!uint64ToSize(
            options->max_instruction_count,
            request.limits.maxInstructionCount) ||
        !uint64ToSize(
            options->max_call_depth,
            request.limits.maxCallDepth) ||
        !uint64ToSize(
            options->max_array_bytes,
            request.limits.maxArrayBytes) ||
        !uint64ToSize(
            options->max_diagnostic_count,
            request.limits.maxDiagnosticCount)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    using Nanoseconds = std::chrono::nanoseconds;
    using Rep = Nanoseconds::rep;
    if (options->max_wall_time_nanoseconds >
        static_cast<uint64_t>(
            std::numeric_limits<Rep>::max())) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    request.limits.maxWallTime = Nanoseconds(
        static_cast<Rep>(
            options->max_wall_time_nanoseconds));
    if (options->cancellation_token) {
        request.cancellationToken =
            options->cancellation_token->token;
    }
    if (options->output_sink) {
        const auto sink = options->output_sink;
        void* const userData = options->output_user_data;
        request.outputSink =
            [sink, userData](const mparser::ModuleOutputEvent& event) {
                const auto sourceName =
                    event.source.available
                        ? std::string_view(event.source.sourceName)
                        : std::string_view{};
                return sink(
                           userData, event.sequence,
                           externalOutputKind(event.kind),
                           event.text.data(), event.text.size(),
                           sourceName.data(), sourceName.size(),
                           event.source.available
                               ? sourcePosition(event.source.begin)
                               : mparser_source_position{},
                           event.source.available
                               ? sourcePosition(event.source.end)
                               : mparser_source_position{}) ==
                       MPARSER_OUTPUT_ACCEPT;
            };
    }
    return MPARSER_API_STATUS_OK;
}

mparser_api_status makeResultHandle(
    mparser::ModuleInvocationResult result,
    std::shared_ptr<ModuleState> owner,
    mparser_result** outResult,
    std::shared_ptr<RuntimeState> runtimeOwner = {}) {
    if (!outResult) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outResult = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ResultPublish);
        auto handle = std::make_unique<mparser_result>();
        handle->value = std::move(result);
        handle->owner = std::move(owner);
        handle->runtimeOwner = std::move(runtimeOwner);
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::DiagnosticCopy);
        handle->diagnostics.reserve(
            handle->value.diagnostics.size());
        for (const auto& diagnostic :
             handle->value.diagnostics) {
            handle->diagnostics.push_back(
                externalDiagnostic(diagnostic));
        }
        *outResult = handle.release();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status executeModule(
    const mparser_module* module,
    const mparser_invocation_options* options,
    std::shared_ptr<mparser::RuntimeSystemContext> systemContext,
    mparser_result** outResult) {
    if (!outResult) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outResult = nullptr;
    if (!module || !module->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        mparser::ModuleInvocationRequest request;
        bool usesSharedGraph = false;
        const auto requestStatus = buildRequest(
            options, module->state, request, usesSharedGraph);
        if (requestStatus != MPARSER_API_STATUS_OK) {
            return requestStatus;
        }
        request.systemContext = std::move(systemContext);
        std::unique_lock<std::recursive_mutex> sharedGraphLock;
        if (usesSharedGraph) {
            sharedGraphLock =
                std::unique_lock<std::recursive_mutex>(
                    module->state->sharedGraphMutex);
        }
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteBeforeCore);
        auto result = module->state->module.execute(request);
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteAfterCore);
        return makeResultHandle(
            std::move(result), module->state, outResult);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status executeRuntime(
    mparser_runtime* runtime,
    const mparser_module* module,
    const mparser_invocation_options* options,
    mparser_result** outResult) {
    if (!outResult) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outResult = nullptr;
    if (!runtime || !runtime->state || !module || !module->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (!module->state->module.valid()) {
        return MPARSER_API_STATUS_COMPILATION_FAILED;
    }
    try {
        std::lock_guard lock(runtime->state->mutex);
        const size_t contextIdentity =
            module->state->module.callableContextIdentity();
        if (contextIdentity == 0) {
            return MPARSER_API_STATUS_INTERNAL_ERROR;
        }
        runtime->state->modules[contextIdentity] = module->state;

        mparser::ModuleInvocationRequest request;
        bool usesSharedGraph = false;
        const auto requestStatus = buildRequest(
            options, runtime->state, request, usesSharedGraph);
        (void)usesSharedGraph;
        if (requestStatus != MPARSER_API_STATUS_OK) {
            return requestStatus;
        }
        request.executionControl =
            std::make_shared<mparser::RuntimeExecutionControl>(
                request.limits, request.cancellationToken);
        request.externalCallableInvoker =
            runtime->state->callableInvoker(
                request.backend, request.executionControl);

        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteBeforeCore);
        auto session = module->state->module.createSession(
            runtime->state->session);
        auto result = session.execute(request);
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteAfterCore);
        return makeResultHandle(
            std::move(result), {}, outResult, runtime->state);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status createSession(
    const mparser_module* module,
    std::shared_ptr<mparser::RuntimeSystemContext> systemContext,
    mparser_session** outSession) {
    if (!outSession) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outSession = nullptr;
    if (!module || !module->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (!module->state->module.valid()) {
        return MPARSER_API_STATUS_COMPILATION_FAILED;
    }
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::SessionCreate);
        auto runtimeState =
            std::make_shared<mparser::RuntimeSessionState>(
                std::move(systemContext));
        auto state = std::make_shared<SessionState>(
            module->state,
            module->state->module.createSession(
                std::move(runtimeState)));
        auto* handle = new mparser_session;
        handle->state = std::move(state);
        *outSession = handle;
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_invocation_status invocationStatus(
    mparser::ModuleInvocationStatus status) noexcept {
    switch (status) {
    case mparser::ModuleInvocationStatus::Succeeded:
        return MPARSER_INVOCATION_SUCCEEDED;
    case mparser::ModuleInvocationStatus::CompilationFailed:
        return MPARSER_INVOCATION_COMPILATION_FAILED;
    case mparser::ModuleInvocationStatus::RequestRejected:
        return MPARSER_INVOCATION_REQUEST_REJECTED;
    case mparser::ModuleInvocationStatus::RuntimeFailed:
        return MPARSER_INVOCATION_RUNTIME_FAILED;
    }
    return MPARSER_INVOCATION_RUNTIME_FAILED;
}

mparser_backend externalBackend(
    mparser::ModuleExecutionBackend value) noexcept {
    switch (value) {
    case mparser::ModuleExecutionBackend::Automatic:
        return MPARSER_BACKEND_AUTOMATIC;
    case mparser::ModuleExecutionBackend::Bytecode:
        return MPARSER_BACKEND_BYTECODE;
    case mparser::ModuleExecutionBackend::Portable:
        return MPARSER_BACKEND_PORTABLE;
    case mparser::ModuleExecutionBackend::Native:
        return MPARSER_BACKEND_NATIVE;
    }
    return MPARSER_BACKEND_AUTOMATIC;
}

mparser_execution_tier externalTier(
    mparser::ModuleExecutionTier value) noexcept {
    switch (value) {
    case mparser::ModuleExecutionTier::Bytecode:
        return MPARSER_EXECUTION_TIER_BYTECODE;
    case mparser::ModuleExecutionTier::Portable:
        return MPARSER_EXECUTION_TIER_PORTABLE;
    case mparser::ModuleExecutionTier::Native:
        return MPARSER_EXECUTION_TIER_NATIVE;
    case mparser::ModuleExecutionTier::Mixed:
        return MPARSER_EXECUTION_TIER_MIXED;
    }
    return MPARSER_EXECUTION_TIER_BYTECODE;
}

mparser_stop_reason externalStopReason(
    mparser::RuntimeExecutionStopReason value) noexcept {
    switch (value) {
    case mparser::RuntimeExecutionStopReason::None:
        return MPARSER_STOP_NONE;
    case mparser::RuntimeExecutionStopReason::Cancelled:
        return MPARSER_STOP_CANCELLED;
    case mparser::RuntimeExecutionStopReason::InstructionLimit:
        return MPARSER_STOP_INSTRUCTION_LIMIT;
    case mparser::RuntimeExecutionStopReason::WallTimeLimit:
        return MPARSER_STOP_WALL_TIME_LIMIT;
    case mparser::RuntimeExecutionStopReason::CallDepthLimit:
        return MPARSER_STOP_CALL_DEPTH_LIMIT;
    case mparser::RuntimeExecutionStopReason::ArrayByteLimit:
        return MPARSER_STOP_ARRAY_BYTE_LIMIT;
    case mparser::RuntimeExecutionStopReason::DiagnosticLimit:
        return MPARSER_STOP_DIAGNOSTIC_LIMIT;
    }
    return MPARSER_STOP_NONE;
}

mparser_value_kind valueKind(
    const mparser::RuntimeValue& value) noexcept {
    switch (value.kind) {
    case mparser::RuntimeValueKind::Missing:
    case mparser::RuntimeValueKind::MissingArray:
        return MPARSER_VALUE_MISSING;
    case mparser::RuntimeValueKind::Number:
    case mparser::RuntimeValueKind::Vector:
    case mparser::RuntimeValueKind::Matrix:
        return MPARSER_VALUE_NUMERIC;
    case mparser::RuntimeValueKind::CharacterArray:
        return MPARSER_VALUE_CHARACTER;
    case mparser::RuntimeValueKind::StringArray:
        return MPARSER_VALUE_STRING;
    case mparser::RuntimeValueKind::Cell:
        return MPARSER_VALUE_CELL;
    case mparser::RuntimeValueKind::Struct:
        return MPARSER_VALUE_STRUCT;
    case mparser::RuntimeValueKind::Object:
        return MPARSER_VALUE_OBJECT;
    case mparser::RuntimeValueKind::FunctionHandle:
        return MPARSER_VALUE_FUNCTION_HANDLE;
    case mparser::RuntimeValueKind::CommaSeparatedList:
    case mparser::RuntimeValueKind::NameValueArgument:
        return MPARSER_VALUE_MISSING;
    }
    return MPARSER_VALUE_MISSING;
}

const mparser::RuntimeWorkspace* structElement(
    const mparser::RuntimeValue& value,
    const std::vector<size_t>& dimensions,
    size_t logicalIndex) {
    const auto offset =
        logicalStorageOffset(dimensions, logicalIndex);
    if (!offset) {
        return nullptr;
    }
    if (!value.structElements.empty()) {
        return *offset < value.structElements.size()
                   ? &value.structElements[*offset]
                   : nullptr;
    }
    if (*offset == 0) {
        return &value.fields;
    }
    return nullptr;
}

template <typename Structure>
mparser_api_status initializeVersionedStructure(
    void* storage, size_t storageSize, size_t minimumSize,
    uint32_t abiGeneration) noexcept {
    if (!storage) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (abiGeneration != MPARSER_C_ABI_GENERATION ||
        storageSize < minimumSize ||
        storageSize > std::numeric_limits<uint32_t>::max()) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    std::memset(storage, 0, storageSize);
    auto* structure = static_cast<Structure*>(storage);
    structure->struct_size = static_cast<uint32_t>(storageSize);
    structure->abi_generation = abiGeneration;
    return MPARSER_API_STATUS_OK;
}

static_assert(
    sizeof(mparser_invocation_options) >=
    MPARSER_INVOCATION_OPTIONS_SIZE);
static_assert(
    sizeof(mparser_execution_summary) >=
    MPARSER_EXECUTION_SUMMARY_SIZE);
static_assert(
    sizeof(mparser_source_load_options) >=
    MPARSER_SOURCE_LOAD_OPTIONS_SIZE);
static_assert(
    sizeof(mparser_system_context_options) >=
    MPARSER_SYSTEM_CONTEXT_OPTIONS_SIZE);

} // namespace
} // namespace mparser_c_detail

extern "C" {

uint32_t mparser_c_abi_generation(void) {
    return MPARSER_C_ABI_GENERATION;
}

uint32_t mparser_c_abi_revision(void) {
    return MPARSER_C_ABI_REVISION;
}

uint32_t mparser_version_major(void) {
    return static_cast<uint32_t>(MPARSER_VERSION_MAJOR);
}

uint32_t mparser_version_minor(void) {
    return static_cast<uint32_t>(MPARSER_VERSION_MINOR);
}

uint32_t mparser_version_patch(void) {
    return static_cast<uint32_t>(MPARSER_VERSION_PATCH);
}

mparser_utf8_view
mparser_api_status_name(mparser_api_status status) {
    using mparser_c_detail::utf8View;
    switch (status) {
    case MPARSER_API_STATUS_OK:
        return utf8View("ok");
    case MPARSER_API_STATUS_INVALID_ARGUMENT:
        return utf8View("invalid-argument");
    case MPARSER_API_STATUS_OUT_OF_RANGE:
        return utf8View("out-of-range");
    case MPARSER_API_STATUS_TYPE_MISMATCH:
        return utf8View("type-mismatch");
    case MPARSER_API_STATUS_OWNER_MISMATCH:
        return utf8View("owner-mismatch");
    case MPARSER_API_STATUS_COMPILATION_FAILED:
        return utf8View("compilation-failed");
    case MPARSER_API_STATUS_ALLOCATION_FAILED:
        return utf8View("allocation-failed");
    case MPARSER_API_STATUS_INTERNAL_ERROR:
        return utf8View("internal-error");
    case MPARSER_API_STATUS_ABI_MISMATCH:
        return utf8View("abi-mismatch");
    case MPARSER_API_STATUS_SOURCE_LOAD_FAILED:
        return utf8View("source-load-failed");
    case MPARSER_API_STATUS_SYSTEM_CONTEXT_FAILED:
        return utf8View("system-context-failed");
    default:
        return utf8View("unknown");
    }
}

mparser_api_status
mparser_invocation_options_init(
    mparser_invocation_options* options) {
    return mparser_invocation_options_init_sized(
        options, MPARSER_INVOCATION_OPTIONS_SIZE,
        MPARSER_C_ABI_GENERATION);
}

mparser_api_status mparser_invocation_options_init_sized(
    void* storage, size_t storage_size,
    uint32_t abi_generation) {
    const auto status =
        mparser_c_detail::initializeVersionedStructure<
            mparser_invocation_options>(
            storage, storage_size,
            MPARSER_INVOCATION_OPTIONS_SIZE, abi_generation);
    if (status == MPARSER_API_STATUS_OK) {
        auto* options =
            static_cast<mparser_invocation_options*>(storage);
        options->backend = MPARSER_BACKEND_AUTOMATIC;
    }
    return status;
}

mparser_api_status
mparser_execution_summary_init(
    mparser_execution_summary* summary) {
    return mparser_execution_summary_init_sized(
        summary, MPARSER_EXECUTION_SUMMARY_SIZE,
        MPARSER_C_ABI_GENERATION);
}

mparser_api_status mparser_execution_summary_init_sized(
    void* storage, size_t storage_size,
    uint32_t abi_generation) {
    return mparser_c_detail::initializeVersionedStructure<
        mparser_execution_summary>(
        storage, storage_size,
        MPARSER_EXECUTION_SUMMARY_SIZE, abi_generation);
}

mparser_api_status
mparser_source_unit_init(mparser_source_unit* source) {
    if (!source) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    static_assert(sizeof(*source) == MPARSER_SOURCE_UNIT_SIZE);
    *source = {};
    source->struct_size = MPARSER_SOURCE_UNIT_SIZE;
    source->abi_generation = MPARSER_C_ABI_GENERATION;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status mparser_source_load_options_init(
    mparser_source_load_options* options) {
    return mparser_source_load_options_init_sized(
        options, MPARSER_SOURCE_LOAD_OPTIONS_SIZE,
        MPARSER_C_ABI_GENERATION);
}

mparser_api_status mparser_source_load_options_init_sized(
    void* storage, size_t storage_size,
    uint32_t abi_generation) {
    return mparser_c_detail::initializeVersionedStructure<
        mparser_source_load_options>(
        storage, storage_size,
        MPARSER_SOURCE_LOAD_OPTIONS_SIZE, abi_generation);
}

mparser_api_status mparser_system_context_options_init(
    mparser_system_context_options* options) {
    return mparser_system_context_options_init_sized(
        options, MPARSER_SYSTEM_CONTEXT_OPTIONS_SIZE,
        MPARSER_C_ABI_GENERATION);
}

mparser_api_status mparser_system_context_options_init_sized(
    void* storage, size_t storage_size,
    uint32_t abi_generation) {
    const auto status =
        mparser_c_detail::initializeVersionedStructure<
            mparser_system_context_options>(
            storage, storage_size,
            MPARSER_SYSTEM_CONTEXT_OPTIONS_SIZE, abi_generation);
    if (status == MPARSER_API_STATUS_OK) {
        auto* options =
            static_cast<mparser_system_context_options*>(storage);
        options->random_seed = 5489U;
        options->maximum_open_files = 256U;
        options->maximum_file_read_bytes = 16U * 1024U * 1024U;
    }
    return status;
}

mparser_api_status mparser_system_context_create_rooted_native(
    const mparser_system_context_options* options,
    mparser_system_context** out_context) {
    using namespace mparser_c_detail;
    if (!out_context) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_context = nullptr;
    try {
        mparser::RuntimeRootedSystemContextOptions copied;
        const auto copyStatus =
            copySystemContextOptions(options, copied);
        if (copyStatus != MPARSER_API_STATUS_OK) {
            return copyStatus;
        }
        auto created = mparser::makeRootedNativeRuntimeSystemContext(
            std::move(copied));
        if (!created.succeeded || !created.value) {
            return MPARSER_API_STATUS_SYSTEM_CONTEXT_FAILED;
        }
        auto* handle = new mparser_system_context;
        handle->state = std::move(created.value);
        *out_context = handle;
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_system_context_retain(mparser_system_context* context) {
    mparser_c_detail::retainHandle(context);
}

void mparser_system_context_release(mparser_system_context* context) {
    mparser_c_detail::releaseHandle(context);
}

mparser_system_capability mparser_system_context_capabilities(
    const mparser_system_context* context) {
    return context && context->state
               ? static_cast<mparser_system_capability>(
                     context->state->capabilities())
               : MPARSER_SYSTEM_CAPABILITY_NONE;
}

mparser_api_status mparser_runtime_create(
    const mparser_system_context* context,
    mparser_runtime** out_runtime) {
    if (!out_runtime) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_runtime = nullptr;
    if (context && !context->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        auto* handle = new mparser_runtime;
        handle->state =
            std::make_shared<mparser_c_detail::RuntimeState>(
                context ? context->state : nullptr);
        *out_runtime = handle;
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_runtime_retain(mparser_runtime* runtime) {
    mparser_c_detail::retainHandle(runtime);
}

void mparser_runtime_release(mparser_runtime* runtime) {
    mparser_c_detail::releaseHandle(runtime);
}

mparser_api_status mparser_module_compile_utf8(
    const char* source,
    size_t source_size,
    const char* source_name,
    size_t source_name_size,
    mparser_module** out_module) {
    using namespace mparser_c_detail;
    if (!out_module) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_module = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::SourceCopy);
        const auto sourceText =
            copyBytes(source, source_size);
        const auto sourceName =
            copyBytes(source_name, source_name_size);
        if (!sourceText || !sourceName) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        std::vector<mparser::SourceUnit> sources;
        sources.push_back(mparser::SourceUnit{
            sourceName->empty() ? "<memory>" : *sourceName,
            *sourceText});
        return publishCompiledModule(
            mparser::CompiledModule::compile(
                std::move(sources)),
            out_module);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_module_compile_sources(
    const mparser_source_unit* sources,
    size_t source_count,
    mparser_module** out_module) {
    using namespace mparser_c_detail;
    if (!out_module) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_module = nullptr;
    if (!sources || source_count == 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::SourceCopy);
        std::vector<mparser::SourceUnit> copiedSources;
        copiedSources.reserve(source_count);
        for (size_t index = 0; index < source_count; ++index) {
            const auto& source = sources[index];
            if (source.struct_size != MPARSER_SOURCE_UNIT_SIZE ||
                source.abi_generation != MPARSER_C_ABI_GENERATION) {
                return MPARSER_API_STATUS_ABI_MISMATCH;
            }
            const auto sourceName = copyBytes(
                source.source_name, source.source_name_size);
            const auto sourceText =
                copyBytes(source.source, source.source_size);
            if (!sourceName || !sourceText) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
            copiedSources.push_back(mparser::SourceUnit{
                *sourceName, *sourceText});
        }
        return publishCompiledModule(
            mparser::CompiledModule::compile(
                std::move(copiedSources)),
            out_module);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_module_compile_utf8_with_options(
    const char* source,
    size_t source_size,
    const char* source_name,
    size_t source_name_size,
    const mparser_source_load_options* options,
    mparser_module** out_module) {
    using namespace mparser_c_detail;
    if (!out_module) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_module = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::SourceCopy);
        const auto sourceText = copyBytes(source, source_size);
        const auto copiedSourceName =
            copyBytes(source_name, source_name_size);
        if (!sourceText || !copiedSourceName) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }

        const std::string effectiveSourceName =
            copiedSourceName->empty() ? "<memory>.m" : *copiedSourceName;
        if (!validPathText(effectiveSourceName)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        mparser::SourceLoaderOptions loaderOptions;
        const auto optionsStatus =
            copySourceLoadOptions(options, loaderOptions);
        if (optionsStatus != MPARSER_API_STATUS_OK) {
            return optionsStatus;
        }

        mparser::SourceLoaderResult loaded;
        try {
            loaded = mparser::SourceLoader{}.loadSource(
                mparser::pathFromUtf8(effectiveSourceName),
                *sourceText, loaderOptions);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            return publishSourceLoadFailure(
                effectiveSourceName, error.what(), out_module);
        }
        return publishCompiledModule(
            mparser::CompiledModule::compile(
                std::move(loaded.sources)),
            out_module);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_module_load_file_utf8(
    const char* entry_path,
    size_t entry_path_size,
    const mparser_source_load_options* options,
    mparser_module** out_module) {
    using namespace mparser_c_detail;
    if (!out_module) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_module = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::SourceCopy);
        const auto entryText =
            copyBytes(entry_path, entry_path_size);
        if (!entryText || !validPathText(*entryText)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }

        mparser::SourceLoaderOptions loaderOptions;
        const auto optionsStatus =
            copySourceLoadOptions(options, loaderOptions);
        if (optionsStatus != MPARSER_API_STATUS_OK) {
            return optionsStatus;
        }

        mparser::SourceLoaderResult loaded;
        try {
            loaded = mparser::SourceLoader{}.load(
                mparser::pathFromUtf8(*entryText), loaderOptions);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            return publishSourceLoadFailure(
                *entryText, error.what(), out_module);
        }
        return publishCompiledModule(
            mparser::CompiledModule::compile(
                std::move(loaded.sources)),
            out_module);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_module_retain(mparser_module* module) {
    mparser_c_detail::retainHandle(module);
}

void mparser_module_release(mparser_module* module) {
    mparser_c_detail::releaseHandle(module);
}

uint32_t
mparser_module_is_valid(const mparser_module* module) {
    return module && module->state &&
                   module->state->module.valid()
               ? 1u
               : 0u;
}

size_t
mparser_module_source_count(const mparser_module* module) {
    return module && module->state
               ? module->state->module.sources().size()
               : 0;
}

mparser_utf8_view
mparser_module_source_name(
    const mparser_module* module, size_t index) {
    if (!module || !module->state ||
        index >= module->state->module.sources().size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        module->state->module.sourceName(index));
}

mparser_source_kind
mparser_module_source_kind(
    const mparser_module* module, size_t index) {
    if (!module || !module->state) {
        return MPARSER_SOURCE_UNKNOWN;
    }
    const auto* info = module->state->module.sourceInfo(index);
    return info ? mparser_c_detail::externalSourceKind(info->kind)
                : MPARSER_SOURCE_UNKNOWN;
}

mparser_utf8_view
mparser_module_source_primary_function(
    const mparser_module* module, size_t index) {
    if (!module || !module->state) {
        return mparser_c_detail::emptyUtf8View();
    }
    const auto* info = module->state->module.sourceInfo(index);
    return info
               ? mparser_c_detail::utf8View(info->primaryFunction)
               : mparser_c_detail::emptyUtf8View();
}

uint32_t
mparser_module_source_has_top_level_statements(
    const mparser_module* module, size_t index) {
    if (!module || !module->state) {
        return 0u;
    }
    const auto* info = module->state->module.sourceInfo(index);
    return info && info->hasTopLevelStatements ? 1u : 0u;
}

uint32_t
mparser_module_source_is_pure_function_file(
    const mparser_module* module, size_t index) {
    if (!module || !module->state) {
        return 0u;
    }
    const auto* info = module->state->module.sourceInfo(index);
    return info && info->pureFunctionFile() ? 1u : 0u;
}

size_t
mparser_module_diagnostic_count(
    const mparser_module* module) {
    return module && module->state
               ? module->state->diagnostics.size()
               : 0;
}

const mparser_diagnostic*
mparser_module_diagnostic(
    const mparser_module* module, size_t index) {
    if (!module || !module->state ||
        index >= module->state->diagnostics.size()) {
        return nullptr;
    }
    return &module->state->diagnostics[index];
}

size_t
mparser_module_function_count(
    const mparser_module* module) {
    return module && module->state
               ? module->state->module.functions().size()
               : 0;
}

mparser_utf8_view
mparser_module_function_name(
    const mparser_module* module, size_t index) {
    if (!module || !module->state ||
        index >= module->state->module.functions().size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        module->state->module.functions()[index].name);
}

mparser_api_status mparser_module_execute(
    const mparser_module* module,
    const mparser_invocation_options* options,
    mparser_result** out_result) {
    return mparser_c_detail::executeModule(
        module, options, {}, out_result);
}

mparser_api_status mparser_module_execute_with_system_context(
    const mparser_module* module,
    const mparser_system_context* context,
    const mparser_invocation_options* options,
    mparser_result** out_result) {
    if (!context || !context->state) {
        if (out_result) {
            *out_result = nullptr;
        }
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    return mparser_c_detail::executeModule(
        module, options, context->state, out_result);
}

mparser_api_status mparser_runtime_execute(
    mparser_runtime* runtime,
    const mparser_module* module,
    const mparser_invocation_options* options,
    mparser_result** out_result) {
    return mparser_c_detail::executeRuntime(
        runtime, module, options, out_result);
}

mparser_api_status mparser_runtime_clear_global(
    mparser_runtime* runtime,
    const char* name,
    size_t name_size) {
    if (!runtime || !runtime->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        const auto copied =
            mparser_c_detail::copyBytes(name, name_size);
        if (!copied || copied->empty()) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        std::lock_guard lock(runtime->state->mutex);
        runtime->state->session->clearGlobal(*copied);
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status
mparser_runtime_clear_globals(mparser_runtime* runtime) {
    if (!runtime || !runtime->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(runtime->state->mutex);
        runtime->state->session->clearGlobals();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_runtime_reset(mparser_runtime* runtime) {
    if (!runtime || !runtime->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(runtime->state->mutex);
        runtime->state->session->reset();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_module_create_session(
    const mparser_module* module,
    mparser_session** out_session) {
    return mparser_c_detail::createSession(
        module, {}, out_session);
}

mparser_api_status
mparser_module_create_session_with_system_context(
    const mparser_module* module,
    const mparser_system_context* context,
    mparser_session** out_session) {
    if (!context || !context->state) {
        if (out_session) {
            *out_session = nullptr;
        }
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    return mparser_c_detail::createSession(
        module, context->state, out_session);
}

void mparser_session_retain(mparser_session* session) {
    mparser_c_detail::retainHandle(session);
}

void mparser_session_release(mparser_session* session) {
    mparser_c_detail::releaseHandle(session);
}

mparser_api_status mparser_session_execute(
    mparser_session* session,
    const mparser_invocation_options* options,
    mparser_result** out_result) {
    using namespace mparser_c_detail;
    if (!out_result) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_result = nullptr;
    if (!session || !session->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        mparser::ModuleInvocationRequest request;
        bool usesSharedGraph = false;
        const auto requestStatus = buildRequest(
            options, session->state->module, request,
            usesSharedGraph);
        if (requestStatus != MPARSER_API_STATUS_OK) {
            return requestStatus;
        }
        (void)usesSharedGraph;
        std::lock_guard sharedGraphLock(
            session->state->module->sharedGraphMutex);
        std::lock_guard lock(session->state->mutex);
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteBeforeCore);
        auto result = session->state->session.execute(request);
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::ExecuteAfterCore);
        return makeResultHandle(
            std::move(result),
            session->state->module, out_result);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status
mparser_session_clear_global(
    mparser_session* session,
    const char* name,
    size_t name_size) {
    if (!session || !session->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        const auto copied =
            mparser_c_detail::copyBytes(name, name_size);
        if (!copied || copied->empty()) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        std::lock_guard sharedGraphLock(
            session->state->module->sharedGraphMutex);
        std::lock_guard lock(session->state->mutex);
        return session->state->session.clearGlobal(*copied)
                   ? MPARSER_API_STATUS_OK
                   : MPARSER_API_STATUS_OUT_OF_RANGE;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status
mparser_session_clear_globals(mparser_session* session) {
    if (!session || !session->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard sharedGraphLock(
            session->state->module->sharedGraphMutex);
        std::lock_guard lock(session->state->mutex);
        session->state->session.clearGlobals();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status
mparser_session_reset(mparser_session* session) {
    if (!session || !session->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard sharedGraphLock(
            session->state->module->sharedGraphMutex);
        std::lock_guard lock(session->state->mutex);
        session->state->session.reset();
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_result_retain(mparser_result* result) {
    mparser_c_detail::retainHandle(result);
}

void mparser_result_release(mparser_result* result) {
    mparser_c_detail::releaseHandle(result);
}

mparser_invocation_status
mparser_result_status(const mparser_result* result) {
    return result
               ? mparser_c_detail::invocationStatus(
                     result->value.status)
               : MPARSER_INVOCATION_RUNTIME_FAILED;
}

uint32_t
mparser_result_succeeded(const mparser_result* result) {
    return result && result->value.succeeded() ? 1u : 0u;
}

mparser_utf8_view
mparser_result_entry_name(const mparser_result* result) {
    return result
               ? mparser_c_detail::utf8View(
                     result->value.entryFunction)
               : mparser_c_detail::emptyUtf8View();
}

size_t
mparser_result_requested_output_count(
    const mparser_result* result) {
    return result ? result->value.requestedOutputCount : 0;
}

size_t
mparser_result_output_count(const mparser_result* result) {
    return result ? result->value.outputs.size() : 0;
}

mparser_utf8_view
mparser_result_output_name(
    const mparser_result* result, size_t index) {
    if (!result ||
        index >= result->value.outputNames.size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        result->value.outputNames[index]);
}

mparser_api_status mparser_result_output(
    const mparser_result* result,
    size_t index,
    mparser_value** out_value) {
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!result) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (index >= result->value.outputs.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    return mparser_c_detail::makeValueHandle(
        result->value.outputs[index], result->owner, out_value,
        result->runtimeOwner);
}

size_t
mparser_result_output_event_count(const mparser_result* result) {
    return result ? result->value.outputEvents.size() : 0;
}

mparser_output_kind
mparser_result_output_event_kind(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.outputEvents.size()) {
        return MPARSER_OUTPUT_STANDARD;
    }
    return mparser_c_detail::externalOutputKind(
        result->value.outputEvents[index].kind);
}

uint64_t
mparser_result_output_event_sequence(
    const mparser_result* result, size_t index) {
    return result && index < result->value.outputEvents.size()
               ? result->value.outputEvents[index].sequence
               : 0;
}

mparser_utf8_view
mparser_result_output_event_text(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.outputEvents.size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        result->value.outputEvents[index].text);
}

mparser_utf8_view
mparser_result_output_event_source_name(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.outputEvents.size() ||
        !result->value.outputEvents[index].source.available) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        result->value.outputEvents[index].source.sourceName);
}

mparser_source_position
mparser_result_output_event_source_begin(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.outputEvents.size() ||
        !result->value.outputEvents[index].source.available) {
        return {};
    }
    return mparser_c_detail::sourcePosition(
        result->value.outputEvents[index].source.begin);
}

mparser_source_position
mparser_result_output_event_source_end(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.outputEvents.size() ||
        !result->value.outputEvents[index].source.available) {
        return {};
    }
    return mparser_c_detail::sourcePosition(
        result->value.outputEvents[index].source.end);
}

size_t
mparser_result_top_level_expression_count(
    const mparser_result* result) {
    return result ? result->value.topLevelExpressions.size() : 0;
}

mparser_api_status
mparser_result_top_level_expression_value(
    const mparser_result* result,
    size_t index,
    mparser_value** out_value) {
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!result) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (index >= result->value.topLevelExpressions.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    return mparser_c_detail::makeValueHandle(
        result->value.topLevelExpressions[index].value,
        result->owner, out_value, result->runtimeOwner);
}

uint32_t
mparser_result_top_level_expression_output_suppressed(
    const mparser_result* result, size_t index) {
    return result && index < result->value.topLevelExpressions.size() &&
                   result->value.topLevelExpressions[index].outputSuppressed
               ? 1u
               : 0u;
}

uint64_t
mparser_result_top_level_expression_sequence(
    const mparser_result* result, size_t index) {
    return result && index < result->value.topLevelExpressions.size()
               ? result->value.topLevelExpressions[index].sequence
               : 0;
}

mparser_utf8_view
mparser_result_top_level_expression_source_name(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.topLevelExpressions.size() ||
        !result->value.topLevelExpressions[index].source.available) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        result->value.topLevelExpressions[index].source.sourceName);
}

mparser_source_position
mparser_result_top_level_expression_source_begin(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.topLevelExpressions.size() ||
        !result->value.topLevelExpressions[index].source.available) {
        return {};
    }
    return mparser_c_detail::sourcePosition(
        result->value.topLevelExpressions[index].source.begin);
}

mparser_source_position
mparser_result_top_level_expression_source_end(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->value.topLevelExpressions.size() ||
        !result->value.topLevelExpressions[index].source.available) {
        return {};
    }
    return mparser_c_detail::sourcePosition(
        result->value.topLevelExpressions[index].source.end);
}

size_t
mparser_result_variable_count(
    const mparser_result* result) {
    return result ? result->value.variables.size() : 0;
}

mparser_api_status mparser_result_variable(
    const mparser_result* result,
    size_t index,
    mparser_utf8_view* out_name,
    mparser_value** out_value) {
    if (!out_name || !out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_name = mparser_c_detail::emptyUtf8View();
    *out_value = nullptr;
    if (!result) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (index >= result->value.variables.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    const auto& variable = result->value.variables[index];
    const auto status = mparser_c_detail::makeValueHandle(
        variable.value, result->owner, out_value,
        result->runtimeOwner);
    if (status == MPARSER_API_STATUS_OK) {
        *out_name =
            mparser_c_detail::utf8View(variable.name);
    }
    return status;
}

size_t
mparser_result_diagnostic_count(
    const mparser_result* result) {
    return result ? result->diagnostics.size() : 0;
}

const mparser_diagnostic*
mparser_result_diagnostic(
    const mparser_result* result, size_t index) {
    if (!result || index >= result->diagnostics.size()) {
        return nullptr;
    }
    return &result->diagnostics[index];
}

mparser_api_status
mparser_result_execution_summary(
    const mparser_result* result,
    mparser_execution_summary* out_summary) {
    using namespace mparser_c_detail;
    if (!result || !out_summary) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t storageSize = out_summary->struct_size;
    if (storageSize < MPARSER_EXECUTION_SUMMARY_SIZE ||
        out_summary->abi_generation != MPARSER_C_ABI_GENERATION) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    const auto& source = result->value.execution;
    mparser_execution_summary summary{};
    summary.struct_size = storageSize;
    summary.abi_generation = MPARSER_C_ABI_GENERATION;
    summary.requested_backend =
        externalBackend(source.requestedBackend);
    summary.effective_tier =
        externalTier(source.effectiveTier);
    summary.profiling_collected =
        source.profilingCollected ? 1u : 0u;
    summary.fallback_occurred =
        source.fallbackOccurred ? 1u : 0u;
    summary.resource_controls_active =
        source.resourceControlsActive ? 1u : 0u;
    summary.optimized_execution_suppressed =
        source.optimizedExecutionSuppressed ? 1u : 0u;
    summary.stop_reason =
        externalStopReason(source.stopReason);
    summary.executed_instruction_count =
        sizeToUint64(source.executedInstructionCount);
    summary.typed_region_count =
        sizeToUint64(source.typedRegionCount);
    summary.typed_region_attempt_count =
        sizeToUint64(source.typedRegionAttemptCount);
    summary.typed_region_execution_count =
        sizeToUint64(source.typedRegionExecutionCount);
    summary.typed_region_fallback_count =
        sizeToUint64(source.typedRegionFallbackCount);
    summary.native_compilation_count =
        sizeToUint64(source.nativeCompilationCount);
    summary.native_cache_hit_count =
        sizeToUint64(source.nativeCacheHitCount);
    summary.maximum_call_depth =
        sizeToUint64(source.maximumCallDepth);
    summary.maximum_array_bytes =
        sizeToUint64(source.maximumArrayBytes);
    summary.maximum_diagnostic_count =
        sizeToUint64(source.maximumDiagnosticCount);
    summary.elapsed_nanoseconds =
        source.elapsedNanoseconds;
    std::memcpy(
        out_summary, &summary,
        std::min<size_t>(storageSize, sizeof(summary)));
    return MPARSER_API_STATUS_OK;
}

mparser_diagnostic_phase
mparser_diagnostic_get_phase(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->phase
                      : MPARSER_DIAGNOSTIC_EXECUTION;
}

mparser_diagnostic_severity
mparser_diagnostic_get_severity(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->severity
                      : MPARSER_DIAGNOSTIC_ERROR;
}

mparser_utf8_view
mparser_diagnostic_identifier(
    const mparser_diagnostic* diagnostic) {
    return diagnostic
               ? mparser_c_detail::utf8View(
                     diagnostic->identifier)
               : mparser_c_detail::emptyUtf8View();
}

mparser_utf8_view
mparser_diagnostic_message(
    const mparser_diagnostic* diagnostic) {
    return diagnostic
               ? mparser_c_detail::utf8View(
                     diagnostic->message)
               : mparser_c_detail::emptyUtf8View();
}

uint32_t
mparser_diagnostic_has_source(
    const mparser_diagnostic* diagnostic) {
    return diagnostic && diagnostic->sourceAvailable ? 1u : 0u;
}

mparser_utf8_view
mparser_diagnostic_source_name(
    const mparser_diagnostic* diagnostic) {
    return diagnostic
               ? mparser_c_detail::utf8View(
                     diagnostic->sourceName)
               : mparser_c_detail::emptyUtf8View();
}

mparser_source_position
mparser_diagnostic_source_begin(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->begin
                      : mparser_source_position{};
}

mparser_source_position
mparser_diagnostic_source_end(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->end
                      : mparser_source_position{};
}

size_t
mparser_diagnostic_stack_count(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->stack.size() : 0;
}

mparser_utf8_view
mparser_diagnostic_stack_source(
    const mparser_diagnostic* diagnostic, size_t index) {
    if (!diagnostic || index >= diagnostic->stack.size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        diagnostic->stack[index].source);
}

mparser_utf8_view
mparser_diagnostic_stack_function(
    const mparser_diagnostic* diagnostic, size_t index) {
    if (!diagnostic || index >= diagnostic->stack.size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        diagnostic->stack[index].function);
}

int32_t
mparser_diagnostic_stack_line(
    const mparser_diagnostic* diagnostic, size_t index) {
    if (!diagnostic || index >= diagnostic->stack.size()) {
        return 0;
    }
    return diagnostic->stack[index].line;
}

size_t
mparser_diagnostic_cause_count(
    const mparser_diagnostic* diagnostic) {
    return diagnostic ? diagnostic->causes.size() : 0;
}

const mparser_diagnostic*
mparser_diagnostic_cause(
    const mparser_diagnostic* diagnostic, size_t index) {
    if (!diagnostic || index >= diagnostic->causes.size()) {
        return nullptr;
    }
    return &diagnostic->causes[index];
}

mparser_api_status
mparser_value_create_missing(mparser_value** out_value) {
    return mparser_c_detail::makeValueHandle(
        mparser::makeRuntimeMissingArrayValue(), {}, out_value);
}

mparser_api_status mparser_value_create_missing_array(
    const size_t* dimensions, size_t rank,
    mparser_value** out_value) {
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!dimensions || rank < 2) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::vector<size_t> shape(dimensions, dimensions + rank);
        if (!mparser::checkedRuntimeDimensionProduct(shape)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        return mparser_c_detail::makeValueHandle(
            mparser::makeRuntimeMissingArrayValue(std::move(shape)), {},
            out_value);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_numeric(
    const size_t* dimensions, size_t rank,
    const mparser_numeric_buffer* buffer,
    mparser_value** out_value) {
    using namespace mparser_c_detail;
    if (!out_value || !buffer) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (buffer->is_complex > 1 ||
        (!buffer->real_data && buffer->element_count != 0) ||
        (buffer->is_complex != 0 && !buffer->imaginary_data &&
         buffer->element_count != 0) ||
        (buffer->is_complex == 0 && buffer->imaginary_data)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::vector<size_t> shape;
        const auto shapeStatus = copyDimensions(
            dimensions, rank, buffer->element_count, shape);
        if (shapeStatus != MPARSER_API_STATUS_OK) {
            return shapeStatus;
        }
        mparser::RuntimeNumericClass runtimeClass;
        if (!numericClass(buffer->numeric_class, runtimeClass) ||
            (runtimeClass == mparser::RuntimeNumericClass::Logical &&
             buffer->is_complex != 0)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        auto elements = copyNumericBufferElements(*buffer, runtimeClass);
        if (!elements) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        auto value = mparser::runtimeNumericValueFromElements(
            std::move(shape), std::move(*elements), runtimeClass);
        if (!value) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        return makeValueHandle(
            std::move(*value), {}, out_value);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_character_array(
    const size_t* dimensions,
    size_t rank,
    const uint16_t* column_major_code_units,
    size_t code_unit_count,
    mparser_value** out_value) {
    using namespace mparser_c_detail;
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!column_major_code_units &&
        code_unit_count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::vector<size_t> shape;
        const auto shapeStatus = copyDimensions(
            dimensions, rank, code_unit_count, shape);
        if (shapeStatus != MPARSER_API_STATUS_OK) {
            return shapeStatus;
        }
        auto value = mparser::makeRuntimeCharacterArray(
            std::move(shape),
            std::u16string(code_unit_count, u'\0'));
        for (size_t index = 0;
             index < code_unit_count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    value.dimensions, index);
            if (!offset ||
                *offset >= value.characterElements.size()) {
                return MPARSER_API_STATUS_INTERNAL_ERROR;
            }
            value.characterElements[*offset] =
                static_cast<char16_t>(
                    column_major_code_units[index]);
        }
        return makeValueHandle(
            std::move(value), {}, out_value);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_string_array(
    const size_t* dimensions,
    size_t rank,
    const mparser_utf16_view* column_major_elements,
    size_t element_count,
    mparser_value** out_value) {
    using namespace mparser_c_detail;
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!column_major_elements && element_count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::vector<size_t> shape;
        const auto shapeStatus = copyDimensions(
            dimensions, rank, element_count, shape);
        if (shapeStatus != MPARSER_API_STATUS_OK) {
            return shapeStatus;
        }
        auto value = mparser::makeRuntimeStringArray(
            std::move(shape),
            std::vector<mparser::RuntimeStringElement>(
                element_count));
        for (size_t index = 0;
             index < element_count; ++index) {
            const auto& external =
                column_major_elements[index];
            if ((!external.data && external.size != 0) ||
                external.missing > 1) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
            const auto offset =
                logicalStorageOffset(
                    value.dimensions, index);
            if (!offset ||
                *offset >= value.stringElements.size()) {
                return MPARSER_API_STATUS_INTERNAL_ERROR;
            }
            auto& internal = value.stringElements[*offset];
            internal.missing = external.missing != 0;
            internal.value.reserve(external.size);
            for (size_t codeUnit = 0;
                 codeUnit < external.size; ++codeUnit) {
                internal.value.push_back(
                    static_cast<char16_t>(
                        external.data[codeUnit]));
            }
        }
        return makeValueHandle(
            std::move(value), {}, out_value);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_cell(
    const size_t* dimensions,
    size_t rank,
    const mparser_value* const* column_major_elements,
    size_t element_count,
    mparser_value** out_value) {
    using namespace mparser_c_detail;
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::CellCompose);
        std::vector<size_t> shape;
        const auto shapeStatus = copyDimensions(
            dimensions, rank, element_count, shape);
        if (shapeStatus != MPARSER_API_STATUS_OK) {
            return shapeStatus;
        }
        std::shared_ptr<ModuleState> owner;
        std::shared_ptr<RuntimeState> runtimeOwner;
        const auto ownerStatus = ownerForComposedValues(
            column_major_elements, element_count, owner, runtimeOwner);
        if (ownerStatus != MPARSER_API_STATUS_OK) {
            return ownerStatus;
        }
        auto value = mparser::makeRuntimeCellValue(
            std::move(shape),
            std::vector<mparser::RuntimeValue>(
                element_count,
                mparser::makeRuntimeMissingValue()));
        for (size_t index = 0;
             index < element_count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    value.dimensions, index);
            if (!offset || *offset >= value.cells.size()) {
                return MPARSER_API_STATUS_INTERNAL_ERROR;
            }
            value.cells[*offset] =
                column_major_elements[index]->state->value;
        }
        return makeValueHandle(
            std::move(value), std::move(owner), out_value,
            std::move(runtimeOwner));
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_struct(
    const mparser_named_value* fields,
    size_t field_count,
    mparser_value** out_value) {
    using namespace mparser_c_detail;
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    if (!fields && field_count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::StructCompose);
        mparser::RuntimeWorkspace runtimeFields;
        std::vector<std::string> fieldOrder;
        fieldOrder.reserve(field_count);
        std::shared_ptr<ModuleState> owner;
        std::shared_ptr<RuntimeState> runtimeOwner;
        for (size_t index = 0;
             index < field_count; ++index) {
            const auto& field = fields[index];
            const auto name =
                copyBytes(field.name, field.name_size);
            if (!name || name->empty() ||
                !field.value || !field.value->state) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
            if (field.value->state->owner) {
                if (runtimeOwner ||
                    (owner &&
                     owner != field.value->state->owner)) {
                    return MPARSER_API_STATUS_OWNER_MISMATCH;
                }
                owner = field.value->state->owner;
            }
            if (field.value->state->runtimeOwner) {
                if (owner ||
                    (runtimeOwner &&
                     runtimeOwner != field.value->state->runtimeOwner)) {
                    return MPARSER_API_STATUS_OWNER_MISMATCH;
                }
                runtimeOwner = field.value->state->runtimeOwner;
            }
            if (!runtimeFields.emplace(
                    *name, field.value->state->value).second) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
            fieldOrder.push_back(*name);
        }
        auto runtimeValue = mparser::makeRuntimeStructValue(
            std::move(runtimeFields));
        runtimeValue.fieldOrder = std::move(fieldOrder);
        return makeValueHandle(
            std::move(runtimeValue),
            std::move(owner), out_value,
            std::move(runtimeOwner));
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_value_retain(mparser_value* value) {
    mparser_c_detail::retainHandle(value);
}

void mparser_value_release(mparser_value* value) {
    mparser_c_detail::releaseHandle(value);
}

mparser_value_kind
mparser_value_get_kind(const mparser_value* value) {
    return value && value->state
               ? mparser_c_detail::valueKind(
                     value->state->value)
               : MPARSER_VALUE_MISSING;
}

mparser_numeric_class
mparser_value_get_numeric_class(
    const mparser_value* value) {
    if (!value || !value->state ||
        mparser_c_detail::valueKind(value->state->value) !=
            MPARSER_VALUE_NUMERIC) {
        return MPARSER_NUMERIC_DOUBLE;
    }
    switch (value->state->value.numericClass) {
    case mparser::RuntimeNumericClass::Double:
        return MPARSER_NUMERIC_DOUBLE;
    case mparser::RuntimeNumericClass::Logical:
        return MPARSER_NUMERIC_LOGICAL;
    case mparser::RuntimeNumericClass::Single:
        return MPARSER_NUMERIC_SINGLE;
    case mparser::RuntimeNumericClass::Int8:
        return MPARSER_NUMERIC_INT8;
    case mparser::RuntimeNumericClass::UInt8:
        return MPARSER_NUMERIC_UINT8;
    case mparser::RuntimeNumericClass::Int16:
        return MPARSER_NUMERIC_INT16;
    case mparser::RuntimeNumericClass::UInt16:
        return MPARSER_NUMERIC_UINT16;
    case mparser::RuntimeNumericClass::Int32:
        return MPARSER_NUMERIC_INT32;
    case mparser::RuntimeNumericClass::UInt32:
        return MPARSER_NUMERIC_UINT32;
    case mparser::RuntimeNumericClass::Int64:
        return MPARSER_NUMERIC_INT64;
    case mparser::RuntimeNumericClass::UInt64:
        return MPARSER_NUMERIC_UINT64;
    }
    return MPARSER_NUMERIC_DOUBLE;
}

uint32_t
mparser_value_is_module_bound(
    const mparser_value* value) {
    return value && value->state &&
                   (value->state->owner ||
                    value->state->runtimeOwner)
               ? 1u
               : 0u;
}

size_t
mparser_value_rank(const mparser_value* value) {
    return value && value->state
               ? value->state->dimensions.size()
               : 0;
}

mparser_api_status mparser_value_dimension(
    const mparser_value* value,
    size_t index,
    size_t* out_dimension) {
    if (!out_dimension) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_dimension = 0;
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (index >= value->state->dimensions.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    *out_dimension =
        value->state->dimensions[index];
    return MPARSER_API_STATUS_OK;
}

size_t
mparser_value_element_count(const mparser_value* value) {
    return value && value->state
               ? value->state->elementCount
               : 0;
}

mparser_api_status mparser_value_get_numeric_buffer(
    const mparser_value* value,
    mparser_numeric_buffer* out_buffer) {
    if (!out_buffer) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_buffer = mparser_numeric_buffer{};
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (mparser_c_detail::valueKind(value->state->value) !=
        MPARSER_VALUE_NUMERIC) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    out_buffer->numeric_class =
        mparser_value_get_numeric_class(value);
    out_buffer->is_complex =
        value->state->value.numericComplex ? 1u : 0u;
    out_buffer->real_data = mparser_c_detail::numericStorageData(
        value->state->numericRealColumnMajor);
    out_buffer->imaginary_data =
        value->state->value.numericComplex
            ? mparser_c_detail::numericStorageData(
                  value->state->numericImaginaryColumnMajor)
            : nullptr;
    out_buffer->element_count = value->state->elementCount;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status mparser_value_character_data(
    const mparser_value* value,
    const uint16_t** out_column_major_code_units,
    size_t* out_code_unit_count) {
    if (!out_column_major_code_units ||
        !out_code_unit_count) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_column_major_code_units = nullptr;
    *out_code_unit_count = 0;
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (value->state->value.kind !=
        mparser::RuntimeValueKind::CharacterArray) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    *out_column_major_code_units =
        value->state->characterColumnMajor.data();
    *out_code_unit_count =
        value->state->characterColumnMajor.size();
    return MPARSER_API_STATUS_OK;
}

mparser_api_status mparser_value_string_element(
    const mparser_value* value,
    size_t index,
    mparser_utf16_view* out_element) {
    if (!out_element) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_element = {};
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (value->state->value.kind !=
        mparser::RuntimeValueKind::StringArray) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    if (index >= value->state->stringColumnMajor.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    const auto offset =
        mparser_c_detail::logicalStorageOffset(
            value->state->dimensions, index);
    if (!offset ||
        *offset >=
            value->state->value.stringElements.size()) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
    const auto& codeUnits =
        value->state->stringColumnMajor[index];
    out_element->data = codeUnits.data();
    out_element->size = codeUnits.size();
    out_element->missing =
        value->state->value.stringElements[*offset].missing
            ? 1u
            : 0u;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status mparser_value_cell_element(
    const mparser_value* value,
    size_t index,
    mparser_value** out_element) {
    if (!out_element) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_element = nullptr;
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (value->state->value.kind !=
        mparser::RuntimeValueKind::Cell) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    const auto offset =
        mparser_c_detail::logicalStorageOffset(
            value->state->dimensions, index);
    if (!offset ||
        *offset >= value->state->value.cells.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    return mparser_c_detail::makeValueHandle(
        value->state->value.cells[*offset],
        value->state->owner, out_element,
        value->state->runtimeOwner);
}

size_t
mparser_value_struct_field_count(
    const mparser_value* value) {
    return value && value->state &&
                   value->state->value.kind ==
                       mparser::RuntimeValueKind::Struct
               ? value->state->structFieldNames.size()
               : 0;
}

mparser_utf8_view mparser_value_struct_field_name(
    const mparser_value* value, size_t field_index) {
    if (!value || !value->state ||
        value->state->value.kind !=
            mparser::RuntimeValueKind::Struct ||
        field_index >=
            value->state->structFieldNames.size()) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        value->state->structFieldNames[field_index]);
}

mparser_api_status mparser_value_struct_field(
    const mparser_value* value,
    size_t element_index,
    size_t field_index,
    mparser_value** out_field) {
    if (!out_field) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_field = nullptr;
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (value->state->value.kind !=
        mparser::RuntimeValueKind::Struct) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    if (field_index >=
        value->state->structFieldNames.size()) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    const auto* fields = mparser_c_detail::structElement(
        value->state->value,
        value->state->dimensions,
        element_index);
    if (!fields) {
        return MPARSER_API_STATUS_OUT_OF_RANGE;
    }
    const auto field = fields->find(
        value->state->structFieldNames[field_index]);
    if (field == fields->end()) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
    return mparser_c_detail::makeValueHandle(
        field->second, value->state->owner, out_field,
        value->state->runtimeOwner);
}

mparser_utf8_view
mparser_value_class_name(const mparser_value* value) {
    if (!value || !value->state ||
        value->state->value.kind !=
            mparser::RuntimeValueKind::Object) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        value->state->value.className);
}

mparser_utf8_view
mparser_value_function_text(const mparser_value* value) {
    if (!value || !value->state ||
        value->state->value.kind !=
            mparser::RuntimeValueKind::FunctionHandle) {
        return mparser_c_detail::emptyUtf8View();
    }
    return mparser_c_detail::utf8View(
        value->state->functionText);
}

mparser_api_status
mparser_cancel_token_create(
    mparser_cancel_token** out_token) {
    if (!out_token) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_token = nullptr;
    try {
        mparser::c_api_test::inject(
            mparser::c_api_test::FaultPoint::CancelTokenCreate);
        *out_token = new mparser_cancel_token;
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

void mparser_cancel_token_retain(
    mparser_cancel_token* token) {
    mparser_c_detail::retainHandle(token);
}

void mparser_cancel_token_release(
    mparser_cancel_token* token) {
    mparser_c_detail::releaseHandle(token);
}

void mparser_cancel_token_request(
    mparser_cancel_token* token) {
    if (token) {
        token->token.requestCancellation();
    }
}

uint32_t mparser_cancel_token_is_requested(
    const mparser_cancel_token* token) {
    return token && token->token.cancellationRequested()
               ? 1u
               : 0u;
}

} // extern "C"
