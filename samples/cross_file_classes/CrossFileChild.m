classdef CrossFileChild < CrossFileBase
    properties (Access = private)
        Value = 2
    end

    methods
        function obj = CrossFileChild(baseValue, childValue)
            obj = obj@CrossFileBase(baseValue);
            obj.Value = childValue;
        end

        function value = childCode(obj)
            value = obj.code();
        end

        function value = step(obj)
            value = 2;
        end
    end

    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 200;
        end
    end
end
