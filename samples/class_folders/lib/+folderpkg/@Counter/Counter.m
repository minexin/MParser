classdef Counter
    properties (Access = private)
        Value = 0
    end

    methods
        function obj = Counter(value)
            obj.Value = value;
        end

        result = scale(obj, factor)
        result = reveal(obj)
    end

    methods (Static)
        result = twice(value)
    end

    methods (Access = private)
        result = secret(obj)
    end
end
