#include "mparser/runtime/core/runtime_metadata.h"

#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace mparser {
namespace {

const std::vector<RuntimeMetadataTypeDescriptor>&
runtimeMetadataDescriptors() {
    static const std::vector<RuntimeMetadataTypeDescriptor> descriptors = {
        {
            RuntimeMetadataKind::MetaData,
            "matlab.metadata.MetaData",
            {"meta.MetaData"},
            std::nullopt,
            {},
            {"eq", "ne"},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::Class,
            "matlab.metadata.Class",
            {"meta.class"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "Hidden",
                "Sealed", "Abstract", "Enumeration", "ConstructOnLoad",
                "HandleCompatible", "InferiorClasses", "Namespace",
                "Aliases", "RestrictsSubclassing", "PropertyList",
                "MethodList", "EventList", "EnumerationMemberList",
                "SuperclassList",
            },
            {"fromName", "lt", "le", "gt", "ge"},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::Property,
            "matlab.metadata.Property",
            {"meta.property"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "GetAccess",
                "SetAccess", "Dependent", "Constant", "Abstract",
                "Transient", "Hidden", "GetObservable", "SetObservable",
                "AbortSet", "NonCopyable", "WeakHandle",
                "PartialMatchPriority", "GetMethod", "SetMethod",
                "HasDefault", "DefaultValue", "Validation",
                "DefiningClass",
            },
            {},
            true,
            false,
            false,
            true,
            true,
            true,
        },
        {
            RuntimeMetadataKind::DynamicProperty,
            "matlab.metadata.DynamicProperty",
            {"meta.DynamicProperty"},
            RuntimeMetadataKind::Property,
            {},
            {"delete", "isvalid"},
            false,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::Method,
            "matlab.metadata.Method",
            {"meta.method"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "Access",
                "Static", "Abstract", "Sealed", "Hidden", "InputNames",
                "OutputNames", "Signature", "FullPath", "DefiningClass",
            },
            {},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::Event,
            "matlab.metadata.Event",
            {"meta.event"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "Hidden",
                "ListenAccess", "NotifyAccess", "DefiningClass",
            },
            {},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::EnumerationMember,
            "matlab.metadata.EnumerationMember",
            {"meta.EnumerationMember", "meta.EnumeratedValue"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "Hidden",
                "DefiningClass",
            },
            {},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::Namespace,
            "matlab.metadata.Namespace",
            {"meta.package"},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "ClassList",
                "FunctionList", "InnerNamespaces", "OuterNamespace",
            },
            {},
            true,
            true,
            true,
            true,
            true,
            false,
            true,
        },
        {
            RuntimeMetadataKind::Function,
            "matlab.metadata.Function",
            {},
            RuntimeMetadataKind::MetaData,
            {
                "Name", "Description", "DetailedDescription", "FullPath",
                "NamespaceName", "Signature",
            },
            {},
            true,
            false,
            false,
            true,
            true,
        },
        {
            RuntimeMetadataKind::CallSignature,
            "matlab.metadata.CallSignature",
            {},
            std::nullopt,
            {
                "Inputs", "Outputs", "HasInputValidation",
                "HasOutputValidation",
            },
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::Argument,
            "matlab.metadata.Argument",
            {},
            std::nullopt,
            {
                "Identifier", "Description", "DetailedDescription",
                "Required", "Repeating", "NameValue", "Validation",
                "DefaultValue", "SourceClass",
            },
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::ArgumentIdentifier,
            "matlab.metadata.ArgumentIdentifier",
            {},
            std::nullopt,
            {"Name", "GroupName"},
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::ArgumentValidation,
            "matlab.metadata.ArgumentValidation",
            {},
            std::nullopt,
            {"Class", "Size", "Functions"},
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::ArgumentValidator,
            "matlab.metadata.ArgumentValidator",
            {},
            std::nullopt,
            {"Name", "Function", "ReferencedArguments"},
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::DefaultArgumentValue,
            "matlab.metadata.DefaultArgumentValue",
            {},
            std::nullopt,
            {"Expression", "ReferencedArguments"},
            {"eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::PropertyValidation,
            "matlab.metadata.PropertyValidation",
            {"matlab.metadata.Validation", "meta.Validation"},
            std::nullopt,
            {"Class", "Size", "ValidationFunctions"},
            {"isValidValue", "validateValue", "eq", "ne"},
            false,
            true,
        },
        {
            RuntimeMetadataKind::ArrayDimension,
            "matlab.metadata.ArrayDimension",
            {"meta.ArrayDimension"},
            std::nullopt,
            {},
            {"eq", "ne"},
            true,
        },
        {
            RuntimeMetadataKind::FixedDimension,
            "matlab.metadata.FixedDimension",
            {"meta.FixedDimension"},
            RuntimeMetadataKind::ArrayDimension,
            {"Length"},
            {},
            false,
            true,
        },
        {
            RuntimeMetadataKind::UnrestrictedDimension,
            "matlab.metadata.UnrestrictedDimension",
            {"meta.UnrestrictedDimension"},
            RuntimeMetadataKind::ArrayDimension,
            {},
            {},
            false,
            true,
        },
    };
    return descriptors;
}

void appendMetadataMembers(
    const RuntimeMetadataTypeDescriptor& descriptor, bool properties,
    std::vector<std::string>& result, std::set<std::string>& seen) {
    if (descriptor.superclass) {
        appendMetadataMembers(
            runtimeMetadataTypeDescriptor(*descriptor.superclass),
            properties, result, seen);
    }
    const auto& declared = properties ? descriptor.declaredProperties
                                      : descriptor.declaredMethods;
    for (const std::string_view member : declared) {
        if (seen.insert(std::string(member)).second) {
            result.emplace_back(member);
        }
    }
}

} // namespace

const RuntimeMetadataTypeDescriptor&
runtimeMetadataTypeDescriptor(RuntimeMetadataKind kind) {
    const auto& descriptors = runtimeMetadataDescriptors();
    const auto found = std::find_if(
        descriptors.begin(), descriptors.end(),
        [kind](const RuntimeMetadataTypeDescriptor& descriptor) {
            return descriptor.kind == kind;
        });
    return found == descriptors.end() ? descriptors.front() : *found;
}

const RuntimeMetadataTypeDescriptor*
findRuntimeMetadataTypeDescriptor(std::string_view className) {
    const auto& descriptors = runtimeMetadataDescriptors();
    const auto found = std::find_if(
        descriptors.begin(), descriptors.end(),
        [className](const RuntimeMetadataTypeDescriptor& descriptor) {
            return descriptor.canonicalName == className ||
                   std::find(descriptor.aliases.begin(),
                             descriptor.aliases.end(),
                             className) != descriptor.aliases.end();
        });
    return found == descriptors.end() ? nullptr : &*found;
}

std::vector<std::string>
runtimeMetadataPropertyNames(std::string_view className) {
    const auto* descriptor =
        findRuntimeMetadataTypeDescriptor(className);
    if (!descriptor) {
        return {};
    }
    std::vector<std::string> result;
    std::set<std::string> seen;
    appendMetadataMembers(*descriptor, true, result, seen);
    return result;
}

std::vector<std::string>
runtimeMetadataMethodNames(std::string_view className) {
    const auto* descriptor =
        findRuntimeMetadataTypeDescriptor(className);
    if (!descriptor) {
        return {};
    }
    std::vector<std::string> result;
    std::set<std::string> seen;
    appendMetadataMembers(*descriptor, false, result, seen);
    return result;
}

bool runtimeMetadataClassIsa(std::string_view actualClassName,
                             std::string_view targetClassName) {
    const auto* actual =
        findRuntimeMetadataTypeDescriptor(actualClassName);
    if (!actual) {
        return false;
    }

    if (targetClassName == "handle") {
        std::optional<RuntimeMetadataKind> current = actual->kind;
        while (current) {
            const auto& descriptor =
                runtimeMetadataTypeDescriptor(*current);
            if (descriptor.handleClass) {
                return true;
            }
            current = descriptor.superclass;
        }
        return false;
    }

    const auto* target =
        findRuntimeMetadataTypeDescriptor(targetClassName);
    if (!target) {
        return false;
    }
    std::optional<RuntimeMetadataKind> current = actual->kind;
    while (current) {
        if (*current == target->kind) {
            return true;
        }
        current = runtimeMetadataTypeDescriptor(*current).superclass;
    }
    return false;
}

std::string_view runtimeMetadataClassName(RuntimeMetadataKind kind) {
    return runtimeMetadataTypeDescriptor(kind).canonicalName;
}

std::string canonicalRuntimeMetadataClassName(std::string_view name) {
    const auto* descriptor =
        findRuntimeMetadataTypeDescriptor(name);
    return descriptor ? std::string(descriptor->canonicalName)
                      : std::string(name);
}

std::optional<RuntimeMetadataKind>
runtimeMetadataKind(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Object) {
        return std::nullopt;
    }

    const auto* descriptor =
        findRuntimeMetadataTypeDescriptor(value.className);
    return descriptor ? std::optional(descriptor->kind) : std::nullopt;
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
    const auto actualKind = runtimeMetadataKind(value);
    if (!actualKind) {
        return false;
    }
    return runtimeMetadataClassIsa(
        runtimeMetadataClassName(*actualKind), className);
}

std::string runtimeValueClassName(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::MissingArray:
        return "missing";
    case RuntimeValueKind::Number:
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        return std::string(
            runtimeNumericClassName(value.numericClass));
    case RuntimeValueKind::CharacterArray:
        return "char";
    case RuntimeValueKind::StringArray:
        return "string";
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
