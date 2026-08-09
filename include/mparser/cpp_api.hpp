#ifndef MPARSER_CPP_API_HPP
#define MPARSER_CPP_API_HPP

#include "mparser/c_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mparser::sdk {

/*
 * Copy a wrapper before handing it to another thread. Independent wrapper
 * copies own independent retained references; concurrent assignment,
 * move, reset, or destruction of the same wrapper object is not supported.
 * Pure stateless calls may run concurrently. Module-bound mutable values and
 * sessions are serialized by their owning module.
 */
using ApiStatus = mparser_api_status;

inline constexpr std::uint32_t kSourceApiVersionMajor = 1;
inline constexpr std::uint32_t kSourceApiVersionMinor = 2;

struct SourceApiVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
};

[[nodiscard]] inline constexpr SourceApiVersion
sourceApiVersion() noexcept {
    return SourceApiVersion{
        kSourceApiVersionMajor,
        kSourceApiVersionMinor};
}

enum class Backend : std::uint32_t {
    Automatic = MPARSER_BACKEND_AUTOMATIC,
    Bytecode = MPARSER_BACKEND_BYTECODE,
    Portable = MPARSER_BACKEND_PORTABLE,
    Native = MPARSER_BACKEND_NATIVE,
};

enum class ExecutionTier : std::uint32_t {
    Bytecode = MPARSER_EXECUTION_TIER_BYTECODE,
    Portable = MPARSER_EXECUTION_TIER_PORTABLE,
    Native = MPARSER_EXECUTION_TIER_NATIVE,
    Mixed = MPARSER_EXECUTION_TIER_MIXED,
};

enum class InvocationStatus : std::uint32_t {
    Succeeded = MPARSER_INVOCATION_SUCCEEDED,
    CompilationFailed = MPARSER_INVOCATION_COMPILATION_FAILED,
    RequestRejected = MPARSER_INVOCATION_REQUEST_REJECTED,
    RuntimeFailed = MPARSER_INVOCATION_RUNTIME_FAILED,
};

enum class StopReason : std::uint32_t {
    None = MPARSER_STOP_NONE,
    Cancelled = MPARSER_STOP_CANCELLED,
    InstructionLimit = MPARSER_STOP_INSTRUCTION_LIMIT,
    WallTimeLimit = MPARSER_STOP_WALL_TIME_LIMIT,
    CallDepthLimit = MPARSER_STOP_CALL_DEPTH_LIMIT,
    ArrayByteLimit = MPARSER_STOP_ARRAY_BYTE_LIMIT,
    DiagnosticLimit = MPARSER_STOP_DIAGNOSTIC_LIMIT,
};

enum class DiagnosticPhase : std::uint32_t {
    Compilation = MPARSER_DIAGNOSTIC_COMPILATION,
    Validation = MPARSER_DIAGNOSTIC_VALIDATION,
    Execution = MPARSER_DIAGNOSTIC_EXECUTION,
};

enum class DiagnosticSeverity : std::uint32_t {
    Error = MPARSER_DIAGNOSTIC_ERROR,
    Warning = MPARSER_DIAGNOSTIC_WARNING,
};

enum class ValueKind : std::uint32_t {
    Missing = MPARSER_VALUE_MISSING,
    Numeric = MPARSER_VALUE_NUMERIC,
    Character = MPARSER_VALUE_CHARACTER,
    String = MPARSER_VALUE_STRING,
    Cell = MPARSER_VALUE_CELL,
    Structure = MPARSER_VALUE_STRUCT,
    Object = MPARSER_VALUE_OBJECT,
    FunctionHandle = MPARSER_VALUE_FUNCTION_HANDLE,
};

enum class NumericClass : std::uint32_t {
    Double = MPARSER_NUMERIC_DOUBLE,
    Logical = MPARSER_NUMERIC_LOGICAL,
    Single = MPARSER_NUMERIC_SINGLE,
    Int8 = MPARSER_NUMERIC_INT8,
    UInt8 = MPARSER_NUMERIC_UINT8,
    Int16 = MPARSER_NUMERIC_INT16,
    UInt16 = MPARSER_NUMERIC_UINT16,
    Int32 = MPARSER_NUMERIC_INT32,
    UInt32 = MPARSER_NUMERIC_UINT32,
    Int64 = MPARSER_NUMERIC_INT64,
    UInt64 = MPARSER_NUMERIC_UINT64,
};

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

[[nodiscard]] inline Version runtimeVersion() noexcept {
    return Version{
        mparser_version_major(),
        mparser_version_minor(),
        mparser_version_patch()};
}

[[nodiscard]] inline std::uint32_t abiMajor() noexcept {
    return mparser_c_abi_version();
}

[[nodiscard]] inline std::uint32_t abiRevision() noexcept {
    return mparser_c_abi_revision();
}

class ApiError : public std::runtime_error {
public:
    ApiError(ApiStatus status, std::string_view context)
        : std::runtime_error(makeMessage(status, context)),
          status_(status) {}

    [[nodiscard]] ApiStatus status() const noexcept {
        return status_;
    }

private:
    static std::string makeMessage(
        ApiStatus status, std::string_view context) {
        const auto view = mparser_api_status_name(status);
        std::string message(context);
        if (!message.empty()) {
            message += ": ";
        }
        if (view.data && view.size != 0) {
            message.append(view.data, view.size);
        } else {
            message += "unknown-status";
        }
        message += " (" + std::to_string(status) + ")";
        return message;
    }

    ApiStatus status_;
};

struct SourcePosition {
    std::uint64_t offset = 0;
    std::int32_t line = 1;
    std::int32_t column = 1;
};

struct SourceRange {
    std::string sourceName;
    SourcePosition begin;
    SourcePosition end;
};

struct DiagnosticFrame {
    std::string sourceName;
    std::string functionName;
    std::int32_t line = 1;
};

struct Diagnostic {
    DiagnosticPhase phase = DiagnosticPhase::Execution;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string identifier;
    std::string message;
    std::optional<SourceRange> source;
    std::vector<DiagnosticFrame> stack;
    std::vector<Diagnostic> causes;
};

struct ExecutionLimits {
    std::uint64_t maximumInstructionCount = 0;
    std::uint64_t maximumWallTimeNanoseconds = 0;
    std::uint64_t maximumCallDepth = 0;
    std::uint64_t maximumArrayBytes = 0;
    std::uint64_t maximumDiagnosticCount = 0;
};

struct ExecutionSummary {
    Backend requestedBackend = Backend::Automatic;
    ExecutionTier effectiveTier = ExecutionTier::Bytecode;
    bool profilingCollected = false;
    bool fallbackOccurred = false;
    bool resourceControlsActive = false;
    bool optimizedExecutionSuppressed = false;
    StopReason stopReason = StopReason::None;
    std::uint64_t executedInstructionCount = 0;
    std::uint64_t typedRegionCount = 0;
    std::uint64_t typedRegionAttemptCount = 0;
    std::uint64_t typedRegionExecutionCount = 0;
    std::uint64_t typedRegionFallbackCount = 0;
    std::uint64_t nativeCompilationCount = 0;
    std::uint64_t nativeCacheHitCount = 0;
    std::uint64_t maximumCallDepth = 0;
    std::uint64_t maximumArrayBytes = 0;
    std::uint64_t maximumDiagnosticCount = 0;
    std::uint64_t elapsedNanoseconds = 0;
};

struct SourceUnit {
    std::string name;
    std::string source;
};

struct SourceLoadOptions {
    std::vector<std::string> searchPaths;
};

class Value;
struct NamedValue;
struct Invocation;
class Result;
class Session;

namespace detail {

struct AdoptHandle {};
inline constexpr AdoptHandle adoptHandle{};

inline void checkStatus(
    ApiStatus status, std::string_view context) {
    if (status != MPARSER_API_STATUS_OK) {
        throw ApiError(status, context);
    }
}

inline std::string copyUtf8(mparser_utf8_view view) {
    if (!view.data || view.size == 0) {
        return {};
    }
    return std::string(view.data, view.size);
}

inline SourcePosition copyPosition(
    mparser_source_position position) noexcept {
    return SourcePosition{
        position.offset, position.line, position.column};
}

inline Diagnostic copyDiagnostic(
    const mparser_diagnostic* source) {
    if (!source) {
        throw ApiError(
            MPARSER_API_STATUS_INTERNAL_ERROR,
            "diagnostic handle is unavailable");
    }

    Diagnostic result;
    result.phase = static_cast<DiagnosticPhase>(
        mparser_diagnostic_get_phase(source));
    result.severity = static_cast<DiagnosticSeverity>(
        mparser_diagnostic_get_severity(source));
    result.identifier = copyUtf8(
        mparser_diagnostic_identifier(source));
    result.message = copyUtf8(
        mparser_diagnostic_message(source));
    if (mparser_diagnostic_has_source(source) != 0) {
        result.source = SourceRange{
            copyUtf8(mparser_diagnostic_source_name(source)),
            copyPosition(mparser_diagnostic_source_begin(source)),
            copyPosition(mparser_diagnostic_source_end(source))};
    }

    const auto stackCount =
        mparser_diagnostic_stack_count(source);
    result.stack.reserve(stackCount);
    for (std::size_t index = 0; index < stackCount; ++index) {
        result.stack.push_back(DiagnosticFrame{
            copyUtf8(mparser_diagnostic_stack_source(
                source, index)),
            copyUtf8(mparser_diagnostic_stack_function(
                source, index)),
            mparser_diagnostic_stack_line(source, index)});
    }

    const auto causeCount =
        mparser_diagnostic_cause_count(source);
    result.causes.reserve(causeCount);
    for (std::size_t index = 0; index < causeCount; ++index) {
        result.causes.push_back(copyDiagnostic(
            mparser_diagnostic_cause(source, index)));
    }
    return result;
}

template <typename Raw, typename Policy>
class SharedHandle {
public:
    SharedHandle() noexcept = default;

    SharedHandle(Raw* value, AdoptHandle) noexcept
        : value_(value) {}

    SharedHandle(const SharedHandle& other) noexcept
        : value_(other.value_) {
        Policy::retain(value_);
    }

    SharedHandle(SharedHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    SharedHandle& operator=(SharedHandle other) noexcept {
        swap(other);
        return *this;
    }

    ~SharedHandle() {
        Policy::release(value_);
    }

    void swap(SharedHandle& other) noexcept {
        std::swap(value_, other.value_);
    }

    [[nodiscard]] Raw* get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    Raw* value_ = nullptr;
};

struct ModulePolicy {
    static void retain(mparser_module* value) noexcept {
        mparser_module_retain(value);
    }
    static void release(mparser_module* value) noexcept {
        mparser_module_release(value);
    }
};

struct SessionPolicy {
    static void retain(mparser_session* value) noexcept {
        mparser_session_retain(value);
    }
    static void release(mparser_session* value) noexcept {
        mparser_session_release(value);
    }
};

struct ResultPolicy {
    static void retain(mparser_result* value) noexcept {
        mparser_result_retain(value);
    }
    static void release(mparser_result* value) noexcept {
        mparser_result_release(value);
    }
};

struct ValuePolicy {
    static void retain(mparser_value* value) noexcept {
        mparser_value_retain(value);
    }
    static void release(mparser_value* value) noexcept {
        mparser_value_release(value);
    }
};

struct CancellationPolicy {
    static void retain(mparser_cancel_token* value) noexcept {
        mparser_cancel_token_retain(value);
    }
    static void release(mparser_cancel_token* value) noexcept {
        mparser_cancel_token_release(value);
    }
};

struct InvocationBridge;

} // namespace detail

class Value {
public:
    Value() noexcept = default;

    [[nodiscard]] static Value missing() {
        mparser_value* value = nullptr;
        const auto status = mparser_value_create_missing(&value);
        return takeCreated(status, value, "create missing value");
    }

    [[nodiscard]] static Value scalar(double value) {
        return numericScalar(NumericClass::Double, value);
    }

    template <typename Element>
    [[nodiscard]] static Value numericScalar(
        NumericClass numericClass, Element real,
        std::optional<Element> imaginary = std::nullopt) {
        const std::array<std::size_t, 2> dimensions{1, 1};
        const std::span<const Element> realData(&real, 1);
        const std::optional<std::span<const Element>> imaginaryData =
            imaginary
                ? std::optional<std::span<const Element>>(
                      std::span<const Element>(&*imaginary, 1))
                : std::nullopt;
        return numericArray(
            numericClass, dimensions, realData, imaginaryData);
    }

    template <typename Element>
    [[nodiscard]] static Value numericArray(
        NumericClass numericClass,
        std::span<const std::size_t> dimensions,
        std::span<const Element> columnMajorReal,
        std::optional<std::span<const Element>> columnMajorImaginary =
            std::nullopt) {
        if (!numericClassMatches<Element>(numericClass) ||
            (columnMajorImaginary &&
             columnMajorImaginary->size() != columnMajorReal.size())) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "numeric buffer type, class, or component sizes differ");
        }
        mparser_numeric_buffer buffer{};
        buffer.numeric_class =
            static_cast<mparser_numeric_class>(numericClass);
        buffer.is_complex = columnMajorImaginary ? 1u : 0u;
        buffer.real_data = columnMajorReal.data();
        buffer.imaginary_data = columnMajorImaginary
                                    ? columnMajorImaginary->data()
                                    : nullptr;
        buffer.element_count = columnMajorReal.size();
        mparser_value* created = nullptr;
        const auto status = mparser_value_create_numeric(
            dimensions.data(), dimensions.size(),
            &buffer, &created);
        return takeCreated(status, created, "create numeric value");
    }

    [[nodiscard]] static Value characterArray(
        std::span<const std::size_t> dimensions,
        std::span<const std::uint16_t> columnMajorCodeUnits) {
        mparser_value* created = nullptr;
        const auto status = mparser_value_create_character_array(
            dimensions.data(), dimensions.size(),
            columnMajorCodeUnits.data(),
            columnMajorCodeUnits.size(), &created);
        return takeCreated(
            status, created, "create character array");
    }

    [[nodiscard]] static Value stringArray(
        std::span<const std::size_t> dimensions,
        std::span<const std::optional<std::u16string>>
            columnMajorElements) {
        std::vector<std::vector<std::uint16_t>> storage;
        std::vector<mparser_utf16_view> views;
        storage.reserve(columnMajorElements.size());
        views.reserve(columnMajorElements.size());
        for (const auto& element : columnMajorElements) {
            if (!element) {
                storage.emplace_back();
                views.push_back(mparser_utf16_view{nullptr, 0, 1});
                continue;
            }
            std::vector<std::uint16_t> converted;
            converted.reserve(element->size());
            for (const char16_t codeUnit : *element) {
                converted.push_back(
                    static_cast<std::uint16_t>(codeUnit));
            }
            storage.push_back(std::move(converted));
            const auto& stored = storage.back();
            views.push_back(mparser_utf16_view{
                stored.data(), stored.size(), 0});
        }

        mparser_value* created = nullptr;
        const auto status = mparser_value_create_string_array(
            dimensions.data(), dimensions.size(), views.data(),
            views.size(), &created);
        return takeCreated(
            status, created, "create string array");
    }

    [[nodiscard]] static Value cell(
        std::span<const std::size_t> dimensions,
        std::span<const Value> columnMajorElements) {
        std::vector<const mparser_value*> values;
        values.reserve(columnMajorElements.size());
        for (const auto& element : columnMajorElements) {
            values.push_back(element.raw());
        }

        mparser_value* created = nullptr;
        const auto status = mparser_value_create_cell(
            dimensions.data(), dimensions.size(), values.data(),
            values.size(), &created);
        return takeCreated(status, created, "create cell array");
    }

    [[nodiscard]] static Value structure(
        std::span<const NamedValue> fields);

    [[nodiscard]] bool hasValue() const noexcept {
        return static_cast<bool>(handle_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasValue();
    }

    [[nodiscard]] ValueKind kind() const {
        return static_cast<ValueKind>(
            mparser_value_get_kind(requireRaw()));
    }

    [[nodiscard]] NumericClass numericClass() const {
        return static_cast<NumericClass>(
            mparser_value_get_numeric_class(requireRaw()));
    }

    [[nodiscard]] bool isModuleBound() const {
        return mparser_value_is_module_bound(requireRaw()) != 0;
    }

    [[nodiscard]] std::vector<std::size_t> dimensions() const {
        const auto* value = requireRaw();
        const auto rank = mparser_value_rank(value);
        std::vector<std::size_t> result;
        result.reserve(rank);
        for (std::size_t index = 0; index < rank; ++index) {
            std::size_t dimension = 0;
            detail::checkStatus(
                mparser_value_dimension(
                    value, index, &dimension),
                "read value dimension");
            result.push_back(dimension);
        }
        return result;
    }

    [[nodiscard]] std::size_t elementCount() const {
        return mparser_value_element_count(requireRaw());
    }

    [[nodiscard]] bool isComplex() const {
        return numericBuffer().is_complex != 0;
    }

    template <typename Element = double>
    [[nodiscard]] std::span<const Element> numericData() const {
        const auto buffer = numericBuffer();
        const auto numericClass =
            static_cast<NumericClass>(buffer.numeric_class);
        if (!numericClassMatches<Element>(numericClass)) {
            throw ApiError(
                MPARSER_API_STATUS_TYPE_MISMATCH,
                "numeric data element type does not match its class");
        }
        return std::span<const Element>(
            static_cast<const Element*>(buffer.real_data),
            buffer.element_count);
    }

    template <typename Element = double>
    [[nodiscard]] std::span<const Element>
    numericImaginaryData() const {
        const auto buffer = numericBuffer();
        const auto numericClass =
            static_cast<NumericClass>(buffer.numeric_class);
        if (!numericClassMatches<Element>(numericClass)) {
            throw ApiError(
                MPARSER_API_STATUS_TYPE_MISMATCH,
                "numeric data element type does not match its class");
        }
        return buffer.is_complex != 0
                   ? std::span<const Element>(
                         static_cast<const Element*>(
                             buffer.imaginary_data),
                         buffer.element_count)
                   : std::span<const Element>{};
    }

    [[nodiscard]] std::span<const std::uint16_t>
    characterData() const {
        const std::uint16_t* data = nullptr;
        std::size_t count = 0;
        detail::checkStatus(
            mparser_value_character_data(
                requireRaw(), &data, &count),
            "read character data");
        return std::span<const std::uint16_t>(data, count);
    }

    [[nodiscard]] std::optional<std::u16string>
    stringElement(std::size_t index) const {
        mparser_utf16_view view{};
        detail::checkStatus(
            mparser_value_string_element(
                requireRaw(), index, &view),
            "read string element");
        if (view.missing != 0) {
            return std::nullopt;
        }
        std::u16string result;
        result.reserve(view.size);
        for (std::size_t offset = 0; offset < view.size; ++offset) {
            result.push_back(
                static_cast<char16_t>(view.data[offset]));
        }
        return result;
    }

    [[nodiscard]] Value cellElement(std::size_t index) const {
        mparser_value* value = nullptr;
        const auto status = mparser_value_cell_element(
            requireRaw(), index, &value);
        return takeCreated(status, value, "read cell element");
    }

    [[nodiscard]] std::vector<std::string>
    structFieldNames() const {
        const auto* value = requireRaw();
        const auto count = mparser_value_struct_field_count(value);
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(detail::copyUtf8(
                mparser_value_struct_field_name(value, index)));
        }
        return result;
    }

    [[nodiscard]] Value structField(
        std::size_t elementIndex,
        std::size_t fieldIndex) const {
        mparser_value* value = nullptr;
        const auto status = mparser_value_struct_field(
            requireRaw(), elementIndex, fieldIndex, &value);
        return takeCreated(status, value, "read structure field");
    }

    [[nodiscard]] std::string className() const {
        return detail::copyUtf8(
            mparser_value_class_name(requireRaw()));
    }

    [[nodiscard]] std::string functionText() const {
        return detail::copyUtf8(
            mparser_value_function_text(requireRaw()));
    }

private:
    template <typename Element>
    [[nodiscard]] static constexpr bool numericClassMatches(
        NumericClass numericClass) noexcept {
        using ValueType = std::remove_cv_t<Element>;
        if constexpr (std::is_same_v<ValueType, double>) {
            return numericClass == NumericClass::Double;
        } else if constexpr (std::is_same_v<ValueType, float>) {
            return numericClass == NumericClass::Single;
        } else if constexpr (std::is_same_v<ValueType, std::int8_t>) {
            return numericClass == NumericClass::Int8;
        } else if constexpr (std::is_same_v<ValueType, std::uint8_t>) {
            return numericClass == NumericClass::Logical ||
                   numericClass == NumericClass::UInt8;
        } else if constexpr (std::is_same_v<ValueType, std::int16_t>) {
            return numericClass == NumericClass::Int16;
        } else if constexpr (std::is_same_v<ValueType, std::uint16_t>) {
            return numericClass == NumericClass::UInt16;
        } else if constexpr (std::is_same_v<ValueType, std::int32_t>) {
            return numericClass == NumericClass::Int32;
        } else if constexpr (std::is_same_v<ValueType, std::uint32_t>) {
            return numericClass == NumericClass::UInt32;
        } else if constexpr (std::is_same_v<ValueType, std::int64_t>) {
            return numericClass == NumericClass::Int64;
        } else if constexpr (std::is_same_v<ValueType, std::uint64_t>) {
            return numericClass == NumericClass::UInt64;
        } else {
            return false;
        }
    }

    [[nodiscard]] mparser_numeric_buffer numericBuffer() const {
        mparser_numeric_buffer buffer{};
        detail::checkStatus(
            mparser_value_get_numeric_buffer(requireRaw(), &buffer),
            "read numeric buffer");
        return buffer;
    }

    using Handle = detail::SharedHandle<
        mparser_value, detail::ValuePolicy>;

    Value(mparser_value* value, detail::AdoptHandle) noexcept
        : handle_(value, detail::adoptHandle) {}

    static Value takeCreated(
        ApiStatus status, mparser_value* value,
        std::string_view context) {
        if (status != MPARSER_API_STATUS_OK) {
            mparser_value_release(value);
            detail::checkStatus(status, context);
        }
        if (!value) {
            throw ApiError(
                MPARSER_API_STATUS_INTERNAL_ERROR,
                std::string(context) + " returned no value");
        }
        return Value(value, detail::adoptHandle);
    }

    [[nodiscard]] mparser_value* requireRaw() const {
        if (!handle_) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "value handle is empty");
        }
        return handle_.get();
    }

    [[nodiscard]] mparser_value* raw() const noexcept {
        return handle_.get();
    }

    Handle handle_;

    friend struct detail::InvocationBridge;
    friend class Result;
};

struct NamedValue {
    std::string name;
    Value value;
};

inline Value Value::structure(
    std::span<const NamedValue> fields) {
    std::vector<mparser_named_value> descriptors;
    descriptors.reserve(fields.size());
    for (const auto& field : fields) {
        descriptors.push_back(mparser_named_value{
            field.name.data(), field.name.size(),
            field.value.raw()});
    }

    mparser_value* created = nullptr;
    const auto status = mparser_value_create_struct(
        descriptors.data(), descriptors.size(), &created);
    return takeCreated(status, created, "create structure value");
}

class CancellationToken {
public:
    CancellationToken() {
        mparser_cancel_token* token = nullptr;
        const auto status = mparser_cancel_token_create(&token);
        if (status != MPARSER_API_STATUS_OK) {
            mparser_cancel_token_release(token);
            detail::checkStatus(status, "create cancellation token");
        }
        if (!token) {
            throw ApiError(
                MPARSER_API_STATUS_INTERNAL_ERROR,
                "create cancellation token returned no handle");
        }
        handle_ = Handle(token, detail::adoptHandle);
    }

    void request() const {
        mparser_cancel_token_request(requireRaw());
    }

    [[nodiscard]] bool requested() const {
        return mparser_cancel_token_is_requested(requireRaw()) != 0;
    }

private:
    using Handle = detail::SharedHandle<
        mparser_cancel_token, detail::CancellationPolicy>;

    [[nodiscard]] mparser_cancel_token* requireRaw() const {
        if (!handle_) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "cancellation token handle is empty");
        }
        return handle_.get();
    }

    [[nodiscard]] mparser_cancel_token* raw() const noexcept {
        return handle_.get();
    }

    Handle handle_;

    friend struct detail::InvocationBridge;
};

struct Invocation {
    std::string entryFunction;
    std::vector<Value> arguments;
    std::optional<std::size_t> requestedOutputCount;
    std::vector<NamedValue> initialWorkspace;
    Backend backend = Backend::Automatic;
    bool collectProfile = false;
    ExecutionLimits limits;
    std::optional<CancellationToken> cancellationToken;
};

namespace detail {

struct InvocationBridge {
    explicit InvocationBridge(const Invocation& source) {
        checkStatus(
            MPARSER_INVOCATION_OPTIONS_INIT(&options),
            "initialize invocation options");

        arguments.reserve(source.arguments.size());
        for (const auto& argument : source.arguments) {
            arguments.push_back(argument.raw());
        }

        workspace.reserve(source.initialWorkspace.size());
        for (const auto& variable : source.initialWorkspace) {
            workspace.push_back(mparser_named_value{
                variable.name.data(), variable.name.size(),
                variable.value.raw()});
        }

        options.entry_name = source.entryFunction.data();
        options.entry_name_size = source.entryFunction.size();
        options.arguments = arguments.data();
        options.argument_count = arguments.size();
        if (source.requestedOutputCount) {
            options.requested_output_count =
                *source.requestedOutputCount;
            options.has_requested_output_count = 1;
        }
        options.initial_workspace = workspace.data();
        options.initial_workspace_count = workspace.size();
        options.backend = static_cast<mparser_backend>(source.backend);
        options.collect_profile = source.collectProfile ? 1u : 0u;
        options.max_instruction_count =
            source.limits.maximumInstructionCount;
        options.max_wall_time_nanoseconds =
            source.limits.maximumWallTimeNanoseconds;
        options.max_call_depth = source.limits.maximumCallDepth;
        options.max_array_bytes = source.limits.maximumArrayBytes;
        options.max_diagnostic_count =
            source.limits.maximumDiagnosticCount;
        options.cancellation_token = source.cancellationToken
            ? source.cancellationToken->raw()
            : nullptr;
    }

    mparser_invocation_options options{};
    std::vector<const mparser_value*> arguments;
    std::vector<mparser_named_value> workspace;
};

inline ExecutionSummary copyExecutionSummary(
    const mparser_execution_summary& source) noexcept {
    ExecutionSummary result;
    result.requestedBackend =
        static_cast<Backend>(source.requested_backend);
    result.effectiveTier =
        static_cast<ExecutionTier>(source.effective_tier);
    result.profilingCollected = source.profiling_collected != 0;
    result.fallbackOccurred = source.fallback_occurred != 0;
    result.resourceControlsActive =
        source.resource_controls_active != 0;
    result.optimizedExecutionSuppressed =
        source.optimized_execution_suppressed != 0;
    result.stopReason =
        static_cast<StopReason>(source.stop_reason);
    result.executedInstructionCount =
        source.executed_instruction_count;
    result.typedRegionCount = source.typed_region_count;
    result.typedRegionAttemptCount =
        source.typed_region_attempt_count;
    result.typedRegionExecutionCount =
        source.typed_region_execution_count;
    result.typedRegionFallbackCount =
        source.typed_region_fallback_count;
    result.nativeCompilationCount =
        source.native_compilation_count;
    result.nativeCacheHitCount = source.native_cache_hit_count;
    result.maximumCallDepth = source.maximum_call_depth;
    result.maximumArrayBytes = source.maximum_array_bytes;
    result.maximumDiagnosticCount =
        source.maximum_diagnostic_count;
    result.elapsedNanoseconds = source.elapsed_nanoseconds;
    return result;
}

} // namespace detail

class Result {
public:
    Result() noexcept = default;

    [[nodiscard]] bool hasHandle() const noexcept {
        return static_cast<bool>(handle_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasHandle();
    }

    [[nodiscard]] InvocationStatus status() const {
        return static_cast<InvocationStatus>(
            mparser_result_status(requireRaw()));
    }

    [[nodiscard]] bool succeeded() const {
        return mparser_result_succeeded(requireRaw()) != 0;
    }

    [[nodiscard]] std::string entryFunction() const {
        return detail::copyUtf8(
            mparser_result_entry_name(requireRaw()));
    }

    [[nodiscard]] std::size_t requestedOutputCount() const {
        return mparser_result_requested_output_count(requireRaw());
    }

    [[nodiscard]] std::size_t outputCount() const {
        return mparser_result_output_count(requireRaw());
    }

    [[nodiscard]] std::string outputName(
        std::size_t index) const {
        return detail::copyUtf8(
            mparser_result_output_name(requireRaw(), index));
    }

    [[nodiscard]] Value output(std::size_t index) const {
        mparser_value* value = nullptr;
        const auto status = mparser_result_output(
            requireRaw(), index, &value);
        return Value::takeCreated(status, value, "read result output");
    }

    [[nodiscard]] std::vector<Value> outputs() const {
        const auto count = outputCount();
        std::vector<Value> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(output(index));
        }
        return result;
    }

    [[nodiscard]] std::vector<NamedValue> variables() const {
        const auto* resultHandle = requireRaw();
        const auto count =
            mparser_result_variable_count(resultHandle);
        std::vector<NamedValue> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            mparser_utf8_view name{};
            mparser_value* value = nullptr;
            const auto status = mparser_result_variable(
                resultHandle, index, &name, &value);
            auto ownedValue = Value::takeCreated(
                status, value, "read result variable");
            result.push_back(NamedValue{
                detail::copyUtf8(name),
                std::move(ownedValue)});
        }
        return result;
    }

    [[nodiscard]] std::vector<Diagnostic> diagnostics() const {
        const auto* result = requireRaw();
        const auto count =
            mparser_result_diagnostic_count(result);
        std::vector<Diagnostic> copied;
        copied.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            copied.push_back(detail::copyDiagnostic(
                mparser_result_diagnostic(result, index)));
        }
        return copied;
    }

    [[nodiscard]] ExecutionSummary executionSummary() const {
        mparser_execution_summary summary{};
        detail::checkStatus(
            MPARSER_EXECUTION_SUMMARY_INIT(&summary),
            "initialize execution summary");
        detail::checkStatus(
            mparser_result_execution_summary(
                requireRaw(), &summary),
            "read execution summary");
        return detail::copyExecutionSummary(summary);
    }

private:
    using Handle = detail::SharedHandle<
        mparser_result, detail::ResultPolicy>;

    Result(mparser_result* value, detail::AdoptHandle) noexcept
        : handle_(value, detail::adoptHandle) {}

    static Result takeCreated(
        ApiStatus status, mparser_result* value,
        std::string_view context) {
        if (status != MPARSER_API_STATUS_OK) {
            mparser_result_release(value);
            detail::checkStatus(status, context);
        }
        if (!value) {
            throw ApiError(
                MPARSER_API_STATUS_INTERNAL_ERROR,
                std::string(context) + " returned no result");
        }
        return Result(value, detail::adoptHandle);
    }

    [[nodiscard]] mparser_result* requireRaw() const {
        if (!handle_) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "result handle is empty");
        }
        return handle_.get();
    }

    Handle handle_;

    friend class Module;
    friend class Session;
};

class Module {
public:
    Module() noexcept = default;

    [[nodiscard]] static Module compile(
        std::string_view source,
        std::string_view sourceName = "<memory>") {
        mparser_module* module = nullptr;
        const auto status = mparser_module_compile_utf8(
            source.data(), source.size(), sourceName.data(),
            sourceName.size(), &module);
        return takeCompiled(status, module, "compile source");
    }

    [[nodiscard]] static Module compile(
        std::span<const SourceUnit> sources) {
        std::vector<mparser_source_unit> descriptors;
        descriptors.reserve(sources.size());
        for (const auto& source : sources) {
            mparser_source_unit descriptor{};
            detail::checkStatus(
                mparser_source_unit_init(&descriptor),
                "initialize source unit");
            descriptor.source_name = source.name.data();
            descriptor.source_name_size = source.name.size();
            descriptor.source = source.source.data();
            descriptor.source_size = source.source.size();
            descriptors.push_back(descriptor);
        }

        mparser_module* module = nullptr;
        const auto status = mparser_module_compile_sources(
            descriptors.data(), descriptors.size(), &module);
        return takeCompiled(status, module, "compile source graph");
    }

    [[nodiscard]] static Module loadFile(
        std::string_view entryPath,
        const SourceLoadOptions& sourceOptions = {}) {
        std::vector<mparser_utf8_view> searchPaths;
        searchPaths.reserve(sourceOptions.searchPaths.size());
        for (const auto& path : sourceOptions.searchPaths) {
            searchPaths.push_back(mparser_utf8_view{
                path.data(), path.size()});
        }

        mparser_source_load_options options{};
        detail::checkStatus(
            MPARSER_SOURCE_LOAD_OPTIONS_INIT(&options),
            "initialize source-load options");
        options.search_paths = searchPaths.data();
        options.search_path_count = searchPaths.size();

        mparser_module* module = nullptr;
        const auto status = mparser_module_load_file_utf8(
            entryPath.data(), entryPath.size(), &options, &module);
        return takeCompiled(status, module, "load source graph");
    }

    [[nodiscard]] bool hasHandle() const noexcept {
        return static_cast<bool>(handle_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasHandle();
    }

    [[nodiscard]] bool isValid() const {
        return mparser_module_is_valid(requireRaw()) != 0;
    }

    [[nodiscard]] std::vector<std::string> sourceNames() const {
        const auto* module = requireRaw();
        const auto count = mparser_module_source_count(module);
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(detail::copyUtf8(
                mparser_module_source_name(module, index)));
        }
        return result;
    }

    [[nodiscard]] std::vector<std::string> functionNames() const {
        const auto* module = requireRaw();
        const auto count = mparser_module_function_count(module);
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(detail::copyUtf8(
                mparser_module_function_name(module, index)));
        }
        return result;
    }

    [[nodiscard]] std::vector<Diagnostic> diagnostics() const {
        const auto* module = requireRaw();
        const auto count =
            mparser_module_diagnostic_count(module);
        std::vector<Diagnostic> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(detail::copyDiagnostic(
                mparser_module_diagnostic(module, index)));
        }
        return result;
    }

    [[nodiscard]] Result execute() const;
    [[nodiscard]] Result execute(const Invocation& invocation) const;
    [[nodiscard]] Session createSession() const;

private:
    using Handle = detail::SharedHandle<
        mparser_module, detail::ModulePolicy>;

    Module(mparser_module* value, detail::AdoptHandle) noexcept
        : handle_(value, detail::adoptHandle) {}

    static Module takeCompiled(
        ApiStatus status, mparser_module* value,
        std::string_view context) {
        const bool diagnosticOutcome =
            status == MPARSER_API_STATUS_COMPILATION_FAILED ||
            status == MPARSER_API_STATUS_SOURCE_LOAD_FAILED;
        if (status != MPARSER_API_STATUS_OK && !diagnosticOutcome) {
            mparser_module_release(value);
            detail::checkStatus(status, context);
        }
        if (!value) {
            throw ApiError(
                MPARSER_API_STATUS_INTERNAL_ERROR,
                std::string(context) + " returned no module");
        }
        return Module(value, detail::adoptHandle);
    }

    [[nodiscard]] mparser_module* requireRaw() const {
        if (!handle_) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "module handle is empty");
        }
        return handle_.get();
    }

    Handle handle_;
};

class Session {
public:
    Session() noexcept = default;

    [[nodiscard]] bool hasHandle() const noexcept {
        return static_cast<bool>(handle_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hasHandle();
    }

    [[nodiscard]] Result execute();
    [[nodiscard]] Result execute(const Invocation& invocation);

    void clearGlobal(std::string_view name) {
        detail::checkStatus(
            mparser_session_clear_global(
                requireRaw(), name.data(), name.size()),
            "clear session global");
    }

    void clearGlobals() {
        detail::checkStatus(
            mparser_session_clear_globals(requireRaw()),
            "clear session globals");
    }

    void reset() {
        detail::checkStatus(
            mparser_session_reset(requireRaw()),
            "reset session");
    }

private:
    using Handle = detail::SharedHandle<
        mparser_session, detail::SessionPolicy>;

    Session(mparser_session* value, detail::AdoptHandle) noexcept
        : handle_(value, detail::adoptHandle) {}

    [[nodiscard]] mparser_session* requireRaw() const {
        if (!handle_) {
            throw ApiError(
                MPARSER_API_STATUS_INVALID_ARGUMENT,
                "session handle is empty");
        }
        return handle_.get();
    }

    Handle handle_;

    friend class Module;
};

inline Result Module::execute() const {
    return execute(Invocation{});
}

inline Result Module::execute(
    const Invocation& invocation) const {
    detail::InvocationBridge bridge(invocation);
    mparser_result* result = nullptr;
    const auto status = mparser_module_execute(
        requireRaw(), &bridge.options, &result);
    return Result::takeCreated(status, result, "execute module");
}

inline Session Module::createSession() const {
    mparser_session* session = nullptr;
    const auto status = mparser_module_create_session(
        requireRaw(), &session);
    if (status != MPARSER_API_STATUS_OK) {
        mparser_session_release(session);
        detail::checkStatus(status, "create module session");
    }
    if (!session) {
        throw ApiError(
            MPARSER_API_STATUS_INTERNAL_ERROR,
            "create module session returned no handle");
    }
    return Session(session, detail::adoptHandle);
}

inline Result Session::execute() {
    return execute(Invocation{});
}

inline Result Session::execute(
    const Invocation& invocation) {
    detail::InvocationBridge bridge(invocation);
    mparser_result* result = nullptr;
    const auto status = mparser_session_execute(
        requireRaw(), &bridge.options, &result);
    return Result::takeCreated(status, result, "execute session");
}

} // namespace mparser::sdk

#endif
