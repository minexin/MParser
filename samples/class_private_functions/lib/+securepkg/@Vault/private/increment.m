function [obj, value] = increment(obj)
    obj.Secret = obj.Secret + 1;
    value = obj.Secret;
end
