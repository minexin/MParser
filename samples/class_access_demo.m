classdef TunableReading
    properties (Access = private)
        Raw(1,1) double = 1
    end
    properties (Access = protected)
        Calibration(1,1) double {mustBePositive} = 2
    end
    properties (Dependent)
        Value(1,1) double {mustBePositive}
    end
    properties (Constant)
        UnitScale(1,1) double = 10
    end
    methods
        function value = get.Value(obj)
            value = obj.Raw;
        end
        function obj = set.Value(obj, value)
            obj.Raw = value;
        end
    end
end

classdef TaggedReading < TunableReading
    properties (SetAccess = immutable)
        Id(1,1) double {mustBeInteger, mustBePositive} = 1
    end
    methods
        function obj = TaggedReading(id, value)
            obj = obj@TunableReading();
            obj.Id = id;
            obj.Value = value;
        end
        function value = normalized(obj)
            value = obj.Value / TaggedReading.UnitScale;
        end
        function value = calibrated(obj)
            value = obj.Value * obj.Calibration;
        end
    end
end

reading = TaggedReading(7, 40);
value = reading.Value;
normalized = reading.normalized();
calibrated = reading.calibrated();
identifier = reading.Id;
scale = TaggedReading.UnitScale;
