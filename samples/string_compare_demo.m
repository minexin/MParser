function y = string_compare_demo()
mode = "fast";
y = 0;

if mode == "fast"
    y = y + 1;
end

if mode ~= "slow"
    y = y + 10;
end

if strcmp(mode, "fast")
    y = y + 100;
end

if strcmp(mode, "slow")
    y = 999;
else
    y = y + 1000;
end
end
