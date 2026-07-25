#include "mparser/c_api.h"

#include "mparser/compiled_module.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value.h"
#include "mparser/source_loader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <utility>
#include <vector>

#ifndef MPARSER_VERSION_MAJOR
#define MPARSER_VERSION_MAJOR 0
#endif
#ifndef MPARSER_VERSION_MINOR
#define MPARSER_VERSION_MINOR 0
#endif
#ifndef MPARSER_VERSION_PATCH
#define MPARSER_VERSION_PATCH 0
#endif

namespace mparser_c_detail {

struct DiagnosticFrame {
    std::string source;
    std::string function;
    int32_t line = 1;
};

struct ModuleState;

struct ValueState {
    mparser::RuntimeValue value;
    std::shared_ptr<ModuleState> owner;
    std::vector<size_t> dimensions;
    size_t elementCount = 0;
    std::vector<double> numericColumnMajor;
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

struct mparser_result {
    std::atomic_size_t references{1};
    mparser::ModuleInvocationResult value;
    std::shared_ptr<mparser_c_detail::ModuleState> owner;
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

std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(encoded);
}

bool validPathText(std::string_view value) noexcept {
    return !value.empty() &&
           value.find('\0') == std::string_view::npos;
}

mparser_api_status publishCompiledModule(
    mparser::CompiledModule compiled,
    mparser_module** out_module) {
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

bool runtimeValueRequiresModule(
    const mparser::RuntimeValue& value,
    std::set<const void*>& functionHandles) {
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
                functionHandles)) {
            return true;
        }
        for (const auto& [name, captured] :
             value.functionHandle->capturedVariables) {
            (void)name;
            if (runtimeValueRequiresModule(
                    captured, functionHandles)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Object:
        return true;
    case mparser::RuntimeValueKind::Cell:
    case mparser::RuntimeValueKind::CommaSeparatedList:
    case mparser::RuntimeValueKind::NameValueArgument:
        for (const auto& element : value.cells) {
            if (runtimeValueRequiresModule(
                    element, functionHandles)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Struct:
        for (const auto& element : value.structElements) {
            for (const auto& [name, field] : element) {
                (void)name;
                if (runtimeValueRequiresModule(
                        field, functionHandles)) {
                    return true;
                }
            }
        }
        for (const auto& [name, field] : value.fields) {
            (void)name;
            if (runtimeValueRequiresModule(
                    field, functionHandles)) {
                return true;
            }
        }
        return false;
    case mparser::RuntimeValueKind::Missing:
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
    return runtimeValueRequiresModule(
        value, functionHandles);
}

bool externallyTransportable(
    const mparser::RuntimeValue& value) {
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

bool buildExternalCaches(ValueState& state) {
    const auto& value = state.value;
    state.dimensions = mparser::runtimeDimensions(value);
    const auto count = mparser::checkedRuntimeDimensionProduct(
        state.dimensions);
    if (!count) {
        return false;
    }
    state.elementCount = *count;
    if (value.kind == mparser::RuntimeValueKind::Number) {
        state.numericColumnMajor = {value.number};
    } else if (
        value.kind == mparser::RuntimeValueKind::Vector ||
        value.kind == mparser::RuntimeValueKind::Matrix) {
        state.numericColumnMajor.resize(*count);
        for (size_t index = 0; index < *count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    state.dimensions, index);
            if (!offset || *offset >= value.elements.size()) {
                return false;
            }
            state.numericColumnMajor[index] =
                value.elements[*offset];
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
    mparser_value** outValue) {
    if (!outValue) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outValue = nullptr;
    try {
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
        if (owner &&
            runtimeValueRequiresModule(state->value)) {
            state->owner = std::move(owner);
        }
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
    return false;
}

mparser_api_status ownerForComposedValues(
    const mparser_value* const* values,
    size_t count,
    std::shared_ptr<ModuleState>& owner) {
    owner.reset();
    if (!values && count != 0) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!values[index] || !values[index]->state) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        const auto& candidate = values[index]->state->owner;
        if (!candidate) {
            continue;
        }
        if (owner && owner != candidate) {
            return MPARSER_API_STATUS_OWNER_MISMATCH;
        }
        owner = candidate;
    }
    return MPARSER_API_STATUS_OK;
}

bool ownerCompatible(
    const mparser_value* value,
    const std::shared_ptr<ModuleState>& module) noexcept {
    return value && value->state &&
           (!value->state->owner ||
            value->state->owner == module);
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

mparser_api_status buildRequest(
    const mparser_invocation_options* options,
    const std::shared_ptr<ModuleState>& module,
    mparser::ModuleInvocationRequest& request) {
    if (!options) {
        return MPARSER_API_STATUS_OK;
    }
    if (options->struct_size < sizeof(*options) ||
        options->abi_version != MPARSER_C_ABI_VERSION) {
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
        if (!ownerCompatible(value, module)) {
            return value && value->state
                       ? MPARSER_API_STATUS_OWNER_MISMATCH
                       : MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
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
            !ownerCompatible(variable.value, module)) {
            if (variable.value && variable.value->state &&
                variable.value->state->owner &&
                variable.value->state->owner != module) {
                return MPARSER_API_STATUS_OWNER_MISMATCH;
            }
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
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
    return MPARSER_API_STATUS_OK;
}

mparser_api_status makeResultHandle(
    mparser::ModuleInvocationResult result,
    std::shared_ptr<ModuleState> owner,
    mparser_result** outResult) {
    if (!outResult) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *outResult = nullptr;
    try {
        auto handle = std::make_unique<mparser_result>();
        handle->value = std::move(result);
        handle->owner = std::move(owner);
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

} // namespace
} // namespace mparser_c_detail

extern "C" {

uint32_t mparser_c_abi_version(void) {
    return MPARSER_C_ABI_VERSION;
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
    default:
        return utf8View("unknown");
    }
}

mparser_api_status
mparser_invocation_options_init(
    mparser_invocation_options* options) {
    if (!options) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *options = {};
    options->struct_size =
        static_cast<uint32_t>(sizeof(*options));
    options->abi_version = MPARSER_C_ABI_VERSION;
    options->backend = MPARSER_BACKEND_AUTOMATIC;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status
mparser_execution_summary_init(
    mparser_execution_summary* summary) {
    if (!summary) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *summary = {};
    summary->struct_size =
        static_cast<uint32_t>(sizeof(*summary));
    summary->abi_version = MPARSER_C_ABI_VERSION;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status
mparser_source_unit_init(mparser_source_unit* source) {
    if (!source) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *source = {};
    source->struct_size =
        static_cast<uint32_t>(sizeof(*source));
    source->abi_version = MPARSER_C_ABI_VERSION;
    return MPARSER_API_STATUS_OK;
}

mparser_api_status mparser_source_load_options_init(
    mparser_source_load_options* options) {
    if (!options) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *options = {};
    options->struct_size =
        static_cast<uint32_t>(sizeof(*options));
    options->abi_version = MPARSER_C_ABI_VERSION;
    return MPARSER_API_STATUS_OK;
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
        std::vector<mparser::SourceUnit> copiedSources;
        copiedSources.reserve(source_count);
        for (size_t index = 0; index < source_count; ++index) {
            const auto& source = sources[index];
            if (source.struct_size < sizeof(source) ||
                source.abi_version != MPARSER_C_ABI_VERSION) {
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
        const auto entryText =
            copyBytes(entry_path, entry_path_size);
        if (!entryText || !validPathText(*entryText)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }

        mparser::SourceLoaderOptions loaderOptions;
        if (options) {
            if (options->struct_size < sizeof(*options) ||
                options->abi_version != MPARSER_C_ABI_VERSION) {
                return MPARSER_API_STATUS_ABI_MISMATCH;
            }
            if (!options->search_paths &&
                options->search_path_count != 0) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
            loaderOptions.searchPaths.reserve(
                options->search_path_count);
            for (size_t index = 0;
                 index < options->search_path_count; ++index) {
                const auto path = copyBytes(
                    options->search_paths[index].data,
                    options->search_paths[index].size);
                if (!path || !validPathText(*path)) {
                    return MPARSER_API_STATUS_INVALID_ARGUMENT;
                }
                loaderOptions.searchPaths.push_back(
                    pathFromUtf8(*path));
            }
        }

        mparser::SourceLoaderResult loaded;
        try {
            loaded = mparser::SourceLoader{}.load(
                pathFromUtf8(*entryText), loaderOptions);
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
    using namespace mparser_c_detail;
    if (!out_result) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_result = nullptr;
    if (!module || !module->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    try {
        mparser::ModuleInvocationRequest request;
        const auto requestStatus =
            buildRequest(options, module->state, request);
        if (requestStatus != MPARSER_API_STATUS_OK) {
            return requestStatus;
        }
        return makeResultHandle(
            module->state->module.execute(request),
            module->state, out_result);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_module_create_session(
    const mparser_module* module,
    mparser_session** out_session) {
    using namespace mparser_c_detail;
    if (!out_session) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_session = nullptr;
    if (!module || !module->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (!module->state->module.valid()) {
        return MPARSER_API_STATUS_COMPILATION_FAILED;
    }
    try {
        auto state = std::make_shared<SessionState>(
            module->state,
            module->state->module.createSession());
        auto* handle = new mparser_session;
        handle->state = std::move(state);
        *out_session = handle;
        return MPARSER_API_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
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
        const auto requestStatus = buildRequest(
            options, session->state->module, request);
        if (requestStatus != MPARSER_API_STATUS_OK) {
            return requestStatus;
        }
        std::lock_guard lock(session->state->mutex);
        return makeResultHandle(
            session->state->session.execute(request),
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
        result->value.outputs[index], result->owner, out_value);
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
        variable.value, result->owner, out_value);
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
    if (out_summary->struct_size < sizeof(*out_summary) ||
        out_summary->abi_version != MPARSER_C_ABI_VERSION) {
        return MPARSER_API_STATUS_ABI_MISMATCH;
    }
    const auto& source = result->value.execution;
    mparser_execution_summary summary{};
    summary.struct_size =
        static_cast<uint32_t>(sizeof(summary));
    summary.abi_version = MPARSER_C_ABI_VERSION;
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
    *out_summary = summary;
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
        mparser::makeRuntimeMissingValue(), {}, out_value);
}

mparser_api_status mparser_value_create_scalar(
    double value,
    mparser_numeric_class numeric_class,
    mparser_value** out_value) {
    if (!out_value) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_value = nullptr;
    mparser::RuntimeNumericClass runtimeClass;
    if (!mparser_c_detail::numericClass(
            numeric_class, runtimeClass)) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (runtimeClass ==
        mparser::RuntimeNumericClass::Logical) {
        value = value == 0.0 ? 0.0 : 1.0;
    }
    try {
        auto runtimeValue =
            mparser::makeRuntimeNumberValue(
                value, runtimeClass);
        return mparser_c_detail::makeValueHandle(
            std::move(runtimeValue), {}, out_value);
    } catch (const std::bad_alloc&) {
        return MPARSER_API_STATUS_ALLOCATION_FAILED;
    } catch (...) {
        return MPARSER_API_STATUS_INTERNAL_ERROR;
    }
}

mparser_api_status mparser_value_create_numeric_array(
    mparser_numeric_class numeric_class,
    const size_t* dimensions,
    size_t rank,
    const double* column_major_elements,
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
        mparser::RuntimeNumericClass runtimeClass;
        if (!numericClass(numeric_class, runtimeClass)) {
            return MPARSER_API_STATUS_INVALID_ARGUMENT;
        }
        if (element_count == 1 &&
            mparser::normalizeRuntimeDimensions(shape) ==
                std::vector<size_t>{1, 1}) {
            double scalar = column_major_elements[0];
            if (runtimeClass ==
                mparser::RuntimeNumericClass::Logical) {
                scalar = scalar == 0.0 ? 0.0 : 1.0;
            }
            return makeValueHandle(
                mparser::makeRuntimeNumberValue(
                    scalar, runtimeClass),
                {}, out_value);
        }

        mparser::RuntimeValue value;
        value.kind = mparser::RuntimeValueKind::Matrix;
        value.numericClass = runtimeClass;
        value.elements.resize(element_count);
        mparser::setRuntimeDimensions(value, std::move(shape));
        for (size_t index = 0;
             index < element_count; ++index) {
            const auto offset =
                logicalStorageOffset(
                    value.dimensions, index);
            if (!offset || *offset >= value.elements.size()) {
                return MPARSER_API_STATUS_INTERNAL_ERROR;
            }
            const double element =
                runtimeClass ==
                        mparser::RuntimeNumericClass::Logical
                    ? (column_major_elements[index] == 0.0
                           ? 0.0
                           : 1.0)
                    : column_major_elements[index];
            value.elements[*offset] = element;
        }
        return makeValueHandle(
            std::move(value), {}, out_value);
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
        std::vector<size_t> shape;
        const auto shapeStatus = copyDimensions(
            dimensions, rank, element_count, shape);
        if (shapeStatus != MPARSER_API_STATUS_OK) {
            return shapeStatus;
        }
        std::shared_ptr<ModuleState> owner;
        const auto ownerStatus = ownerForComposedValues(
            column_major_elements, element_count, owner);
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
            std::move(value), std::move(owner), out_value);
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
        mparser::RuntimeWorkspace runtimeFields;
        std::shared_ptr<ModuleState> owner;
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
                if (owner &&
                    owner != field.value->state->owner) {
                    return MPARSER_API_STATUS_OWNER_MISMATCH;
                }
                owner = field.value->state->owner;
            }
            if (!runtimeFields.emplace(
                    *name, field.value->state->value).second) {
                return MPARSER_API_STATUS_INVALID_ARGUMENT;
            }
        }
        return makeValueHandle(
            mparser::makeRuntimeStructValue(
                std::move(runtimeFields)),
            std::move(owner), out_value);
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
    return value->state->value.numericClass ==
                   mparser::RuntimeNumericClass::Logical
               ? MPARSER_NUMERIC_LOGICAL
               : MPARSER_NUMERIC_DOUBLE;
}

uint32_t
mparser_value_is_module_bound(
    const mparser_value* value) {
    return value && value->state && value->state->owner
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

mparser_api_status mparser_value_numeric_data(
    const mparser_value* value,
    const double** out_column_major_elements,
    size_t* out_element_count) {
    if (!out_column_major_elements ||
        !out_element_count) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    *out_column_major_elements = nullptr;
    *out_element_count = 0;
    if (!value || !value->state) {
        return MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    if (mparser_c_detail::valueKind(value->state->value) !=
        MPARSER_VALUE_NUMERIC) {
        return MPARSER_API_STATUS_TYPE_MISMATCH;
    }
    *out_column_major_elements =
        value->state->numericColumnMajor.data();
    *out_element_count =
        value->state->numericColumnMajor.size();
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
        value->state->owner, out_element);
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
        field->second, value->state->owner, out_field);
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
