classdef ReflectionDemo
    properties
        Value = 0
    end

    methods
        function obj = ReflectionDemo(value)
            arguments
                value (1,1) double = 1
            end
            obj.Value = value;
        end

        function output = scale(obj, factor)
            arguments
                obj ReflectionDemo
                factor (1,1) double
            end
            output = obj.Value * factor;
        end
    end

    methods (Static)
        function output = tag(value)
            output = value;
        end
    end
end

function_info = metafunction('inspectFunction');
selected_function_by_value = metafunction('inspectFunction', Arguments={2});
selected_function_by_type = metafunction('inspectFunction', ...
    ArgumentTypes={'double'});
signature = function_info.Signature;
first_input = signature.Inputs(1);
option_input = signature.Inputs(2);
constructor_info = metafunction('ReflectionDemo');
method_info = metafunction('ReflectionDemo/scale');
static_info = metafunction('ReflectionDemo.tag');
selected_info = metafunction('scale', ...
    ArgumentTypes={'ReflectionDemo','double'});

summary = isa(function_info, 'matlab.metadata.Function') + ...
    (selected_function_by_value == function_info) + ...
    (selected_function_by_type == function_info) + ...
    numel(signature.Inputs) + numel(signature.Outputs) + ...
    signature.HasInputValidation + signature.HasOutputValidation + ...
    first_input.Required + option_input.NameValue + ...
    strcmp(option_input.DefaultValue.Expression, 'a') + ...
    strcmp(first_input.Validation.Functions(1).Name, 'mustBePositive') + ...
    first_input.Validation.Size(1).Length + ...
    strcmp(constructor_info.Name, 'ReflectionDemo') + ...
    strcmp(static_info.Name, 'tag') + ...
    (selected_info == method_info) + ...
    isa(method_info.Signature, 'matlab.metadata.CallSignature');

function output = inspectFunction(a, options)
arguments (Input)
    a (1,1) double {mustBePositive}
    options.Scale (1,1) double = a
end
arguments (Output)
    output (1,1) double
end
output = a * options.Scale;
end
