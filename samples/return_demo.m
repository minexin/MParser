function y = return_demo()
y = 1;

for i = 1:5
    if i == 3
        y = y + helper();
        return
    end
    y = y + i;
end

y = 999;
end

function out = helper()
out = 100;
return
out = 1000;
end
