classdef SampleValue
    properties
        Value = 0
    end
    methods
        function obj = SampleValue(value)
        arguments
            value (1,1) double = 0
        end
            obj.Value = value;
        end
        function result = total(obj)
            result = sum([obj.Value]);
        end
    end
end

classdef SampleHandle < handle
    properties
        Value = 0
    end
    methods
        function obj = SampleHandle(value)
        arguments
            value (1,1) double = 0
        end
            obj.Value = value;
        end
    end
end

classdef SampleShape < matlab.mixin.Heterogeneous
    properties
        Code = 0
    end
    methods
        function obj = SampleShape(code)
        arguments
            code (1,1) double = 0
        end
            obj.Code = code;
        end
        function result = total(obj)
            result = sum([obj.Code]);
        end
    end
end

classdef SampleCircle < SampleShape
    methods
        function obj = SampleCircle(code)
            obj.Code = code;
        end
    end
end

classdef SampleSquare < SampleShape
    methods
        function obj = SampleSquare(code)
            obj.Code = code;
        end
    end
end

items = SampleValue(1);
items(3).Value = 30;
growth_values = isequal([items.Value], [1 0 30]);
array_method = items.total() == 31;

copied = items;
copied(1).Value = 99;
value_copy = items(1).Value == 1 && copied(1).Value == 99;

grid = [SampleValue(1), SampleValue(2); ...
        SampleValue(3), SampleValue(4)];
cube = reshape(grid, 2, 1, 2);
permuted = permute(cube, [3 2 1]);
transform_layout = cube(2, 1, 2).Value == 4 && ...
                   permuted(1, 1, 2).Value == 3;
trimmed = grid;
trimmed(2, :) = [];
deletion_layout = isequal([trimmed.Value], [1 2]);

handle_value = SampleHandle(7);
aliases = [handle_value, handle_value];
aliases(1).Value = 11;
handle_alias = aliases(2).Value == 11;
grown_handles = handle_value;
grown_handles(3) = SampleHandle(30);
independent_gap = grown_handles(1) ~= grown_handles(2) && ...
                  grown_handles(2) ~= grown_handles(3);
valid_before = all(isvalid(grown_handles));
delete(aliases);
alias_deleted = all(~isvalid(aliases));
remaining_valid = isequal(isvalid(grown_handles), logical([0 1 1]));
delete(grown_handles);
all_deleted = all(~isvalid(grown_handles));

mixed = [SampleCircle(5), SampleSquare(7)];
heterogeneous_class = strcmp(class(mixed), 'SampleShape');
heterogeneous_method = mixed.total() == 12;
first = mixed(1);
heterogeneous_narrowing = strcmp(class(first), 'SampleCircle');

summary = sum([growth_values, array_method, value_copy, ...
    transform_layout, deletion_layout, handle_alias, independent_gap, ...
    valid_before, alias_deleted, remaining_valid, all_deleted, ...
    heterogeneous_class, heterogeneous_method, heterogeneous_narrowing]);
