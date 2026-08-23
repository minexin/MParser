#include "mparser/embedding/machine_protocol.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace mparser {
namespace {

constexpr size_t kMaximumProtocolNestingDepth = 128;
constexpr std::string_view kEmergencyMachineResultJsonV1 =
    "{\"protocol\":{\"name\":\"mparser.result\",\"major\":1,\"minor\":1},"
    "\"engine\":{\"name\":\"MParser\",\"version\":\"unknown\"},"
    "\"status\":\"request-rejected\",\"entry_function\":\"\","
    "\"requested_output_count\":0,\"outputs\":[],\"workspace\":[],"
    "\"diagnostics\":[{\"phase\":\"validation\",\"severity\":\"error\","
    "\"identifier\":\"MParser:ProtocolFailure\","
    "\"message\":\"host failure while producing machine result\","
    "\"source\":null,\"stack\":[],\"causes\":[]}],"
    "\"execution\":{\"requested_backend\":\"automatic\","
    "\"effective_tier\":\"bytecode\",\"profiling_collected\":false,"
    "\"fallback_occurred\":false,\"resource_controls_active\":false,"
    "\"optimized_execution_suppressed\":false,\"stop_reason\":\"none\","
    "\"executed_instruction_count\":0,\"typed_region_count\":0,"
    "\"typed_region_attempt_count\":0,"
    "\"typed_region_execution_count\":0,"
    "\"typed_region_fallback_count\":0,\"native_compilation_count\":0,"
    "\"native_cache_hit_count\":0,\"maximum_call_depth\":0,"
    "\"maximum_array_bytes\":0,\"maximum_diagnostic_count\":0,"
    "\"elapsed_nanoseconds\":0}}";

bool writeMachineDocument(
    std::FILE* output, std::string_view document) noexcept {
    if (!output) {
        return false;
    }
#if defined(_WIN32)
    if (::_setmode(::_fileno(output), _O_BINARY) == -1) {
        return false;
    }
#endif
    if (!document.empty() &&
        std::fwrite(
            document.data(), 1, document.size(), output) !=
            document.size()) {
        return false;
    }
    constexpr char terminalLf = '\n';
    if (std::fwrite(&terminalLf, 1, 1, output) != 1) {
        return false;
    }
    return std::fflush(output) == 0;
}

class JsonWriter {
public:
    void beginObject() {
        beforeValue();
        output_.push_back('{');
        stack_.push_back(Context{ContainerKind::Object});
    }

    void endObject() {
        requireContainer(ContainerKind::Object);
        if (stack_.back().awaitingValue) {
            throw std::logic_error("JSON object member has no value");
        }
        output_.push_back('}');
        stack_.pop_back();
    }

    void beginArray() {
        beforeValue();
        output_.push_back('[');
        stack_.push_back(Context{ContainerKind::Array});
    }

    void endArray() {
        requireContainer(ContainerKind::Array);
        output_.push_back(']');
        stack_.pop_back();
    }

    void key(std::string_view name) {
        requireContainer(ContainerKind::Object);
        Context& context = stack_.back();
        if (context.awaitingValue) {
            throw std::logic_error("JSON object member has no value");
        }
        appendSeparator(context);
        appendQuoted(name);
        output_.push_back(':');
        context.awaitingValue = true;
    }

    void nullValue() {
        beforeValue();
        output_ += "null";
    }

    void booleanValue(bool value) {
        beforeValue();
        output_ += value ? "true" : "false";
    }

    void stringValue(std::string_view value) {
        beforeValue();
        appendQuoted(value);
    }

    void unsignedValue(std::uint64_t value) {
        beforeValue();
        appendInteger(value);
    }

    void signedValue(std::int64_t value) {
        beforeValue();
        appendInteger(value);
    }

    void doubleValue(double value) {
        beforeValue();
        if (std::isnan(value)) {
            appendQuoted("NaN");
            return;
        }
        if (std::isinf(value)) {
            appendQuoted(value < 0.0 ? "-Infinity" : "Infinity");
            return;
        }

        char buffer[128];
        const auto converted = std::to_chars(
            std::begin(buffer), std::end(buffer), value,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (converted.ec != std::errc{}) {
            throw std::runtime_error(
                "failed to format a machine-protocol number");
        }
        output_.append(buffer, converted.ptr);
    }

    void field(std::string_view name, std::string_view value) {
        key(name);
        stringValue(value);
    }

    void field(std::string_view name, const char* value) {
        field(name, std::string_view(value));
    }

    void field(std::string_view name, bool value) {
        key(name);
        booleanValue(value);
    }

    void field(std::string_view name, std::uint64_t value) {
        key(name);
        unsignedValue(value);
    }

    std::string finish() {
        if (!stack_.empty() || !rootWritten_) {
            throw std::logic_error("JSON document is incomplete");
        }
        return std::move(output_);
    }

private:
    enum class ContainerKind {
        Object,
        Array,
    };

    struct Context {
        ContainerKind kind;
        bool first = true;
        bool awaitingValue = false;
    };

    void requireContainer(ContainerKind kind) const {
        if (stack_.empty() || stack_.back().kind != kind) {
            throw std::logic_error("JSON container mismatch");
        }
    }

    static void appendSeparator(Context& context,
                                std::string& output) {
        if (!context.first) {
            output.push_back(',');
        }
        context.first = false;
    }

    void appendSeparator(Context& context) {
        appendSeparator(context, output_);
    }

    void beforeValue() {
        if (stack_.empty()) {
            if (rootWritten_) {
                throw std::logic_error(
                    "JSON document has more than one root value");
            }
            rootWritten_ = true;
            return;
        }

        Context& context = stack_.back();
        if (context.kind == ContainerKind::Array) {
            appendSeparator(context);
            return;
        }
        if (!context.awaitingValue) {
            throw std::logic_error(
                "JSON object value has no member name");
        }
        context.awaitingValue = false;
    }

    template <typename Integer>
    void appendInteger(Integer value) {
        char buffer[64];
        const auto converted =
            std::to_chars(std::begin(buffer), std::end(buffer), value);
        if (converted.ec != std::errc{}) {
            throw std::runtime_error(
                "failed to format a machine-protocol integer");
        }
        output_.append(buffer, converted.ptr);
    }

    static size_t validUtf8SequenceLength(
        std::string_view value, size_t offset) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        const size_t remaining = value.size() - offset;
        const auto continuation = [&](size_t index) {
            return index < remaining &&
                   (static_cast<unsigned char>(value[offset + index]) &
                    0xc0U) == 0x80U;
        };

        if (byte >= 0xc2U && byte <= 0xdfU && continuation(1)) {
            return 2;
        }
        if (byte == 0xe0U && remaining >= 3) {
            const auto second =
                static_cast<unsigned char>(value[offset + 1]);
            return second >= 0xa0U && second <= 0xbfU &&
                           continuation(2)
                       ? 3
                       : 0;
        }
        if (((byte >= 0xe1U && byte <= 0xecU) ||
             (byte >= 0xeeU && byte <= 0xefU)) &&
            continuation(1) && continuation(2)) {
            return 3;
        }
        if (byte == 0xedU && remaining >= 3) {
            const auto second =
                static_cast<unsigned char>(value[offset + 1]);
            return second >= 0x80U && second <= 0x9fU &&
                           continuation(2)
                       ? 3
                       : 0;
        }
        if (byte == 0xf0U && remaining >= 4) {
            const auto second =
                static_cast<unsigned char>(value[offset + 1]);
            return second >= 0x90U && second <= 0xbfU &&
                           continuation(2) && continuation(3)
                       ? 4
                       : 0;
        }
        if (byte >= 0xf1U && byte <= 0xf3U &&
            continuation(1) && continuation(2) && continuation(3)) {
            return 4;
        }
        if (byte == 0xf4U && remaining >= 4) {
            const auto second =
                static_cast<unsigned char>(value[offset + 1]);
            return second >= 0x80U && second <= 0x8fU &&
                           continuation(2) && continuation(3)
                       ? 4
                       : 0;
        }
        return 0;
    }

    void appendQuoted(std::string_view value) {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        output_.push_back('"');
        for (size_t index = 0; index < value.size();) {
            const auto byte =
                static_cast<unsigned char>(value[index]);
            if (byte >= 0x80U) {
                const size_t length =
                    validUtf8SequenceLength(value, index);
                if (length == 0) {
                    output_ += "\\ufffd";
                    ++index;
                } else {
                    output_.append(value.substr(index, length));
                    index += length;
                }
                continue;
            }

            switch (byte) {
            case '"':
                output_ += "\\\"";
                break;
            case '\\':
                output_ += "\\\\";
                break;
            case '\b':
                output_ += "\\b";
                break;
            case '\f':
                output_ += "\\f";
                break;
            case '\n':
                output_ += "\\n";
                break;
            case '\r':
                output_ += "\\r";
                break;
            case '\t':
                output_ += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    output_ += "\\u00";
                    output_.push_back(kHexDigits[byte >> 4U]);
                    output_.push_back(kHexDigits[byte & 0x0fU]);
                } else {
                    output_.push_back(static_cast<char>(byte));
                }
                break;
            }
            ++index;
        }
        output_.push_back('"');
    }

    std::string output_;
    std::vector<Context> stack_;
    bool rootWritten_ = false;
};

std::uint64_t protocolSize(size_t value) {
    static_assert(sizeof(size_t) <= sizeof(std::uint64_t));
    return static_cast<std::uint64_t>(value);
}

size_t valueElementCount(const RuntimeValue& value) {
    const auto count =
        checkedRuntimeDimensionProduct(runtimeDimensions(value));
    if (!count) {
        throw std::runtime_error(
            "runtime value dimensions overflow the machine protocol");
    }
    return *count;
}

size_t storageOffset(const RuntimeValue& value, size_t logicalIndex) {
    const auto offset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    if (!offset) {
        throw std::runtime_error(
            "runtime value shape cannot be projected to column-major order");
    }
    return *offset;
}

void writeDimensions(JsonWriter& writer, const RuntimeValue& value) {
    writer.beginArray();
    for (const size_t dimension : runtimeDimensions(value)) {
        writer.unsignedValue(protocolSize(dimension));
    }
    writer.endArray();
}

std::string_view numericClassName(RuntimeNumericClass numericClass) {
    return runtimeNumericClassName(numericClass);
}

std::string_view functionHandleKindName(
    RuntimeFunctionHandleKind kind) {
    switch (kind) {
    case RuntimeFunctionHandleKind::Anonymous:
        return "anonymous";
    case RuntimeFunctionHandleKind::Function:
        return "function";
    case RuntimeFunctionHandleKind::Builtin:
        return "builtin";
    case RuntimeFunctionHandleKind::Method:
        return "method";
    }
    return "unknown";
}

std::string_view functionHandleBackendName(
    RuntimeFunctionHandleBackend backend) {
    switch (backend) {
    case RuntimeFunctionHandleBackend::Independent:
        return "independent";
    case RuntimeFunctionHandleBackend::Hir:
        return "hir";
    case RuntimeFunctionHandleBackend::Bytecode:
        return "bytecode";
    }
    return "unknown";
}

void writeRuntimeValue(JsonWriter& writer, const RuntimeValue& value,
                       size_t depth);

void writeNumericElement(JsonWriter& writer,
                         const RuntimeNumericElementValue& value,
                         bool imaginary) {
    if (value.numericClass == RuntimeNumericClass::Logical) {
        writer.booleanValue(value.real != 0.0);
        return;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        const std::uint64_t bits =
            imaginary ? value.integerImaginaryBits
                      : value.integerRealBits;
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            writer.signedValue(std::bit_cast<std::int64_t>(bits));
        } else {
            writer.unsignedValue(bits);
        }
        return;
    }
    writer.doubleValue(imaginary ? value.imaginary : value.real);
}

void writeNumericValue(JsonWriter& writer, const RuntimeValue& value) {
    writer.beginObject();
    writer.field("kind", "numeric");
    writer.field("class", numericClassName(value.numericClass));
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.key("data");
    writer.beginArray();
    const size_t count = valueElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
            throw std::runtime_error(
                "numeric runtime payload does not match its shape");
        }
        writeNumericElement(writer, *element, false);
    }
    writer.endArray();
    if (value.numericComplex) {
        writer.field("complex", true);
        writer.key("imaginary_data");
        writer.beginArray();
        for (size_t index = 0; index < count; ++index) {
            const auto element = runtimeNumericElementValue(value, index);
            if (!element || !element->complex) {
                throw std::runtime_error(
                    "complex runtime payload does not match its shape");
            }
            writeNumericElement(writer, *element, true);
        }
        writer.endArray();
    }
    writer.endObject();
}

void writeCharacterValue(JsonWriter& writer,
                         const RuntimeValue& value) {
    writer.beginObject();
    writer.field("kind", "character");
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.key("utf16");
    writer.beginArray();
    const size_t count = valueElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeCharacterElement(value, index);
        if (!element) {
            throw std::runtime_error(
                "character runtime payload does not match its shape");
        }
        writer.unsignedValue(
            static_cast<std::uint16_t>(*element));
    }
    writer.endArray();
    writer.endObject();
}

void writeStringValue(JsonWriter& writer,
                      const RuntimeValue& value) {
    writer.beginObject();
    writer.field("kind", "string");
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.key("data");
    writer.beginArray();
    const size_t count = valueElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const RuntimeStringElement* element =
            runtimeStringElement(value, index);
        if (!element) {
            throw std::runtime_error(
                "string runtime payload does not match its shape");
        }
        if (element->missing) {
            writer.nullValue();
        } else {
            writer.stringValue(runtimeUtf16ToUtf8(element->value));
        }
    }
    writer.endArray();
    writer.endObject();
}

void writeCellLikeValue(JsonWriter& writer,
                        const RuntimeValue& value,
                        std::string_view kind, size_t depth) {
    writer.beginObject();
    writer.field("kind", kind);
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.key("data");
    writer.beginArray();
    const size_t count = valueElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = storageOffset(value, index);
        if (offset >= value.cells.size()) {
            throw std::runtime_error(
                "cell runtime payload does not match its shape");
        }
        writeRuntimeValue(writer, value.cells[offset], depth + 1);
    }
    writer.endArray();
    writer.endObject();
}

void writeStructValue(JsonWriter& writer,
                      const RuntimeValue& value, size_t depth) {
    writer.beginObject();
    writer.field("kind", "struct");
    writer.key("dimensions");
    writeDimensions(writer, value);
    const auto fieldOrder = runtimeStructFieldOrder(value);
    writer.key("fields");
    writer.beginArray();
    for (const auto& field : fieldOrder) {
        writer.stringValue(field);
    }
    writer.endArray();
    writer.key("data");
    writer.beginArray();
    const size_t count = valueElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = storageOffset(value, index);
        const RuntimeStructElement* element =
            runtimeStructElement(value, offset);
        if (!element) {
            throw std::runtime_error(
                "structure runtime payload does not match its shape");
        }
        writer.beginObject();
        for (const auto& field : fieldOrder) {
            const auto found = element->find(field);
            if (found == element->end()) {
                throw std::runtime_error(
                    "structure element is missing a declared field");
            }
            writer.key(field);
            writeRuntimeValue(writer, found->second, depth + 1);
        }
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
}

void writeFunctionHandleValue(JsonWriter& writer,
                              const RuntimeValue& value) {
    if (!value.functionHandle) {
        throw std::runtime_error(
            "function-handle runtime descriptor is missing");
    }
    const RuntimeFunctionHandle& handle = *value.functionHandle;
    writer.beginObject();
    writer.field("kind", "function-handle");
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.field("handle_kind", functionHandleKindName(handle.kind));
    writer.field("backend", functionHandleBackendName(handle.backend));
    writer.field("display", handle.display);
    writer.field("target_name", handle.targetName);
    writer.field("class_name", handle.className);
    writer.field("method_name", handle.methodName);
    writer.field("declaring_class", handle.declaringClass);
    writer.field(
        "module_bound",
        handle.backend != RuntimeFunctionHandleBackend::Independent);
    writer.endObject();
}

void writeObjectValue(JsonWriter& writer,
                      const RuntimeValue& value) {
    writer.beginObject();
    writer.field("kind", "object");
    writer.field("class", value.className);
    writer.key("dimensions");
    writeDimensions(writer, value);
    writer.field("handle", value.handleObject);
    writer.key("enumeration_member");
    if (value.enumerationMemberName.empty()) {
        writer.nullValue();
    } else {
        writer.stringValue(value.enumerationMemberName);
    }
    writer.field("representation", "opaque");
    writer.endObject();
}

void writeRuntimeValue(JsonWriter& writer, const RuntimeValue& value,
                       size_t depth) {
    if (depth > kMaximumProtocolNestingDepth) {
        throw std::runtime_error(
            "runtime value exceeds the machine-protocol nesting limit");
    }

    switch (value.kind) {
    case RuntimeValueKind::Missing:
        writer.beginObject();
        writer.field("kind", "missing");
        writer.endObject();
        return;
    case RuntimeValueKind::MissingArray:
        writer.beginObject();
        writer.field("kind", "missing");
        writer.key("dimensions");
        writeDimensions(writer, value);
        writer.endObject();
        return;
    case RuntimeValueKind::Number:
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        writeNumericValue(writer, value);
        return;
    case RuntimeValueKind::CharacterArray:
        writeCharacterValue(writer, value);
        return;
    case RuntimeValueKind::StringArray:
        writeStringValue(writer, value);
        return;
    case RuntimeValueKind::Cell:
        writeCellLikeValue(writer, value, "cell", depth);
        return;
    case RuntimeValueKind::FunctionHandle:
        writeFunctionHandleValue(writer, value);
        return;
    case RuntimeValueKind::Struct:
        writeStructValue(writer, value, depth);
        return;
    case RuntimeValueKind::CommaSeparatedList:
        writeCellLikeValue(
            writer, value, "comma-separated-list", depth);
        return;
    case RuntimeValueKind::NameValueArgument:
        if (value.cells.size() != 1) {
            throw std::runtime_error(
                "name-value runtime payload is missing its value");
        }
        writer.beginObject();
        writer.field("kind", "name-value-argument");
        writer.field("name", value.text);
        writer.key("value");
        writeRuntimeValue(writer, value.cells.front(), depth + 1);
        writer.endObject();
        return;
    case RuntimeValueKind::Object:
        writeObjectValue(writer, value);
        return;
    }
    throw std::runtime_error(
        "unsupported runtime value kind in the machine protocol");
}

void writePosition(JsonWriter& writer,
                   const ModuleSourcePosition& position) {
    writer.beginObject();
    writer.field("offset", protocolSize(position.offset));
    writer.key("line");
    writer.signedValue(position.line);
    writer.key("column");
    writer.signedValue(position.column);
    writer.endObject();
}

void writeSourceRange(JsonWriter& writer,
                      const ModuleSourceRange& source) {
    if (!source.available) {
        writer.nullValue();
        return;
    }
    writer.beginObject();
    writer.field("name", source.sourceName);
    writer.key("begin");
    writePosition(writer, source.begin);
    writer.key("end");
    writePosition(writer, source.end);
    writer.endObject();
}

void writeFrame(JsonWriter& writer,
                const ModuleDiagnosticFrame& frame) {
    writer.beginObject();
    writer.field("source", frame.sourceName);
    writer.field("function", frame.functionName);
    writer.key("line");
    writer.signedValue(frame.line);
    writer.endObject();
}

void writeCause(JsonWriter& writer,
                const ModuleDiagnosticCause& cause, size_t depth) {
    if (depth > kMaximumProtocolNestingDepth) {
        throw std::runtime_error(
            "diagnostic cause exceeds the machine-protocol nesting limit");
    }
    writer.beginObject();
    writer.field("identifier", cause.identifier);
    writer.field("message", cause.message);
    writer.key("stack");
    writer.beginArray();
    for (const auto& frame : cause.stack) {
        writeFrame(writer, frame);
    }
    writer.endArray();
    writer.key("causes");
    writer.beginArray();
    for (const auto& nested : cause.causes) {
        writeCause(writer, nested, depth + 1);
    }
    writer.endArray();
    writer.endObject();
}

void writeDiagnostic(JsonWriter& writer,
                     const ModuleDiagnostic& diagnostic) {
    writer.beginObject();
    writer.field("phase",
                 moduleDiagnosticPhaseName(diagnostic.phase));
    writer.field(
        "severity",
        moduleDiagnosticSeverityName(diagnostic.severity));
    writer.field("identifier", diagnostic.identifier);
    writer.field("message", diagnostic.message);
    writer.key("source");
    writeSourceRange(writer, diagnostic.source);
    writer.key("stack");
    writer.beginArray();
    for (const auto& frame : diagnostic.stack) {
        writeFrame(writer, frame);
    }
    writer.endArray();
    writer.key("causes");
    writer.beginArray();
    for (const auto& cause : diagnostic.causes) {
        writeCause(writer, cause, 0);
    }
    writer.endArray();
    writer.endObject();
}

void writeExecutionSummary(JsonWriter& writer,
                           const ModuleExecutionSummary& execution) {
    writer.beginObject();
    writer.field(
        "requested_backend",
        moduleExecutionBackendName(execution.requestedBackend));
    writer.field(
        "effective_tier",
        moduleExecutionTierName(execution.effectiveTier));
    writer.field("profiling_collected",
                 execution.profilingCollected);
    writer.field("fallback_occurred", execution.fallbackOccurred);
    writer.field("resource_controls_active",
                 execution.resourceControlsActive);
    writer.field("optimized_execution_suppressed",
                 execution.optimizedExecutionSuppressed);
    writer.field(
        "stop_reason",
        runtimeExecutionStopReasonName(execution.stopReason));
    writer.field(
        "executed_instruction_count",
        protocolSize(execution.executedInstructionCount));
    writer.field("typed_region_count",
                 protocolSize(execution.typedRegionCount));
    writer.field(
        "typed_region_attempt_count",
        protocolSize(execution.typedRegionAttemptCount));
    writer.field(
        "typed_region_execution_count",
        protocolSize(execution.typedRegionExecutionCount));
    writer.field(
        "typed_region_fallback_count",
        protocolSize(execution.typedRegionFallbackCount));
    writer.field(
        "native_compilation_count",
        protocolSize(execution.nativeCompilationCount));
    writer.field(
        "native_cache_hit_count",
        protocolSize(execution.nativeCacheHitCount));
    writer.field("maximum_call_depth",
                 protocolSize(execution.maximumCallDepth));
    writer.field("maximum_array_bytes",
                 protocolSize(execution.maximumArrayBytes));
    writer.field("maximum_diagnostic_count",
                 protocolSize(execution.maximumDiagnosticCount));
    writer.field("elapsed_nanoseconds",
                 execution.elapsedNanoseconds);
    writer.endObject();
}

} // namespace

std::string serializeMachineResultJsonV1(
    const ModuleInvocationResult& result,
    std::string_view engineVersion) {
    JsonWriter writer;
    writer.beginObject();
    writer.key("protocol");
    writer.beginObject();
    writer.field("name", "mparser.result");
    writer.field(
        "major",
        static_cast<std::uint64_t>(kMachineResultProtocolMajor));
    writer.field(
        "minor",
        static_cast<std::uint64_t>(kMachineResultProtocolMinor));
    writer.endObject();
    writer.key("engine");
    writer.beginObject();
    writer.field("name", "MParser");
    writer.field("version", engineVersion);
    writer.endObject();
    writer.field("status", moduleInvocationStatusName(result.status));
    writer.field("entry_function", result.entryFunction);
    writer.field(
        "requested_output_count",
        protocolSize(result.requestedOutputCount));
    writer.key("outputs");
    writer.beginArray();
    for (size_t index = 0; index < result.outputs.size(); ++index) {
        writer.beginObject();
        writer.key("name");
        if (index < result.outputNames.size()) {
            writer.stringValue(result.outputNames[index]);
        } else {
            writer.nullValue();
        }
        writer.key("value");
        writeRuntimeValue(writer, result.outputs[index], 0);
        writer.endObject();
    }
    writer.endArray();
    writer.key("output_events");
    writer.beginArray();
    for (const auto& event : result.outputEvents) {
        writer.beginObject();
        writer.field("sequence", event.sequence);
        writer.field("kind", moduleOutputKindName(event.kind));
        writer.field("text", event.text);
        writer.key("source");
        writeSourceRange(writer, event.source);
        writer.endObject();
    }
    writer.endArray();
    writer.key("top_level_expressions");
    writer.beginArray();
    for (const auto& expression : result.topLevelExpressions) {
        writer.beginObject();
        writer.field("sequence", expression.sequence);
        writer.field("output_suppressed", expression.outputSuppressed);
        writer.key("source");
        writeSourceRange(writer, expression.source);
        writer.key("value");
        writeRuntimeValue(writer, expression.value, 0);
        writer.endObject();
    }
    writer.endArray();
    writer.key("workspace");
    writer.beginArray();
    for (const auto& variable : result.variables) {
        writer.beginObject();
        writer.field("name", variable.name);
        writer.key("value");
        writeRuntimeValue(writer, variable.value, 0);
        writer.endObject();
    }
    writer.endArray();
    writer.key("diagnostics");
    writer.beginArray();
    for (const auto& diagnostic : result.diagnostics) {
        writeDiagnostic(writer, diagnostic);
    }
    writer.endArray();
    writer.key("execution");
    writeExecutionSummary(writer, result.execution);
    writer.endObject();
    return writer.finish();
}

std::string_view machineProtocolEmergencyJsonV1() noexcept {
    return kEmergencyMachineResultJsonV1;
}

int writeMachineProtocolEmergencyJsonV1(
    std::FILE* output) noexcept {
    (void)writeMachineDocument(
        output, kEmergencyMachineResultJsonV1);
    return 4;
}

int writeMachineResultJsonV1(
    std::FILE* output,
    const ModuleInvocationResult& result,
    std::string_view engineVersion) noexcept {
    try {
        const auto document =
            serializeMachineResultJsonV1(result, engineVersion);
        if (!writeMachineDocument(output, document)) {
            return 4;
        }
        return machineResultExitCode(result.status);
    } catch (...) {
        return writeMachineProtocolEmergencyJsonV1(output);
    }
}

int machineResultExitCode(ModuleInvocationStatus status) noexcept {
    switch (status) {
    case ModuleInvocationStatus::Succeeded:
        return 0;
    case ModuleInvocationStatus::CompilationFailed:
        return 1;
    case ModuleInvocationStatus::RequestRejected:
        return 2;
    case ModuleInvocationStatus::RuntimeFailed:
        return 3;
    }
    return 4;
}

} // namespace mparser
