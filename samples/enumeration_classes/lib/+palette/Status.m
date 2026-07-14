classdef Status
    properties
        Code
        Label
    end

    methods
        function obj = Status(code, label)
            obj.Code = code;
            obj.Label = label;
        end

        function value = isReady(obj)
            value = obj == palette.Status.Ready;
        end
    end

    enumeration
        Ready(10, 'ready'), Busy(20, 'busy')
    end

    enumeration (Hidden)
        Internal(99, 'internal')
    end
end
