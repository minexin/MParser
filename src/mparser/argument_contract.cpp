#include "mparser/argument_contract.h"

#include "mparser/function_signature.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <unordered_set>

namespace mparser {
namespace {

enum class ClassResolutionState {
    Unresolved,
    Resolving,
    Resolved,
};

std::string lowerAscii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::string trimAscii(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n\v\f");
    return std::string(text.substr(first, last - first + 1));
}

bool logicalAttributeEnabled(const AttributeSyntax& attribute) {
    if (attribute.value.empty()) {
        return !attribute.negated;
    }
    if (attribute.negated) {
        return false;
    }
    return lowerAscii(trimAscii(attribute.value)) == "true";
}

ClassPropertyArgumentSetAccess accessValue(
    const AttributeSyntax& attribute, bool allowImmutable) {
    if (attribute.negated || attribute.value.empty() ||
        attribute.hasMetaClassList) {
        return ClassPropertyArgumentSetAccess::NonPublic;
    }
    const std::string value = lowerAscii(trimAscii(attribute.value));
    if (value == "public") {
        return ClassPropertyArgumentSetAccess::Public;
    }
    if (allowImmutable && value == "immutable") {
        return ClassPropertyArgumentSetAccess::Immutable;
    }
    return ClassPropertyArgumentSetAccess::NonPublic;
}

ClassPropertyArgument collectProperty(const HirNode& node,
                                      std::string className) {
    ClassPropertyArgument property;
    property.name = node.label;
    property.declaringClass = std::move(className);
    property.property = node.property;
    property.span = node.span;

    for (const auto& attribute : node.attributes) {
        if (lowerAscii(attribute.name) == "access") {
            property.setAccess = accessValue(attribute, false);
        }
    }
    for (const auto& attribute : node.attributes) {
        const std::string name = lowerAscii(attribute.name);
        if (name == "setaccess") {
            property.setAccess = accessValue(attribute, true);
        } else if (name == "constant") {
            property.constant = logicalAttributeEnabled(attribute);
        }
    }
    return property;
}

void collectProperties(const HirNode& node, const std::string& className,
                       std::vector<ClassPropertyArgument>& properties) {
    for (const auto& child : node.children) {
        if (child->kind == HirKind::Class ||
            child->kind == HirKind::Function) {
            continue;
        }
        if (child->kind == HirKind::Property) {
            properties.push_back(collectProperty(*child, className));
            continue;
        }
        collectProperties(*child, className, properties);
    }
}

void collectClasses(const HirNode& node,
                    std::map<std::string, ClassArgumentContract>& classes) {
    if (node.kind == HirKind::Class) {
        ClassArgumentContract klass;
        klass.name = node.label;
        klass.span = node.span;
        klass.superclasses = node.superclasses;
        collectProperties(node, node.label, klass.properties);
        classes[node.label] = std::move(klass);
        return;
    }
    for (const auto& child : node.children) {
        collectClasses(*child, classes);
    }
}

void mergeProperty(std::vector<ClassPropertyArgument>& properties,
                   ClassPropertyArgument property, bool declaredHere) {
    const auto existing = std::find_if(
        properties.begin(), properties.end(),
        [&](const ClassPropertyArgument& candidate) {
            return candidate.name == property.name;
        });
    if (existing == properties.end()) {
        properties.push_back(std::move(property));
        return;
    }
    if (declaredHere &&
        property.setAccess != ClassPropertyArgumentSetAccess::NonPublic) {
        *existing = std::move(property);
    }
}

void resolveClass(
    const std::string& name,
    const std::map<std::string, ClassArgumentContract>& declared,
    std::map<std::string, ClassArgumentContract>& resolved,
    std::map<std::string, ClassResolutionState>& states) {
    if (states[name] == ClassResolutionState::Resolved) {
        return;
    }
    if (states[name] == ClassResolutionState::Resolving) {
        return;
    }
    const auto source = declared.find(name);
    if (source == declared.end()) {
        return;
    }

    states[name] = ClassResolutionState::Resolving;
    ClassArgumentContract effective;
    effective.name = source->second.name;
    effective.span = source->second.span;
    effective.superclasses = source->second.superclasses;
    for (const auto& superclass : source->second.superclasses) {
        if (superclass == "handle") {
            continue;
        }
        resolveClass(superclass, declared, resolved, states);
        const auto inherited = resolved.find(superclass);
        if (inherited == resolved.end()) {
            continue;
        }
        for (const auto& property : inherited->second.properties) {
            mergeProperty(effective.properties, property, false);
        }
    }
    for (const auto& property : source->second.properties) {
        mergeProperty(effective.properties, property, true);
    }
    resolved[name] = std::move(effective);
    states[name] = ClassResolutionState::Resolved;
}

bool isConstructor(const HirNode& function) {
    return !function.lexicalClassName.empty() &&
           function.label == function.lexicalClassName;
}

std::string argumentRoot(std::string_view name) {
    const size_t dot = name.find('.');
    return dot == std::string_view::npos
               ? std::string(name)
               : std::string(name.substr(0, dot));
}

std::string argumentField(std::string_view name) {
    const size_t dot = name.find('.');
    return dot == std::string_view::npos
               ? std::string{}
               : std::string(name.substr(dot + 1));
}

void validateFunctionContracts(
    const HirNode& function, const ArgumentContractCatalog& catalog,
    std::vector<Diagnostic>& diagnostics) {
    const auto resolution = resolveArgumentContracts(function, catalog);
    diagnostics.insert(diagnostics.end(), resolution.diagnostics.begin(),
                       resolution.diagnostics.end());

    std::unordered_set<std::string> nameValueRoots;
    for (const auto& contract : resolution.contracts) {
        if (contract.blockKind == ArgumentBlockKind::Input &&
            contract.name.find('.') != std::string::npos) {
            nameValueRoots.insert(argumentRoot(contract.name));
        }
    }

    const FunctionSignature signature = parseFunctionSignature(function);
    std::set<std::string> reportedFields;
    for (const auto& contract : resolution.contracts) {
        if (!contract.classDerived ||
            contract.blockKind != ArgumentBlockKind::Input) {
            continue;
        }
        const std::string field = argumentField(contract.name);
        if (field.empty()) {
            continue;
        }
        for (const auto& other : resolution.contracts) {
            if (&other == &contract ||
                other.blockKind != ArgumentBlockKind::Input ||
                other.name == contract.name ||
                argumentField(other.name) != field) {
                continue;
            }
            if (reportedFields.insert(field).second) {
                diagnostics.push_back(Diagnostic{
                    contract.span,
                    "name-value argument field must be globally unique: " +
                        field});
            }
            break;
        }
        if (nameValueRoots.contains(field)) {
            continue;
        }
        if (std::find(signature.parameters.begin(),
                      signature.parameters.end(),
                      field) != signature.parameters.end()) {
            diagnostics.push_back(Diagnostic{
                contract.span,
                "name-value argument conflicts with a positional or repeating parameter: " +
                    field});
        }
    }
}

void validateFunctions(const HirNode& node,
                       const ArgumentContractCatalog& catalog,
                       std::vector<Diagnostic>& diagnostics) {
    if (node.kind == HirKind::Function) {
        validateFunctionContracts(node, catalog, diagnostics);
    }
    for (const auto& child : node.children) {
        validateFunctions(*child, catalog, diagnostics);
    }
}

} // namespace

ArgumentContractCatalog buildArgumentContractCatalog(const HirNode& root) {
    std::map<std::string, ClassArgumentContract> declared;
    collectClasses(root, declared);

    ArgumentContractCatalog catalog;
    std::map<std::string, ClassResolutionState> states;
    for (const auto& [name, klass] : declared) {
        (void)klass;
        resolveClass(name, declared, catalog.classes, states);
    }
    return catalog;
}

ArgumentContractResolution resolveArgumentContracts(
    const HirNode& function, const ArgumentContractCatalog& catalog) {
    ArgumentContractResolution resolution;
    std::unordered_set<std::string> explicitNames;
    size_t sourceCount = 0;

    for (const auto& block : function.children) {
        if (block->kind != HirKind::ArgumentBlock) {
            continue;
        }
        for (const auto& declaration : block->children) {
            if (declaration->kind != HirKind::Argument) {
                continue;
            }
            if (declaration->nameValueSourceClass.empty()) {
                explicitNames.insert(declaration->label);
            } else {
                ++sourceCount;
            }
        }
    }

    if (sourceCount > 1) {
        resolution.diagnostics.push_back(Diagnostic{
            function.span,
            "function can contain only one class-property name-value source"});
    }

    const bool constructor = isConstructor(function);
    for (const auto& block : function.children) {
        if (block->kind != HirKind::ArgumentBlock) {
            continue;
        }
        for (const auto& declaration : block->children) {
            if (declaration->kind != HirKind::Argument) {
                continue;
            }
            if (declaration->nameValueSourceClass.empty()) {
                resolution.contracts.push_back(ResolvedArgumentContract{
                    declaration->label, declaration->property,
                    declaration->span, block->argumentBlock.kind,
                    declaration.get(), false});
                continue;
            }

            const auto klass =
                catalog.classes.find(declaration->nameValueSourceClass);
            if (klass == catalog.classes.end()) {
                resolution.diagnostics.push_back(Diagnostic{
                    declaration->nameValueSourceSpan,
                    "name-value property source class is not available: " +
                        declaration->nameValueSourceClass});
                continue;
            }
            for (const auto& property : klass->second.properties) {
                if (property.constant ||
                    property.setAccess ==
                        ClassPropertyArgumentSetAccess::NonPublic ||
                    (property.setAccess ==
                         ClassPropertyArgumentSetAccess::Immutable &&
                     (!constructor ||
                      property.declaringClass !=
                          function.lexicalClassName))) {
                    continue;
                }
                const std::string name =
                    declaration->label + "." + property.name;
                if (explicitNames.contains(name)) {
                    continue;
                }
                PropertySpec validation = property.property;
                validation.hasExplicitDefault = false;
                resolution.contracts.push_back(ResolvedArgumentContract{
                    name, std::move(validation), property.span,
                    block->argumentBlock.kind, declaration.get(), true});
            }
        }
    }
    return resolution;
}

std::vector<Diagnostic> validateClassPropertyArgumentContracts(
    const HirNode& root, const ArgumentContractCatalog& catalog) {
    std::vector<Diagnostic> diagnostics;
    validateFunctions(root, catalog, diagnostics);
    return diagnostics;
}

} // namespace mparser
