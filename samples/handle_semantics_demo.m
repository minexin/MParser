classdef MutableBox < handle
    properties
        Value
        LastNargout
    end

    methods
        function obj = MutableBox(value)
            obj.Value = value;
            obj.LastNargout = 99;
        end

        function setValue(obj, value)
            obj.Value = value;
            obj.LastNargout = nargout;
        end
    end
end

original = MutableBox(1);
alias = original;
alias.setValue(9);
shared_value = original.Value;
statement_nargout = original.LastNargout;
