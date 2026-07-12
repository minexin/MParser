function y = switch_demo()
mode = "beta";
y = 0;

switch mode
    case "alpha"
        y = 1;
    case "beta"
        y = 2;
    otherwise
        y = 3;
end

n = 3;
switch n
    case 1
        y = y + 10;
    case 3
        y = y + 30;
    otherwise
        y = y + 100;
end

v = [1 2];
switch v
    case [2 1]
        y = y + 1000;
    case [1 2]
        y = y + 300;
end

switch 9
    case 1
        y = y + 1000;
    otherwise
        y = y + 4000;
end
end
