function y = short_circuit_demo()
y = 0;

if false && missingName
    y = 999;
else
    y = y + 1;
end

if true || missingName
    y = y + 10;
end

try
    if true && missingName
        y = 999;
    end
catch err
    y = y + 100;
end

try
    if false || missingName
        y = 999;
    end
catch err2
    y = y + 1000;
end
end
