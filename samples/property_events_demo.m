classdef ObservableCounter < dynamicprops
    properties
        Backing = 1
        Trace = 0
        LastProperty = ''
        LastEvent = ''
        SourceMatches = 0
        AffectedMatches = 0
        DataClassMatches = 0
    end

    properties (Dependent, GetObservable, SetObservable)
        Value
    end

    methods
        function obj = ObservableCounter()
        end

        function value = get.Value(obj)
            obj.Trace = obj.Trace * 10 + 2;
            value = obj.Backing;
        end

        function set.Value(obj, value)
            obj.Trace = obj.Trace * 10 + 5;
            obj.Backing = value;
        end

        function value = record(obj, code, property, data)
            obj.Trace = obj.Trace * 10 + code;
            obj.LastProperty = property.Name;
            obj.LastEvent = data.EventName;
            obj.SourceMatches = data.Source == property;
            obj.AffectedMatches = data.AffectedObject == obj;
            obj.DataClassMatches = strcmp(class(data), ...
                'event.PropertyEvent');
            value = obj.Trace;
        end
    end
end

counter = ObservableCounter();
pre_get = counter.addlistener('Value', 'PreGet', ...
    @(property, data) counter.record(1, property, data));
post_get = counter.addlistener('Value', 'PostGet', ...
    @(property, data) counter.record(3, property, data));
pre_set = counter.addlistener('Value', 'PreSet', ...
    @(property, data) counter.record(4, property, data));
post_set = counter.addlistener('Value', 'PostSet', ...
    @(property, data) counter.record(6, property, data));

counter.Trace = 0;
counter.Value = 7;
set_trace = counter.Trace;
counter.Trace = 0;
value = counter.Value;
get_trace = counter.Trace;
source_matches = counter.SourceMatches;
affected_matches = counter.AffectedMatches;
data_class_matches = counter.DataClassMatches;
listener_isa = isa(pre_get, 'event.listener');

delete(pre_get);
delete(post_get);
delete(pre_set);
delete(post_set);

dynamic_property = addprop(counter, 'Live');
dynamic_property.SetObservable = true;
dynamic_pre = addlistener(counter, dynamic_property, 'PreSet', ...
    @(property, data) counter.record(8, property, data));
dynamic_post = addlistener(counter, 'Live', 'PostSet', ...
    @(property, data) counter.record(9, property, data));
counter.Trace = 0;
counter.Live = 5;
dynamic_set_trace = counter.Trace;
delete(dynamic_property);
dynamic_listener_invalid = ~isvalid(dynamic_post);

value_property = findprop(counter, 'Value');
constructor_listener = event.proplistener(counter, value_property, ...
    'PostSet', @(property, data) counter.record(7, property, data));
constructor_class_matches = strcmp(class(constructor_listener), ...
    'event.proplistener');
counter.Trace = 0;
counter.Value = 8;
constructor_trace = counter.Trace;
constructor_listener.delete();

summary = set_trace + get_trace + value + dynamic_set_trace + ...
    constructor_trace + source_matches + affected_matches + ...
    data_class_matches + listener_isa + dynamic_listener_invalid + ...
    constructor_class_matches;
