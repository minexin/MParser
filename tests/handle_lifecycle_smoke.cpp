#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_text.h"
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
    std::vector<mparser::RuntimeVariable> workspace = {}) {
    auto pipeline = lower(source);
    mparser::BytecodeVmOptions options;
    options.initialWorkspace = std::move(workspace);
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
          "unexpected value for " + std::string(name) + ": " +
              (value ? mparser::runtimeValueToString(*value) :
                       "missing"));
}

void checkString(const mparser::BytecodeVmResult& result,
                 std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing text variable: " + std::string(name));
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    check(text.has_value(), "variable is not text: " + std::string(name));
    check(*text == expected,
          "unexpected text for " + std::string(name) + ": " +
              *text);
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

constexpr std::string_view kLifecycleClasses = R"(classdef LifecycleRecorder < handle
    properties
        Trace = 0
        InvalidObservations = 0
        EventMatches = 0
    end
    methods
        function obj = LifecycleRecorder()
        end
        function value = recordEvent(obj, source, data)
            obj.Trace = obj.Trace * 10 + 1;
            obj.InvalidObservations = obj.InvalidObservations + ...
                ~isvalid(source);
            obj.EventMatches = obj.EventMatches + ...
                strcmp(data.EventName, 'ObjectBeingDestroyed') + ...
                (data.Source == source);
            value = obj.Trace;
        end
        function recordDestructor(obj, code, source)
            obj.Trace = obj.Trace * 10 + code;
            obj.InvalidObservations = obj.InvalidObservations + ...
                ~isvalid(source);
        end
    end
end

classdef LifecycleBase < dynamicprops
    properties
        Recorder
    end
    methods
        function obj = LifecycleBase(recorder)
            obj.Recorder = recorder;
        end
        function delete(obj)
            obj.Recorder.recordDestructor(3, obj);
        end
    end
end

classdef LifecycleChild < LifecycleBase
    methods
        function obj = LifecycleChild(recorder)
            obj@LifecycleBase(recorder);
        end
        function delete(obj)
            obj.Recorder.recordDestructor(2, obj);
        end
    end
end

)";

void runLifecycleSmoke() {
    const auto result = run(std::string(kLifecycleClasses) + R"(
recorder = LifecycleRecorder();
target = LifecycleChild(recorder);
alias = target;
coupled = addlistener(target, 'ObjectBeingDestroyed', ...
    @(source, data) recorder.recordEvent(source, data));
dynamic_property = addprop(target, 'Temporary');
target.Temporary = 9;
event_names = events(target);
event_name = event_names{1};
event_metadata = metaclass(target).EventList(1);
event_metadata_name = event_metadata.Name;
event_defining_class = event_metadata.DefiningClass.Name;
handle_metadata = ?handle;
builtin_event_name = handle_metadata.EventList(1).Name;
has_delete_method = ismethod(target, 'delete');
delete(target);
target_invalid = ~isvalid(target);
alias_invalid = ~isvalid(alias);
coupled_invalid = ~isvalid(coupled);
dynamic_invalid = ~isvalid(dynamic_property);
trace_before_repeat = recorder.Trace;
delete(alias);
idempotent = recorder.Trace == trace_before_repeat;
)" );

    check(result.diagnostics.empty(),
          "lifecycle diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "target_invalid", 1.0);
    checkNumber(result, "alias_invalid", 1.0);
    checkNumber(result, "coupled_invalid", 1.0);
    checkNumber(result, "dynamic_invalid", 1.0);
    checkNumber(result, "idempotent", 1.0);
    checkNumber(result, "has_delete_method", 1.0);
    checkNumber(result, "trace_before_repeat", 123.0);
    const auto* recorder = findVariable(result, "recorder");
    check(recorder && recorder->sharedFields,
          "recorder handle state is unavailable");
    const auto trace = recorder->sharedFields->find(
        "LifecycleRecorder::Trace");
    const auto invalid = recorder->sharedFields->find(
        "LifecycleRecorder::InvalidObservations");
    const auto matches = recorder->sharedFields->find(
        "LifecycleRecorder::EventMatches");
    check(trace != recorder->sharedFields->end() &&
              trace->second.number == 123.0,
          "unexpected destruction trace");
    check(invalid != recorder->sharedFields->end() &&
              invalid->second.number == 3.0,
          "destruction did not expose invalid handles");
    check(matches != recorder->sharedFields->end() &&
              matches->second.number == 2.0,
          "destruction event data did not match its source");
    checkString(result, "event_name", "ObjectBeingDestroyed");
    checkString(result, "event_metadata_name",
                "ObjectBeingDestroyed");
    checkString(result, "event_defining_class", "handle");
    checkString(result, "builtin_event_name",
                "ObjectBeingDestroyed");

    const auto continued = run(R"(
alias_still_invalid = ~isvalid(alias);
listener_still_invalid = ~isvalid(coupled);
property_still_invalid = ~isvalid(dynamic_property);
)", result.variables);
    check(continued.diagnostics.empty(),
          "continued-workspace diagnostics:\n" +
              diagnosticsText(continued.diagnostics));
    checkNumber(continued, "alias_still_invalid", 1.0);
    checkNumber(continued, "listener_still_invalid", 1.0);
    checkNumber(continued, "property_still_invalid", 1.0);
}

void runMethodAndListenerLifetimeSmoke() {
    const auto result = run(std::string(kLifecycleClasses) + R"(
method_recorder = LifecycleRecorder();
method_target = LifecycleChild(method_recorder);
method_target.delete();
method_invalid = ~isvalid(method_target);

listener_recorder = LifecycleRecorder();
listener_target = LifecycleChild(listener_recorder);
uncoupled = listener(listener_target, 'ObjectBeingDestroyed', ...
    @(source, data) isvalid(source));
delete(listener_target);
uncoupled_valid = isvalid(uncoupled);
source_invalid = ~isvalid(listener_target);
delete(uncoupled);
)" );
    check(result.diagnostics.empty(),
          "method/listener diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "method_invalid", 1.0);
    checkNumber(result, "uncoupled_valid", 1.0);
    checkNumber(result, "source_invalid", 1.0);
    const auto* recorder = findVariable(result, "method_recorder");
    check(recorder && recorder->sharedFields,
          "method recorder is unavailable");
    const auto trace = recorder->sharedFields->find(
        "LifecycleRecorder::Trace");
    check(trace != recorder->sharedFields->end() &&
              trace->second.number == 23.0,
          "method-form delete skipped a destructor");
}

void runAccessAndInvalidHandleSmoke() {
    const auto denied = run(R"(classdef LockedHandle < handle
    methods (Access=private)
        function delete(obj)
        end
    end
end
target = LockedHandle();
delete(target);
)" );
    check(hasDiagnostic(denied,
                        "method access is denied: LockedHandle.delete"),
          "private destructor did not restrict explicit deletion");

    const auto internal = run(R"(classdef LockedHandle < handle
    methods (Access=private)
        function delete(obj)
        end
    end
    methods
        function close(obj)
            delete(obj);
        end
    end
end
target = LockedHandle();
target.close();
closed = ~isvalid(target);
)" );
    check(internal.diagnostics.empty(),
          "private internal delete diagnostics:\n" +
              diagnosticsText(internal.diagnostics));
    checkNumber(internal, "closed", 1.0);

    const auto inheritedPrivate = run(R"(classdef PrivateBaseRecorder < handle
    properties
        Trace = 0
    end
end
classdef PrivateDestructorBase < handle
    properties
        Recorder
    end
    methods
        function obj = PrivateDestructorBase(recorder)
            obj.Recorder = recorder;
        end
    end
    methods (Access=private)
        function delete(obj)
            recorder = obj.Recorder;
            recorder.Trace = recorder.Trace + 1;
        end
    end
end
classdef PublicDeletionChild < PrivateDestructorBase
    methods
        function obj = PublicDeletionChild(recorder)
            obj@PrivateDestructorBase(recorder);
        end
    end
end
recorder = PrivateBaseRecorder();
target = PublicDeletionChild(recorder);
delete(target);
invalid = ~isvalid(target);
trace = recorder.Trace;
)" );
    check(inheritedPrivate.diagnostics.empty(),
          "inherited private destructor diagnostics:\n" +
              diagnosticsText(inheritedPrivate.diagnostics));
    checkNumber(inheritedPrivate, "invalid", 1.0);
    checkNumber(inheritedPrivate, "trace", 1.0);

    const auto invalidAccess = run(R"(classdef DeleteProbe < handle
    properties
        Value = 4
    end
end
target = DeleteProbe();
target.delete();
answer = target.Value;
)" );
    check(hasDiagnostic(invalidAccess,
                        "invalid or deleted object: DeleteProbe"),
          "deleted object property access was accepted");

    const auto invalidUnboundMethod = run(R"(classdef InvocationRecorder < handle
    properties
        Trace = 0
    end
end
classdef InvalidInvocationProbe < handle
    methods
        function close(obj, recorder)
            delete(obj);
            touch(obj, recorder);
        end
        function touch(obj, recorder)
            recorder.Trace = 1;
        end
    end
end
recorder = InvocationRecorder();
target = InvalidInvocationProbe();
target.close(recorder);
)" );
    check(hasDiagnostic(invalidUnboundMethod,
                        "invalid or deleted object: InvalidInvocationProbe"),
          "unbound method call accepted a deleted receiver");
    const auto* invocationRecorder =
        findVariable(invalidUnboundMethod, "recorder");
    check(invocationRecorder && invocationRecorder->sharedFields,
          "invocation recorder is unavailable");
    const auto invocationTrace = invocationRecorder->sharedFields->find(
        "InvocationRecorder::Trace");
    check(invocationTrace != invocationRecorder->sharedFields->end() &&
              invocationTrace->second.number == 0.0,
          "deleted receiver entered an unbound instance method");

    const auto privateNotify = run(R"(classdef DeleteProbe < handle
end
target = DeleteProbe();
notify(target, 'ObjectBeingDestroyed');
)" );
    check(hasDiagnostic(
              privateNotify,
              "event notify access is denied: handle.ObjectBeingDestroyed"),
          "ObjectBeingDestroyed accepted explicit notification");
}

void runMultipleInheritanceOrderSmoke() {
    const auto result = run(R"(classdef MultipleRecorder < handle
    properties
        Trace = 0
    end
    methods
        function record(obj, code)
            obj.Trace = obj.Trace * 10 + code;
        end
    end
end
classdef LeftDestructor < handle
    properties
        LeftRecorder
    end
    methods
        function obj = LeftDestructor(recorder)
            obj.LeftRecorder = recorder;
        end
        function delete(obj)
            obj.LeftRecorder.record(2);
        end
    end
end
classdef RightDestructor < handle
    properties
        RightRecorder
    end
    methods
        function obj = RightDestructor(recorder)
            obj.RightRecorder = recorder;
        end
        function delete(obj)
            obj.RightRecorder.record(3);
        end
    end
end
classdef MultipleDestructor < LeftDestructor & RightDestructor
    methods
        function obj = MultipleDestructor(recorder)
            obj@LeftDestructor(recorder);
            obj@RightDestructor(recorder);
        end
        function delete(obj)
            obj.LeftRecorder.record(1);
        end
    end
end
recorder = MultipleRecorder();
target = MultipleDestructor(recorder);
delete(target);
trace = recorder.Trace;
)" );
    check(result.diagnostics.empty(),
          "multiple-inheritance lifecycle diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "trace", 123.0);

    const auto diamond = run(R"(classdef DiamondRecorder < handle
    properties
        Trace = 0
        RootCount = 0
    end
    methods
        function record(obj, code)
            obj.Trace = obj.Trace * 10 + code;
            if code == 4
                obj.RootCount = obj.RootCount + 1;
            end
        end
    end
end
classdef DiamondRoot < handle
    properties
        Recorder
    end
    methods
        function delete(obj)
            obj.Recorder.record(4);
        end
    end
end
classdef DiamondLeft < DiamondRoot
    methods
        function delete(obj)
            obj.Recorder.record(2);
        end
    end
end
classdef DiamondRight < DiamondRoot
    methods
        function delete(obj)
            obj.Recorder.record(3);
        end
    end
end
classdef DiamondChild < DiamondLeft & DiamondRight
    methods
        function delete(obj)
            obj.Recorder.record(1);
        end
    end
end
recorder = DiamondRecorder();
target = DiamondChild();
target.Recorder = recorder;
delete(target);
trace = recorder.Trace;
root_count = recorder.RootCount;
)" );
    check(diamond.diagnostics.empty(),
          "diamond lifecycle diagnostics:\n" +
              diagnosticsText(diamond.diagnostics));
    checkNumber(diamond, "trace", 1243.0);
    checkNumber(diamond, "root_count", 1.0);
}

void runNondestructorAndCallbackFailureSmoke() {
    const auto ordinary = run(R"(classdef OrdinaryDelete < handle
    properties
        Trace = 0
    end
    methods
        function delete(obj, code)
            obj.Trace = code;
        end
    end
end
target = OrdinaryDelete();
target.delete(7);
trace = target.Trace;
still_valid = isvalid(target);
)" );
    check(ordinary.diagnostics.empty(),
          "ordinary delete diagnostics:\n" +
              diagnosticsText(ordinary.diagnostics));
    checkNumber(ordinary, "trace", 7.0);
    checkNumber(ordinary, "still_valid", 1.0);

    const auto privateOrdinary = run(R"(classdef PrivateOrdinaryDelete < handle
    methods (Access=private)
        function delete(obj, code)
        end
    end
end
target = PrivateOrdinaryDelete();
delete_is_visible = ismethod(target, 'delete');
delete(target);
)" );
    checkNumber(privateOrdinary, "delete_is_visible", 0.0);
    check(hasDiagnostic(
              privateOrdinary,
              "method access is denied: PrivateOrdinaryDelete.delete"),
          "private ordinary delete method was externally callable");

    const auto callbackFailure = run(R"(classdef FailureRecorder < handle
    properties
        Trace = 0
    end
    methods
        function value = record(obj, code)
            obj.Trace = obj.Trace * 10 + code;
            value = obj.Trace;
        end
    end
end
classdef FailureTarget < handle
    properties
        Recorder
    end
    methods
        function obj = FailureTarget(recorder)
            obj.Recorder = recorder;
        end
        function delete(obj)
            obj.Recorder.record(2);
        end
    end
end
recorder = FailureRecorder();
target = FailureTarget(recorder);
bad = addlistener(target, 'ObjectBeingDestroyed', ...
    @(source, data) missingName);
good = addlistener(target, 'ObjectBeingDestroyed', ...
    @(source, data) recorder.record(1));
delete(target);
)" );
    check(hasDiagnostic(callbackFailure,
                        "unknown bytecode runtime variable: missingName"),
          "failing destruction callback produced no diagnostic");
    const auto* recorder = findVariable(callbackFailure, "recorder");
    check(recorder && recorder->sharedFields,
          "failure recorder is unavailable");
    const auto trace = recorder->sharedFields->find(
        "FailureRecorder::Trace");
    check(trace != recorder->sharedFields->end() &&
              trace->second.number == 12.0,
          "callback failure prevented later cleanup stages");
}

} // namespace

int main() {
    try {
        runLifecycleSmoke();
        runMethodAndListenerLifetimeSmoke();
        runAccessAndInvalidHandleSmoke();
        runMultipleInheritanceOrderSmoke();
        runNondestructorAndCallbackFailureSmoke();
        std::cout << "handle lifecycle smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
