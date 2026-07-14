classdef Counter < pkgbase.CounterBase
    properties (Access = private)
        Value = 2
    end

    methods
        function obj = Counter(baseValue, childValue)
            obj = obj@pkgbase.CounterBase(baseValue);
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

    methods (Static)
        function value = tag()
            value = 35;
        end
    end
end
