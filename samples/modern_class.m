classdef (Abstract, Sealed = false) Fancy < pkg.Base & matlab.mixin.SetGet
    properties (Access = private, Dependent)
        Name (1,1) string {mustBeTextScalar} = "demo"
    end

    methods (Abstract, Access = protected)
        y = compute(obj, x)
    end

    events (NotifyAccess = private)
        Updated
    end
end
