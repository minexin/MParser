values = [1 2 3 4];

score = 0;
for i = 1:4
    score = score + abs(i - 3);
end

total = sum(values);
running = cumsum(values);
reshaped = reshape(values, 2, 2);
[maximum, maximum_index] = max(values);

lastwarn("registry ready", "MParser:RegistryDemo");
[message, identifier] = lastwarn();
flags = strcmp(message, "registry ready") + ...
        strcmp(identifier, "MParser:RegistryDemo");

summary = score * 100000 + total * 10000 + ...
          running(4) * 1000 + reshaped(2, 2) * 100 + ...
          maximum * 10 + maximum_index + flags
