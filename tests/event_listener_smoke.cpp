#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
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

mparser::SemanticResult analyze(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    check(parsed.diagnostics.empty(),
          "parse diagnostics:\n" + diagnosticsText(parsed.diagnostics));
    mparser::SemanticAnalyzer analyzer;
    return analyzer.analyze(*parsed.root);
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
    check(value != nullptr,
          "missing runtime variable: " + std::string(name));
    check(value->kind == mparser::RuntimeValueKind::Number,
          "runtime variable is not numeric: " + std::string(name));
    check(std::fabs(value->number - expected) < 1e-9,
          "unexpected numeric value for: " + std::string(name));
}

void checkString(const mparser::BytecodeVmResult& result,
                 std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr,
          "missing runtime variable: " + std::string(name));
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    check(text.has_value(),
          "runtime variable is not text: " + std::string(name));
    check(*text == expected,
          "unexpected text value for: " + std::string(name));
}

bool hasDiagnostic(const std::vector<mparser::Diagnostic>& diagnostics,
                   std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void collectHir(const mparser::HirNode& node, mparser::HirKind kind,
                std::vector<const mparser::HirNode*>& nodes) {
    if (node.kind == kind) {
        nodes.push_back(&node);
    }
    for (const auto& child : node.children) {
        collectHir(*child, kind, nodes);
    }
}

constexpr std::string_view kPulseClass = R"(classdef Pulse < handle
    properties
        Count = 0
        CallbackCount = 0
        RecursiveCount = 0
        LastEvent = ''
    end

    events (ListenAccess = public, NotifyAccess = private)
        Tick
    end

    events (Hidden)
        Internal
    end

    methods
        function obj = Pulse()
        end

        function value = fire(obj, delta)
            obj.Count = obj.Count + delta;
            notify(obj, 'Tick');
            value = obj.Count;
        end

        function value = fireWithData(obj, eventData)
            notify(obj, 'Tick', eventData);
            value = obj.Count;
        end

        function value = record(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 1;
            obj.LastEvent = eventName;
            value = obj.CallbackCount;
        end

        function value = recordAmount(obj, amount)
            obj.CallbackCount = obj.CallbackCount + amount;
            value = obj.CallbackCount;
        end

        function recordWithoutOutput(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 20;
            obj.LastEvent = eventName;
        end

        function value = reenter(obj, eventName)
            obj.RecursiveCount = obj.RecursiveCount + 1;
            obj.LastEvent = eventName;
            if obj.RecursiveCount < 2
                notify(obj, 'Tick');
            end
            value = obj.RecursiveCount;
        end

        function callback = privateCallback(obj)
            callback = @obj.secretRecord;
        end

        function callback = privateAnonymousCallback(obj)
            callback = @(src, evt) obj.secretRecord(evt.EventName);
        end
    end

    methods (Access = private)
        function value = secretRecord(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 10;
            obj.LastEvent = eventName;
            value = obj.CallbackCount;
        end
    end
end
)";

void runFrontendAndBytecodeSmoke() {
    auto pipeline = lower(R"(classdef EventSource < handle
    events (ListenAccess = public, NotifyAccess = private)
        Started, ...
        Stopped
    end
end

scale = 3;
callback = @(source, data) scale + source + data;
)" );

    std::vector<const mparser::HirNode*> events;
    collectHir(*pipeline.semantic.root, mparser::HirKind::Event, events);
    check(events.size() == 2, "event declarations did not reach HIR");
    for (const auto* event : events) {
        check(event->binding.kind == mparser::BindingKind::Event,
              "event declaration has the wrong binding kind");
    }

    bool foundHandle = false;
    for (size_t pc = 0; pc < pipeline.bytecode.instructions.size(); ++pc) {
        const auto& instruction = pipeline.bytecode.instructions[pc];
        if (instruction.op != mparser::BytecodeOp::MakeFunctionHandle ||
            instruction.operand != "@()") {
            continue;
        }
        foundHandle = true;
        check(instruction.parameters.size() == 2,
              "anonymous function parameters were not preserved");
        check(instruction.target > static_cast<int>(pc + 1),
              "anonymous function body does not have a skip target");
    }
    check(foundHandle, "anonymous function bytecode was not emitted");
}

void runFunctionHandleSmoke() {
    const auto result = run(R"(classdef HandleTools
    methods (Static)
        function result = triple(value)
            result = value * 3;
        end
    end
end

factor = 3;
anonymous = @(value) value * factor;
factor = 10;
anonymous_value = anonymous(4);
handle_class = class(anonymous);
handle_isa = isa(anonymous, 'function_handle');
builtin = @sin;
builtin_value = builtin(0);
named = @doubleValue;
named_value = named(7);
static_handle = @HandleTools.triple;
static_value = static_handle(5);
static_text_handle = str2func('HandleTools.triple');
static_text_value = static_text_handle(6);
static_text_name = func2str(static_text_handle);

function result = doubleValue(value)
    result = value * 2;
end
)" );
    check(result.diagnostics.empty(),
          "function-handle diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "anonymous_value", 12.0);
    checkNumber(result, "builtin_value", 0.0);
    checkNumber(result, "named_value", 14.0);
    checkNumber(result, "static_value", 15.0);
    checkNumber(result, "static_text_value", 18.0);
    checkNumber(result, "handle_isa", 1.0);
    checkString(result, "handle_class", "function_handle");
    checkString(result, "static_text_name", "HandleTools.triple");
    const auto* anonymous = findVariable(result, "anonymous");
    check(anonymous &&
              anonymous->kind == mparser::RuntimeValueKind::FunctionHandle,
          "anonymous function is not a first-class runtime value");
}

void runEventLifecycleSmoke() {
    const std::string source = std::string(kPulseClass) + R"(
classdef TickData < event.EventData
    properties
        Payload = 0
    end
    methods
        function obj = TickData(payload)
            obj.Payload = payload;
        end
    end
end

pulse = Pulse();
listenerHandle = addlistener(pulse, 'Tick', @(src, evt) src.record(evt.EventName));
first = pulse.fire(2);
callback_after_first = pulse.CallbackCount;
last_after_first = pulse.LastEvent;
valid_before = isvalid(listenerHandle);
listenerHandle.Enabled = false;
second = pulse.fire(3);
callback_while_disabled = pulse.CallbackCount;
listenerHandle.Enabled = true;
third = pulse.fire(4);
callback_after_enabled = pulse.CallbackCount;
event_names = events(pulse);
first_event = event_names{1};
declared_event = event_names{2};
event_count = numel(event_names);

replacementPulse = Pulse();
replacementListener = addlistener(replacementPulse, 'Tick', ...
    @(src, evt) src.record(evt.EventName));
replacementListener.Callback = ...
    @(src, evt) src.recordAmount(2);
replacementPulse.fire(0);
replacement_callback_count = replacementPulse.CallbackCount;

delete(listenerHandle);
valid_after = isvalid(listenerHandle);
pulse.fire(1);
callback_after_delete = pulse.CallbackCount;

bound = @pulse.record;
bound_value = bound('Bound');
private_bound = pulse.privateCallback();
private_bound_value = private_bound('Private');
private_anonymous = pulse.privateAnonymousCallback();
private_listener = addlistener(pulse, 'Tick', private_anonymous);
pulse.fire(0);
private_callback_value = pulse.CallbackCount;

recursivePulse = Pulse();
recursiveListener = addlistener(recursivePulse, 'Tick', @(src, evt) src.reenter(evt.EventName));
recursivePulse.fire(0);
nonrecursive_count = recursivePulse.RecursiveCount;
recursivePulse.RecursiveCount = 0;
recursiveListener.Recursive = true;
recursivePulse.fire(0);
recursive_count = recursivePulse.RecursiveCount;

coupledPulse = Pulse();
addlistener(coupledPulse, 'Tick', @(src, evt) src.record(evt.EventName));
coupledPulse.fire(0);
coupled_callback_count = coupledPulse.CallbackCount;

uncoupledPulse = Pulse();
listener(uncoupledPulse, 'Tick', @(src, evt) src.record(evt.EventName));
uncoupledPulse.fire(0);
expression_retained_listener_count = uncoupledPulse.CallbackCount;
manualListener = listener(uncoupledPulse, 'Tick', ...
    @(src, evt) src.record(evt.EventName));
uncoupledPulse.fire(0);
retained_listener_count = uncoupledPulse.CallbackCount;
delete(manualListener);
uncoupledPulse.fire(0);
deleted_manual_listener_count = uncoupledPulse.CallbackCount;

dataPulse = Pulse();
dataListener = addlistener(dataPulse, 'Tick', ...
    @(src, evt) src.recordAmount(evt.Payload + ...
        strcmp(evt.EventName, 'Tick') + (evt.Source == src)));
data = TickData(7);
dataPulse.fireWithData(data);
custom_data_count = dataPulse.CallbackCount;
custom_data_isa = isa(data, 'event.EventData');

zeroOutputPulse = Pulse();
% Generalizes cap_196 to callbacks whose root method has no output.
zeroOutputListener = addlistener(zeroOutputPulse, 'Tick', ...
    @(src, evt) src.recordWithoutOutput(evt.EventName));
zeroOutputPulse.fire(0);
zero_output_callback_count = zeroOutputPulse.CallbackCount;
)";

    const auto result = run(source);
    check(result.diagnostics.empty(),
          "event lifecycle diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "first", 2.0);
    checkNumber(result, "callback_after_first", 1.0);
    checkString(result, "last_after_first", "Tick");
    checkNumber(result, "valid_before", 1.0);
    checkNumber(result, "second", 5.0);
    checkNumber(result, "callback_while_disabled", 1.0);
    checkNumber(result, "third", 9.0);
    checkNumber(result, "callback_after_enabled", 2.0);
    checkString(result, "first_event", "ObjectBeingDestroyed");
    checkString(result, "declared_event", "Tick");
    checkNumber(result, "event_count", 2.0);
    checkNumber(result, "replacement_callback_count", 2.0);
    checkNumber(result, "valid_after", 0.0);
    checkNumber(result, "callback_after_delete", 2.0);
    checkNumber(result, "bound_value", 3.0);
    checkNumber(result, "private_bound_value", 13.0);
    checkNumber(result, "private_callback_value", 23.0);
    checkNumber(result, "nonrecursive_count", 1.0);
    checkNumber(result, "recursive_count", 2.0);
    checkNumber(result, "coupled_callback_count", 1.0);
    checkNumber(result, "expression_retained_listener_count", 1.0);
    checkNumber(result, "retained_listener_count", 3.0);
    checkNumber(result, "deleted_manual_listener_count", 4.0);
    checkNumber(result, "custom_data_count", 9.0);
    checkNumber(result, "custom_data_isa", 1.0);
    checkNumber(result, "zero_output_callback_count", 20.0);
}

void runListenerOwnershipReleaseSmoke() {
    std::weak_ptr<std::map<std::string, mparser::RuntimeValue>>
        sourceFields;
    std::weak_ptr<std::map<std::string, mparser::RuntimeValue>>
        listenerFields;
    std::weak_ptr<mparser::RuntimeFunctionHandle> callbackDescriptor;

    {
        const auto result =
            run(std::string(kPulseClass) + R"(
pulse = Pulse();
listenerHandle = addlistener(pulse, 'Tick', ...
    @(src, evt) pulse.record(evt.EventName));
pulse.fire(0);
callback_count = pulse.CallbackCount;
)");
        check(result.diagnostics.empty(),
              "listener ownership diagnostics:\n" +
                  diagnosticsText(result.diagnostics));
        checkNumber(result, "callback_count", 1.0);
        const auto* pulse = findVariable(result, "pulse");
        const auto* listener = findVariable(result, "listenerHandle");
        check(pulse && pulse->sharedFields,
              "ownership source fields are unavailable");
        check(listener && listener->sharedFields,
              "ownership listener fields are unavailable");
        sourceFields = pulse->sharedFields;
        listenerFields = listener->sharedFields;

        const auto callback = listener->sharedFields->find("Callback");
        check(callback != listener->sharedFields->end() &&
                  callback->second.functionHandle,
              "ownership callback descriptor is unavailable");
        callbackDescriptor = callback->second.functionHandle;
        check(callback->second.functionHandle->capturedVariables.size() ==
                      1 &&
                  callback->second.functionHandle->capturedVariables
                      .contains("pulse"),
              "listener callback did not retain its exact free variable");
    }

    check(sourceFields.expired(),
          "source/listener ownership formed a retained source cycle");
    check(listenerFields.expired(),
          "source/listener ownership formed a retained listener cycle");
    check(callbackDescriptor.expired(),
          "source/listener ownership formed a retained callback cycle");
}

void runInheritanceAndDiagnosticsSmoke() {
    const auto inherited = run(R"(classdef BaseSource < handle
    events
        Ping
    end
    methods
        function obj = BaseSource()
        end
        function fire(obj)
            notify(obj, 'Ping');
        end
    end
end

classdef DerivedSource < BaseSource
end

source = DerivedSource();
calls = 4;
callback = @(src, evt) calls + strcmp(evt.EventName, 'Ping');
listenerHandle = addlistener(source, 'Ping', callback);
source.fire();
names = events(source);
name = names{1};
declared_name = names{2};
)" );
    check(inherited.diagnostics.empty(),
          "inherited-event diagnostics:\n" +
              diagnosticsText(inherited.diagnostics));
    checkString(inherited, "name", "ObjectBeingDestroyed");
    checkString(inherited, "declared_name", "Ping");

    const auto valueClass = run(R"(classdef InvalidValueEvent
    events
        Changed
    end
end

value = 1;
)" );
    check(hasDiagnostic(valueClass.diagnostics,
                        "events can be declared only by handle classes"),
          "value-class event declaration was not rejected");

    const std::string privateNotify = std::string(kPulseClass) + R"(
pulse = Pulse();
notify(pulse, 'Tick');
)";
    const auto denied = run(privateNotify);
    check(hasDiagnostic(denied.diagnostics,
                        "event notify access is denied"),
          "private NotifyAccess was not enforced");

    const auto privateListenDenied = run(R"(classdef PrivateListenSource < handle
    events (ListenAccess = private)
        Secret
    end
end

source = PrivateListenSource();
listenerHandle = addlistener(source, 'Secret', @(src, evt) 1);
)" );
    check(hasDiagnostic(privateListenDenied.diagnostics,
                        "event listen access is denied"),
          "private ListenAccess was not enforced");

    const auto privateListenAllowed = run(R"(classdef PrivateListenOwner < handle
    properties
        CallbackCount = 0
    end
    events (ListenAccess = private, NotifyAccess = private)
        Secret
    end
    methods
        function obj = PrivateListenOwner()
        end
        function listenerHandle = attach(obj)
            listenerHandle = addlistener(obj, 'Secret', ...
                @(src, evt) src.record());
        end
        function count = record(obj)
            obj.CallbackCount = obj.CallbackCount + 1;
            count = obj.CallbackCount;
        end
        function fire(obj)
            notify(obj, 'Secret');
        end
    end
end

source = PrivateListenOwner();
listenerHandle = source.attach();
source.fire();
private_listen_count = source.CallbackCount;
private_listener_valid = isvalid(listenerHandle);
)" );
    check(privateListenAllowed.diagnostics.empty(),
          "private class-owned listener diagnostics:\n" +
              diagnosticsText(privateListenAllowed.diagnostics));
    checkNumber(privateListenAllowed, "private_listen_count", 1.0);
    checkNumber(privateListenAllowed, "private_listener_valid", 1.0);

    const auto conflict = analyze(R"(classdef ConflictSource < handle
    events
        Changed, ConflictSource
    end
    properties
        Changed
    end
end
)" );
    check(hasDiagnostic(conflict.diagnostics,
                        "event conflicts with another class member"),
          "event/member conflict was not diagnosed");
    check(hasDiagnostic(conflict.diagnostics,
                        "event cannot have the class name"),
          "class-name event conflict was not diagnosed");
}

void runSessionListenerSourceReleaseSmoke() {
    const auto pipeline = lower("classdef WeakSource < handle\nevents\nTick\nend\nend\n"
        "source=WeakSource(); listener=addlistener(source,'Tick',@(src,data)1); "
        "same=(listener.Source==source);");
    auto session = std::make_shared<mparser::RuntimeSessionState>();
    std::weak_ptr<mparser::RuntimeWorkspace> sourceFields;
    std::weak_ptr<mparser::RuntimeWorkspace> listenerFields;
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        mparser::BytecodeVm vm;
        const auto result = vm.run(pipeline.bytecode,pipeline.semantic,options);
        check(result.diagnostics.empty(),"weak listener fixture failed");
        const auto* source = findVariable(result,"source");
        const auto* listener = findVariable(result,"listener");
        check(source && listener,"weak listener fixture missing values");
        sourceFields = source->sharedFields;
        listenerFields = listener->sharedFields;
        check(findVariable(result,"same")->number == 1,"weak Source getter lost identity");
    }
    check(sourceFields.expired(),"session coupled listener strongly retained its source");
    session->reset();
    check(listenerFields.expired(),"session reset did not release coupled listener fields");
    const auto propertyPipeline = lower(
        "classdef WeakPropertySource < handle\nproperties (SetObservable)\nValue=1\nend\nend\n"
        "source=WeakPropertySource(); listener=addlistener(source,'Value','PostSet',@(src,data)1); "
        "same=(listener.Object{1}==source);");
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        mparser::BytecodeVm vm;
        const auto result = vm.run(propertyPipeline.bytecode,propertyPipeline.semantic,options);
        check(result.diagnostics.empty(),"weak property listener fixture failed");
        const auto* source = findVariable(result,"source");
        const auto* listener = findVariable(result,"listener");
        check(source && listener,"weak property listener fixture missing values");
        sourceFields = source->sharedFields;
        listenerFields = listener->sharedFields;
        check(findVariable(result,"same")->number == 1,"weak Object getter lost identity");
    }
    check(sourceFields.expired(),"session property listener strongly retained its source");
    session->reset();
    check(listenerFields.expired(),"session reset did not release property listener fields");
}

void runCapturedSourceSessionReleaseSmoke() {
    auto pipeline = lower(std::string(kPulseClass) + R"(
pulse=Pulse();
watcher=addlistener(pulse,'Tick',@(source,data)pulse.record(data.EventName));
pulse.fire(0);
count=pulse.CallbackCount;
)");
    auto session = std::make_shared<mparser::RuntimeSessionState>();
    std::weak_ptr<mparser::RuntimeWorkspace> sourceFields;
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        mparser::BytecodeVm vm;
        const auto result = vm.run(pipeline.bytecode,pipeline.semantic,options);
        check(result.diagnostics.empty(),"capturing listener fixture failed");
        checkNumber(result,"count",1);
        const auto* source = findVariable(result,"pulse");
        const auto* watcher = findVariable(result,"watcher");
        check(source && watcher,"capturing listener exports missing");
        sourceFields = source->sharedFields;
        const auto& callback = watcher->sharedFields->at("Callback");
        check(callback.functionHandle && callback.functionHandle->capturedVariables.contains("pulse"),
            "capturing listener lost its exact source free variable");
    }
    auto followup = lower("unrelated=1;");
    mparser::BytecodeVmOptions options;
    options.sessionState = session;
    mparser::BytecodeVm vm;
    const auto result = vm.run(followup.bytecode,followup.semantic,options);
    check(result.diagnostics.empty(),"capturing listener cleanup invocation failed");
    check(sourceFields.expired(),"session listener callback still strongly retains an otherwise unreachable source");
    for (int rootKind = 0; rootKind < 4; ++rootKind) {
        auto rootedSession = std::make_shared<mparser::RuntimeSessionState>();
        mparser::RuntimeValue externalRoot;
        {
            mparser::BytecodeVmOptions create;
            create.sessionState = rootedSession;
            mparser::BytecodeVm creator;
            const auto created = creator.run(pipeline.bytecode,pipeline.semantic,create);
            check(created.diagnostics.empty(),"listener external-root fixture failed");
            const auto* source = findVariable(created,"pulse");
            const auto* watcher = findVariable(created,"watcher");
            check(source && watcher,"listener external-root exports missing");
            sourceFields = source->sharedFields;
            if (rootKind == 0) { externalRoot = *source; }
            else if (rootKind == 1) { externalRoot = watcher->sharedFields->at("Callback"); }
            else if (rootKind == 2) { externalRoot = *watcher; }
            else { externalRoot = mparser::makeRuntimeCellValue({watcher->sharedFields->at("Callback")}); }
        }
        mparser::BytecodeVmOptions next;
        next.sessionState = rootedSession;
        mparser::BytecodeVm executor;
        const auto kept = executor.run(followup.bytecode,followup.semantic,next);
        check(kept.diagnostics.empty() && !sourceFields.expired(),
            "listener collection discarded an externally rooted source/callback/listener");
        externalRoot = {};
        const auto released = executor.run(followup.bytecode,followup.semantic,next);
        check(released.diagnostics.empty() && sourceFields.expired(),
            "listener collection did not observe external-root release");
    }
    auto sharedCallback = lower(std::string(kPulseClass) + R"(
pulse=Pulse();
callback=@(source,data)pulse.record(data.EventName);
first=addlistener(pulse,'Tick',callback);
second=addlistener(pulse,'Tick',callback);
pulse.fire(0);
count=pulse.CallbackCount;
)");
    {
        mparser::BytecodeVmOptions create;
        create.sessionState = session;
        mparser::BytecodeVm creator;
        const auto created = creator.run(sharedCallback.bytecode,sharedCallback.semantic,create);
        check(created.diagnostics.empty(),"shared callback fixture failed");
        checkNumber(created,"count",2);
        sourceFields = findVariable(created,"pulse")->sharedFields;
    }
    (void)vm.run(followup.bytecode,followup.semantic,options);
    check(sourceFields.expired(),"shared listener callback was mistaken for an external root");
}

void runCallbackExceptionRecoverySmoke() {
    const std::string declaration = "classdef RecoverySource < handle\nevents\nTick\nend\nend\n";
    auto setup = lower(declaration + "obj=RecoverySource(); watcher=addlistener(obj,'Tick',callback);");
    auto notification = lower(declaration + "notify(obj,'Tick');");
    mparser::BytecodeVmOptions options;
    options.sessionState = std::make_shared<mparser::RuntimeSessionState>();
    options.callableContext = mparser::makeRuntimeCallableContext();
    mparser::RuntimeFunctionHandle callback;
    callback.kind = mparser::RuntimeFunctionHandleKind::Function;
    callback.backend = mparser::RuntimeFunctionHandleBackend::Bytecode;
    callback.context = mparser::makeRuntimeCallableContext();
    callback.display = "@external_callback";
    callback.targetName = "external_callback";
    options.initialWorkspace = {{"callback",mparser::makeRuntimeFunctionHandleValue(std::move(callback))}};
    mparser::BytecodeVm vm;
    const auto created = vm.run(setup.bytecode,setup.semantic,options);
    check(created.diagnostics.empty(),"listener recovery fixture failed");
    const auto* object = findVariable(created,"obj");
    const auto* watcher = findVariable(created,"watcher");
    check(object && watcher,"listener recovery fixture exports missing");
    options.initialWorkspace = {{"obj",*object},{"watcher",*watcher}};
    size_t calls = 0;
    options.inheritedCallableInvoker = [&](const mparser::RuntimeValue&,
        const std::vector<mparser::RuntimeValue>&,size_t,mparser::SourceSpan,mparser::RuntimeWorkspace*)
        -> mparser::RuntimeSourceCallableInvocationResult {
        if (++calls == 1) { throw std::runtime_error("injected listener callback failure"); }
        mparser::RuntimeSourceCallableInvocationResult result;
        result.succeeded = true;
        return result;
    };
    bool threw = false;
    try { (void)vm.run(notification.bytecode,notification.semantic,options); }
    catch (const std::runtime_error& error) {
        threw = std::string_view(error.what()) == "injected listener callback failure";
    }
    check(threw,"listener callback injection did not throw");
    const auto recovered = vm.run(notification.bytecode,notification.semantic,options);
    check(recovered.diagnostics.empty(),"listener callback did not recover after C++ exception");
    check(calls == 2,"callbackActive remained set after C++ exception");
    auto attachDestruction = lower(declaration +
        "destructionWatcher=addlistener(obj,'ObjectBeingDestroyed',callback);");
    options.initialWorkspace.push_back({"callback",watcher->sharedFields->at("Callback")});
    const auto attached = vm.run(attachDestruction.bytecode,attachDestruction.semantic,options);
    check(attached.diagnostics.empty(),"destruction exception fixture failed");
    const auto* destructionWatcher = findVariable(attached,"destructionWatcher");
    check(destructionWatcher,"destruction exception listener missing");
    auto destruction = lower(declaration + "delete(obj);");
    options.inheritedCallableInvoker = [](const mparser::RuntimeValue&,
        const std::vector<mparser::RuntimeValue>&,size_t,mparser::SourceSpan,mparser::RuntimeWorkspace*)
        -> mparser::RuntimeSourceCallableInvocationResult {
        throw std::runtime_error("injected destruction failure");
    };
    threw = false;
    try { (void)vm.run(destruction.bytecode,destruction.semantic,options); }
    catch (const std::runtime_error& error) { threw = std::string_view(error.what()) == "injected destruction failure"; }
    check(threw,"destruction callback injection did not throw");
    check(object->sharedFields->at("__mparser_handle_valid").number == 0,
        "destruction exception restored source validity");
    check(destructionWatcher->sharedFields->at("__mparser_valid").number == 0,
        "destruction exception left a coupled listener valid");
    const auto invalidNotification = vm.run(notification.bytecode,notification.semantic,options);
    check(hasDiagnostic(invalidNotification.diagnostics,"invalid or deleted object"),
        "native destruction exception left the source in the destroying scope");
    const auto repeated = vm.run(destruction.bytecode,destruction.semantic,options);
    check(repeated.diagnostics.empty(),"repeated deletion after native exception was not idempotent");
    options.sessionState->reset();
    const auto beforeReset = vm.run(setup.bytecode,setup.semantic,options);
    check(beforeReset.diagnostics.empty(),"reentrant reset fixture failed");
    const auto* oldObject = findVariable(beforeReset,"obj");
    const auto* oldWatcher = findVariable(beforeReset,"watcher");
    options.initialWorkspace = {{"obj",*oldObject},{"callback",oldWatcher->sharedFields->at("Callback")}};
    mparser::RuntimeValue newObject,newWatcher;
    options.inheritedCallableInvoker = [&](const mparser::RuntimeValue&,
        const std::vector<mparser::RuntimeValue>&,size_t,mparser::SourceSpan,mparser::RuntimeWorkspace*) {
        options.sessionState->reset();
        mparser::BytecodeVmOptions nested;
        nested.sessionState = options.sessionState;
        nested.callableContext = options.callableContext;
        nested.initialWorkspace = {{"callback",oldWatcher->sharedFields->at("Callback")}};
        mparser::BytecodeVm nestedVm;
        const auto created = nestedVm.run(setup.bytecode,setup.semantic,nested);
        check(created.diagnostics.empty(),"new registry creation during reset failed");
        newObject = *findVariable(created,"obj");
        newWatcher = *findVariable(created,"watcher");
        mparser::RuntimeSourceCallableInvocationResult result;
        result.succeeded = true;
        return result;
    };
    const auto reset = vm.run(notification.bytecode,notification.semantic,options);
    check(reset.diagnostics.empty(),"callback reentrant reset failed");
    check(oldWatcher->opaqueId == newWatcher.opaqueId,
        "reentrant reset did not exercise listener ID reuse");
    check(oldWatcher->sharedFields->at("__mparser_valid").number == 0 &&
        newWatcher.sharedFields->at("__mparser_valid").number == 1,
        "old callback scope changed the replacement listener registry");
    options.initialWorkspace = {{"obj",newObject},{"watcher",newWatcher}};
    calls = 0;
    options.inheritedCallableInvoker = [&](const mparser::RuntimeValue&,
        const std::vector<mparser::RuntimeValue>&,size_t,mparser::SourceSpan,mparser::RuntimeWorkspace*) {
        ++calls;
        mparser::RuntimeSourceCallableInvocationResult result;
        result.succeeded = true;
        return result;
    };
    const auto afterReset = vm.run(notification.bytecode,notification.semantic,options);
    check(afterReset.diagnostics.empty() && calls == 1,
        "replacement listener failed after old callback scope returned");
}

void runCoupledListenerReachabilitySmoke() {
    const auto setup = lower(std::string(kPulseClass) + R"(
if phase==0
root=Pulse();
middle=Pulse();
leaf=Pulse();
rootWatcher=addlistener(root,'Tick',@(source,data)middle.fire(0));
middleWatcher=addlistener(middle,'Tick',@(source,data)leaf.fire(0));
leafWatcher=addlistener(leaf,'Tick',@(source,data)source.record(data.EventName));
else
root.fire(0);
end
)");
    const auto cleanup = lower("unrelated=1;");
    auto session = std::make_shared<mparser::RuntimeSessionState>();
    auto context = mparser::makeRuntimeCallableContext();
    mparser::RuntimeValue root;
    std::weak_ptr<mparser::RuntimeWorkspace> middleFields,leafFields,leafListener;
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        options.callableContext = context;
        options.initialWorkspace = {{"phase",mparser::makeRuntimeNumberValue(0)}};
        mparser::BytecodeVm vm;
        const auto created = vm.run(setup.bytecode,setup.semantic,options);
        check(created.diagnostics.empty(),"coupled listener chain fixture failed");
        root = *findVariable(created,"root");
        middleFields = findVariable(created,"middle")->sharedFields;
        leafFields = findVariable(created,"leaf")->sharedFields;
        leafListener = findVariable(created,"leafWatcher")->sharedFields;
    }
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        options.callableContext = context;
        options.initialWorkspace = {{"root",root},{"phase",mparser::makeRuntimeNumberValue(1)}};
        mparser::BytecodeVm vm;
        const auto fired = vm.run(setup.bytecode,setup.semantic,options);
        check(fired.diagnostics.empty(),"coupled listener chain notification failed:\n" +
            diagnosticsText(fired.diagnostics));
        const auto listener = leafListener.lock();
        check(listener && listener->at("__mparser_valid").number == 1,
            "live source did not retain a transitively captured source's listener");
        const auto leaf = leafFields.lock();
        check(leaf && leaf->at("Pulse::CallbackCount").number == 1,
            "transitive coupled listener callback was silently lost");
    }
    root = {};
    {
        mparser::BytecodeVmOptions options;
        options.sessionState = session;
        mparser::BytecodeVm vm;
        const auto released = vm.run(cleanup.bytecode,cleanup.semantic,options);
        check(released.diagnostics.empty(),"coupled listener chain cleanup failed");
    }
    check(middleFields.expired() && leafFields.expired() && leafListener.expired(),
        "unrooted coupled listener chain was retained after its root was released");
}

} // namespace

int main() {
    try {
        runFrontendAndBytecodeSmoke();
        runFunctionHandleSmoke();
        runEventLifecycleSmoke();
        runListenerOwnershipReleaseSmoke();
        runInheritanceAndDiagnosticsSmoke();
        runCallbackExceptionRecoverySmoke();
        runSessionListenerSourceReleaseSmoke();
        runCapturedSourceSessionReleaseSmoke();
        runCoupledListenerReachabilitySmoke();
        std::cout << "event listener smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
