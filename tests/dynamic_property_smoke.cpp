#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_metadata.h"
#include "mparser/semantic/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string diagnosticsText(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) {
        text += std::to_string(diagnostic.span.begin.line) + ":" +
                std::to_string(diagnostic.span.begin.column) + ": " +
                diagnostic.message + "\n";
    }
    return text;
}

struct Pipeline {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

Pipeline lower(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    check(parsed.diagnostics.empty(),
          "parse diagnostics:\n" + diagnosticsText(parsed.diagnostics));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    check(semantic.diagnostics.empty(),
          "semantic diagnostics:\n" +
              diagnosticsText(semantic.diagnostics));

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    check(bytecode.diagnostics.empty(),
          "bytecode diagnostics:\n" +
              diagnosticsText(bytecode.diagnostics));
    return Pipeline{std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(
    std::string_view source,
    const mparser::BytecodeVmOptions& options = {}) {
    auto pipeline = lower(source);
    mparser::BytecodeVm vm;
    return vm.run(pipeline.bytecode, pipeline.semantic, options);
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

void checkNumber(const mparser::BytecodeVmResult& result,
                 std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    check(value && value->kind == mparser::RuntimeValueKind::Number,
          "missing numeric variable: " + std::string(name));
    check(std::fabs(value->number - expected) < 1e-9,
          "unexpected value for: " + std::string(name));
}

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kDynamicBagClass = R"(classdef DynamicBag < dynamicprops
    properties
        Backing = 5
    end

    methods
        function obj = DynamicBag()
        end

        function obj = setBacking(obj, value)
            obj.Backing = value;
        end
    end
end

)";

void runLifecycleAndReflectionSmoke() {
    const std::string source = std::string(kDynamicBagClass) + R"(
classdef DynamicChild < DynamicBag
end

bag = DynamicBag();
other = DynamicBag();
child = DynamicChild();
child_property = addprop(child, 'ChildValue');
child.ChildValue = 6;
child_value = child.ChildValue;
child_dynamicprops_isa = isa(child, 'dynamicprops');
property = addprop(bag, 'Score');
bag.Score = 7;
alias = bag;
alias.Score = 9;
shared_score = bag.Score;

computed = bag.addprop('Computed');
computed.GetMethod = @(obj) obj.Backing * 2;
computed.SetMethod = @(obj, value) obj.setBacking(value / 2);
computed.Transient = true;
computed.NonCopyable = true;
computed.WeakHandle = true;
computed.PartialMatchPriority = 2.5;
bag.Computed = 30;
computed_value = bag.Computed;
backing_value = bag.Backing;
transient_value = computed.Transient;
noncopyable_value = computed.NonCopyable;
weak_value = computed.WeakHandle;
priority_value = computed.PartialMatchPriority;

hidden = addprop(bag, 'Internal');
hidden.Hidden = true;
restricted = addprop(bag, 'Restricted');
restricted.GetAccess = 'private';
property_count = numel(properties(bag));
restricted_isprop = isprop(bag, 'Restricted');
found = bag.findprop('Score');
static_property = findprop(bag, 'Backing');
missing_property = findprop(bag, 'Missing');
valid_before = property.isvalid();
name_matches = strcmp(property.Name, 'Score');
class_matches = strcmp(class(property), ...
    'matlab.metadata.DynamicProperty');
dynamic_isa = isa(property, 'meta.DynamicProperty');
property_isa = isa(property, 'matlab.metadata.Property');
same_descriptor = found == property;
static_property_isa = isa(static_property, 'matlab.metadata.Property');
missing_is_empty = isempty(missing_property);
has_addprop = ismethod(bag, 'addprop');
has_delete = ismethod(property, 'delete');
handle_isa = isa(bag, 'handle');
dynamicprops_isa = isa(bag, 'dynamicprops');
superclass_matches = strcmp(metaclass(bag).SuperclassList(1).Name, ...
    'dynamicprops');
dynamicprops_superclass_matches = strcmp(...
    metaclass(bag).SuperclassList(1).SuperclassList(1).Name, 'handle');
descriptor_superclass_matches = strcmp(...
    metaclass(property).SuperclassList(1).Name, ...
    'matlab.metadata.Property');

property.delete();
valid_after = isvalid(property);
has_after_delete = isprop(bag, 'Score');
other_has_property = isprop(other, 'Score');

orphan_property = makeOrphanProperty();
orphan_valid = isvalid(orphan_property);

function property = makeOrphanProperty()
    owner = DynamicBag();
    property = addprop(owner, 'Temporary');
end
)";

    const auto result = run(source);
    check(result.diagnostics.empty(),
          "dynamic property diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "shared_score", 9.0);
    checkNumber(result, "child_value", 6.0);
    checkNumber(result, "child_dynamicprops_isa", 1.0);
    checkNumber(result, "computed_value", 30.0);
    checkNumber(result, "backing_value", 15.0);
    checkNumber(result, "transient_value", 1.0);
    checkNumber(result, "noncopyable_value", 1.0);
    checkNumber(result, "weak_value", 1.0);
    checkNumber(result, "priority_value", 2.5);
    checkNumber(result, "property_count", 3.0);
    checkNumber(result, "restricted_isprop", 1.0);
    checkNumber(result, "valid_before", 1.0);
    checkNumber(result, "valid_after", 0.0);
    checkNumber(result, "has_after_delete", 0.0);
    checkNumber(result, "other_has_property", 0.0);
    checkNumber(result, "orphan_valid", 0.0);
    checkNumber(result, "name_matches", 1.0);
    checkNumber(result, "class_matches", 1.0);
    checkNumber(result, "dynamic_isa", 1.0);
    checkNumber(result, "property_isa", 1.0);
    checkNumber(result, "same_descriptor", 1.0);
    checkNumber(result, "static_property_isa", 1.0);
    checkNumber(result, "missing_is_empty", 1.0);
    checkNumber(result, "has_addprop", 1.0);
    checkNumber(result, "has_delete", 1.0);
    checkNumber(result, "handle_isa", 1.0);
    checkNumber(result, "dynamicprops_isa", 1.0);
    checkNumber(result, "superclass_matches", 1.0);
    checkNumber(result, "dynamicprops_superclass_matches", 1.0);
    checkNumber(result, "descriptor_superclass_matches", 1.0);

    const auto* descriptor = findVariable(result, "property");
    check(descriptor &&
              mparser::runtimeMetadataKind(*descriptor) ==
                  mparser::RuntimeMetadataKind::DynamicProperty,
          "addprop did not return DynamicProperty metadata");
}

void runWorkspaceRebindingSmoke() {
    const auto first = run(std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
first_property = addprop(bag, 'First');
bag.First = 11;
delete_property = addprop(bag, 'DeleteMe');
bag.DeleteMe = 12;
)");
    check(first.diagnostics.empty(),
          "first workspace diagnostics:\n" +
              diagnosticsText(first.diagnostics));

    mparser::BytecodeVmOptions options;
    options.initialWorkspace = first.variables;
    const auto second = run(std::string(kDynamicBagClass) + R"(
delete(delete_property);
deleted_descriptor_valid = isvalid(delete_property);
deleted_property_present = isprop(bag, 'DeleteMe');
restored_value = bag.First;
second_property = addprop(bag, 'Second');
bag.Second = 22;
first_after_add = bag.First;
second_value = bag.Second;
first_descriptor = findprop(bag, 'First');
first_descriptor_valid = isvalid(first_descriptor);
)", options);
    check(second.diagnostics.empty(),
          "rebound workspace diagnostics:\n" +
              diagnosticsText(second.diagnostics));
    checkNumber(second, "restored_value", 11.0);
    checkNumber(second, "first_after_add", 11.0);
    checkNumber(second, "second_value", 22.0);
    checkNumber(second, "first_descriptor_valid", 1.0);
    checkNumber(second, "deleted_descriptor_valid", 0.0);
    checkNumber(second, "deleted_property_present", 0.0);
}

void runDiagnosticSmoke() {
    const auto plainHandle = run(R"(classdef PlainHandle < handle
end

object = PlainHandle();
property = addprop(object, 'Extra');
)");
    check(hasDiagnostic(plainHandle,
                        "must derive from dynamicprops"),
          "addprop accepted a non-dynamic handle object");

    const auto valueFindprop = run(R"(classdef ValueObject
    properties
        Value = 1
    end
end

object = ValueObject();
property = findprop(object, 'Value');
)");
    check(hasDiagnostic(valueFindprop, "expects a handle object"),
          "findprop accepted a value object");

    const auto invalidName = run(std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'not valid');
)");
    check(hasDiagnostic(invalidName, "not a valid identifier"),
          "addprop accepted an invalid property name");

    const auto duplicate = run(std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
first = addprop(bag, 'Extra');
second = addprop(bag, 'Extra');
)");
    check(hasDiagnostic(duplicate, "property already exists"),
          "addprop accepted a duplicate dynamic property");

    const auto staticDuplicate = run(std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Backing');
)");
    check(hasDiagnostic(staticDuplicate, "property already exists"),
          "addprop accepted a declared property name");

    const auto privateSet = run(std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Locked');
property.SetAccess = 'private';
bag.Locked = 4;
)");
    check(hasDiagnostic(privateSet, "set access is denied"),
          "dynamic SetAccess was not enforced");

    const auto readOnlyMetadata = run(
        std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Extra');
property.Name = 'Changed';
)");
    check(hasDiagnostic(readOnlyMetadata,
                        "metadata is read-only"),
          "dynamic property Name was writable");

    const auto invalidCallback = run(
        std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Extra');
property.GetMethod = 3;
)");
    check(hasDiagnostic(invalidCallback,
                        "requires a function handle"),
          "dynamic GetMethod accepted a non-handle value");

    const auto deletedAccess = run(
        std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Extra');
delete(property);
name = property.Name;
)");
    check(hasDiagnostic(deletedAccess, "descriptor is not valid"),
          "deleted dynamic descriptor remained readable");

    const auto complexLogicalMetadata = run(
        std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Extra');
property.Hidden = complex(1, 0);
)");
    check(hasDiagnostic(complexLogicalMetadata,
                        "real scalar numeric value"),
          "dynamic logical metadata accepted a complex value");

    const auto complexPriority = run(
        std::string(kDynamicBagClass) + R"(
bag = DynamicBag();
property = addprop(bag, 'Extra');
property.PartialMatchPriority = complex(2, 0);
)");
    check(hasDiagnostic(complexPriority, "positive real scalar"),
          "PartialMatchPriority accepted a complex value");
}

} // namespace

int main() {
    try {
        runLifecycleAndReflectionSmoke();
        runWorkspaceRebindingSmoke();
        runDiagnosticSmoke();
        std::cout << "dynamic property smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
