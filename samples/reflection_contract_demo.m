classdef ReflectionSample < handle
    properties
        Values(1,:) double {mustBeFinite, mustBePositive} = [1 2]
        Count = 3
    end

    methods (Static)
        function value = staticValue()
            value = 7;
        end
    end
end

classdef ReflectionDynamic < dynamicprops
    properties
        Tag = 0
    end
end

class_info = ?ReflectionSample;
[first_property, second_property] = class_info.PropertyList.Name;
property = findobj(class_info.PropertyList, 'Name', 'Values');
static_method = findobj(class_info.MethodList, 'Name', 'staticValue');
validation = property.Validation;
validation_functions = validation.ValidationFunctions;
validator = validation_functions{2};

valid_value = validation.isValidValue([2 3]);
invalid_value = validation.isValidValue([2, -1]);
validation.validateValue([3 4]);
validator([3 4]);

dynamic_object = ReflectionDynamic();
dynamic_object.Tag = 4;
dynamic_property = addprop(dynamic_object, 'Extra');
found_object = dynamic_object.findobj('Tag', 4);

summary = numel(class_info.PropertyList) + ...
    strcmp(first_property, 'Values') + ...
    strcmp(second_property, 'Count') + ...
    strcmp(property.Name, 'Values') + ...
    isa(validation, 'matlab.metadata.Validation') + ...
    isa(validation, 'meta.Validation') + ...
    strcmp(validation.Class.Name, 'double') + ...
    numel(validation.Size) + validation.Size(1).Length + ...
    isa(validation.Size(2), 'matlab.metadata.UnrestrictedDimension') + ...
    numel(validation.ValidationFunctions) + ...
    strcmp(func2str(validation_functions{1}), 'mustBeFinite') + ...
    valid_value + ~invalid_value + dynamic_property.NonCopyable + ...
    isempty(dynamic_property.Validation) + ...
    strcmp(metaclass("text").Name, 'string') + static_method.Static + ...
    (found_object.Tag == 4)
