#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CompiledSource {
    mparser::ParseResult parsed;
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

CompiledSource compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    auto compiled = compile(source);
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
}

const mparser::SyntaxNode* findSyntax(const mparser::SyntaxNode& node,
                                      mparser::SyntaxKind kind,
                                      std::string_view label = {}) {
    if (node.kind == kind && (label.empty() || node.label == label)) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* result = findSyntax(*child, kind, label)) {
            return result;
        }
    }
    return nullptr;
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

void assertNumber(const mparser::BytecodeVmResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

bool containsOp(const mparser::BytecodeProgram& bytecode,
                mparser::BytecodeOp op, std::string_view operand) {
    for (const auto& instruction : bytecode.instructions) {
        if (instruction.op == op && instruction.operand == operand) {
            return true;
        }
    }
    return false;
}

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasParseDiagnostic(const mparser::ParseResult& result,
                        std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void parseStructuredPropertySmoke() {
    auto compiled = compile(R"(classdef Structured
    properties (SetAccess = private)
        Samples(1,:) double {mustBeFinite, mustBeGreaterThan(Samples, 0)} = [1 2]
    end
end
)");

    const auto* property = findSyntax(
        *compiled.parsed.root, mparser::SyntaxKind::PropertyDecl, "Samples");
    assert(property != nullptr);
    assert(property->attributes.size() == 1);
    assert(property->attributes.front().name == "SetAccess");
    assert(property->property.dimensions.size() == 2);
    assert(property->property.dimensions[0].text == "1");
    assert(property->property.dimensions[1].text == ":");
    assert(property->property.className == "double");
    assert(property->property.validators.size() == 2);
    assert(property->property.validators[0].name == "mustBeFinite");
    assert(property->property.validators[1].name == "mustBeGreaterThan");
    assert(property->property.validators[1].arguments.size() == 2);
    assert(property->property.validators[1].arguments[0] == "Samples");
    assert(property->property.validators[1].arguments[1] == "0");
    assert(property->property.hasExplicitDefault);
    assert(property->children.size() == 1);
    assert(property->children.front()->kind == mparser::SyntaxKind::MatrixExpr);

    bool typedSymbol = false;
    for (const auto& symbol : compiled.semantic.symbols) {
        if (symbol.kind == mparser::SymbolKind::Property &&
            symbol.name == "Samples" && symbol.typeName == "double") {
            typedSymbol = true;
        }
    }
    assert(typedSymbol);
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::EnterPropertyInitializer,
                      "Samples"));
    assert(containsOp(compiled.bytecode,
                      mparser::BytecodeOp::LeavePropertyInitializer,
                      "Samples"));
}

void rejectMalformedPropertySyntaxSmoke() {
    mparser::Lexer lexer(R"(classdef Invalid
    properties
        Value(0,1) double
        Other(1,1) double {}
    end
end
)");
    mparser::Parser parser(lexer.lex());
    const auto parsed = parser.parse();
    assert(hasParseDiagnostic(parsed, "positive integers or ':'"));
    assert(hasParseDiagnostic(parsed,
                              "property validation declaration is empty"));
}

constexpr std::string_view kValidatedClasses = R"(classdef ValidationBase
    properties
        BaseValue(1,1) double {mustBePositive} = 2
        ImplicitRow(1,3) double
        Seen
    end
    methods
        function obj = ValidationBase()
            obj.Seen = obj.BaseValue + 1;
        end
    end
end

classdef ValidationRecord < ValidationBase
    properties
        Coordinates(1,2) double {mustBeFinite} = [1 2]
        Count(1,1) double {mustBeInteger, mustBeNonnegative} = 0
        Limit(1,1) double {mustBeGreaterThan(Limit, 10)} = 11
        Label(1,1) string {mustBeNonzeroLengthText} = "origin"
    end
    methods
        function obj = ValidationRecord()
            obj = obj@ValidationBase();
            obj.Count = obj.BaseValue + 1;
        end
    end
end
)";

void executePropertyDefaultsAndValidationSmoke() {
    const std::string source = std::string(kValidatedClasses) + R"(
record = ValidationRecord();
record.Coordinates = [4; 5];
record.Count = 4;
second = ValidationRecord();
coordinate_sum = sum(record.Coordinates, "all");
implicit_sum = sum(record.ImplicitRow, "all");
constructor_saw_default = record.Seen;
first_count = record.Count;
second_count = second.Count;
limit = record.Limit;
label = record.Label;
)";
    const auto result = run(source);

    assert(result.diagnostics.empty());
    assertNumber(result, "coordinate_sum", 9);
    assertNumber(result, "implicit_sum", 0);
    assertNumber(result, "constructor_saw_default", 3);
    assertNumber(result, "first_count", 4);
    assertNumber(result, "second_count", 3);
    assertNumber(result, "limit", 11);
    const auto* label = findVariable(result, "label");
    assert(label != nullptr);
    assert(mparser::runtimeTextScalarUtf8(*label) == "origin");
}

void cacheHandleDefaultOnceSmoke() {
    const auto result = run(R"(classdef SharedToken < handle
    properties
        Value(1,1) double = 1
    end
end

classdef Envelope
    properties
        Token(1,1) SharedToken
    end
end

first = Envelope();
second = Envelope();
token = first.Token;
token.Value = 9;
shared_value = second.Token.Value;
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "shared_value", 9);
}

void executeNdPropertyDimensionsSmoke() {
    const auto result = run(R"(classdef TensorRecord
    properties
        Tensor(2,3,2) double {mustBeFinite}
        Slots(2,1,2) cell
    end
end

record = TensorRecord();
record.Tensor = 1:12;
slot_input = cell(1,4);
slot_input{1} = 21;
slot_input{2} = 22;
slot_input{3} = 23;
slot_input{4} = 24;
record.Slots = slot_input;
tensor = record.Tensor;
slots = record.Slots;
tensor_probe = tensor(2,2,2);
slot_probe = slots{2,1,2};
tensor_count = numel(tensor);
slot_count = numel(slots);
tensor_rank = ndims(tensor);
slot_rank = ndims(slots);
)");

    assert(result.diagnostics.empty());
    const auto* tensor = findVariable(result, "tensor");
    const auto* slots = findVariable(result, "slots");
    assert(tensor != nullptr);
    assert(slots != nullptr);
    assert(mparser::runtimeDimensions(*tensor) ==
           std::vector<size_t>({2, 3, 2}));
    assert(mparser::runtimeDimensions(*slots) ==
           std::vector<size_t>({2, 1, 2}));
    assert(tensor->elements ==
           std::vector<double>({1, 7, 3, 9, 5, 11,
                                2, 8, 4, 10, 6, 12}));
    assertNumber(result, "tensor_probe", 10);
    assertNumber(result, "slot_probe", 24);
    assertNumber(result, "tensor_count", 12);
    assertNumber(result, "slot_count", 4);
    assertNumber(result, "tensor_rank", 3);
    assertNumber(result, "slot_rank", 3);
}

void executeTextPropertyDimensionsSmoke() {
    const auto result = run(R"(classdef TextRecord
    properties
        Letters(2,2) char = ['ab'; 'cd']
        Labels(2,2) string = ["one", "two"; "three", "four"]
    end
end

record = TextRecord();
record.Letters = 'abcd';
record.Labels = ["one", "two", "three", "four"];
letters = record.Letters;
labels = record.Labels;
letter_probe = strcmp(letters(2,2), 'd');
label_probe = strcmp(labels(2,2), "four");
)");

    assert(result.diagnostics.empty());
    const auto* letters = findVariable(result, "letters");
    const auto* labels = findVariable(result, "labels");
    assert(letters != nullptr);
    assert(labels != nullptr);
    assert(letters->kind == mparser::RuntimeValueKind::CharacterArray);
    assert(labels->kind == mparser::RuntimeValueKind::StringArray);
    assert(mparser::runtimeDimensions(*letters) ==
           std::vector<size_t>({2, 2}));
    assert(mparser::runtimeDimensions(*labels) ==
           std::vector<size_t>({2, 2}));
    assert(mparser::runtimeCharacterElement(*letters, 3) == u'd');
    const auto* fourth = mparser::runtimeStringElement(*labels, 3);
    assert(fourth != nullptr && !fourth->missing &&
           mparser::runtimeUtf16ToUtf8(fourth->value) == "four");
    assertNumber(result, "letter_probe", 1);
    assertNumber(result, "label_probe", 1);
}

void rejectInvalidDefaultSmoke() {
    const auto result = run(R"(classdef BadDefault
    properties
        Value(1,1) double {mustBePositive} = 0
    end
end

bad = BadDefault();
)");
    assert(result.diagnostics.size() == 1);
    assert(hasDiagnostic(result, "BadDefault.Value"));
    assert(hasDiagnostic(result, "value must be positive"));
}

void rejectInvalidAssignmentsSmoke() {
    auto result = run(std::string(kValidatedClasses) + R"(
record = ValidationRecord();
record.Count = 2.5;
)");
    assert(result.diagnostics.size() == 1);
    assert(hasDiagnostic(result, "value must contain integers"));

    result = run(std::string(kValidatedClasses) + R"(
record = ValidationRecord();
record.Coordinates = [1 2 3];
)");
    assert(result.diagnostics.size() == 1);
    assert(hasDiagnostic(result, "property requires 2"));

    result = run(std::string(kValidatedClasses) + R"(
record = ValidationRecord();
record.Limit = 10;
)");
    assert(result.diagnostics.size() == 1);
    assert(hasDiagnostic(result, "mustBeGreaterThan"));

    result = run(std::string(kValidatedClasses) + R"(
record = ValidationRecord();
record.Label = 3;
)");
    assert(result.diagnostics.size() == 1);
    assert(hasDiagnostic(result, "class string"));
}

} // namespace

int main() {
    parseStructuredPropertySmoke();
    rejectMalformedPropertySyntaxSmoke();
    executePropertyDefaultsAndValidationSmoke();
    cacheHandleDefaultOnceSmoke();
    executeNdPropertyDimensionsSmoke();
    executeTextPropertyDimensionsSmoke();
    rejectInvalidDefaultSmoke();
    rejectInvalidAssignmentsSmoke();
    std::cout << "property validation smoke tests passed\n";
    return 0;
}
