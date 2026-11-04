classdef PrivateDeleteOwner < handle
    properties
        Recorder
    end
    methods
        function obj = PrivateDeleteOwner(recorder)
            obj.Recorder = recorder;
        end
        function value = makeDeleter(obj)
            value = @obj.delete;
        end
    end
    methods (Access=private)
        function delete(obj)
            obj.Recorder.Count = obj.Recorder.Count + 1;
        end
    end
end
