#include "mparser/runtime_metadata.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <utility>

namespace mparser {
namespace {

constexpr std::string_view kMetadataMetaDataClass =
    "matlab.metadata.MetaData";
constexpr std::string_view kMetadataClassClass =
    "matlab.metadata.Class";
constexpr std::string_view kMetadataPropertyClass =
    "matlab.metadata.Property";
constexpr std::string_view kMetadataMethodClass =
    "matlab.metadata.Method";
constexpr std::string_view kMetadataEventClass =
    "matlab.metadata.Event";
constexpr std::string_view kMetadataEnumerationMemberClass =
    "matlab.metadata.EnumerationMember";
constexpr std::string_view kMetadataNamespaceClass =
    "matlab.metadata.Namespace";

} // namespace

std::string_view runtimeMetadataClassName(RuntimeMetadataKind kind) {
    switch (kind) {
    case RuntimeMetadataKind::MetaData:
        return kMetadataMetaDataClass;
    case RuntimeMetadataKind::Class:
        return kMetadataClassClass;
    case RuntimeMetadataKind::Property:
        return kMetadataPropertyClass;
    case RuntimeMetadataKind::Method:
        return kMetadataMethodClass;
    case RuntimeMetadataKind::Event:
        return kMetadataEventClass;
    case RuntimeMetadataKind::EnumerationMember:
        return kMetadataEnumerationMemberClass;
    case RuntimeMetadataKind::Namespace:
        return kMetadataNamespaceClass;
    }
    return kMetadataMetaDataClass;
}

std::string canonicalRuntimeMetadataClassName(std::string_view name) {
    if (name == "meta.MetaData") {
        return std::string(kMetadataMetaDataClass);
    }
    if (name == "meta.class") {
        return std::string(kMetadataClassClass);
    }
    if (name == "meta.property") {
        return std::string(kMetadataPropertyClass);
    }
    if (name == "meta.method") {
        return std::string(kMetadataMethodClass);
    }
    if (name == "meta.event") {
        return std::string(kMetadataEventClass);
    }
    if (name == "meta.EnumerationMember" ||
        name == "meta.EnumeratedValue") {
        return std::string(kMetadataEnumerationMemberClass);
    }
    if (name == "meta.package") {
        return std::string(kMetadataNamespaceClass);
    }
    return std::string(name);
}

std::optional<RuntimeMetadataKind>
runtimeMetadataKind(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Object) {
        return std::nullopt;
    }

    const std::string canonical =
        canonicalRuntimeMetadataClassName(value.className);
    if (canonical == kMetadataMetaDataClass) {
        return RuntimeMetadataKind::MetaData;
    }
    if (canonical == kMetadataClassClass) {
        return RuntimeMetadataKind::Class;
    }
    if (canonical == kMetadataPropertyClass) {
        return RuntimeMetadataKind::Property;
    }
    if (canonical == kMetadataMethodClass) {
        return RuntimeMetadataKind::Method;
    }
    if (canonical == kMetadataEventClass) {
        return RuntimeMetadataKind::Event;
    }
    if (canonical == kMetadataEnumerationMemberClass) {
        return RuntimeMetadataKind::EnumerationMember;
    }
    if (canonical == kMetadataNamespaceClass) {
        return RuntimeMetadataKind::Namespace;
    }
    return std::nullopt;
}

bool isRuntimeMetadataObject(const RuntimeValue& value) {
    return runtimeMetadataKind(value).has_value();
}

bool isRuntimeMetadataScalar(const RuntimeValue& value) {
    return isRuntimeMetadataObject(value) && !value.text.empty();
}

bool isRuntimeMetadataArray(const RuntimeValue& value) {
    return isRuntimeMetadataObject(value) && value.text.empty();
}

RuntimeValue makeRuntimeMetadataObject(RuntimeMetadataKind kind,
                                       std::string identity) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(runtimeMetadataClassName(kind));
    result.text = std::move(identity);
    result.handleObject = true;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue makeRuntimeMetadataArray(
    RuntimeMetadataKind kind, std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(runtimeMetadataClassName(kind));
    result.handleObject = true;
    result.cells = std::move(elements);
    if (dimensions.empty()) {
        dimensions = {result.cells.size(), 1};
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

bool runtimeMetadataIsa(const RuntimeValue& value,
                        std::string_view className) {
    if (!isRuntimeMetadataObject(value)) {
        return false;
    }

    const std::string actual =
        canonicalRuntimeMetadataClassName(value.className);
    const std::string target =
        canonicalRuntimeMetadataClassName(className);
    if (actual == target) {
        return true;
    }
    if (target == "handle" || target == kMetadataMetaDataClass) {
        return true;
    }
    return false;
}

std::string runtimeValueClassName(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return "missing";
    case RuntimeValueKind::Number:
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        return std::string(
            runtimeNumericClassName(value.numericClass));
    case RuntimeValueKind::String:
        return "char";
    case RuntimeValueKind::Cell:
        return "cell";
    case RuntimeValueKind::FunctionHandle:
        return "function_handle";
    case RuntimeValueKind::Struct:
        return "struct";
    case RuntimeValueKind::NameValueArgument:
        return "name_value_argument";
    case RuntimeValueKind::Object:
        return canonicalRuntimeMetadataClassName(value.className);
    }
    return "missing";
}

} // namespace mparser
