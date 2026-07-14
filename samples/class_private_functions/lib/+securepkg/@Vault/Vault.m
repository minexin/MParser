classdef Vault
    properties (Access = private)
        Secret = 0
    end
    methods
        function obj = Vault(value)
            obj.Secret = value;
        end
        value = reveal(obj)
        function [obj, value] = bump(obj)
            [obj, value] = increment(obj);
        end
        function value = functionChoice(obj)
            value = choose(obj);
        end
        function value = dotChoice(obj)
            value = obj.choose();
        end
        function value = choose(obj)
            value = obj.Secret + 1000;
        end
    end
    methods (Static)
        function value = staticValue()
            value = privateConstant();
        end
    end
end
