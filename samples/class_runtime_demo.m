classdef Meter
    properties
        Value
    end

    methods
        function obj = Meter(value)
            obj.Value = value;
        end

        function result = scale(obj, factor)
            result = obj.Value * factor;
        end
    end

    methods (Static)
        function obj = make(value)
            obj = Meter(value);
        end
    end
end

meter = Meter(4);
meter.Value = 5;
scaled = meter.scale(3);
created = Meter.make(2);
created_value = created.Value;
