classdef MethodIdentityBase
    methods
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
            value = 10;
        end
    end
end

classdef MethodIdentityChild < MethodIdentityBase
    methods
        function value = childCode(obj)
            value = obj.code();
        end

        function value = step(obj)
            value = 2;
        end
    end

    methods (Access = private)
        function value = code(obj)
            value = 20;
        end
    end
end

classdef LeftMethodIdentity
    methods
        function value = leftCode(obj)
            value = obj.sharedCode();
        end
    end

    methods (Access = private)
        function value = sharedCode(obj)
            value = 3;
        end
    end
end

classdef RightMethodIdentity
    methods
        function value = rightCode(obj)
            value = obj.sharedCode();
        end
    end

    methods (Access = private)
        function value = sharedCode(obj)
            value = 4;
        end
    end
end

classdef CombinedMethodIdentity < LeftMethodIdentity & RightMethodIdentity
end

object = MethodIdentityChild();
base_code = object.baseCode();
child_code = object.childCode();
dynamic_code = object.dispatchCode();

combined = CombinedMethodIdentity();
left_code = combined.leftCode();
right_code = combined.rightCode();
