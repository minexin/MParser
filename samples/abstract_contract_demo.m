classdef AbstractMeasure
    properties (Abstract, SetAccess = protected)
        Scale(1,1) double {mustBePositive}
    end

    methods (Abstract, Access = protected)
        value = transform(obj, input)
    end

    methods
        function obj = AbstractMeasure(scale)
            obj.Scale = scale;
        end

        function value = evaluate(obj, input)
            value = obj.transform(input) + obj.Scale;
        end
    end

    methods (Sealed)
        function value = kindCode(obj)
            value = 9;
        end
    end
end

classdef (Sealed) LinearMeasure < AbstractMeasure
    properties (SetAccess = protected)
        Scale = 2
    end

    methods
        function obj = LinearMeasure(scale)
            obj = obj@AbstractMeasure(scale);
        end

        function output = transform(self, input)
            output = input * self.Scale;
        end
    end
end

measure = LinearMeasure(3);
evaluated = measure.evaluate(4);
scale = measure.Scale;
kind_code = measure.kindCode();
