#include "mparser/embedding/compiled_module.h"
#include "mparser/runtime/core/runtime_metadata.h"
#include "mparser/runtime/core/runtime_text.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string diagnosticsText(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics) {
        if (!result.empty()) {
            result += " | ";
        }
        result += std::to_string(diagnostic.span.begin.line);
        result += ":";
        result += std::to_string(diagnostic.span.begin.column);
        result += " ";
        result += diagnostic.message;
    }
    return result;
}

bool hasDiagnostic(const std::vector<mparser::Diagnostic>& diagnostics,
                   std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::RuntimeValue* variable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return &candidate.value;
        }
    }
    return nullptr;
}

void requireNumber(const mparser::BytecodeVmResult& result,
                   std::string_view name, double expected) {
    const auto* value = variable(result, name);
    require(value != nullptr, "expected numeric variable");
    require(value->kind == mparser::RuntimeValueKind::Number,
            "runtime variable is not numeric");
    require(std::fabs(value->number - expected) < 1e-9,
            "numeric runtime value mismatch");
}

void requireString(const mparser::BytecodeVmResult& result,
                   std::string_view name, std::string_view expected) {
    const auto* value = variable(result, name);
    require(value != nullptr, "expected string variable");
    require(mparser::runtimeTextScalarUtf8(*value) == expected,
            "text runtime value mismatch");
}

std::vector<mparser::SourceUnit> metadataSources() {
    return {
        {
            "app/reflection_main.m",
            R"(
classdef ReflectOptions
    properties
        Width (1,1) double
    end
end

classdef ReflectThing
    properties
        Value = 0
    end
    methods
        function obj = ReflectThing(value)
            arguments
                value (1,1) double = 1
            end
            obj.Value = value;
        end
        function output = scale(obj, factor)
            arguments
                obj ReflectThing
                factor (1,1) double
            end
            output = obj.Value * factor;
        end
    end
    methods (Static)
        function output = tag(value)
            arguments
                value (1,1) double
            end
            output = value;
        end
    end
end

function_info = metafunction('pkg.inspectable');
selected_function_by_type = metafunction('pkg.inspectable', ...
    ArgumentTypes={'double','double'});
selected_function_by_value = metafunction('pkg.inspectable', ...
    Arguments={2,[1,2]});
selected_function_type_matches = selected_function_by_type == function_info;
selected_function_value_matches = selected_function_by_value == function_info;
function_name = function_info.Name;
function_path = function_info.FullPath;
function_namespace = function_info.NamespaceName;
function_kind = isa(function_info, 'matlab.metadata.Function');
signature = function_info.Signature;
signature_kind = isa(signature, 'matlab.metadata.CallSignature');
input_count = numel(signature.Inputs);
output_count = numel(signature.Outputs);
has_input_block = signature.HasInputValidation;
has_output_block = signature.HasOutputValidation;
first_input = signature.Inputs(1);
first_name = first_input.Identifier.Name;
first_required = first_input.Required;
first_class = first_input.Validation.Class.Name;
first_validator = first_input.Validation.Functions(1).Name;
shape_size = signature.Inputs(2).Validation.Size;
shape_first_length = shape_size(1).Length;
shape_second_unrestricted = isa(shape_size(2), ...
    'matlab.metadata.UnrestrictedDimension');
option_input = signature.Inputs(3);
option_name = option_input.Identifier.Name;
option_group = option_input.Identifier.GroupName;
option_is_name_value = option_input.NameValue;
option_default = option_input.DefaultValue.Expression;
option_default_reference = ...
    option_input.DefaultValue.ReferencedArguments(1).Name;
option_validator_reference = ...
    option_input.Validation.Functions(1).ReferencedArguments(1).Name;
output_name = signature.Outputs(1).Identifier.Name;

local_info = metafunction('pkg.inspectable>helper');
local_name = local_info.Name;
local_path = local_info.FullPath;
local_has_input_block = local_info.Signature.HasInputValidation;
local_validation_empty = isempty(local_info.Signature.Inputs(1).Validation);
local_default_empty = isempty(local_info.Signature.Inputs(1).DefaultValue);

repeating_info = metafunction('repeatable');
repeating_signature = repeating_info.Signature;
repeating_input_count = numel(repeating_signature.Inputs);
repeating_output_count = numel(repeating_signature.Outputs);
fixed_input_repeating = repeating_signature.Inputs(1).Repeating;
first_repeating_input = repeating_signature.Inputs(2).Repeating;
second_repeating_input = repeating_signature.Inputs(3).Repeating;
fixed_output_repeating = repeating_signature.Outputs(1).Repeating;
repeating_output = repeating_signature.Outputs(2).Repeating;
repeating_output_required = repeating_signature.Outputs(2).Required;

constructor_info = metafunction('ReflectThing');
constructor_name = constructor_info.Name;
method_info = metafunction('ReflectThing/scale');
method_name = method_info.Name;
method_path = method_info.FullPath;
method_signature_kind = isa(method_info.Signature, ...
    'matlab.metadata.CallSignature');
static_info = metafunction('ReflectThing.tag');
static_name = static_info.Name;

selected_by_type = metafunction('scale', ...
    ArgumentTypes={'ReflectThing','double'});
selected_type_matches = selected_by_type == method_info;
object = ReflectThing(2);
selected_by_value = metafunction('scale', Arguments={object,3});
selected_value_matches = selected_by_value == method_info;

class_source = metafunction('fromClass');
source_class_name = class_source.Signature.Inputs(1).SourceClass.Name;

marker_info = ?pkg.Marker;
namespace_info = marker_info.Namespace;
namespace_function_count = numel(namespace_info.FunctionList);
namespace_function_name = namespace_info.FunctionList(1).Name;
)",
        },
        {
            "lib/fromClass.m",
            R"(
function output = fromClass(options)
arguments
    options.?ReflectOptions
end
output = 0;
end
)",
        },
        {
            "lib/+pkg/inspectable.m",
            R"(
function result = inspectable(a, shape, options)
arguments (Input)
    a (1,1) double {mustBePositive}
    shape (1,:) double
    options.Scale (1,1) double {mustBeGreaterThan(a)} = a
end
arguments (Output)
    result (1,1) double
end
result = a + numel(shape) + options.Scale;
end

function result = helper(value)
result = value;
end
)",
            "pkg",
        },
        {
            "lib/+pkg/Marker.m",
            R"(
classdef Marker
end
)",
            "pkg",
        },
        {
            "lib/repeatable.m",
            R"(
function [head, tail] = repeatable(seed, x, y)
arguments
    seed (1,1) double
end
arguments (Repeating)
    x (1,1) double
    y (1,1) double
end
arguments (Output)
    head (1,1) double
end
arguments (Output,Repeating)
    tail (1,1) double
end
head = seed;
tail{1} = seed;
end
)",
        },
    };
}

void runFunctionMetadataSmoke() {
    const auto module =
        mparser::CompiledModule::compile(metadataSources());
    require(module.valid(),
            "function metadata module failed to compile: " +
                diagnosticsText(module.diagnostics()));

    const auto result = module.invoke();
    require(result.diagnostics.empty(),
            "function metadata runtime diagnostics: " +
                diagnosticsText(result.diagnostics));

    requireString(result, "function_name", "inspectable");
    requireString(result, "function_path", "lib/+pkg/inspectable.m");
    requireString(result, "function_namespace", "pkg");
    requireString(result, "first_name", "a");
    requireString(result, "first_class", "double");
    requireString(result, "first_validator", "mustBePositive");
    requireString(result, "option_name", "Scale");
    requireString(result, "option_group", "options");
    requireString(result, "option_default", "a");
    requireString(result, "option_default_reference", "a");
    requireString(result, "option_validator_reference", "a");
    requireString(result, "output_name", "result");
    requireString(result, "local_name", "helper");
    requireString(result, "local_path", "lib/+pkg/inspectable.m");
    requireString(result, "constructor_name", "ReflectThing");
    requireString(result, "method_name", "scale");
    requireString(result, "method_path", "app/reflection_main.m");
    requireString(result, "static_name", "tag");
    requireString(result, "source_class_name", "ReflectOptions");
    requireString(result, "namespace_function_name", "inspectable");

    requireNumber(result, "function_kind", 1);
    requireNumber(result, "selected_function_type_matches", 1);
    requireNumber(result, "selected_function_value_matches", 1);
    requireNumber(result, "signature_kind", 1);
    requireNumber(result, "input_count", 3);
    requireNumber(result, "output_count", 1);
    requireNumber(result, "has_input_block", 1);
    requireNumber(result, "has_output_block", 1);
    requireNumber(result, "first_required", 1);
    requireNumber(result, "shape_first_length", 1);
    requireNumber(result, "shape_second_unrestricted", 1);
    requireNumber(result, "option_is_name_value", 1);
    requireNumber(result, "method_signature_kind", 1);
    requireNumber(result, "selected_type_matches", 1);
    requireNumber(result, "selected_value_matches", 1);
    requireNumber(result, "namespace_function_count", 1);
    requireNumber(result, "local_has_input_block", 0);
    requireNumber(result, "local_validation_empty", 1);
    requireNumber(result, "local_default_empty", 1);
    requireNumber(result, "repeating_input_count", 3);
    requireNumber(result, "repeating_output_count", 2);
    requireNumber(result, "fixed_input_repeating", 0);
    requireNumber(result, "first_repeating_input", 1);
    requireNumber(result, "second_repeating_input", 1);
    requireNumber(result, "fixed_output_repeating", 0);
    requireNumber(result, "repeating_output", 1);
    requireNumber(result, "repeating_output_required", 0);

    const auto* functionInfo = variable(result, "function_info");
    require(functionInfo != nullptr, "function metadata value missing");
    require(mparser::runtimeMetadataKind(*functionInfo) ==
                mparser::RuntimeMetadataKind::Function,
            "function metadata has the wrong runtime kind");
    require(mparser::runtimeValueToString(*functionInfo) ==
                "<matlab.metadata.Function pkg.inspectable>",
            "function metadata display is incomplete");
}

void runMetafunctionDiagnosticSmoke() {
    auto sources = metadataSources();
    sources.front().content = R"(
classdef SelectorThing
    methods
        function output = scale(obj, value)
            output = value;
        end
    end
end
bad = metafunction('scale', ArgumentTypes={'double','double'});
)";
    sources.resize(1);
    const auto module =
        mparser::CompiledModule::compile(std::move(sources));
    require(module.valid(), "selector diagnostic module must compile");
    const auto result = module.invoke();
    require(hasDiagnostic(
                result.diagnostics,
                "function or method metadata is not available"),
            "selector mismatch did not produce a diagnostic");
}

} // namespace

int main() {
    try {
        runFunctionMetadataSmoke();
        runMetafunctionDiagnosticSmoke();
        std::cout << "function metadata smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
