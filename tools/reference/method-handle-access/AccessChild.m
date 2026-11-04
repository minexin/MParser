classdef AccessChild < AccessOwner
    methods
        function callback = protectedFactory(obj)
            callback = @obj.protectedRead;
        end
    end
end
