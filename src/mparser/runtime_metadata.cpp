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
constexpr std::string_view kMetadataDynamicPropertyClass =
    "matlab.metadata.DynamicProperty";
constexpr std::string_view kMetadataMethodClass =
    "matlab.metadata.Method";
constexpr std::string_view kMetadataEventClass =
    "matlab.metadata.Event";
constexpr std::string_view kMetadataEnumerationMemberClass =
    "matlab.metadata.EnumerationMember";
constexpr std::string_view kMetadataNamespaceClass =
    "matlab.metadata.Namespace";
constexpr std::string_view kMetadataFunctionClass =
    "matlab.metadata.Function";
constexpr std::string_view kMetadataCallSignatureClass =
    "matlab.metadata.CallSignature";
constexpr std::string_view kMetadataArgumentClass =
    "matlab.metadata.Argument";
constexpr std::string_view kMetadataArgumentIdentifierClass =
    "matlab.metadata.ArgumentIdentifier";
constexpr std::string_view kMetadataArgumentValidationClass =
    "matlab.metadata.ArgumentValidation";
constexpr std::string_view kMetadataArgumentValidatorClass =
    "matlab.metadata.ArgumentValidator";
constexpr std::string_view kMetadataDefaultArgumentValueClass =
    "matlab.metadata.DefaultArgumentValue";
constexpr std::string_view kMetadataArrayDimensionClass =
    "matlab.metadata.ArrayDimension";
constexpr std::string_view kMetadataFixedDimensionClass =
    "matlab.metadata.FixedDimension";
constexpr std::string_view kMetadataUnrestrictedDimensionClass =
    "matlab.metadata.UnrestrictedDimension";

} // namespace

std::string_view runtimeMetadataClassName(RuntimeMetadataKind kind) {
    switch (kind) {
    case RuntimeMetadataKind::MetaData:
        return kMetadataMetaDataClass;
    case RuntimeMetadataKind::Class:
        return kMetadataClassClass;
    case RuntimeMetadataKind::Property:
        return kMetadataPropertyClass;
    case RuntimeMetadataKind::DynamicProperty:
        return kMetadataDynamicPropertyClass;
    case RuntimeMetadataKind::Method:
        return kMetadataMethodClass;
    case RuntimeMetadataKind::Event:
        return kMetadataEventClass;
    case RuntimeMetadataKind::EnumerationMember:
        return kMetadataEnumerationMemberClass;
    case RuntimeMetadataKind::Namespace:
        return kMetadataNamespaceClass;
    case RuntimeMetadataKind::Function:
        return kMetadataFunctionClass;
    case RuntimeMetadataKind::CallSignature:
        return kMetadataCallSignatureClass;
    case RuntimeMetadataKind::Argument:
        return kMetadataArgumentClass;
    case RuntimeMetadataKind::ArgumentIdentifier:
        return kMetadataArgumentIdentifierClass;
    case RuntimeMetadataKind::ArgumentValidation:
        return kMetadataArgumentValidationClass;
    case RuntimeMetadataKind::ArgumentValidator:
        return kMetadataArgumentValidatorClass;
    case RuntimeMetadataKind::DefaultArgumentValue:
        return kMetadataDefaultArgumentValueClass;
    case RuntimeMetadataKind::ArrayDimension:
        return kMetadataArrayDimensionClass;
    case RuntimeMetadataKind::FixedDimension:
        return kMetadataFixedDimensionClass;
    case RuntimeMetadataKind::UnrestrictedDimension:
        return kMetadataUnrestrictedDimensionClass;
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
    if (name == "meta.DynamicProperty") {
        return std::string(kMetadataDynamicPropertyClass);
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
    if (name == "meta.ArrayDimension") {
        return std::string(kMetadataArrayDimensionClass);
    }
    if (name == "meta.FixedDimension") {
        return std::string(kMetadataFixedDimensionClass);
    }
    if (name == "meta.UnrestrictedDimension") {
        return std::string(kMetadataUnrestrictedDimensionClass);
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
    if (canonical == kMetadataDynamicPropertyClass) {
        return RuntimeMetadataKind::DynamicProperty;
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
    if (canonical == kMetadataFunctionClass) {
        return RuntimeMetadataKind::Function;
    }
    if (canonical == kMetadataCallSignatureClass) {
        return RuntimeMetadataKind::CallSignature;
    }
    if (canonical == kMetadataArgumentClass) {
        return RuntimeMetadataKind::Argument;
    }
    if (canonical == kMetadataArgumentIdentifierClass) {
        return RuntimeMetadataKind::ArgumentIdentifier;
    }
    if (canonical == kMetadataArgumentValidationClass) {
        return RuntimeMetadataKind::ArgumentValidation;
    }
    if (canonical == kMetadataArgumentValidatorClass) {
        return RuntimeMetadataKind::ArgumentValidator;
    }
    if (canonical == kMetadataDefaultArgumentValueClass) {
        return RuntimeMetadataKind::DefaultArgumentValue;
    }
    if (canonical == kMetadataArrayDimensionClass) {
        return RuntimeMetadataKind::ArrayDimension;
    }
    if (canonical == kMetadataFixedDimensionClass) {
        return RuntimeMetadataKind::FixedDimension;
    }
    if (canonical == kMetadataUnrestrictedDimensionClass) {
        return RuntimeMetadataKind::UnrestrictedDimension;
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
    if (target == kMetadataArrayDimensionClass &&
        (actual == kMetadataFixedDimensionClass ||
         actual == kMetadataUnrestrictedDimensionClass)) {
        return true;
    }
    if (target == kMetadataPropertyClass &&
        actual == kMetadataDynamicPropertyClass) {
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
    case RuntimeValueKind::CommaSeparatedList:
        return "comma_separated_list";
    case RuntimeValueKind::NameValueArgument:
        return "name_value_argument";
    case RuntimeValueKind::Object:
        return canonicalRuntimeMetadataClassName(value.className);
    }
    return "missing";
}

} // namespace mparser
