classdef DispatchOwner < handle
    properties
        Value = 7
    end
    methods
        y = read(obj)
        y = combine(x,obj)
        [a,b] = parts(obj)
        function touch(obj)
            obj.Value = obj.Value + 1;
        end
        function y = length(obj)
            y = 17;
        end
        function y = pick(varargin)
            y = 1;
        end
        function y = marker(x,obj)
            y = x + 23;
        end
        function y = inside(obj)
            y = secret(obj);
        end
    end
    methods (Access=private)
        function y = secret(obj)
            y = 31;
        end
    end
end
