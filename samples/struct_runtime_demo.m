s = struct("beta", 2, "alpha", 1);
field = "gamma";
s.(field) = 3;
s.delta = {4, 5};

created.dynamic = 7;
names = fieldnames(s);
flags = isfield(s, {"alpha", "missing", "gamma"});
trimmed = rmfield(s, {"beta", "delta"});
wrapped = struct("value", {9});
selected = s.(field);
nonstruct_flag = isfield(41, "alpha");

order_ok = strcmp(names{1}, "beta") + ...
           strcmp(names{2}, "alpha") + ...
           strcmp(names{3}, "gamma") + ...
           strcmp(names{4}, "delta");

summary = s.beta * 1000 + s.alpha * 100 + selected * 10 + ...
          created.dynamic + wrapped.value + trimmed.alpha + ...
          trimmed.gamma + sum(flags, "all") + order_ok * 10 + ...
          isstruct(s) + nonstruct_flag
