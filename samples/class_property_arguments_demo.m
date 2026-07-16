answer = configure(Width=3, Height=4);
partial = configure(Wid=5, Height=2);

function y = configure(options)
arguments
    options.?PlotOptions
    options.Height (1,1) double {mustBeNonnegative}
end
y = options.Width * 10 + options.Height;
end

classdef BaseOptions
properties
    Width (1,1) double {mustBePositive} = 100
end
properties (SetAccess = private)
    Secret double
end
end

classdef PlotOptions < BaseOptions
properties
    Height (1,1) double {mustBePositive} = 200
end
end
