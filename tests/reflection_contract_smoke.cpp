#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_metadata.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Compilation {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

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

Compilation compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.root != nullptr, "parser did not produce a root");
    require(parsed.diagnostics.empty(),
            "parser diagnostics: " +
                diagnosticsText(parsed.diagnostics));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    require(semantic.root != nullptr,
            "semantic analyzer produced no root");
    require(semantic.diagnostics.empty(),
            "semantic diagnostics: " +
                diagnosticsText(semantic.diagnostics));

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "bytecode diagnostics: " +
                diagnosticsText(bytecode.diagnostics));
    return {std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    const auto compiled = compile(source);
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
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
    if (!value) {
        throw std::runtime_error(
            "numeric variable is missing: " + std::string(name));
    }
    if (value->kind != mparser::RuntimeValueKind::Number) {
        throw std::runtime_error(
            "runtime variable is not numeric: " +
            std::string(name));
    }
    if (std::fabs(value->number - expected) >= 1e-9) {
        throw std::runtime_error(
            "numeric runtime value mismatch for " +
            std::string(name) + ": expected " +
            std::to_string(expected) + ", got " +
            std::to_string(value->number));
    }
}

void requireText(const mparser::BytecodeVmResult& result,
                 std::string_view name, std::string_view expected) {
    const auto* value = variable(result, name);
    require(value != nullptr, "text variable is missing");
    require(mparser::runtimeTextScalarUtf8(*value) == expected,
            "text runtime value mismatch");
}

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    return std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [text](const mparser::Diagnostic& diagnostic) {
            return diagnostic.message.find(text) != std::string::npos;
        });
}

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

mparser::BytecodeVmResult invoke(
    const mparser::CompiledModule& module, std::string entry,
    std::vector<mparser::RuntimeValue> arguments = {}) {
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = std::move(entry);
    options.arguments = std::move(arguments);
    return module.invoke(options);
}

void runSchemaSmoke() {
    const auto* validation =
        mparser::findRuntimeMetadataTypeDescriptor(
            "matlab.metadata.Validation");
    require(validation != nullptr,
            "PropertyValidation legacy alias is missing");
    require(validation->kind ==
                mparser::RuntimeMetadataKind::PropertyValidation,
            "PropertyValidation alias resolves to the wrong kind");
    require(validation->canonicalName ==
                "matlab.metadata.PropertyValidation",
            "PropertyValidation canonical name is wrong");
    require(validation->sealedClass,
            "PropertyValidation must be sealed");

    const auto properties =
        mparser::runtimeMetadataPropertyNames(
            "matlab.metadata.Property");
    require(std::find(properties.begin(), properties.end(),
                      "Validation") != properties.end(),
            "Property metadata does not expose Validation");
    const auto methods =
        mparser::runtimeMetadataMethodNames(
            "meta.Validation");
    require(std::find(methods.begin(), methods.end(),
                      "isValidValue") != methods.end() &&
                std::find(methods.begin(), methods.end(),
                          "validateValue") != methods.end(),
            "PropertyValidation methods are missing");

    const auto dynamic = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::DynamicProperty, "dynamic/1");
    require(mparser::runtimeMetadataIsa(
                dynamic, "matlab.metadata.Property"),
            "DynamicProperty does not inherit Property");
    require(mparser::runtimeMetadataIsa(
                dynamic, "matlab.metadata.MetaData"),
            "DynamicProperty does not inherit MetaData");
    require(mparser::runtimeMetadataIsa(dynamic, "handle"),
            "DynamicProperty does not inherit handle");

    const auto signature = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::CallSignature, "signature/1");
    require(!mparser::runtimeMetadataIsa(
                signature, "matlab.metadata.MetaData"),
            "CallSignature incorrectly inherits MetaData");
    require(!mparser::runtimeMetadataIsa(signature, "handle"),
            "CallSignature incorrectly inherits handle");

    const auto fixed = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::FixedDimension, "dimension/1");
    require(mparser::runtimeMetadataIsa(
                fixed, "matlab.metadata.ArrayDimension"),
            "FixedDimension does not inherit ArrayDimension");
    require(!mparser::runtimeMetadataIsa(
                fixed, "matlab.metadata.MetaData"),
            "FixedDimension incorrectly inherits MetaData");

    const auto& nameSpace =
        mparser::runtimeMetadataTypeDescriptor(
            mparser::RuntimeMetadataKind::Namespace);
    require(nameSpace.abstractClass && nameSpace.sealedClass &&
                nameSpace.hiddenClass &&
                nameSpace.restrictsSubclassing,
            "Namespace metadata class flags are incomplete");
}

constexpr std::string_view kReflectionClasses = R"(
classdef ReflectionBase < handle
    properties
        Sample(1,:) double {mustBeFinite, ...
            mustBeGreaterThan(Sample, 0)} = [1 2]
    end

    properties (GetAccess = private, SetAccess = private)
        Secret = 9
    end

    properties (Constant)
        Limit = 4
    end

    methods
        function value = publicValue(obj)
            value = obj.Sample(1);
        end
    end

    methods (Static)
        function value = staticValue()
            value = 12;
        end
    end

    methods (Access = private)
        function value = privateValue(obj)
            value = obj.Secret;
        end
    end
end

classdef ReflectionChild < ReflectionBase
    properties
        Child = 5
    end
end

classdef ReflectionBag < dynamicprops
    properties
        Tag = 0
    end
end
)";

void runReflectionRuntimeSmoke() {
    const std::string source =
        std::string(kReflectionClasses) + R"(
child_info = ?ReflectionChild;
base_info = ?ReflectionBase;
[projected_first, projected_second, projected_third] = ...
    child_info.PropertyList.Name;
projected_count = numel(child_info.PropertyList);

selected = findobj(child_info.PropertyList, 'Name', 'Sample', ...
    'GetAccess', 'public');
selected_name = selected.Name;
selected_owner = selected.DefiningClass.Name;
selected_is_metadata = isa(selected, 'matlab.metadata.MetaData');
has_validation_property = isprop(selected, 'Validation');
public_properties = findobj(child_info.PropertyList, ...
    'GetAccess', 'public');
public_property_count = numel(public_properties);
missing_property = findobj(child_info.PropertyList, ...
    'Name', 'Missing');
missing_property_is_empty = isempty(missing_property);

private_selected = findobj(base_info.PropertyList, 'Name', 'Secret');
private_access = private_selected.GetAccess;
static_selected = findobj(child_info.MethodList, 'Name', 'staticValue');
static_flag = static_selected.Static;
static_access = static_selected.Access;

validation = selected.Validation;
validation_class = class(validation);
validation_alias = isa(validation, 'matlab.metadata.Validation');
validation_old_alias = isa(validation, 'meta.Validation');
validation_is_handle = isa(validation, 'handle');
validation_is_metadata = isa(validation, 'matlab.metadata.MetaData');
has_probe = ismethod(validation, 'isValidValue');
has_validate = ismethod(validation, 'validateValue');
has_findobj = ismethod(validation, 'findobj');
validation_type = validation.Class.Name;
dimension_count = numel(validation.Size);
first_dimension = validation.Size(1).Length;
second_dimension_class = class(validation.Size(2));
validation_function_count = numel(validation.ValidationFunctions);
validation_functions = validation.ValidationFunctions;
first_validator = validation_functions{1};
second_validator = validation_functions{2};
first_validator_name = func2str(first_validator);
second_validator_name = func2str(second_validator);

valid_value = validation.isValidValue([2 3]);
coerced_shape = validation.isValidValue([2; 3]);
invalid_value = validation.isValidValue([2, -1]);
validation.validateValue([3 4]);
first_validator([3 4]);
second_validator([3 4]);

validate_rejected = false;
validate_identifier = '';
try
    validation.validateValue([3, -1]);
catch validation_error
    validate_rejected = true;
    validate_identifier = validation_error.identifier;
end

validator_rejected = false;
try
    second_validator([3, -1]);
catch validator_error
    validator_rejected = true;
end

signature = static_selected.Signature;
signature_is_exact = isa(signature, 'matlab.metadata.CallSignature');
signature_is_metadata = isa(signature, 'matlab.metadata.MetaData');
signature_is_handle = isa(signature, 'handle');

string_metaclass_name = metaclass("reflection").Name;

bag = ReflectionBag();
dynamic_property = addprop(bag, 'DynamicValue');
dynamic_noncopyable_default = dynamic_property.NonCopyable;
dynamic_validation_empty = isempty(dynamic_property.Validation);
dynamic_is_property = isa(dynamic_property, 'matlab.metadata.Property');
dynamic_is_metadata = isa(dynamic_property, 'matlab.metadata.MetaData');

first_bag = ReflectionBag();
first_bag.Tag = 1;
second_bag = ReflectionBag();
second_bag.Tag = 2;
bags = [first_bag second_bag];
found_bag = findobj(bags, 'Tag', 2);
found_bag_tag = found_bag.Tag;
method_found_bag = second_bag.findobj('Tag', 2);
method_found_bag_tag = method_found_bag.Tag;
all_bag_count = numel(findobj(bags));
missing_bag = findobj(bags, 'Tag', 9);
missing_bag_is_empty = isempty(missing_bag);
)";

    const auto result = run(source);
    require(result.diagnostics.empty(),
            "reflection runtime diagnostics: " +
                diagnosticsText(result.diagnostics));

    requireText(result, "projected_first", "Sample");
    requireText(result, "projected_second", "Limit");
    requireText(result, "projected_third", "Child");
    requireNumber(result, "projected_count", 3.0);
    requireText(result, "selected_name", "Sample");
    requireText(result, "selected_owner", "ReflectionBase");
    requireText(result, "private_access", "private");
    requireNumber(result, "static_flag", 1.0);
    requireText(result, "static_access", "public");
    requireNumber(result, "selected_is_metadata", 1.0);
    requireNumber(result, "has_validation_property", 1.0);
    requireNumber(result, "public_property_count", 3.0);
    requireNumber(result, "missing_property_is_empty", 1.0);

    requireText(result, "validation_class",
                "matlab.metadata.PropertyValidation");
    requireNumber(result, "validation_alias", 1.0);
    requireNumber(result, "validation_old_alias", 1.0);
    requireNumber(result, "validation_is_handle", 0.0);
    requireNumber(result, "validation_is_metadata", 0.0);
    requireNumber(result, "has_probe", 1.0);
    requireNumber(result, "has_validate", 1.0);
    requireNumber(result, "has_findobj", 0.0);
    requireText(result, "validation_type", "double");
    requireNumber(result, "dimension_count", 2.0);
    requireNumber(result, "first_dimension", 1.0);
    requireText(result, "second_dimension_class",
                "matlab.metadata.UnrestrictedDimension");
    requireNumber(result, "validation_function_count", 2.0);
    requireText(result, "first_validator_name", "mustBeFinite");
    requireText(result, "second_validator_name",
                "mustBeGreaterThan");
    requireNumber(result, "valid_value", 1.0);
    requireNumber(result, "coerced_shape", 1.0);
    requireNumber(result, "invalid_value", 0.0);
    requireNumber(result, "validate_rejected", 1.0);
    requireText(result, "validate_identifier",
                "MParser:RuntimeError");
    requireNumber(result, "validator_rejected", 1.0);

    requireNumber(result, "signature_is_exact", 1.0);
    requireNumber(result, "signature_is_metadata", 0.0);
    requireNumber(result, "signature_is_handle", 0.0);
    requireText(result, "string_metaclass_name", "string");

    requireNumber(result, "dynamic_noncopyable_default", 1.0);
    requireNumber(result, "dynamic_validation_empty", 1.0);
    requireNumber(result, "dynamic_is_property", 1.0);
    requireNumber(result, "dynamic_is_metadata", 1.0);
    requireNumber(result, "found_bag_tag", 2.0);
    requireNumber(result, "method_found_bag_tag", 2.0);
    requireNumber(result, "all_bag_count", 2.0);
    requireNumber(result, "missing_bag_is_empty", 1.0);
}

void runReflectionDiagnosticSmoke() {
    const std::string missingProperty =
        std::string(kReflectionClasses) + R"(
info = ?ReflectionChild;
value = info.PropertyList.UnsupportedField;
)";
    const auto missingPropertyResult = run(missingProperty);
    require(hasDiagnostic(
                missingPropertyResult,
                "metadata array property is not available"),
            "metadata array unsupported field diagnostic is missing");

    const std::string missingFilter =
        std::string(kReflectionClasses) + R"(
info = ?ReflectionChild;
value = findobj(info.PropertyList, 'UnsupportedField', 1);
)";
    const auto missingFilterResult = run(missingFilter);
    require(hasDiagnostic(
                missingFilterResult,
                "findobj metadata property is not available"),
            "findobj unsupported field diagnostic is missing");

    const std::string invalidValidation =
        std::string(kReflectionClasses) + R"(
info = ?ReflectionChild;
property = findobj(info.PropertyList, 'Name', 'Sample');
property.Validation.validateValue([1, -1]);
)";
    const auto invalidValidationResult = run(invalidValidation);
    require(hasDiagnostic(
                invalidValidationResult,
                "property validation failed for ReflectionBase.Sample"),
            "validateValue did not preserve property validation diagnostics");
}

void runValidationHandleIdentitySmoke() {
    auto producer = mparser::CompiledModule::compile(R"(
classdef ModuleValidation
    properties
        Value(1,1) double {mustBePositive} = 1
    end
end

function validator = makeValidator()
    info = ?ModuleValidation;
    property = findobj(info.PropertyList, 'Name', 'Value');
    functions = property.Validation.ValidationFunctions;
    validator = functions{1};
end

function applyValidator(validator, value)
    validator(value);
end
)");
    require(producer.valid(),
            "validation handle producer module did not compile");
    const auto created = invoke(producer, "makeValidator");
    require(created.diagnostics.empty() &&
                created.outputs.size() == 1 &&
                created.outputs.front().kind ==
                    mparser::RuntimeValueKind::FunctionHandle,
            "property validator handle was not returned");

    const auto sameModule = invoke(
        producer, "applyValidator",
        {created.outputs.front(), number(2)});
    require(sameModule.diagnostics.empty(),
            "same-module property validator invocation failed");
    const auto rejected = invoke(
        producer, "applyValidator",
        {created.outputs.front(), number(-1)});
    require(hasDiagnostic(rejected, "value must be positive"),
            "same-module property validator did not reject invalid data");

    auto consumer = mparser::CompiledModule::compile(R"(
function applyExternal(validator, value)
    validator(value);
end
)");
    require(consumer.valid(),
            "validation handle consumer module did not compile");
    const auto foreign = invoke(
        consumer, "applyExternal",
        {created.outputs.front(), number(2)});
    require(hasDiagnostic(foreign, "different compiled module"),
            "foreign module accepted a module-bound property validator");
}

} // namespace

int main() {
    try {
        runSchemaSmoke();
        runReflectionRuntimeSmoke();
        runReflectionDiagnosticSmoke();
        runValidationHandleIdentitySmoke();
        std::cout << "reflection contract smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
