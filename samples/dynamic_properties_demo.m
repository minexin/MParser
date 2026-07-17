classdef DynamicRecord < dynamicprops
    properties
        Backing = 5
    end

    methods
        function obj = DynamicRecord()
        end

        function obj = setBacking(obj, value)
            obj.Backing = value;
        end
    end
end

record = DynamicRecord();
other = DynamicRecord();

score_property = addprop(record, 'Score');
record.Score = 7;
alias = record;
alias.Score = 9;
shared_score = record.Score;

computed_property = record.addprop('Computed');
computed_property.GetMethod = @(obj) obj.Backing * 2;
computed_property.SetMethod = @(obj, value) obj.setBacking(value / 2);
record.Computed = 30;
computed_value = record.Computed;
backing_value = record.Backing;

hidden_property = addprop(record, 'Internal');
hidden_property.Hidden = true;
property_count = numel(properties(record));

found_score = record.findprop('Score');
static_property = findprop(record, 'Backing');
missing_property = findprop(record, 'Missing');
valid_before = score_property.isvalid();
score_name_matches = strcmp(score_property.Name, 'Score');
descriptor_class_matches = strcmp(class(score_property), ...
    'matlab.metadata.DynamicProperty');
dynamic_isa = isa(score_property, 'meta.DynamicProperty');
property_isa = isa(score_property, 'matlab.metadata.Property');
same_descriptor = found_score == score_property;
static_property_isa = isa(static_property, 'matlab.metadata.Property');
missing_is_empty = isempty(missing_property);
has_addprop = ismethod(record, 'addprop');
superclass_matches = strcmp(metaclass(record).SuperclassList(1).Name, ...
    'dynamicprops');
descriptor_superclass_matches = strcmp(...
    metaclass(score_property).SuperclassList(1).Name, ...
    'matlab.metadata.Property');

score_property.delete();
valid_after = isvalid(score_property);
has_score_after_delete = isprop(record, 'Score');
other_has_score = isprop(other, 'Score');

summary = shared_score + computed_value + backing_value + valid_before + ...
    valid_after + has_score_after_delete + other_has_score + dynamic_isa + ...
    property_isa + same_descriptor + static_property_isa + ...
    missing_is_empty + property_count + has_addprop + superclass_matches + ...
    descriptor_superclass_matches + score_name_matches + ...
    descriptor_class_matches;
