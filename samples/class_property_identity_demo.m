classdef PrivateCounter
    properties (Access = private)
        Count(1,1) double {mustBeNonnegative} = 2
    end

    methods
        function obj = PrivateCounter(value)
            obj.Count = value;
        end

        function value = baseCount(obj)
            value = obj.Count;
        end

        function obj = setBaseCount(obj, value)
            obj.Count = value;
        end
    end
end

classdef VisibleCounter < PrivateCounter
    properties
        Count(1,1) double = 1
    end

    methods
        function obj = VisibleCounter(baseValue, visibleValue)
            obj = obj@PrivateCounter(baseValue);
            obj.Count = visibleValue;
        end
    end
end

classdef PrivateHandleCounter < handle
    properties (Access = private)
        Count = 10
    end

    methods
        function setBaseCount(obj, value)
            obj.Count = value;
        end

        function value = baseCount(obj)
            value = obj.Count;
        end
    end
end

classdef VisibleHandleCounter < PrivateHandleCounter
    properties
        Count = 20
    end
end

counter = VisibleCounter(6, 7);
base_before = counter.baseCount();
visible_before = counter.Count;

copy = counter;
copy = copy.setBaseCount(9);
copy.Count = 10;
base_after = copy.baseCount();
visible_after = copy.Count;
original_base = counter.baseCount();
original_visible = counter.Count;

handle_counter = VisibleHandleCounter();
handle_alias = handle_counter;
handle_counter.setBaseCount(11);
handle_alias.Count = 22;
handle_base = handle_alias.baseCount();
handle_visible = handle_counter.Count;
