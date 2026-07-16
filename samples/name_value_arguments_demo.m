[modern, modernNargin] = configure(2, Scale=4, Offset=3);
[legacy, legacyNargin] = configure(2, "Scale", 5);
[partial, partialNargin] = configure(2, Sc=6);
[optionalText, optionalTextNargin] = choose("Scale", 5);
[groupedValue, groupedNargin] = grouped(10, 1, 2, Scale=4);
[missingField, assigned] = sparseOptions(Offset=2);

summary = modern + legacy + partial + optionalText + groupedValue + ...
    missingField + assigned + modernNargin + legacyNargin + ...
    partialNargin + optionalTextNargin + groupedNargin;

function [y, seen] = configure(x, options)
arguments
    x (1,1) double
    options.Scale (1,1) double {mustBePositive} = 3
    options.Offset (1,1) double = 1
end
y = x * options.Scale + options.Offset;
seen = nargin;
end

function [y, seen] = choose(label, options)
arguments
    label string = "base"
    options.Scale (1,1) double = 1
end
y = options.Scale;
seen = nargin;
end

function [y, seen] = grouped(seed, x, options)
arguments
    seed (1,1) double
end
arguments (Repeating)
    x (1,1) double
end
arguments
    options.Scale (1,1) double = 1
end
y = seed + numel(x) + options.Scale;
seen = nargin;
end

function [hasMissing, y] = sparseOptions(options)
arguments
    options.Missing double
    options.Offset (1,1) double = 1
end
hasMissing = isfield(options, "Missing");
options.Offset = options.Offset + 1;
y = options.Offset;
end
