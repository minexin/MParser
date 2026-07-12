function [total, input_count, output_count] = function_contract_demo(x, n)
total = 0;
for i = 1:n
    total = total + x * i;
end
input_count = nargin;
output_count = nargout;
end
