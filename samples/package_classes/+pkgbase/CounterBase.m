classdef (AllowedSubclasses = ?pkgchild.Counter) CounterBase
    properties (Access = private)
        Value = 1
    end

    methods
        function obj = CounterBase(value)
            obj.Value = value;
        end

        function value = baseCode(obj)
            value = obj.code();
        end

        function value = dispatchCode(obj)
            value = obj.step();
        end

        function value = step(obj)
            value = 1;
        end
    end

    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 100;
        end
    end
end
