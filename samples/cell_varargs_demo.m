function [input_count, first_extra, varargout] = cell_varargs_demo(seed, varargin)
C = {seed, "cell"};
C{3} = varargin{2};
input_count = nargin;
first_extra = varargin{1};
varargout{1} = C{3};
varargout{2} = nargout;
end
