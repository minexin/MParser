classdef AccessOwner < handle
    properties
        PrivateCalls = 0
        ProtectedCalls = 0
    end
    methods
        function value = publicRead(obj)
            value = 7;
        end
        function callback = privateFactory(obj)
            callback = @obj.privateRead;
        end
        function value = invoke(obj, callback)
            value = callback();
        end
    end
    methods (Access=private)
        function value = privateRead(obj)
            obj.PrivateCalls = obj.PrivateCalls + 1;
            value = 31;
        end
    end
    methods (Access=protected)
        function value = protectedRead(obj)
            obj.ProtectedCalls = obj.ProtectedCalls + 1;
            value = 41;
        end
    end
end
