#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::ApiError;
using mparser::sdk::Backend;
using mparser::sdk::CancellationToken;
using mparser::sdk::DiagnosticPhase;
using mparser::sdk::Invocation;
using mparser::sdk::InputMode;
using mparser::sdk::InputRequest;
using mparser::sdk::InputResult;
using mparser::sdk::InputStatus;
using mparser::sdk::InvocationStatus;
using mparser::sdk::Module;
using mparser::sdk::NamedValue;
using mparser::sdk::NumericClass;
using mparser::sdk::OutputKind;
using mparser::sdk::Result;
using mparser::sdk::SourceKind;
using mparser::sdk::SourceLoadOptions;
using mparser::sdk::SourceUnit;
using mparser::sdk::StopReason;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

template <typename T>
concept TemporaryNumericDataReadable = requires {
    T{}.template numericData<double>();
};

template <typename T>
concept TemporaryImaginaryDataReadable = requires {
    T{}.template numericImaginaryData<double>();
};

template <typename T>
concept TemporaryCharacterDataReadable = requires {
    T{}.characterData();
};

static_assert(!TemporaryNumericDataReadable<Value>);
static_assert(!TemporaryImaginaryDataReadable<Value>);
static_assert(!TemporaryCharacterDataReadable<Value>);

constexpr double kTolerance = 1e-9;

const std::string kModuleSource = R"(function [total, last] = accumulate(limit)
total = 0;
for i = 1:limit
    total = total + i;
end
last = i;
end

function out = nextCounter(step)
persistent count
if isempty(count)
    count = 0;
end
count = count + step;
out = count;
end

function out = emitWarning()
warning("Embed:Notice", "notice %d", 3);
out = 7;
end

function out = failNow()
error("Embed:Failure", "failed %d", 9);
out = 0;
end

function out = spin()
out = 0;
while 1
    out = out + 1;
end
end

function out = identity(value)
out = value;
end
)";

double scalar(const Value& value) {
    assert(value.kind() == ValueKind::Numeric);
    const auto data = value.numericData();
    assert(data.size() == 1);
    return data[0];
}

const NamedValue* findVariable(
    const std::vector<NamedValue>& variables,
    const std::string& name) {
    const auto found = std::find_if(
        variables.begin(), variables.end(),
        [&](const NamedValue& variable) {
            return variable.name == name;
        });
    return found == variables.end() ? nullptr : &*found;
}

bool hasDiagnostic(
    const std::vector<mparser::sdk::Diagnostic>& diagnostics,
    const std::string& identifier) {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) {
            return diagnostic.identifier == identifier;
        });
}

void runValueSmoke() {
    const std::array<std::size_t, 2> matrixShape{2, 2};
    const std::array<double, 4> matrixData{1, 3, 2, 4};
    const auto matrix = Value::numericArray(
        NumericClass::Double, matrixShape,
        std::span<const double>(matrixData));
    auto matrixCopy = matrix;
    auto matrixMoved = std::move(matrixCopy);
    assert(!matrixCopy.hasValue());
    assert(matrix.kind() == ValueKind::Numeric);
    assert(matrix.numericClass() == NumericClass::Double);
    assert(matrix.dimensions() ==
           std::vector<std::size_t>({2, 2}));
    assert(matrix.elementCount() == 4);
    assert(std::equal(
        matrixMoved.numericData().begin(), matrixMoved.numericData().end(),
        matrixData.begin()));

    const std::array<std::uint8_t, 4> logicalData{0, 1, 1, 0};
    const auto logical = Value::numericArray(
        NumericClass::Logical, matrixShape,
        std::span<const std::uint8_t>(logicalData));
    assert(logical.numericClass() == NumericClass::Logical);

    const auto single = Value::numericScalar(
        NumericClass::Single, float{0.1F});
    assert(single.numericClass() == NumericClass::Single);
    assert(single.numericData<float>().front() == 0.1F);

    const std::array<float, 4> complexReal{
        1.0F, -2.0F, 3.5F, 0.0F};
    const std::array<float, 4> complexImaginary{
        -1.0F, 0.5F, 8.0F, -3.0F};
    const auto complexSingle = Value::numericArray(
        NumericClass::Single, matrixShape,
        std::span<const float>(complexReal),
        std::optional<std::span<const float>>{
            std::span<const float>(complexImaginary)});
    assert(complexSingle.numericClass() == NumericClass::Single);
    assert(complexSingle.isComplex());
    assert(std::equal(
        complexSingle.numericData<float>().begin(),
        complexSingle.numericData<float>().end(),
        complexReal.begin()));
    assert(std::equal(
        complexSingle.numericImaginaryData<float>().begin(),
        complexSingle.numericImaginaryData<float>().end(),
        complexImaginary.begin()));

    bool rejectedComplexLogical = false;
    try {
        static_cast<void>(Value::numericArray(
            NumericClass::Logical, matrixShape,
            std::span<const std::uint8_t>(logicalData),
            std::optional<std::span<const std::uint8_t>>{
                std::span<const std::uint8_t>(logicalData)}));
    } catch (const ApiError& error) {
        rejectedComplexLogical =
            error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    assert(rejectedComplexLogical);

    const std::array<std::int8_t, 4> int8Data{-128, -1, 2, 127};
    const auto int8Array = Value::numericArray(
        NumericClass::Int8, matrixShape,
        std::span<const std::int8_t>(int8Data));
    assert(int8Array.numericClass() == NumericClass::Int8);
    assert(std::equal(int8Array.numericData<std::int8_t>().begin(),
                      int8Array.numericData<std::int8_t>().end(),
                      int8Data.begin()));

    const auto int64Scalar = Value::numericScalar(
        NumericClass::Int64,
        std::numeric_limits<std::int64_t>::min());
    assert(int64Scalar.numericClass() == NumericClass::Int64);
    assert(int64Scalar.numericData<std::int64_t>().front() ==
           std::numeric_limits<std::int64_t>::min());

    const std::array<std::uint64_t, 4> uint64Data{
        UINT64_C(9007199254740993),
        std::numeric_limits<std::uint64_t>::max(), 7, 11};
    const auto uint64Array = Value::numericArray(
        NumericClass::UInt64, matrixShape,
        std::span<const std::uint64_t>(uint64Data));
    assert(uint64Array.numericClass() == NumericClass::UInt64);
    assert(std::equal(uint64Array.numericData<std::uint64_t>().begin(),
                      uint64Array.numericData<std::uint64_t>().end(),
                      uint64Data.begin()));

    const std::array<std::size_t, 2> rowShape{1, 2};
    const std::array<std::uint16_t, 2> characters{'A', 'B'};
    const auto character = Value::characterArray(
        rowShape, characters);
    assert(character.kind() == ValueKind::Character);
    assert(std::equal(
        character.characterData().begin(),
        character.characterData().end(), characters.begin()));

    const std::array<std::optional<std::u16string>, 2> strings{
        std::u16string(u"alpha"), std::nullopt};
    const auto text = Value::stringArray(rowShape, strings);
    assert(text.kind() == ValueKind::String);
    assert(text.stringElement(0) == std::u16string(u"alpha"));
    assert(!text.stringElement(1));

    const std::array<Value, 2> cells{
        Value::scalar(7), text};
    const auto cell = Value::cell(rowShape, cells);
    assert(cell.kind() == ValueKind::Cell);
    assert(std::abs(scalar(cell.cellElement(0)) - 7.0) <
           kTolerance);
    assert(cell.cellElement(1).stringElement(0) ==
           std::u16string(u"alpha"));

    const std::array<std::size_t, 2> scalarShape{1, 1};
    const std::array<std::optional<std::u16string>, 1> label{
        std::u16string(u"record")};
    const std::array<NamedValue, 2> fields{
        NamedValue{"value", Value::scalar(9)},
        NamedValue{
            "label", Value::stringArray(scalarShape, label)}};
    const auto structure = Value::structure(fields);
    assert(structure.kind() == ValueKind::Structure);
    assert(structure.structFieldNames() ==
           std::vector<std::string>({"value", "label"}));
    assert(std::abs(scalar(structure.structField(0, 0)) - 9.0) <
           kTolerance);
    assert(structure.structField(0, 1).stringElement(0) ==
           std::u16string(u"record"));

    const auto missing = Value::missing();
    assert(missing.kind() == ValueKind::Missing);
    assert(missing.dimensions() ==
           std::vector<std::size_t>({1, 1}));
    assert(missing.elementCount() == 1);
    const std::array<std::size_t, 3> missingShape{2, 3, 4};
    const auto missingArray = Value::missingArray(missingShape);
    assert(missingArray.kind() == ValueKind::Missing);
    assert(missingArray.dimensions() ==
           std::vector<std::size_t>({2, 3, 4}));
    assert(missingArray.elementCount() == 24);
    bool rejectedMissingShape = false;
    try {
        const std::array<std::size_t, 1> invalidShape{1};
        (void)Value::missingArray(invalidShape);
    } catch (const ApiError& error) {
        rejectedMissingShape =
            error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    assert(rejectedMissingShape);

    bool rejectedEmptyHandle = false;
    try {
        const Value empty;
        static_cast<void>(empty.numericData());
    } catch (const ApiError& error) {
        rejectedEmptyHandle =
            error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    assert(rejectedEmptyHandle);
}

void runHostOutputSmoke() {
    const auto module = Module::compile(
        R"(formatted = sprintf("value=%d", 42);
disp(formatted)
written = fprintf("pi=%.1f\n", 3.14);
40 + 2
41 + 2;
)",
        "host_output_cpp.m");
    assert(module.isValid());
    const auto metadata = module.sourceMetadata();
    assert(metadata.size() == 1);
    assert(metadata.front().name == "host_output_cpp.m");
    assert(metadata.front().kind == SourceKind::Script);
    assert(metadata.front().hasTopLevelStatements);
    assert(!metadata.front().pureFunctionFile);

    std::vector<mparser::sdk::OutputEvent> observed;
    Invocation invocation;
    invocation.outputSink = [&observed](const auto& event) {
        observed.push_back(event);
        return true;
    };
    const auto result = module.execute(invocation);
    assert(result.succeeded());
    const auto events = result.outputEvents();
    assert(events.size() == 2 && observed.size() == events.size());
    assert(events[0].kind == OutputKind::Display);
    assert(events[0].text == "value=42\n\n");
    assert(events[0].sequence == 0 && observed[0].sequence == 0);
    assert(events[0].source &&
           events[0].source->sourceName == "host_output_cpp.m");
    assert(events[1].kind == OutputKind::StandardOutput);
    assert(events[1].text == "pi=3.1\n");
    assert(events[1].sequence == 1 && observed[1].sequence == 1);

    const auto expressions = result.topLevelExpressions();
    assert(expressions.size() == 2);
    assert(std::abs(scalar(expressions[0].value) - 42.0) < kTolerance);
    assert(!expressions[0].outputSuppressed && expressions[0].source);
    assert(expressions[0].sequence == 2);
    assert(std::abs(scalar(expressions[1].value) - 43.0) < kTolerance);
    assert(expressions[1].outputSuppressed && expressions[1].source);
    assert(expressions[1].sequence == 3);
    const auto variables = result.variables();
    const auto* answer = findVariable(variables, "ans");
    assert(answer && std::abs(scalar(answer->value) - 43.0) < kTolerance);

    Invocation rejected;
    rejected.outputSink = [](const auto&) { return false; };
    const auto rejectedResult = module.execute(rejected);
    assert(!rejectedResult.succeeded());
    assert(hasDiagnostic(
        rejectedResult.diagnostics(), "MParser:OutputSinkRejected"));

    Invocation throwing;
    throwing.outputSink = [](const auto&) -> bool {
        throw std::runtime_error("host sink failed");
    };
    bool propagated = false;
    try {
        (void)module.execute(throwing);
    } catch (const std::runtime_error& error) {
        propagated = std::string_view(error.what()) == "host sink failed";
    }
    assert(propagated);
}

void runMetadataExportSmoke() {
    const auto module = Module::compile(
        R"(classdef ExportMetadata < handle
end

class_info = ?ExportMetadata;
superclass_info = class_info.SuperclassList;
)",
        "metadata_export_cpp.m");
    assert(module.isValid());

    const auto result = module.execute();
    assert(result.succeeded());
    const auto variables = result.variables();
    const auto* classInfo = findVariable(variables, "class_info");
    const auto* superclassInfo =
        findVariable(variables, "superclass_info");
    assert(classInfo && superclassInfo);

    assert(classInfo->value.kind() == ValueKind::Object);
    assert(classInfo->value.className() == "matlab.metadata.Class");
    assert(classInfo->value.dimensions() ==
           std::vector<std::size_t>({1, 1}));
    assert(!classInfo->value.isModuleBound());

    assert(superclassInfo->value.kind() == ValueKind::Object);
    assert(superclassInfo->value.className() ==
           "matlab.metadata.Class");
    assert(superclassInfo->value.dimensions() ==
           std::vector<std::size_t>({1, 1}));
    assert(!superclassInfo->value.isModuleBound());
}

void runInputSourceSmoke() {
    const auto module = Module::compile(
        "answer=length(input('prompt\\npath\\\\end','s'));", "input_sdk.m");
    for (const auto backend : {Backend::Bytecode, Backend::Automatic,
                               Backend::Portable, Backend::Native}) {
        Invocation request;
        request.backend = backend;
        size_t calls = 0;
        request.inputSource = [&](const InputRequest& input) {
            if (input.mode != InputMode::Text || input.prompt != "prompt\npath\\end") {
                throw std::runtime_error("SDK input request mismatch");
            }
            if (++calls == 1) {
                return InputResult{InputStatus::Pending, {}, {}};
            }
            return InputResult{InputStatus::Ready, "  hi  ", {}};
        };
        const auto result = module.execute(request);
        if (!result.succeeded()) {
            for (const auto& diagnostic : result.diagnostics()) {
                std::cerr << diagnostic.message << '\n';
            }
            throw std::runtime_error("SDK input execution failed");
        }
        const auto values = result.variables();
        const auto* answer = findVariable(values, "answer");
        if (!answer || scalar(answer->value) != 6 || calls != 2) {
            throw std::runtime_error("SDK input result mismatch");
        }
    }
    Invocation request;
    request.inputSource = [](const InputRequest&) -> InputResult {
        throw std::runtime_error("input host exception");
    };
    bool propagated = false;
    try {
        (void)module.execute(request);
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "input host exception";
    }
    if (!propagated) {
        throw std::runtime_error("SDK input exception was not propagated");
    }
}

void runDynamicPropertyExportSmoke() {
    const auto module = Module::compile(R"(
classdef ExportDynamic < dynamicprops
end
obj=ExportDynamic();
p=addprop(obj,'Value');
p.GetMethod=@(owner) 42;
bundle={p,struct('descriptor',p)};
summary=7;
function out=inspect(desc)
desc.Hidden=true;
out=double(desc.Hidden)+10*isvalid(desc);
end
function out=remove(desc)
delete(desc);
out=double(isvalid(desc));
end
function out=valid(desc)
out=double(isvalid(desc));
end
)", "dynamic_export_cpp.m");
    assert(module.isValid());
    for (const auto backend : {Backend::Bytecode, Backend::Automatic,
                               Backend::Portable, Backend::Native}) {
        Invocation request;
        request.backend=backend;
        const auto result=module.execute(request);
        assert(result.succeeded());
        const auto values=result.variables();
        const auto* descriptor=findVariable(values,"p");
        const auto* summary=findVariable(values,"summary");
        const auto* bundle=findVariable(values,"bundle");
        assert(descriptor && summary && bundle);
        assert(scalar(summary->value)==7);
        assert(descriptor->value.className()=="matlab.metadata.DynamicProperty");
        assert(descriptor->value.isModuleBound());
        const auto nested=bundle->value.cellElement(1).structField(0,0);
        assert(nested.isModuleBound());
        Invocation inspect;
        inspect.entryFunction="inspect";
        inspect.arguments={nested};
        inspect.requestedOutputCount=1;
        const auto inspected=module.execute(inspect);
        if (!inspected.succeeded()) {
            for (const auto& diagnostic : inspected.diagnostics()) {
                std::cerr << diagnostic.message << '\n';
            }
            throw std::runtime_error("exported dynamic property invocation failed");
        }
        if (scalar(inspected.output(0)) != 11) {
            throw std::runtime_error("exported dynamic property lost validity");
        }
        inspect.entryFunction = "remove";
        const auto removed = module.execute(inspect);
        if (!removed.succeeded() || scalar(removed.output(0)) != 0) {
            throw std::runtime_error("exported dynamic property deletion failed");
        }
    }
    const auto orphan = [&]() {
        const auto result = module.execute();
        const auto values = result.variables();
        return findVariable(values, "p")->value;
    }();
    Invocation validity;
    validity.entryFunction = "valid";
    validity.arguments = {orphan};
    validity.requestedOutputCount = 1;
    const auto expired = module.execute(validity);
    if (!expired.succeeded() || scalar(expired.output(0)) != 0) {
        throw std::runtime_error("dynamic property retained its expired owner");
    }
}

Result invoke(
    const Module& module, std::string entry,
    std::vector<Value> arguments = {},
    std::optional<std::size_t> outputCount = 1) {
    Invocation request;
    request.entryFunction = std::move(entry);
    request.arguments = std::move(arguments);
    request.requestedOutputCount = outputCount;
    return module.execute(request);
}

void runInvalidAssignmentCompileSmoke() {
    using mparser::sdk::VariableNameStatus;
    using mparser::sdk::validateVariableName;
    assert(mparser::sdk::variableNameMaxLength() == 63);
    assert(validateVariableName("valid_name") == VariableNameStatus::Valid);
    assert(validateVariableName("arguments") == VariableNameStatus::Valid);
    assert(validateVariableName("end") == VariableNameStatus::ReservedKeyword);
    assert(validateVariableName({}) == VariableNameStatus::Empty);
    assert(validateVariableName("_x") == VariableNameStatus::InvalidStart);
    assert(validateVariableName(std::string_view("a\0b", 3)) ==
           VariableNameStatus::InvalidCharacter);
    assert(validateVariableName(std::string(63, 'a')) == VariableNameStatus::Valid);
    assert(validateVariableName(std::string(64, 'a')) == VariableNameStatus::TooLong);
    mparser_variable_name_status reason = 99;
    assert(mparser_validate_variable_name({nullptr, 1}, &reason) ==
           MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(reason == 99);
    assert(mparser_validate_variable_name({"x", 1}, nullptr) ==
           MPARSER_API_STATUS_INVALID_ARGUMENT);
    for (const auto source : {"a=1;\nb=;\n", "a=1;\nend=0;\n",
                              "a=1;\n=1;\n", "a=1;\n1=2;\n"}) {
        const auto module = Module::compile(source, "invalid_assignment.m");
        assert(!module.isValid());
        const auto diagnostics = module.diagnostics();
        assert(!diagnostics.empty());
        assert(std::any_of(diagnostics.begin(), diagnostics.end(),
                          [](const auto& diagnostic) {
            return diagnostic.phase == DiagnosticPhase::Compilation &&
                   diagnostic.source &&
                   diagnostic.source->sourceName == "invalid_assignment.m" &&
                   diagnostic.source->begin.line == 2;
        }));
    }
    const auto valid = Module::compile(
        "a=[1 2]; a(end)=3; s.end=1;\n", "valid_assignment.m");
    assert(valid.isValid());
}

void runIdentifierBuiltinSmoke() {
    const auto module = Module::compile(R"(
ok=isvarname('valid_name');
reserved=isvarname('end');
contextual=isvarname('arguments');
keyword=iskeyword('end');
notkeyword=iskeyword('properties');
limit=namelengthmax();
keywords=iskeyword();
count=numel(keywords);
numeric=isvarname(1);
cellinput=iskeyword({'end'});
matrix=isvarname(['ab';'cd']);
stringarray=isvarname(["a","b"]);
arguments=[1 2]; arguments(2)=3;
arguments;
import=4; properties=5; methods=6; events=7; enumeration=8;
contextsum=arguments(2)+import+properties+methods+events+enumeration;
functionsum=contextualFunction(4);
function out=contextualFunction(arguments)
import=3;
arguments;
out=arguments+import;
end
)", "identifier_builtins.m");
    assert(module.isValid());
    for (const auto backend : {Backend::Bytecode, Backend::Portable,
                               Backend::Native, Backend::Automatic}) {
        Invocation request;
        request.backend = backend;
        const auto result = module.execute(request);
        assert(result.succeeded());
        const auto values = result.variables();
        for (const auto name : {"ok", "contextual", "keyword"}) {
            const auto* value = findVariable(values, name);
            assert(value && value->value.numericClass() == NumericClass::Logical);
            assert(value->value.numericData<std::uint8_t>().size() == 1);
            assert(value->value.numericData<std::uint8_t>().front() == 1);
        }
        for (const auto name : {"reserved", "notkeyword", "numeric", "cellinput",
                                "matrix", "stringarray"}) {
            const auto* value = findVariable(values, name);
            assert(value && value->value.numericClass() == NumericClass::Logical);
            assert(value->value.numericData<std::uint8_t>().size() == 1);
            assert(value->value.numericData<std::uint8_t>().front() == 0);
        }
        assert(scalar(findVariable(values, "limit")->value) == 63);
        assert(scalar(findVariable(values, "count")->value) == 20);
        assert(scalar(findVariable(values, "contextsum")->value) == 33);
        assert(scalar(findVariable(values, "functionsum")->value) == 7);
        const auto* keywords = findVariable(values, "keywords");
        assert(keywords && keywords->value.dimensions() ==
               std::vector<std::size_t>({20, 1}));
    }
    for (const auto source : {"x=isvarname();", "x=isvarname('a','b');",
                              "x=iskeyword('a','b');", "x=namelengthmax(1);"}) {
        const auto invalid = Module::compile(source, "identifier_arity.m");
        assert(invalid.isValid());
        assert(!invalid.execute().succeeded());
    }
}

void runFunctionSyntaxMethodSmoke() {
    const std::vector<mparser::sdk::SourceGraphUnit> sources{
        {"DispatchOwner.m",
         "classdef DispatchOwner < handle\nproperties\nValue=7\nend\nmethods\n"
         "y=read(obj)\ny=combine(x,obj)\n[a,b]=parts(obj)\n"
         "function touch(obj)\nobj.Value=obj.Value+1;\nend\n"
         "function y=length(obj)\ny=17;\nend\n"
         "function y=pick(varargin)\ny=1;\nend\n"
         "function y=marker(x,obj)\ny=x+23;\nend\n"
         "function y=inside(obj)\ny=secret(obj);\nend\nend\n"
         "methods (Access=private)\nfunction y=secret(obj)\ny=31;\nend\nend\nend",{}},
        {"read.m","function y=read(obj)\ny=obj.Value;\nend","DispatchOwner"},
        {"combine.m","function y=combine(x,obj)\ny=x+obj.Value;\nend","DispatchOwner"},
        {"parts.m","function [a,b]=parts(obj)\na=obj.Value; b=a+1;\nend","DispatchOwner"},
        {"DispatchChild.m","classdef DispatchChild < DispatchOwner\nend",{}},
        {"DispatchPeer.m","classdef DispatchPeer\nmethods\n"
         "function y=pick(varargin)\ny=2;\nend\nend\nend",{}},
        {"main.m","obj=DispatchOwner(); direct=read(obj); second=combine(4,obj); "
         "[a,b]=parts(obj); touch(obj); changed=read(obj); overridden=length(obj);",{}}
    };
    const auto module = Module::compile(sources);
    assert(module.isValid());
    for (const auto backend : {mparser::sdk::Backend::Automatic,mparser::sdk::Backend::Bytecode,
                               mparser::sdk::Backend::Portable,mparser::sdk::Backend::Native}) {
        mparser::sdk::Invocation request;
        request.backend = backend;
        const auto result = module.execute(request);
        if (!result.succeeded()) {
            for (const auto& diagnostic : result.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
        }
        assert(result.succeeded());
        const auto values = result.variables();
        assert(scalar(findVariable(values,"direct")->value) == 7);
        assert(scalar(findVariable(values,"second")->value) == 11);
        assert(scalar(findVariable(values,"a")->value) == 7);
        assert(scalar(findVariable(values,"b")->value) == 8);
        assert(scalar(findVariable(values,"changed")->value) == 8);
        assert(scalar(findVariable(values,"overridden")->value) == 17);
        auto inherited = sources;
        inherited.back().source = "obj=DispatchChild(); inherited=read(obj); allowed=inside(obj); "
            "peer=DispatchPeer(); left=pick(obj,peer); right=pick(peer,obj); mixed=pick(4,obj,peer);";
        const auto inheritedModule = Module::compile(inherited);
        assert(inheritedModule.isValid());
        const auto inheritedResult = inheritedModule.execute(request);
        assert(inheritedResult.succeeded());
        const auto inheritedValues = inheritedResult.variables();
        assert(scalar(findVariable(inheritedValues,"inherited")->value) == 7);
        assert(scalar(findVariable(inheritedValues,"allowed")->value) == 31);
        assert(scalar(findVariable(inheritedValues,"left")->value) == 1);
        assert(scalar(findVariable(inheritedValues,"right")->value) == 2);
        assert(scalar(findVariable(inheritedValues,"mixed")->value) == 1);
        auto deleted = sources;
        deleted.back().source = "obj=DispatchOwner(); delete(obj); value=marker(4,obj); first=pick(obj);";
        const auto deletedModule = Module::compile(deleted);
        assert(deletedModule.isValid());
        const auto deletedResult = deletedModule.execute(request);
        assert(deletedResult.succeeded());
        const auto deletedValues = deletedResult.variables();
        assert(scalar(findVariable(deletedValues,"value")->value) == 27);
        assert(scalar(findVariable(deletedValues,"first")->value) == 1);
        for (const auto& source : {"obj=DispatchOwner(); denied=secret(obj);",
                                   "obj=DispatchOwner(); delete(obj); invalid=read(obj);",
                                   "obj=DispatchOwner(); missing=read(4);",
                                   "obj=DispatchOwner(); missing=read;",
                                   "obj=DispatchOwner(); peer=DispatchPeer(); missing=read(peer,obj);"}) {
            auto invalid = sources;
            invalid.back().source = source;
            const auto invalidModule = Module::compile(invalid);
            assert(invalidModule.isValid());
            const auto rejected = invalidModule.execute(request);
            assert(!rejected.succeeded());
            assert(!rejected.diagnostics().empty());
            if (invalid.back().source.find("secret") != std::string::npos) {
                assert(rejected.diagnostics().front().message.find("access is denied") != std::string::npos);
            }
        }
    }
    auto local = sources;
    local.back().source = "obj=DispatchOwner(); local=read(obj); "
        "read=@(x)55; shadowed=read(obj);\nfunction y=read(obj)\ny=91;\nend";
    const auto localModule = Module::compile(local);
    assert(localModule.isValid());
    const auto localResult = localModule.execute();
    assert(localResult.succeeded());
    const auto localValues = localResult.variables();
    assert(scalar(findVariable(localValues,"local")->value) == 91);
    assert(scalar(findVariable(localValues,"shadowed")->value) == 55);
    auto fallback = sources;
    fallback.insert(fallback.end()-1,{"fallback/read.m","function y=read(obj)\ny=99;\nend",{}});
    fallback.back().source = "obj=DispatchOwner(); method=read(obj); ordinary=read(4);";
    const auto fallbackModule = Module::compile(fallback);
    assert(fallbackModule.isValid());
    const auto fallbackResult = fallbackModule.execute();
    assert(fallbackResult.succeeded());
    const auto fallbackValues = fallbackResult.variables();
    assert(scalar(findVariable(fallbackValues,"method")->value) == 7);
    assert(scalar(findVariable(fallbackValues,"ordinary")->value) == 99);
    auto sideEffects = sources;
    sideEffects.back().source = "obj=DispatchOwner(); once=read(tick(obj)); count=obj.Value;\n"
        "function value=tick(obj)\nobj.Value=obj.Value+1; value=obj;\nend";
    const auto sideEffectModule = Module::compile(sideEffects);
    assert(sideEffectModule.isValid());
    const auto sideEffectResult = sideEffectModule.execute();
    assert(sideEffectResult.succeeded());
    const auto sideEffectValues = sideEffectResult.variables();
    assert(scalar(findVariable(sideEffectValues,"once")->value) == 8);
    assert(scalar(findVariable(sideEffectValues,"count")->value) == 8);
    auto runtime = mparser::sdk::Runtime::create();
    const auto producer = Module::compile("function obj=make_foreign()\nobj=SameName();\nend\n"
        "classdef SameName < handle\nend","dispatch_producer.m");
    mparser::sdk::Invocation exportedRequest;
    exportedRequest.entryFunction = "make_foreign";
    exportedRequest.requestedOutputCount = 1;
    const auto exported = runtime.execute(producer,exportedRequest);
    assert(exported.succeeded());
    const std::string localClass = "\nclassdef SameName < handle\nmethods\n"
        "function y=read(obj)\ny=99;\nend\nend\nend";
    const auto foreignConsumer = Module::compile("wrong=read(foreign([]));" + localClass,"dispatch_consumer.m");
    assert(foreignConsumer.isValid());
    mparser::sdk::Invocation importedRequest;
    importedRequest.initialWorkspace = {{"foreign",exported.output(0)}};
    const auto foreignResult = runtime.execute(foreignConsumer,importedRequest);
    assert(!foreignResult.succeeded());
    const auto localEmpty = Module::compile("obj=SameName(); empty=obj([]); correct=read(empty);" + localClass,
        "dispatch_empty_local.m");
    assert(localEmpty.isValid());
    const auto localEmptyResult = localEmpty.execute();
    assert(localEmptyResult.succeeded());
    const auto localEmptyValues = localEmptyResult.variables();
    assert(scalar(findVariable(localEmptyValues,"correct")->value) == 99);
}

void runCrossInvocationMethodSmoke() {
    const auto producer = Module::compile(R"(
function obj=make_object()
obj=SharedOwner();
end
classdef SharedOwner < handle
properties
Value=7
end
properties (Dependent)
Trap
end
methods
function y=get.Trap(obj)
obj.Value=999;
error('getter must not execute during method lookup');
y=0;
end
function y=read(obj)
y=obj.Value;
end
function y=combine(x,obj)
y=x+obj.Value;
end
function [a,b]=parts(obj)
a=obj.Value; b=a+1;
end
function touch(obj)
obj.Value=obj.Value+1;
end
function y=report(obj)
y=nargout;
end
function y=marker(obj)
y=23;
end
function y=length(obj)
y=17;
end
end
methods (Access=private)
function y=secret(obj)
y=91;
end
end
end
)","shared_producer.m");
    assert(producer.isValid());
    const auto consumer = Module::compile(
        "direct=read(foreign); second=combine(4,foreign); [a,b]=parts(foreign); "
        "touch(foreign); foreign.touch(); changed=read(foreign); overridden=length(foreign); "
        "try, bad=Trap(foreign); catch, end; lookup_after=read(foreign); empty_value=marker(foreign([])); "
        "assigned=report(foreign); dot_assigned=foreign.report(); foreign.report(); dot_implicit=ans; report(foreign)\n"
        "classdef SharedOwner\nmethods\nfunction y=read(obj)\ny=99;\nend\nend\nend",
        "shared_consumer.m");
    assert(consumer.isValid());
    mparser::sdk::SystemContextOptions contextOptions;
    contextOptions.rootDirectory = std::filesystem::current_path().generic_string();
    contextOptions.capabilities = mparser::sdk::SystemCapability::DynamicEvaluation;
    const auto context = mparser::sdk::SystemContext::rootedNative(contextOptions);
    for (const auto backend : {mparser::sdk::Backend::Automatic,mparser::sdk::Backend::Bytecode,
                               mparser::sdk::Backend::Portable,mparser::sdk::Backend::Native}) {
        auto runtime = mparser::sdk::Runtime::create(context);
        mparser::sdk::Invocation request;
        request.backend = backend;
        request.entryFunction = "make_object";
        request.requestedOutputCount = 1;
        const auto exported = runtime.execute(producer,request);
        assert(exported.succeeded());
        request = {};
        request.backend = backend;
        request.initialWorkspace = {{"foreign",exported.output(0)}};
        const auto result = runtime.execute(consumer,request);
        if (!result.succeeded()) {
            for (const auto& diagnostic : result.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
        }
        assert(result.succeeded());
        const auto values = result.variables();
        for (const auto& [name,expected] : std::vector<std::pair<std::string,double>>{
                 {"direct",7},{"second",11},{"a",7},{"b",8},{"changed",9},{"lookup_after",9},
                 {"overridden",17},{"empty_value",23},{"assigned",1},{"dot_assigned",1},{"dot_implicit",0},{"ans",0}}) {
            assert(scalar(findVariable(values,name)->value) == expected);
        }
        auto deniedRequest = request;
        deniedRequest.eventCapture = mparser::sdk::EventCaptureOptions{};
        const auto denied = runtime.execute(Module::compile("bad=secret(foreign);","denied.m"),deniedRequest);
        assert(!denied.succeeded());
        assert(denied.diagnostics().front().message.find("access is denied") != std::string::npos);
        assert(denied.diagnostics().front().source);
        assert(denied.diagnostics().front().source->sourceName == "denied.m");
        assert(denied.diagnostics().front().source->begin.offset == 4);
        const auto deniedEvents = denied.executionEvents();
        assert(deniedEvents.size() == 1 && deniedEvents.front().kind == mparser::sdk::ExecutionEventKind::Error);
        assert(deniedEvents.front().source.sourceName == "denied.m");
        const auto captured = runtime.execute(Module::compile(
            "text=evalc('captured=read(foreign);'); later=read(foreign);","capture_owner.m"),request);
        assert(captured.succeeded());
        const auto capturedValues = captured.variables();
        assert(scalar(findVariable(capturedValues,"captured")->value) == 9);
        assert(scalar(findVariable(capturedValues,"later")->value) == 9);
        const std::vector<mparser::sdk::SourceUnit> fallbackSources{
            {"main.m","ordinary=ordinary_function(foreign);"},
            {"ordinary_function.m","function y=ordinary_function(obj)\ny=42;\nend"}};
        const auto fallback = runtime.execute(Module::compile(fallbackSources),request);
        assert(fallback.succeeded());
        const auto fallbackValues = fallback.variables();
        assert(scalar(findVariable(fallbackValues,"ordinary")->value) == 42);
    }
}

void runPrivateDestructorHandleSmoke() {
    const std::string definitions = R"(
classdef PrivateDeleteRecorder < handle
properties
Count=0
end
end
classdef PrivateDeleteOwner < handle
properties
Recorder
end
methods
function obj=PrivateDeleteOwner(recorder)
obj.Recorder=recorder;
end
function value=makeDeleter(obj)
value=@obj.delete;
end
function value=makeArrayDeleter(obj)
array=[obj,obj]; value=@array.delete;
end
end
methods (Access=private)
function delete(obj)
obj.Recorder.Count=obj.Recorder.Count+1;
end
end
end
)";
    const auto module = Module::compile(
        "recorder=PrivateDeleteRecorder(); obj=PrivateDeleteOwner(recorder); "
        "deleter=obj.makeDeleter(); deleter(); alive=isvalid(obj); count=recorder.Count;" + definitions,
        "private_delete_handle.m");
    assert(module.isValid());
    const auto result = module.execute();
    if (!result.succeeded()) {
        for (const auto& diagnostic : result.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
    }
    assert(result.succeeded());
    const auto values = result.variables();
    assert(findVariable(values,"alive")->value.numericData<std::uint8_t>().front() == 0);
    assert(scalar(findVariable(values,"count")->value) == 1);
    const auto factory = Module::compile(
        "function [obj,deleter,recorder]=make_private()\n"
        "recorder=PrivateDeleteRecorder(); obj=PrivateDeleteOwner(recorder); deleter=obj.makeDeleter();\nend\n" +
        definitions,"private_delete_factory.m");
    assert(factory.isValid());
    auto runtime = mparser::sdk::Runtime::create();
    mparser::sdk::Invocation create;
    create.entryFunction = "make_private";
    create.requestedOutputCount = 3;
    const auto exported = runtime.execute(factory,create);
    assert(exported.succeeded());
    mparser::sdk::Invocation invoke;
    invoke.initialWorkspace = {{"obj",exported.output(0)},
        {"deleter",exported.output(1)},{"recorder",exported.output(2)}};
    for (const auto source : {"delete(obj);","obj.delete();"}) {
        const auto denied = runtime.execute(Module::compile(source,"private_delete_denied.m"),invoke);
        assert(!denied.succeeded());
        const auto message = denied.diagnostics().front().message;
        assert(message.find("access is denied") != std::string::npos);
    }
    const auto boundOutside = runtime.execute(Module::compile(
        "external=@obj.delete; array=[obj,obj]; externalArray=@array.delete;","private_delete_external_binding.m"),invoke);
    assert(boundOutside.succeeded());
    auto deniedInvoke = invoke;
    const auto boundValues = boundOutside.variables();
    deniedInvoke.initialWorkspace.push_back({"external",findVariable(boundValues,"external")->value});
    deniedInvoke.initialWorkspace.push_back({"externalArray",findVariable(boundValues,"externalArray")->value});
    const auto deniedCall = runtime.execute(Module::compile("external();","private_delete_external_call.m"),deniedInvoke);
    assert(!deniedCall.succeeded());
    assert(deniedCall.diagnostics().front().message.find("access is denied") != std::string::npos);
    const auto deniedArrayCall = runtime.execute(Module::compile("externalArray();","private_delete_external_array_call.m"),deniedInvoke);
    assert(!deniedArrayCall.succeeded());
    const auto unchanged = runtime.execute(Module::compile(
        "alive=isvalid(obj); count=recorder.Count;","private_delete_unchanged.m"),invoke);
    assert(unchanged.succeeded());
    const auto unchangedValues = unchanged.variables();
    assert(findVariable(unchangedValues,"alive")->value.numericData<std::uint8_t>().front() == 1);
    assert(scalar(findVariable(unchangedValues,"count")->value) == 0);
    const auto authorised = runtime.execute(Module::compile(
        "deleter(); deleter(); alive=isvalid(obj); count=recorder.Count;","private_delete_authorised.m"),invoke);
    assert(authorised.succeeded());
    const auto authorisedValues = authorised.variables();
    assert(findVariable(authorisedValues,"alive")->value.numericData<std::uint8_t>().front() == 0);
    assert(scalar(findVariable(authorisedValues,"count")->value) == 1);
    const auto arrayModule = Module::compile(
        "recorder=PrivateDeleteRecorder(); obj=PrivateDeleteOwner(recorder); "
        "deleter=obj.makeArrayDeleter(); deleter(); alive=isvalid(obj); count=recorder.Count;" + definitions,
        "private_delete_array.m");
    assert(arrayModule.isValid());
    const auto arrayResult = arrayModule.execute();
    if (!arrayResult.succeeded()) {
        for (const auto& diagnostic : arrayResult.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
    }
    assert(arrayResult.succeeded());
    const auto arrayValues = arrayResult.variables();
    assert(findVariable(arrayValues,"alive")->value.numericData<std::uint8_t>().front() == 0);
    assert(scalar(findVariable(arrayValues,"count")->value) == 1);
}

void runMethodHandleAccessSmoke() {
    const std::string definitions = R"(
classdef AccessOwner < handle
properties
GetterCalls=0
PrivateCalls=0
ProtectedCalls=0
end
properties (Dependent)
Trap
end
methods
function value=publicRead(obj)
value=7;
end
function callback=privateFactory(obj)
callback=@obj.privateRead;
end
function value=invoke(obj,callback)
value=callback();
end
function value=get.Trap(obj)
obj.GetterCalls=obj.GetterCalls+1; value=99;
end
end
methods (Access=private)
function value=privateRead(obj)
obj.PrivateCalls=obj.PrivateCalls+1;
value=31;
end
end
methods (Access=protected)
function value=protectedRead(obj)
obj.ProtectedCalls=obj.ProtectedCalls+1;
value=41;
end
end
end
classdef AccessChild < AccessOwner
methods
function callback=protectedFactory(obj)
callback=@obj.protectedRead;
end
end
end
)";
    const auto factory = Module::compile(
        "function [obj,child,privateHandle,protectedHandle]=make_values()\n"
        "obj=AccessOwner(); child=AccessChild(); privateHandle=obj.privateFactory(); "
        "protectedHandle=child.protectedFactory();\nend\n" + definitions,"handle_access_owner.m");
    assert(factory.isValid());
    for (const auto backend : {Backend::Automatic,Backend::Bytecode,Backend::Portable,Backend::Native}) {
        auto runtime = mparser::sdk::Runtime::create();
        Invocation create; create.backend=backend; create.entryFunction="make_values"; create.requestedOutputCount=4;
        const auto exported=runtime.execute(factory,create);
        assert(exported.succeeded());
        Invocation request; request.backend=backend;
        request.initialWorkspace={{"obj",exported.output(0)},{"child",exported.output(1)},
            {"authorisedPrivate",exported.output(2)},{"authorisedProtected",exported.output(3)}};
        const auto binding=runtime.execute(Module::compile(
            "publicHandle=@obj.publicRead; deniedPrivate=@obj.privateRead; deniedProtected=@child.protectedRead; "
            "publicValue=publicHandle(); privateValue=authorisedPrivate(); protectedValue=authorisedProtected();\n"
            "classdef AccessOwner < handle\nmethods\nfunction value=privateRead(obj)\nvalue=99;\nend\nend\nend",
            "handle_access_consumer.m"),request);
        assert(binding.succeeded());
        const auto values=binding.variables();
        assert(scalar(findVariable(values,"publicValue")->value)==7);
        assert(scalar(findVariable(values,"privateValue")->value)==31);
        assert(scalar(findVariable(values,"protectedValue")->value)==41);
        request.initialWorkspace.push_back({"deniedPrivate",findVariable(values,"deniedPrivate")->value});
        request.initialWorkspace.push_back({"deniedProtected",findVariable(values,"deniedProtected")->value});
        request.eventCapture=mparser::sdk::EventCaptureOptions{};
        for (const auto source : {"deniedPrivate();","deniedProtected();",
            "obj.invoke(deniedPrivate);","obj.invoke(deniedProtected);"}) {
            const auto denied=runtime.execute(Module::compile(source,"denied_handle_call.m"),request);
            assert(!denied.succeeded());
            assert(denied.diagnostics().front().message.find("access is denied")!=std::string::npos);
            const auto events=denied.executionEvents();
            assert(events.size()==1 && events.front().kind==mparser::sdk::ExecutionEventKind::Error);
            const auto expected=std::string_view(source).starts_with("obj.invoke")
                ? "handle_access_owner.m" : "denied_handle_call.m";
            assert(denied.diagnostics().front().source->sourceName==expected);
            assert(events.front().source.sourceName==expected);
        }
        const auto trap=runtime.execute(Module::compile("bad=@obj.Trap;","handle_getter_trap.m"),request);
        assert(!trap.succeeded());
        const auto observed=runtime.execute(Module::compile(
            "calls=obj.GetterCalls; privateCalls=obj.PrivateCalls; protectedCalls=child.ProtectedCalls;",
            "handle_getter_check.m"),request);
        assert(observed.succeeded());
        const auto observedValues=observed.variables();
        assert(scalar(findVariable(observedValues,"calls")->value)==0);
        assert(scalar(findVariable(observedValues,"privateCalls")->value)==1);
        assert(scalar(findVariable(observedValues,"protectedCalls")->value)==1);
        Invocation localRequest; localRequest.backend=backend;
        const auto local=Module::compile(
            "obj=AccessOwner(); child=AccessChild(); deniedPrivate=@obj.privateRead; deniedProtected=@child.protectedRead; "
            "try, deniedPrivate(); catch, end; try, deniedProtected(); catch, end; "
            "try, obj.invoke(deniedPrivate); catch, end; "
            "legal=obj.privateFactory(); value=legal(); "
            "privateCalls=obj.PrivateCalls; protectedCalls=child.ProtectedCalls;"+definitions,
            "local_handle_access.m").execute(localRequest);
        assert(local.succeeded());
        const auto localValues=local.variables();
        assert(scalar(findVariable(localValues,"value")->value)==31);
        assert(scalar(findVariable(localValues,"privateCalls")->value)==1);
        assert(scalar(findVariable(localValues,"protectedCalls")->value)==0);
    }
}

void runCrossInvocationDeletionSmoke() {
    const auto producer = Module::compile(R"(
function [obj,recorder,watcher]=make_object()
recorder=DeleteRecorder();
obj=DeleteChild(recorder);
watcher=addlistener(obj,'ObjectBeingDestroyed',@(source,data)recorder.note(source,data));
end
function [obj,recorder,watcher]=make_with_callback(callback)
recorder=DeleteRecorder();
obj=DeleteChild(recorder);
watcher=addlistener(obj,'ObjectBeingDestroyed',callback);
end
classdef DeleteRecorder < handle
properties
Deletes=0
Events=0
end
methods
function note(obj,source,data)
obj.Events=obj.Events+1;
end
end
end
classdef DeleteBase < handle
properties
Recorder
end
methods
function obj=DeleteBase(recorder)
obj.Recorder=recorder;
end
function delete(obj)
obj.Recorder.Deletes=obj.Recorder.Deletes+10;
end
end
end
classdef DeleteChild < DeleteBase
methods
function obj=DeleteChild(recorder)
obj@DeleteBase(recorder);
end
function delete(obj)
obj.Recorder.Deletes=obj.Recorder.Deletes+1;
end
end
end
)","delete_owner.m");
    assert(producer.isValid());
    for (const auto backend : {mparser::sdk::Backend::Automatic,mparser::sdk::Backend::Bytecode,
                               mparser::sdk::Backend::Portable,mparser::sdk::Backend::Native}) {
        auto runtime = mparser::sdk::Runtime::create();
        for (const auto& call : {"delete(foreign);","foreign.delete();"}) {
            mparser::sdk::Invocation create;
            create.backend = backend;
            create.entryFunction = "make_object";
            create.requestedOutputCount = 3;
            const auto exported = runtime.execute(producer,create);
            assert(exported.succeeded());
            mparser::sdk::Invocation request;
            request.backend = backend;
            request.initialWorkspace = {{"foreign",exported.output(0)},
                {"recorder",exported.output(1)},{"watcher",exported.output(2)}};
            const auto source = std::string(call) + " alive=isvalid(foreign); " + call +
                " deletes=recorder.Deletes; events=recorder.Events; listener_alive=isvalid(watcher);";
            const auto result = runtime.execute(Module::compile(source,"delete_consumer.m"),request);
            assert(result.succeeded());
            const auto values = result.variables();
            assert(findVariable(values,"alive")->value.numericData<std::uint8_t>().front() == 0);
            assert(scalar(findVariable(values,"deletes")->value) == 11);
            assert(scalar(findVariable(values,"events")->value) == 1);
            assert(findVariable(values,"listener_alive")->value.numericData<std::uint8_t>().front() == 0);
        }
    }
    auto runtime = mparser::sdk::Runtime::create();
    mparser::sdk::Invocation create;
    create.entryFunction = "make_object";
    create.requestedOutputCount = 3;
    auto beforeReset = runtime.execute(producer,create);
    assert(beforeReset.succeeded());
    const auto stale = beforeReset.output(2);
    runtime.reset();
    beforeReset = {};
    auto afterReset = runtime.execute(producer,create);
    assert(afterReset.succeeded());
    mparser::sdk::Invocation request;
    request.initialWorkspace = {{"foreign",afterReset.output(0)},
        {"recorder",afterReset.output(1)},{"stale",stale}};
    afterReset = {};
    const auto resetResult = runtime.execute(Module::compile(
        "old_alive=isvalid(stale); delete(stale); delete(foreign); events=recorder.Events;",
        "delete_after_reset.m"),request);
    assert(resetResult.succeeded());
    const auto resetValues = resetResult.variables();
    assert(findVariable(resetValues,"old_alive")->value.numericData<std::uint8_t>().front() == 0);
    assert(scalar(findVariable(resetValues,"events")->value) == 1);
    const auto callbackModule = Module::compile(R"(
function callback=make_callback()
callback=@observe;
end
function observe(source,data)
recorder=source.Recorder;
recorder.Events=recorder.Events+1;
end
)","foreign_destroy_callback.m");
    assert(callbackModule.isValid());
    mparser::sdk::Invocation callbackRequest;
    callbackRequest.entryFunction = "make_callback";
    callbackRequest.requestedOutputCount = 1;
    const auto callback = runtime.execute(callbackModule,callbackRequest);
    assert(callback.succeeded());
    create.entryFunction = "make_with_callback";
    create.arguments = {callback.output(0)};
    const auto observedObject = runtime.execute(producer,create);
    assert(observedObject.succeeded());
    request.initialWorkspace = {{"foreign",observedObject.output(0)},
        {"recorder",observedObject.output(1)}};
    const auto observedDelete = runtime.execute(Module::compile(
        "delete(foreign); observed=recorder.Events;", "foreign_destroy_entry.m"),request);
    if (!observedDelete.succeeded()) {
        for (const auto& diagnostic : observedDelete.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
    }
    assert(observedDelete.succeeded());
    const auto observedValues = observedDelete.variables();
    assert(scalar(findVariable(observedValues,"observed")->value) == 1);
    const auto afterDestruction = runtime.execute(Module::compile(
        "bad=foreign.Recorder;","after_destruction.m"),request);
    assert(!afterDestruction.succeeded());
    assert(afterDestruction.diagnostics().front().message.find("invalid or deleted object") != std::string::npos);
}

void runForeignEventPermissionSmoke() {
    const auto producer = Module::compile(R"(
function [obj,recorder]=make_source()
recorder=EventCount();
obj=OwnedEvents();
addlistener(obj,'Secret',@(source,data)recorder.increment());
addlistener(obj,'Public',@(source,data)recorder.increment());
addlistener(obj,'Data',@(source,data)recorder.capture(data));
addlistener(obj,'CallbackFailure',@(source,data)error('owner callback failure'));
end
classdef EventCount < handle
properties
Count=0
Last=0
end
methods
function increment(obj)
obj.Count=obj.Count+1;
end
function capture(obj,data)
obj.Count=obj.Count+1; obj.Last=data.Payload;
end
end
end
classdef OwnedEvents < handle
events (NotifyAccess=private)
Secret
end
events
Public
Data
CallbackFailure
end
methods
function fire(obj)
notify(obj,'Secret');
end
end
end
classdef SpoofData < event.EventData
end
)","event_permission_owner.m");
    assert(producer.isValid());
    auto runtime = mparser::sdk::Runtime::create();
    mparser::sdk::Invocation create;
    create.entryFunction = "make_source";
    create.requestedOutputCount = 2;
    const auto exported = runtime.execute(producer,create);
    assert(exported.succeeded());
    mparser::sdk::Invocation invoke;
    invoke.eventCapture = mparser::sdk::EventCaptureOptions{};
    invoke.initialWorkspace = {{"foreign",exported.output(0)},{"recorder",exported.output(1)}};
    for (const auto& command : {
        "marker=1; notify(foreign,'Missing');",
        "marker=1; notify(foreign,'Public',1);",
        "marker=1; addlistener(foreign,'Missing',@(src,data)1);"}) {
        const auto rejected = runtime.execute(Module::compile(command,"event_callsite.m"),invoke);
        assert(!rejected.succeeded());
        assert(rejected.diagnostics().size() == 1);
        assert(rejected.diagnostics().front().source);
        assert(rejected.diagnostics().front().source->sourceName == "event_callsite.m");
        assert(rejected.diagnostics().front().source->begin.offset == 10);
        const auto events = rejected.executionEvents();
        assert(events.size() == 2 && events.back().kind == mparser::sdk::ExecutionEventKind::Error);
        assert(events.back().source.sourceName == "event_callsite.m");
        assert(events.back().source.begin.offset == 10);
        assert(events.back().sourceContext == events.front().sourceContext);
    }
    const auto forged = Module::compile(
        "notify(foreign,'Secret');\nclassdef OwnedEvents < handle\nevents\nSecret\nend\nend",
        "event_permission_consumer.m");
    assert(forged.isValid());
    const auto denied = runtime.execute(forged,invoke);
    assert(!denied.succeeded());
    assert(denied.diagnostics().front().message.find("notify access is denied") != std::string::npos);
    const auto permitted = runtime.execute(Module::compile(
        "notify(foreign,'Public'); foreign.fire(); count=recorder.Count;", "event_permission_public.m"),invoke);
    assert(permitted.succeeded());
    const auto values = permitted.variables();
    assert(scalar(findVariable(values,"count")->value) == 2);
    const auto impostor = Module::compile(
        "function data=make_data()\ndata=SpoofData();\nend\n"
        "classdef SpoofData < handle\nproperties\nPayload=99\nend\nend", "impostor_data_owner.m");
    assert(impostor.isValid());
    mparser::sdk::Invocation makeData;
    makeData.entryFunction = "make_data";
    makeData.requestedOutputCount = 1;
    const auto wrongData = runtime.execute(impostor,makeData);
    assert(wrongData.succeeded());
    invoke.initialWorkspace.push_back({"data",wrongData.output(0)});
    const auto rejectedData = runtime.execute(Module::compile(
        "notify(foreign,'Data',data);", "impostor_data_consumer.m"),invoke);
    assert(!rejectedData.succeeded());
    assert(rejectedData.diagnostics().front().message.find("must derive from event.EventData") != std::string::npos);
    assert(rejectedData.diagnostics().front().source->sourceName == "impostor_data_consumer.m");
    const auto dataOwner = Module::compile(
        "function data=make_data()\ndata=GoodData();\nend\n"
        "classdef GoodData < event.EventData\nproperties\nPayload=17\nend\nend", "good_data_owner.m");
    assert(dataOwner.isValid());
    const auto goodData = runtime.execute(dataOwner,makeData);
    assert(goodData.succeeded());
    invoke.initialWorkspace.back().value = goodData.output(0);
    const auto acceptedData = runtime.execute(Module::compile(
        "notify(foreign,'Data',data); count=recorder.Count; payload=recorder.Last;", "good_data_consumer.m"),invoke);
    if (!acceptedData.succeeded()) {
        for (const auto& diagnostic : acceptedData.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
    }
    assert(acceptedData.succeeded());
    const auto acceptedValues = acceptedData.variables();
    assert(scalar(findVariable(acceptedValues,"count")->value) == 3);
    assert(scalar(findVariable(acceptedValues,"payload")->value) == 17);
    const auto callbackFailure = runtime.execute(Module::compile(
        "notify(foreign,'CallbackFailure');","callback_consumer.m"),invoke);
    assert(!callbackFailure.succeeded());
    assert(callbackFailure.diagnostics().front().source->sourceName == "event_permission_owner.m");
    const auto callbackEvents = callbackFailure.executionEvents();
    assert(callbackEvents.size() == 1 && callbackEvents.front().kind == mparser::sdk::ExecutionEventKind::Error);
    assert(callbackEvents.front().source.sourceName == "event_permission_owner.m");
    const auto thirdOwner = Module::compile(
        "function callback=make_callback()\ncallback=@fail_callback;\nend\n"
        "function fail_callback(source,data)\nerror('third owner callback failure');\nend",
        "third_callback_owner.m");
    mparser::sdk::Invocation makeCallback;
    makeCallback.entryFunction = "make_callback";
    makeCallback.requestedOutputCount = 1;
    const auto thirdCallback = runtime.execute(thirdOwner,makeCallback);
    assert(thirdCallback.succeeded());
    invoke.initialWorkspace.push_back({"callback",thirdCallback.output(0)});
    const auto attached = runtime.execute(Module::compile(
        "watcher=addlistener(foreign,'Public',callback);","third_callback_attach.m"),invoke);
    assert(attached.succeeded());
    for (const bool capture : {false,true}) {
        auto request = invoke;
        if (!capture) { request.eventCapture.reset(); }
        const auto failed = runtime.execute(Module::compile(
            "notify(foreign,'Public');","third_callback_consumer.m"),request);
        assert(!failed.succeeded());
        assert(failed.diagnostics().front().source->sourceName == "third_callback_owner.m");
        const auto failedEvents = failed.executionEvents();
        if (capture) {
            assert(std::count_if(failedEvents.begin(),failedEvents.end(),[](const auto& event) {
                return event.kind == mparser::sdk::ExecutionEventKind::Error;
            }) == 1);
            const auto error = std::find_if(failedEvents.begin(),failedEvents.end(),[](const auto& event) {
                return event.kind == mparser::sdk::ExecutionEventKind::Error;
            });
            assert(error->source.sourceName == "third_callback_owner.m");
            assert(error->disposition == mparser::sdk::ExecutionEventDisposition::Unhandled);
        } else { assert(failedEvents.empty()); }
        const auto caught = runtime.execute(Module::compile(
            "try, notify(foreign,'Public'); catch err, "
            "correct=strcmp(err.stack(1).file,'third_callback_owner.m'); end",
            "third_callback_catch.m"),request);
        assert(caught.succeeded());
        const auto caughtValues = caught.variables();
        assert(findVariable(caughtValues,"correct")->value.numericData<std::uint8_t>().front() == 1);
        const auto events = caught.executionEvents();
        if (capture) {
            assert(std::count_if(events.begin(),events.end(),[](const auto& event) {
                return event.kind == mparser::sdk::ExecutionEventKind::Error;
            }) == 1);
            const auto error = std::find_if(events.begin(),events.end(),[](const auto& event) {
                return event.kind == mparser::sdk::ExecutionEventKind::Error;
            });
            assert(error != events.end() && error->source.sourceName == "third_callback_owner.m");
            assert(error->disposition == mparser::sdk::ExecutionEventDisposition::Caught);
        } else { assert(events.empty()); }
    }
}

void runGraphicsListenerExportSmoke() {
    const auto result = Module::compile(
        "f=figure(); watcher=addlistener(f,'ObjectBeingDestroyed',@(src,data)1); same=(watcher.Source==f);",
        "graphics_listener.m").execute();
    assert(result.succeeded());
    const auto values = result.variables();
    assert(findVariable(values,"watcher"));
    assert(findVariable(values,"same")->value.numericClass() == NumericClass::Logical);
    assert(findVariable(values,"same")->value.numericData<std::uint8_t>().front() == 1);
}

void runDefinitionMetadataSmoke() {
    const std::vector<mparser::sdk::SourceGraphUnit> ownedSources{
        {"ExternalRange.m","classdef ExternalRange\nmethods\ny=read(obj)\nend\nend\n",{}},
        {"read.m","function y=read(obj)\ny=helper();\nfunction z=helper()\nz=7;\nend\nend\n","ExternalRange"},
        {"main.m","obj=ExternalRange(); answer=obj.read();",{}}
    };
    auto owned = Module::compile(ownedSources);
    assert(owned.isValid());
    const auto ownedDefinitions = owned.definitions();
    assert(ownedDefinitions.size() == 3);
    assert(ownedDefinitions[0].declarationOnly);
    assert(!ownedDefinitions[1].declarationOnly);
    assert(ownedDefinitions[1].kind == mparser::sdk::DefinitionKind::Method);
    assert(ownedDefinitions[1].className == "ExternalRange");
    assert(ownedDefinitions[1].sourceIndex == 1);
    assert(ownedDefinitions[2].parentFunction == 1);
    const auto execution = owned.execute();
    if (!execution.succeeded()) {
        for (const auto& diagnostic : execution.diagnostics()) { std::cerr << diagnostic.message << '\n'; }
    }
    assert(execution.succeeded());
    assert(scalar(findVariable(execution.variables(),"answer")->value) == 7);
    owned = Module{};
    assert(ownedDefinitions[1].source.sourceName == "read.m");

    mparser_source_graph_unit descriptor = MPARSER_SOURCE_GRAPH_UNIT_INIT;
    const mparser_source_graph_unit* descriptors[] = {&descriptor};
    mparser_module* rejected = nullptr;
    assert(mparser_module_compile_source_graph(nullptr,1,&rejected) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(!rejected);
    descriptor.struct_size = MPARSER_SOURCE_GRAPH_UNIT_SIZE - 1;
    assert(mparser_module_compile_source_graph(descriptors,1,&rejected) == MPARSER_API_STATUS_ABI_MISMATCH);
    descriptor.struct_size = MPARSER_SOURCE_GRAPH_UNIT_SIZE;
    descriptor.class_method_owner = {nullptr,1};
    assert(mparser_module_compile_source_graph(descriptors,1,&rejected) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(!rejected);

    descriptor = mparser_source_graph_unit MPARSER_SOURCE_GRAPH_UNIT_INIT;
    const std::string plain = "answer=3;";
    descriptor.source_name = {"plain.m",7};
    descriptor.source = {plain.data(),plain.size()};
    assert(mparser_module_compile_source_graph(descriptors,0,&rejected) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(mparser_module_compile_source_graph(descriptors,1,nullptr) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    const mparser_source_graph_unit* nullUnit[] = {nullptr};
    assert(mparser_module_compile_source_graph(nullUnit,1,&rejected) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    descriptor.abi_generation = MPARSER_C_ABI_GENERATION + 1;
    assert(mparser_module_compile_source_graph(descriptors,1,&rejected) == MPARSER_API_STATUS_ABI_MISMATCH);
    descriptor.abi_generation = MPARSER_C_ABI_GENERATION;
    struct ExtendedGraphUnit { mparser_source_graph_unit base; std::uint64_t future; } extended{descriptor,0};
    extended.base.struct_size = sizeof(extended);
    const mparser_source_graph_unit* futureUnits[] = {&extended.base};
    assert(mparser_module_compile_source_graph(futureUnits,1,&rejected) == MPARSER_API_STATUS_OK);
    assert(rejected);
    mparser_module_release(rejected);
    rejected = nullptr;
    for (const auto& owner : {std::string("MissingOwner"),std::string("ExternalRange")}) {
        auto invalid = ownedSources;
        invalid[1].classMethodOwner = owner;
        if (owner == "ExternalRange") invalid[1].name = "mismatch.m";
        const auto rejectedGraph = Module::compile(invalid);
        assert(!rejectedGraph.isValid());
        assert(!rejectedGraph.diagnostics().empty());
    }

    const std::vector<mparser::sdk::SourceUnit> graphSources{
        {"first.m", "% \xc3\xa9\r\nfunction y=first(x)\r\ny=helper(x);\r\nfunction z=helper(x)\r\nz=x;\r\nend\r\nend\r\n"},
        {"second.m", "function y=second(x)\ny=helper(x);\nfunction z=helper(x)\nz=x+1;\nend\nend\n"}
    };
    auto graph = Module::compile(graphSources);
    assert(graph.isValid());
    const auto graphDefinitions = graph.definitions();
    assert(graphDefinitions.size() == 4);
    for (std::size_t index = 0; index < graphDefinitions.size(); ++index) {
        const auto& definition = graphDefinitions[index];
        const auto sourceIndex = index / 2;
        assert(definition.sourceIndex == sourceIndex);
        assert(definition.source.sourceName == graphSources[sourceIndex].name);
        assert(definition.source.end.offset <= graphSources[sourceIndex].source.size());
        assert(graphSources[sourceIndex].source.substr(definition.source.begin.offset,8) == "function");
        if (index % 2) {
            assert(definition.name == "helper");
            assert(definition.parentFunction == index - 1);
            assert(definition.kind == mparser::sdk::DefinitionKind::NestedFunction);
        } else {
            assert(!definition.parentFunction);
        }
    }
    graph = Module{};
    assert(graphDefinitions[3].source.sourceName == "second.m");
    assert(graphDefinitions[3].name == "helper");

    const std::string source = "% \xc3\xa9\r\nfunction y=outer(x)\r\n"
        "y=inner(x);\r\nfunction z=inner(x)\r\nz=x;\r\nend\r\nend\r\n";
    auto module = Module::compile(source, "definitions.m");
    assert(module.isValid());
    const auto definitions = module.definitions();
    assert(definitions.size() == 2);
    assert(definitions[0].kind == mparser::sdk::DefinitionKind::PrimaryFunction);
    assert(definitions[0].source.sourceName == "definitions.m");
    assert(definitions[0].source.begin.line == 2);
    assert(definitions[0].source.begin.offset == source.find("function y"));
    assert(!definitions[0].parentFunction);
    assert(definitions[1].kind == mparser::sdk::DefinitionKind::NestedFunction);
    assert(definitions[1].parentFunction == 0);
    assert(definitions[1].source.begin.line == 4);
    module = Module{};
    assert(definitions[1].name == "inner");
    assert(definitions[1].source.sourceName == "definitions.m");

    mparser_module* raw = nullptr;
    assert(mparser_module_compile_utf8(source.data(), source.size(),
        "definitions.m", 13, &raw) == MPARSER_API_STATUS_OK);
    assert(mparser_module_definition_count(raw) == 2);
    mparser_definition_info info{};
    info.source_index = 99;
    assert(mparser_module_definition_info(raw, 2, &info, sizeof(info)) ==
           MPARSER_API_STATUS_OUT_OF_RANGE);
    assert(info.source_index == 99);
    assert(mparser_module_definition_info(raw, 0, &info, sizeof(info)-1) ==
           MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(info.source_index == 99);
    assert(mparser_module_definition_info(nullptr, 0, &info, sizeof(info)) ==
           MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(mparser_module_definition_info(raw, 0, nullptr, sizeof(info)) ==
           MPARSER_API_STATUS_INVALID_ARGUMENT);
    assert(mparser_module_definition_info(raw, 1, &info, sizeof(info)) ==
           MPARSER_API_STATUS_OK);
    assert(info.parent_function == 0 && info.source_index == 0);
    assert(info.kind == MPARSER_DEFINITION_NESTED_FUNCTION);
    assert(info.source_begin.offset == source.find("function z"));
    assert(std::string_view(info.name.data, info.name.size) == "inner");
    mparser_module_release(raw);
}

void runModuleSmoke(
    const std::string& entryPath,
    const std::string& libraryPath) {
    const auto module = Module::compile(
        kModuleSource, "cpp_api_smoke.m");
    assert(module.isValid());
    assert(module.sourceNames() ==
           std::vector<std::string>({"cpp_api_smoke.m"}));
    const auto functions = module.functionNames();
    assert(std::find(
               functions.begin(), functions.end(), "accumulate") !=
           functions.end());

    Invocation accumulate;
    accumulate.entryFunction = "accumulate";
    accumulate.arguments = {Value::scalar(100)};
    accumulate.requestedOutputCount = 2;
    accumulate.backend = Backend::Automatic;
    accumulate.collectProfile = true;
    auto accumulated = module.execute(accumulate);
    assert(accumulated.succeeded());
    assert(accumulated.status() == InvocationStatus::Succeeded);
    assert(accumulated.entryFunction() == "accumulate");
    assert(accumulated.requestedOutputCount() == 2);
    assert(accumulated.outputCount() == 2);
    assert(accumulated.outputName(0) == "total");
    assert(accumulated.outputName(1) == "last");
    assert(std::abs(scalar(accumulated.output(0)) - 5050.0) <
           kTolerance);
    assert(std::abs(scalar(accumulated.output(1)) - 100.0) <
           kTolerance);
    const auto summary = accumulated.executionSummary();
    assert(summary.profilingCollected);
    assert(summary.executedInstructionCount > 0);

    auto retainedOutput = accumulated.output(0);
    accumulated = Result{};
    assert(std::abs(scalar(retainedOutput) - 5050.0) <
           kTolerance);

    const auto warned = invoke(module, "emitWarning");
    assert(warned.succeeded());
    assert(hasDiagnostic(warned.diagnostics(), "Embed:Notice"));

    const auto failed = invoke(module, "failNow");
    assert(!failed.succeeded());
    assert(failed.status() == InvocationStatus::RuntimeFailed);
    assert(hasDiagnostic(failed.diagnostics(), "Embed:Failure"));

    Invocation limited;
    limited.entryFunction = "spin";
    limited.requestedOutputCount = 1;
    limited.limits.maximumInstructionCount = 64;
    const auto stopped = module.execute(limited);
    assert(!stopped.succeeded());
    assert(stopped.executionSummary().stopReason ==
           StopReason::InstructionLimit);

    CancellationToken token;
    const auto sharedToken = token;
    token.request();
    assert(token.requested() && sharedToken.requested());
    Invocation cancelled;
    cancelled.entryFunction = "spin";
    cancelled.requestedOutputCount = 1;
    cancelled.cancellationToken = token;
    const auto cancelledResult = module.execute(cancelled);
    assert(!cancelledResult.succeeded());
    assert(cancelledResult.executionSummary().stopReason ==
           StopReason::Cancelled);

    auto session = module.createSession();
    Invocation counter;
    counter.entryFunction = "nextCounter";
    counter.arguments = {Value::scalar(2)};
    counter.requestedOutputCount = 1;
    assert(std::abs(scalar(session.execute(counter).output(0)) - 2.0) <
           kTolerance);
    counter.arguments = {Value::scalar(3)};
    assert(std::abs(scalar(session.execute(counter).output(0)) - 5.0) <
           kTolerance);
    session.reset();
    counter.arguments = {Value::scalar(4)};
    assert(std::abs(scalar(session.execute(counter).output(0)) - 4.0) <
           kTolerance);

    const std::array<SourceUnit, 2> sources{
        SourceUnit{
            "entry.m",
            "function out = entry(value)\n"
            "out = helper(value);\nend\n"},
        SourceUnit{
            "helper.m",
            "function out = helper(value)\n"
            "out = value + 2;\nend\n"}};
    const auto sourceGraph = Module::compile(sources);
    assert(sourceGraph.isValid());
    assert(std::abs(scalar(invoke(
               sourceGraph, "entry", {Value::scalar(40)}).output(0)) -
           42.0) < kTolerance);

    const auto invalid = Module::compile(
        "function out = broken(\n", "invalid_cpp_api.m");
    assert(!invalid.isValid());
    const auto compilationDiagnostics = invalid.diagnostics();
    assert(!compilationDiagnostics.empty());
    assert(compilationDiagnostics.front().phase ==
           DiagnosticPhase::Compilation);
    assert(compilationDiagnostics.front().source);
    assert(compilationDiagnostics.front().source->sourceName ==
           "invalid_cpp_api.m");

    const auto loaded = Module::loadFile(
        entryPath, SourceLoadOptions{{libraryPath}});
    assert(loaded.isValid());
    assert(loaded.sourceNames().size() > 1);
    const auto loadedResult = loaded.execute();
    assert(loadedResult.succeeded());
    const auto variables = loadedResult.variables();
    const auto* scaled = findVariable(variables, "scaled");
    const auto* revealed = findVariable(variables, "revealed");
    const auto* offset = findVariable(variables, "offset_value");
    const auto* staticValue = findVariable(variables, "static_value");
    assert(scaled && revealed && offset && staticValue);
    assert(std::abs(scalar(scaled->value) - 21.0) < kTolerance);
    assert(std::abs(scalar(revealed->value) - 107.0) < kTolerance);
    assert(std::abs(scalar(offset->value) - 12.0) < kTolerance);
    assert(std::abs(scalar(staticValue->value) - 12.0) <
           kTolerance);

    const auto memoryGraph = Module::compile(
        "counter = folderpkg.Counter(8);\n"
        "memory_scaled = counter.scale(2);\n",
        "memory_entry.m", SourceLoadOptions{{libraryPath}});
    assert(memoryGraph.isValid());
    const auto memoryMetadata = memoryGraph.sourceMetadata();
    assert(memoryMetadata.size() > 1);
    assert(memoryMetadata.front().kind == SourceKind::Script);
    assert(memoryMetadata.front().hasTopLevelStatements);
    const auto memoryVariables = memoryGraph.execute().variables();
    const auto* memoryScaled =
        findVariable(memoryVariables, "memory_scaled");
    assert(memoryScaled &&
           std::abs(scalar(memoryScaled->value) - 16.0) < kTolerance);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    constexpr auto sourceApiVersion =
        mparser::sdk::sourceApiVersion();
    static_assert(sourceApiVersion.major == 1);
    static_assert(sourceApiVersion.minor == 15);
    assert(mparser::sdk::abiGeneration() == 2);
    assert(mparser::sdk::abiRevision() == 11);
    runValueSmoke();
    runHostOutputSmoke();
    runMetadataExportSmoke();
    try {
        runDynamicPropertyExportSmoke();
        runInputSourceSmoke();
    } catch (const std::exception& error) {
        std::cerr << "dynamic property export: " << error.what() << '\n';
        return 1;
    }
    runInvalidAssignmentCompileSmoke();
    runDefinitionMetadataSmoke();
    runFunctionSyntaxMethodSmoke();
    runPrivateDestructorHandleSmoke();
    runMethodHandleAccessSmoke();
    try { runForeignEventPermissionSmoke(); runGraphicsListenerExportSmoke(); }
    catch (const std::exception& error) { std::cerr << "event export: " << error.what() << '\n'; return 1; }
    try { runCrossInvocationDeletionSmoke(); }
    catch (const std::exception& error) { std::cerr << "cross deletion: " << error.what() << '\n'; return 1; }
    runCrossInvocationMethodSmoke();
    try {
        runIdentifierBuiltinSmoke();
    } catch (const std::exception& error) {
        std::cerr << "identifier builtin: " << error.what() << '\n';
        return 1;
    }
    runModuleSmoke(argv[1], argv[2]);
    std::cout << "cpp api smoke = 5050,42,21,abi-generation-"
              << mparser::sdk::abiGeneration() << "-revision-"
              << mparser::sdk::abiRevision() << ",cpp-api-"
              << sourceApiVersion.major << '.'
              << sourceApiVersion.minor << '\n';
    return 0;
}
