[sumValue, productValue, arityValue] = contractValues(4, 5);
summary = sumValue * 1000 + productValue * 100 + arityValue;

function [sumValue, productValue, arityValue] = contractValues(left, right)
payload = {left, right};
record = struct();
record.sum = payload{1} + payload{2};
record.product = payload{1} * payload{2};
sumValue = record.sum;
productValue = record.product;
arityValue = nargin * 10 + nargout;
end
