classdef RunResult < handle
    properties
        EventCount = 0
    end

    properties (SetObservable)
        Value = 0
    end

    methods
        function obj = RunResult()
        end

        function count = record(obj, property, data)
            obj.EventCount = obj.EventCount + ...
                strcmp(property.Name, 'Value') + ...
                strcmp(data.EventName, 'PostSet') + ...
                (data.AffectedObject == obj);
            count = obj.EventCount;
        end
    end
end

result = RunResult();
listener_handle = addlistener(result, 'Value', 'PostSet', ...
    @(property, data) result.record(property, data));

total = 0;
for j = 1:12
    for i = 1:2:5
        total = total + j * i;
    end
end

result.Value = total;
summary = result.Value + result.EventCount
