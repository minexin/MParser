classdef SharedState < handle
    properties
        Code(1,1) double {mustBeInteger, mustBeNonnegative} = 1
    end
end

classdef Measurement
    properties
        Samples(1,3) double {mustBeFinite, mustBeNonnegative} = [1 2 3]
        Gain(1,1) double {mustBeGreaterThan(Gain, 0)} = 2
        Name(1,1) string {mustBeNonzeroLengthText} = "sensor"
        State(1,1) SharedState = SharedState()
    end
    methods
        function obj = Measurement(scale)
            obj.Samples = obj.Samples * scale;
        end
    end
end

first = Measurement(2);
second = Measurement(1);
first.Samples = 4;
state = first.State;
state.Code = 9;

first_sum = sum(first.Samples, "all");
second_sum = sum(second.Samples, "all");
shared_code = second.State.Code;
gain = first.Gain;
label = first.Name;
