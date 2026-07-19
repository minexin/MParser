classdef LvalueLeaf
    properties
        Value
    end
    methods
        function obj = LvalueLeaf(value)
            obj.Value = value;
        end
    end
end

classdef LvalueHolder
    properties
        Child
    end
    methods
        function obj = LvalueHolder(child)
            obj.Child = child;
        end
    end
end

classdef LvalueHandleHolder < handle
    properties
        Child
    end
    methods
        function obj = LvalueHandleHolder(child)
            obj.Child = child;
        end
    end
end

value_holder = LvalueHolder(LvalueLeaf(3));
value_copy = value_holder;
value_copy.Child.Value = 8;
value_new = value_copy.Child.Value;
value_old = value_holder.Child.Value;

handle_holder = LvalueHandleHolder(LvalueLeaf(4));
handle_alias = handle_holder;
handle_alias.Child.Value = 9;
handle_seen = handle_holder.Child.Value;

summary = value_new * 100 + value_old * 10 + handle_seen;
