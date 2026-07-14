classdef Counter
    properties
        Value = 0
    end

    methods
        function obj = Counter(value)
            obj.Value = value;
        end

        function value = code(obj)
            value = obj.Value + 300;
        end
    end
end
