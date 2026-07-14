function result = scale(obj, factor)
    result = localMultiply(obj.Value, factor);
end

function result = localMultiply(left, right)
    result = left * right;
end
