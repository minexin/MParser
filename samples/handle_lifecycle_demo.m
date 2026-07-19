classdef LifecycleRecorder < handle
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

recorder = LifecycleRecorder();
object = LifecycleChild(recorder);
alias = object;
coupled_listener = addlistener(object, 'ObjectBeingDestroyed', ...
    @(source, data) recorder.recordEvent(source, data));
dynamic_property = addprop(object, 'TransientState');
object.TransientState = 7;

event_names = events(object);
event_name_matches = strcmp(event_names{1}, ...
    'ObjectBeingDestroyed');
metadata_event_name = metaclass(object).EventList(1).Name;
metadata_event_matches = strcmp(metadata_event_name, ...
    'ObjectBeingDestroyed');

delete(object);
object_invalid = ~isvalid(object);
alias_invalid = ~isvalid(alias);
coupled_listener_invalid = ~isvalid(coupled_listener);
dynamic_property_invalid = ~isvalid(dynamic_property);

trace_before_second_delete = recorder.Trace;
delete(alias);
delete_is_idempotent = recorder.Trace == trace_before_second_delete;

uncoupled_recorder = LifecycleRecorder();
uncoupled_object = LifecycleChild(uncoupled_recorder);
uncoupled_listener = listener(uncoupled_object, ...
    'ObjectBeingDestroyed', ...
    @(source, data) isvalid(source));
delete(uncoupled_object);
uncoupled_listener_survives = isvalid(uncoupled_listener);
uncoupled_source_invalid = ~isvalid(uncoupled_object);
delete(uncoupled_listener);

method_recorder = LifecycleRecorder();
method_object = LifecycleChild(method_recorder);
method_object.delete();
method_delete_invalid = ~isvalid(method_object);

summary = recorder.Trace + recorder.InvalidObservations + ...
    recorder.EventMatches + object_invalid + alias_invalid + ...
    coupled_listener_invalid + dynamic_property_invalid + ...
    event_name_matches + metadata_event_matches + ...
    delete_is_idempotent + uncoupled_listener_survives + ...
    uncoupled_source_invalid + method_recorder.Trace + ...
    method_delete_invalid
