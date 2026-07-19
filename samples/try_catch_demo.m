function y = f()
y = 0;
try
    for k = 1:3
        y = y + k;
        x = missingName;
        y = 999;
    end
catch err
    message = err.message;
    y = y + 10;
end

try
    y = y + 2;
catch err2
    y = 999;
end
end
