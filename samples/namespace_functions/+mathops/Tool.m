classdef Tool
    properties
        Value = 0
    end
    methods
        function obj = Tool(value)
            obj.Value = value;
        end
    end
    methods (Static)
        function value = tag()
            value = 42;
        end

        function value = badge()
            value = 43;
        end
    end
end
