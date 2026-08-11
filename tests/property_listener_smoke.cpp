#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

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

mparser::BytecodeVmResult run(std::string_view source) {
    auto pipeline = lower(source);
    mparser::BytecodeVm vm;
    return vm.run(pipeline.bytecode, pipeline.semantic);
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
          "unexpected value for " + std::string(name) + ": " +
              (value ? mparser::runtimeValueToString(*value) : "missing"));
}

void checkString(const mparser::BytecodeVmResult& result,
                 std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing text variable: " + std::string(name));
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    check(text.has_value(), "variable is not text: " + std::string(name));
    check(*text == expected,
          "unexpected text for " + std::string(name) + ": " + *text);
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

constexpr std::string_view kObservableBox = R"(classdef ObservableBox < dynamicprops
    properties
        Backing = 2
        Sequence = 0
        LastProperty = ''
        LastEvent = ''
        LastPropertyClass = ''
        LastDataClass = ''
        SourceMatches = 0
        AffectedMatches = 0
        DataIsaEventData = 0
        RecursiveCount = 0
        EventCount = 0
    end

    properties (Dependent, GetObservable, SetObservable)
        Value
    end

    properties (SetObservable, AbortSet)
        Stable = 10
    end

    events
        Ping
    end

    methods
        function obj = ObservableBox()
        end

        function value = get.Value(obj)
            obj.Sequence = obj.Sequence * 10 + 2;
            value = obj.Backing;
        end

        function set.Value(obj, value)
            obj.Sequence = obj.Sequence * 10 + 5;
            obj.Backing = value;
        end

        function value = record(obj, code, property, data)
            obj.Sequence = obj.Sequence * 10 + code;
            obj.LastProperty = property.Name;
            obj.LastEvent = data.EventName;
            obj.LastPropertyClass = class(property);
            obj.LastDataClass = class(data);
            obj.SourceMatches = data.Source == property;
            obj.AffectedMatches = data.AffectedObject == obj;
            obj.DataIsaEventData = isa(data, 'event.EventData');
            value = obj.Sequence;
        end

        function value = reenterStable(obj, property, data)
            obj.RecursiveCount = obj.RecursiveCount + 1;
            if obj.RecursiveCount < 2
                obj.Stable = obj.Stable + 1;
            end
            value = obj.RecursiveCount;
        end

        function value = recordEvent(obj)
            obj.EventCount = obj.EventCount + 1;
            value = obj.EventCount;
        end
    end
end

)";

void runDeclaredPropertyLifecycleSmoke() {
    const auto result = run(std::string(kObservableBox) + R"(
box = ObservableBox();
pre_get = addlistener(box, 'Value', 'PreGet', ...
    @(property, data) box.record(1, property, data));
post_get = addlistener(box, 'Value', 'PostGet', ...
    @(property, data) box.record(3, property, data));
pre_set = addlistener(box, 'Value', 'PreSet', ...
    @(property, data) box.record(4, property, data));
post_set = addlistener(box, 'Value', 'PostSet', ...
    @(property, data) box.record(6, property, data));

box.Sequence = 0;
box.Value = 7;
set_order = box.Sequence;
box.Sequence = 0;
read_value = box.Value;
get_order = box.Sequence;
last_property = box.LastProperty;
last_event = box.LastEvent;
last_property_class = box.LastPropertyClass;
last_data_class = box.LastDataClass;
source_matches = box.SourceMatches;
affected_matches = box.AffectedMatches;
data_isa_event_data = box.DataIsaEventData;

listener_class = class(pre_get);
listener_isa = isa(pre_get, 'event.listener');
listener_handle_isa = isa(pre_get, 'handle');
listener_superclass = metaclass(pre_get).SuperclassList(1).Name;
listener_source_name = pre_get.Source.Name;
listener_object_matches = pre_get.Object{1} == box;
listener_delete_method = ismethod(pre_get, 'delete');

pre_get.delete();
post_get.delete();
pre_set.delete();
post_set.delete();
listeners_deleted = ~isvalid(pre_get) && ~isvalid(post_set);

descriptor = findprop(box, 'Value');
constructor_listener = event.proplistener(box, descriptor, 'PostSet', ...
    @(property, data) box.record(9, property, data));
constructor_class = class(constructor_listener);
constructor_source_matches = constructor_listener.Source == descriptor;
constructor_object_matches = constructor_listener.Object{1} == box;
box.Sequence = 0;
box.Value = 8;
constructor_order = box.Sequence;
constructor_listener.delete();
constructor_valid_after_delete = isvalid(constructor_listener);

stable_pre = box.addlistener('Stable', 'PreSet', ...
    @(property, data) box.record(7, property, data));
stable_post = box.addlistener('Stable', 'PostSet', ...
    @(property, data) box.record(8, property, data));
box.Sequence = 0;
box.Stable = 10;
abort_set_order = box.Sequence;
box.Stable = 11;
changed_set_order = box.Sequence;
stable_pre.delete();
stable_post.delete();

manual_listener = box.listener('Stable', 'PostSet', ...
    @(property, data) box.record(4, property, data));
box.Sequence = 0;
box.Stable = 12;
retained_listener_order = box.Sequence;
manual_listener.delete();

listener(box, 'Stable', 'PostSet', ...
    @(property, data) box.record(4, property, data));
box.Sequence = 0;
box.Stable = 13;
expression_retained_listener_order = box.Sequence;

recursive_listener = box.addlistener('Stable', 'PostSet', ...
    @(property, data) box.reenterStable(property, data));
box.RecursiveCount = 0;
box.Stable = 14;
nonrecursive_count = box.RecursiveCount;
box.RecursiveCount = 0;
recursive_listener.Recursive = true;
box.Stable = 16;
recursive_count = box.RecursiveCount;
recursive_listener.delete();

normal_listener = box.addlistener('Ping', ...
    @(source, data) source.recordEvent());
box.notify('Ping');
method_notify_count = box.EventCount;
normal_listener.delete();

coupled_box = ObservableBox();
addlistener(coupled_box, 'Stable', 'PostSet', ...
    @(property, data) coupled_box.record(4, property, data));
coupled_box.Stable = 11;
discarded_coupled_order = coupled_box.Sequence;
)");

    check(result.diagnostics.empty(),
          "declared property listener diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "set_order", 456.0);
    checkNumber(result, "read_value", 7.0);
    checkNumber(result, "get_order", 123.0);
    checkString(result, "last_property", "Value");
    checkString(result, "last_event", "PostGet");
    checkString(result, "last_property_class",
                "matlab.metadata.Property");
    checkString(result, "last_data_class", "event.PropertyEvent");
    checkNumber(result, "source_matches", 1.0);
    checkNumber(result, "affected_matches", 1.0);
    checkNumber(result, "data_isa_event_data", 1.0);
    checkString(result, "listener_class", "event.proplistener");
    checkNumber(result, "listener_isa", 1.0);
    checkNumber(result, "listener_handle_isa", 1.0);
    checkString(result, "listener_superclass", "event.listener");
    checkString(result, "listener_source_name", "Value");
    checkNumber(result, "listener_object_matches", 1.0);
    checkNumber(result, "listener_delete_method", 1.0);
    checkNumber(result, "listeners_deleted", 1.0);
    checkString(result, "constructor_class", "event.proplistener");
    checkNumber(result, "constructor_source_matches", 1.0);
    checkNumber(result, "constructor_object_matches", 1.0);
    checkNumber(result, "constructor_order", 59.0);
    checkNumber(result, "constructor_valid_after_delete", 0.0);
    checkNumber(result, "abort_set_order", 0.0);
    checkNumber(result, "changed_set_order", 78.0);
    checkNumber(result, "retained_listener_order", 4.0);
    checkNumber(result, "expression_retained_listener_order", 4.0);
    checkNumber(result, "nonrecursive_count", 1.0);
    checkNumber(result, "recursive_count", 2.0);
    checkNumber(result, "method_notify_count", 1.0);
    checkNumber(result, "discarded_coupled_order", 4.0);
}

void runDynamicPropertyLifecycleSmoke() {
    const auto result = run(std::string(kObservableBox) + R"(
box = ObservableBox();
property = addprop(box, 'Live');
property.GetObservable = true;
property.SetObservable = true;
property.AbortSet = true;
pre_set = addlistener(box, property, 'PreSet', ...
    @(source, data) box.record(1, source, data));
post_set = addlistener(box, 'Live', 'PostSet', ...
    @(source, data) box.record(2, source, data));
pre_get = addlistener(box, property, 'PreGet', ...
    @(source, data) box.record(3, source, data));
post_get = addlistener(box, 'Live', 'PostGet', ...
    @(source, data) box.record(4, source, data));

box.Sequence = 0;
box.Live = 3;
dynamic_set_order = box.Sequence;
box.Sequence = 0;
dynamic_value = box.Live;
dynamic_get_order = box.Sequence;
box.Sequence = 0;
box.Live = 3;
dynamic_abort_order = box.Sequence;
dynamic_property_class = box.LastPropertyClass;
dynamic_listener_valid_before = isvalid(post_set);
delete(property);
dynamic_listener_valid_after = isvalid(post_set);
dynamic_property_valid_after = isvalid(property);
)");

    check(result.diagnostics.empty(),
          "dynamic property listener diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "dynamic_set_order", 12.0);
    checkNumber(result, "dynamic_value", 3.0);
    checkNumber(result, "dynamic_get_order", 34.0);
    checkNumber(result, "dynamic_abort_order", 0.0);
    checkString(result, "dynamic_property_class",
                "matlab.metadata.DynamicProperty");
    checkNumber(result, "dynamic_listener_valid_before", 1.0);
    checkNumber(result, "dynamic_listener_valid_after", 0.0);
    checkNumber(result, "dynamic_property_valid_after", 0.0);
}

void runInheritanceAndIdentitySmoke() {
    const auto result = run(R"(classdef ObservedBase < handle
    properties (SetObservable)
        Value = 1
    end
    properties
        Count = 0
        LastDefiningClass = ''
        AffectedMatches = 0
    end
    methods
        function value = record(obj, property, data)
            obj.Count = obj.Count + 1;
            obj.LastDefiningClass = property.DefiningClass.Name;
            obj.AffectedMatches = data.AffectedObject == obj;
            value = obj.Count;
        end
    end
end

classdef ObservedChild < ObservedBase
end

child = ObservedChild();
other = ObservedChild();
listener_handle = addlistener(child, 'Value', 'PostSet', ...
    @(property, data) child.record(property, data));
alias = child;
alias.Value = 2;
alias_delivery_count = child.Count;
other.Value = 3;
isolated_delivery_count = child.Count;
defining_class = child.LastDefiningClass;
affected_matches = child.AffectedMatches;
descriptor = findprop(child, 'Value');
descriptor_defining_class = descriptor.DefiningClass.Name;
)");

    check(result.diagnostics.empty(),
          "inherited property listener diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "alias_delivery_count", 1.0);
    checkNumber(result, "isolated_delivery_count", 1.0);
    checkString(result, "defining_class", "ObservedBase");
    checkNumber(result, "affected_matches", 1.0);
    checkString(result, "descriptor_defining_class", "ObservedBase");
}

void runDiagnosticsSmoke() {
    const auto nonObservable = run(R"(classdef PlainBox < handle
    properties
        Value = 1
    end
end

box = PlainBox();
listener_handle = addlistener(box, 'Value', 'PostSet', @(source, data) 0);
)");
    check(hasDiagnostic(nonObservable, "SetObservable is false"),
          "non-observable property accepted a PostSet listener");

    const auto wrongEvent = run(std::string(kObservableBox) + R"(
box = ObservableBox();
listener_handle = addlistener(box, 'Value', 'Changed', ...
    @(source, data) 0);
)");
    check(hasDiagnostic(wrongEvent, "unknown property event name"),
          "unknown property event name was accepted");

    const auto missingProperty = run(std::string(kObservableBox) + R"(
box = ObservableBox();
listener_handle = addlistener(box, 'Missing', 'PostSet', ...
    @(source, data) 0);
)");
    check(hasDiagnostic(missingProperty,
                        "property is not available for listener"),
          "missing property accepted a listener");

    const auto constructorString = run(std::string(kObservableBox) + R"(
box = ObservableBox();
listener_handle = event.proplistener(box, 'Value', 'PostSet', ...
    @(source, data) 0);
)");
    check(hasDiagnostic(constructorString,
                        "requires a scalar property metadata descriptor"),
          "event.proplistener accepted a property-name string");

    const auto wrongOwner = run(std::string(kObservableBox) + R"(
first = ObservableBox();
second = ObservableBox();
property = addprop(first, 'Live');
property.SetObservable = true;
listener_handle = addlistener(second, property, 'PostSet', ...
    @(source, data) 0);
)");
    check(hasDiagnostic(wrongOwner,
                        "does not belong to the listener source"),
          "dynamic property descriptor was accepted for another owner");

    const auto valueSource = run(R"(classdef ValueBox
    properties (SetObservable)
        Value = 1
    end
end

box = ValueBox();
listener_handle = addlistener(box, 'Value', 'PostSet', @(source, data) 0);
)");
    check(hasDiagnostic(valueSource, "expects a handle object"),
          "value object accepted a property listener");
}

} // namespace

int main() {
    try {
        runDeclaredPropertyLifecycleSmoke();
        runDynamicPropertyLifecycleSmoke();
        runInheritanceAndIdentitySmoke();
        runDiagnosticsSmoke();
        std::cout << "property listener smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
