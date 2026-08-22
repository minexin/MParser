classdef Pulse < handle
    properties
        Count = 0
        CallbackCount = 0
        LastEvent = ''
    end

    events (ListenAccess = public, NotifyAccess = private)
        Tick
    end

    events (Hidden)
        Internal
    end

    methods
        function obj = Pulse()
        end

        function value = emit(obj, amount)
            obj.Count = obj.Count + amount;
            notify(obj, 'Tick');
            value = obj.Count;
        end

        function value = record(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 1;
            obj.LastEvent = eventName;
            value = obj.CallbackCount;
        end

        function recordWithoutOutput(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 20;
            obj.LastEvent = eventName;
        end

        function callback = privateCallback(obj)
            callback = @(src, evt) obj.recordPrivate(evt.EventName);
        end
    end

    methods (Access = private)
        function value = recordPrivate(obj, eventName)
            obj.CallbackCount = obj.CallbackCount + 10;
            obj.LastEvent = eventName;
            value = obj.CallbackCount;
        end
    end
end
