classdef (Sealed) BasicClass < handle
    properties (SetAccess = private)
        Value double = 0
    end

    methods
        function obj = BasicClass(v)
            arguments
                v double = 0
            end
            obj.Value = v;
        end

        function r = multiplyBy(obj, n)
            r = obj.Value * n;
        end
    end

    methods (Static)
        function obj = zero()
            obj = BasicClass(0);
        end
    end

    events
        Changed
    end

    enumeration
        Red
        Green
    end
end
