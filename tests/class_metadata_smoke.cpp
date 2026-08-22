#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_metadata.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Compilation {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string diagnosticsText(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics) {
        if (!result.empty()) {
            result += " | ";
        }
        result += std::to_string(diagnostic.span.begin.line);
        result += ":";
        result += std::to_string(diagnostic.span.begin.column);
        result += " ";
        result += diagnostic.message;
    }
    return result;
}

bool hasDiagnostic(const std::vector<mparser::Diagnostic>& diagnostics,
                   std::string_view text) {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [&](const mparser::Diagnostic& diagnostic) {
            return diagnostic.message.find(text) != std::string::npos;
        });
}

Compilation compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.root != nullptr, "parser did not produce a root");
    require(parsed.diagnostics.empty(),
            "parser diagnostics: " +
                diagnosticsText(parsed.diagnostics));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    require(semantic.root != nullptr, "semantic analyzer produced no root");
    require(semantic.diagnostics.empty(),
            "semantic diagnostics: " +
                diagnosticsText(semantic.diagnostics));

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "bytecode diagnostics: " +
                diagnosticsText(bytecode.diagnostics));
    return {std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    const auto compiled = compile(source);
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
}

const mparser::RuntimeValue* variable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return &candidate.value;
        }
    }
    return nullptr;
}

void requireNumber(const mparser::BytecodeVmResult& result,
                   std::string_view name, double expected) {
    const auto* value = variable(result, name);
    require(value != nullptr,
            std::string("expected numeric variable: ") +
                std::string(name));
    require(value->kind == mparser::RuntimeValueKind::Number,
            std::string("runtime variable is not numeric: ") +
                std::string(name));
    if (std::fabs(value->number - expected) >= 1e-9) {
        throw std::runtime_error(
            std::string("numeric runtime value mismatch: ") +
            std::string(name) + " expected " +
            std::to_string(expected) + " got " +
            std::to_string(value->number));
    }
}

void requireString(const mparser::BytecodeVmResult& result,
                   std::string_view name, std::string_view expected) {
    const auto* value = variable(result, name);
    require(value != nullptr, "expected string variable");
    require(mparser::runtimeTextScalarUtf8(*value) == expected,
            "text runtime value mismatch");
}

constexpr std::string_view kMetadataClasses = R"(
classdef MetadataBase < handle
    properties
        BaseValue = 3
    end
    properties (Hidden)
        HiddenValue = 9
    end
    properties (GetAccess = private)
        SecretValue = 11
    end
    methods
        function value = baseMethod(obj)
            value = obj.BaseValue;
        end
    end
    methods (Hidden)
        function value = hiddenMethod(obj)
            value = obj.HiddenValue;
        end
    end
    events
        Changed
    end
end

classdef MetadataChild < MetadataBase
    properties
        ChildValue = 4
    end
    methods
        function value = childMethod(obj)
            value = obj.ChildValue;
        end
    end
    events (Hidden)
        HiddenChanged
    end
end
)";

void runMetadataQuerySmoke() {
    const std::string source =
        std::string(kMetadataClasses) + R"(
object = MetadataChild();
class_info = ?MetadataChild;
object_info = metaclass(object);
class_name = class_info.Name;
superclass_name = class_info.SuperclassList(1).Name;
first_property = class_info.PropertyList(1).Name;
first_event = class_info.EventList(1).Name;
same_class = class_info == object_info;
not_same_class = class_info ~= ?MetadataBase;
is_subclass = class_info < ?MetadataBase;
is_subclass_or_same = class_info <= ?MetadataBase;
is_superclass = ?MetadataBase > class_info;
is_superclass_or_same = ?MetadataBase >= class_info;
property_names = properties(object);
method_names = methods(object);
event_names = events(object);
first_visible_property = property_names{1};
first_visible_method = method_names{1};
first_visible_event = event_names{1};
declared_event = event_names{2};
property_count = numel(property_names);
event_count = numel(event_names);
has_base_value = isprop(object, 'BaseValue');
has_hidden_method = ismethod(object, 'hiddenMethod');
has_child_method = ismethod(object, 'childMethod');
old_alias_matches = isa(class_info, 'meta.class');
metadata_is_handle = isa(class_info, 'handle');
lookup_info = matlab.metadata.Class.fromName('MetadataChild');
old_lookup_info = meta.class.fromName('MetadataChild');
lookup_matches = lookup_info == class_info;
old_lookup_matches = old_lookup_info == class_info;
missing_lookup = matlab.metadata.Class.fromName('MissingMetadataClass');
missing_is_empty = isempty(missing_lookup);
method_info = metafunction('MetadataChild/childMethod');
method_name = method_info.Name;
method_owner = method_info.DefiningClass.Name;
method_is_public = strcmp(method_info.Access, 'public');
method_is_static = method_info.Static;
property_owner = class_info.PropertyList(1).DefiningClass.Name;
property_get_access = class_info.PropertyList(1).GetAccess;
metadata_class_name = class(class_info);
metadata_count = numel(class_info.PropertyList);
property_info_list = class_info.PropertyList;
method_count = numel(method_names);
)";

    const auto compiled = compile(source);
    mparser::BytecodeVm vm;
    const auto result = vm.run(compiled.bytecode, compiled.semantic);
    if (!result.diagnostics.empty()) {
        std::string detail =
            "metadata query diagnostics: " +
            diagnosticsText(result.diagnostics);
        const int line = result.diagnostics.front().span.begin.line;
        for (const auto& instruction : compiled.bytecode.instructions) {
            if (instruction.span.begin.line != line) {
                continue;
            }
            detail += " | ";
            detail += mparser::bytecodeOpName(instruction.op);
            detail += "(";
            detail += instruction.operand;
            detail += ", binding=";
            detail += std::to_string(
                static_cast<int>(instruction.binding.kind));
            detail += ")";
        }
        throw std::runtime_error(std::move(detail));
    }
    requireString(result, "class_name", "MetadataChild");
    requireString(result, "superclass_name", "MetadataBase");
    requireString(result, "first_property", "BaseValue");
    requireString(result, "first_event", "ObjectBeingDestroyed");
    requireString(result, "first_visible_property", "BaseValue");
    requireString(result, "first_visible_method", "baseMethod");
    requireString(result, "first_visible_event",
                  "ObjectBeingDestroyed");
    requireString(result, "declared_event", "Changed");
    requireString(result, "method_name", "childMethod");
    requireString(result, "method_owner", "MetadataChild");
    requireString(result, "property_owner", "MetadataBase");
    requireString(result, "property_get_access", "public");
    requireString(result, "metadata_class_name",
                  "matlab.metadata.Class");
    requireNumber(result, "same_class", 1.0);
    requireNumber(result, "not_same_class", 1.0);
    requireNumber(result, "is_subclass", 1.0);
    requireNumber(result, "is_subclass_or_same", 1.0);
    requireNumber(result, "is_superclass", 1.0);
    requireNumber(result, "is_superclass_or_same", 1.0);
    requireNumber(result, "property_count", 2.0);
    requireNumber(result, "method_count", 9.0);
    requireNumber(result, "event_count", 2.0);
    requireNumber(result, "has_base_value", 1.0);
    requireNumber(result, "has_hidden_method", 0.0);
    requireNumber(result, "has_child_method", 1.0);
    requireNumber(result, "old_alias_matches", 1.0);
    requireNumber(result, "metadata_is_handle", 1.0);
    requireNumber(result, "lookup_matches", 1.0);
    requireNumber(result, "old_lookup_matches", 1.0);
    requireNumber(result, "missing_is_empty", 1.0);
    requireNumber(result, "method_is_public", 1.0);
    requireNumber(result, "method_is_static", 0.0);
    requireNumber(result, "metadata_count", 4.0);

    const auto* classInfo = variable(result, "class_info");
    require(classInfo != nullptr, "class metadata variable missing");
    require(mparser::runtimeMetadataKind(*classInfo) ==
                mparser::RuntimeMetadataKind::Class,
            "class metadata has the wrong runtime type");
    require(mparser::runtimeValueToString(*classInfo) ==
                "<matlab.metadata.Class MetadataChild>",
            "class metadata display does not include its identity");

    const auto* propertyInfoList =
        variable(result, "property_info_list");
    require(propertyInfoList != nullptr,
            "property metadata list variable missing");
    require(mparser::runtimeValueToString(*propertyInfoList) ==
                "<matlab.metadata.Property 4x1>",
            "property metadata display does not include its shape");
}

void runMetadataReadOnlySmoke() {
    const std::string source =
        std::string(kMetadataClasses) + R"(
class_info = ?MetadataChild;
class_info.Name = 'Other';
)";
    const auto result = run(source);
    require(hasDiagnostic(result.diagnostics,
                          "metadata properties are read-only"),
            "metadata assignment did not report a read-only diagnostic");
}

void runUnknownMetaclassSmoke() {
    const auto result = run("missing_info = ?MissingMetadataClass;");
    require(hasDiagnostic(result.diagnostics,
                          "class metadata is not available"),
            "unknown metaclass did not report a diagnostic");
}

} // namespace

int main() {
    try {
        runMetadataQuerySmoke();
        runMetadataReadOnlySmoke();
        runUnknownMetaclassSmoke();
        std::cout << "class metadata smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
