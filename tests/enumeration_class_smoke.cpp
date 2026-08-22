#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string diagnosticsText(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) {
        text += std::to_string(diagnostic.span.begin.line) + ":" +
                std::to_string(diagnostic.span.begin.column) + ": " +
                diagnostic.message + "\n";
    }
    return text;
}

struct Pipeline {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

Pipeline lower(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    check(parsed.diagnostics.empty(),
          "parse diagnostics:\n" + diagnosticsText(parsed.diagnostics));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    check(semantic.diagnostics.empty(),
          "semantic diagnostics:\n" +
              diagnosticsText(semantic.diagnostics));

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    check(bytecode.diagnostics.empty(),
          "bytecode diagnostics:\n" +
              diagnosticsText(bytecode.diagnostics));
    return Pipeline{std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    auto pipeline = lower(source);
    mparser::BytecodeVm vm;
    return vm.run(pipeline.bytecode, pipeline.semantic);
}

mparser::SemanticResult analyze(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    check(parsed.diagnostics.empty(),
          "parse diagnostics:\n" + diagnosticsText(parsed.diagnostics));
    mparser::SemanticAnalyzer analyzer;
    return analyzer.analyze(*parsed.root);
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

void checkNumber(const mparser::BytecodeVmResult& result,
                 std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing runtime variable: " +
                                std::string(name));
    check(value->kind == mparser::RuntimeValueKind::Number,
          "runtime variable is not numeric: " + std::string(name));
    check(std::fabs(value->number - expected) < 1e-9,
          "unexpected numeric value for: " + std::string(name));
}

void checkString(const mparser::BytecodeVmResult& result,
                 std::string_view name, std::string_view expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing runtime variable: " +
                                std::string(name));
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    check(text.has_value(),
          "runtime variable is not text: " + std::string(name));
    check(*text == expected,
          "unexpected string value for: " + std::string(name));
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

void collectHir(const mparser::HirNode& node, mparser::HirKind kind,
                std::vector<const mparser::HirNode*>& nodes) {
    if (node.kind == kind) {
        nodes.push_back(&node);
    }
    for (const auto& child : node.children) {
        collectHir(*child, kind, nodes);
    }
}

constexpr std::string_view kStatusSource = R"(classdef Status
    properties
        Code
        Label
    end
    methods
        function obj = Status(code, label)
            obj.Code = code;
            obj.Label = label;
        end
        function value = isReady(obj)
            value = obj == Status.Ready;
        end
    end
    enumeration
        Ready(1, 'ready'), Busy(2, 'busy')
    end
    enumeration (Hidden)
        Internal(99, 'internal')
    end
end

ready = Status.Ready;
again = Status('Ready');
defaultStatus = Status();
busy = Status.Busy;
code = ready.Code;
label = ready.Label;
same = ready == again;
defaultSame = defaultStatus == ready;
different = ready ~= busy;
methodResult = ready.isReady();
enumFlag = isenum(ready);
classFlag = isa(ready, 'Status');
className = class(ready);
busyName = char(busy);
stringName = string(ready);

switch ready
    case Status.Ready
        switchResult = 7;
    otherwise
        switchResult = -1;
end

[members, names] = enumeration(ready);
[membersByName, namesByName] = enumeration('Status');
visibleCount = numel(members);
firstMember = members(1);
secondMember = members(2);
firstName = names{1};
secondName = names{2};
secondNameByClass = namesByName{2};
firstCode = firstMember.Code;
secondMemberName = char(secondMember);
hidden = Status.Internal;
hiddenName = char(hidden);
)";

void runFrontendAndBytecodeSmoke() {
    auto pipeline = lower(kStatusSource);
    std::vector<const mparser::HirNode*> members;
    collectHir(*pipeline.semantic.root,
               mparser::HirKind::EnumerationMember, members);
    check(members.size() == 3,
          "expected three enumeration members in HIR");
    check(members[0]->label == "Ready" &&
              members[0]->children.size() == 2,
          "Ready should preserve two constructor arguments");
    check(members[1]->label == "Busy" &&
              members[1]->children.size() == 2,
          "same-line Busy member was not split correctly");
    check(members[2]->attributes.size() == 1 &&
              members[2]->attributes.front().name == "Hidden",
          "enumeration block attributes were not copied to members");
    for (const auto* member : members) {
        check(member->binding.kind ==
                  mparser::BindingKind::EnumerationMember,
              "enumeration HIR member lacks its semantic binding");
    }

    size_t enterCount = 0;
    for (const auto& instruction : pipeline.bytecode.instructions) {
        if (instruction.op !=
            mparser::BytecodeOp::EnterEnumerationMemberInitializer) {
            continue;
        }
        ++enterCount;
        if (instruction.operand == "Ready" ||
            instruction.operand == "Busy" ||
            instruction.operand == "Internal") {
            check(instruction.operandCount == 2,
                  "enumeration initializer lost its argument count");
        }
    }
    check(enterCount == 3,
          "expected one bytecode initializer region per member");
}

void runValueEnumerationSmoke() {
    const auto result = run(kStatusSource);
    check(result.diagnostics.empty(),
          "value enumeration diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "code", 1);
    checkString(result, "label", "ready");
    checkNumber(result, "same", 1);
    checkNumber(result, "defaultSame", 1);
    checkNumber(result, "different", 1);
    checkNumber(result, "methodResult", 1);
    checkNumber(result, "enumFlag", 1);
    checkNumber(result, "classFlag", 1);
    checkNumber(result, "switchResult", 7);
    checkNumber(result, "firstCode", 1);
    checkNumber(result, "visibleCount", 2);
    checkString(result, "className", "Status");
    checkString(result, "busyName", "Busy");
    checkString(result, "stringName", "Ready");
    checkString(result, "firstName", "Ready");
    checkString(result, "secondName", "Busy");
    checkString(result, "secondNameByClass", "Busy");
    checkString(result, "secondMemberName", "Busy");
    checkString(result, "hiddenName", "Internal");

    const auto* ready = findVariable(result, "ready");
    check(ready != nullptr &&
              ready->kind == mparser::RuntimeValueKind::Object,
          "Ready did not produce a class object");
    check(ready->className == "Status" &&
              ready->enumerationMemberName == "Ready",
          "Ready object lost its enumeration identity");
    check(mparser::runtimeValueToString(*ready) == "<Status.Ready>",
          "enumeration display name is not class-qualified");

    const auto* members = findVariable(result, "members");
    check(members != nullptr &&
              members->kind == mparser::RuntimeValueKind::Object &&
              members->className == "Status" && members->rows == 2 &&
              members->columns == 1 &&
              members->objectElements.size() == 2,
          "enumeration() should return a column enumeration array");
    const auto* membersByName = findVariable(result, "membersByName");
    check(membersByName != nullptr &&
              membersByName->kind == mparser::RuntimeValueKind::Object &&
              membersByName->className == "Status" &&
              membersByName->rows == 2 && membersByName->columns == 1 &&
              membersByName->objectElements.size() == 2,
          "enumeration(className) should return a column enumeration array");
}

void runSimpleEnumerationSmoke() {
    const auto result = run(R"(classdef Direction
    enumeration
        North, South
    end
end

north = Direction.North;
south = Direction('South');
same = Direction() == north;
different = north ~= south;
)"
    );
    check(result.diagnostics.empty(),
          "simple enumeration diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "same", 1);
    checkNumber(result, "different", 1);
}

void runValueImmutabilitySmoke() {
    const auto result = run(R"(classdef Code
    properties
        Value
    end
    methods
        function obj = Code(value)
            obj.Value = value;
        end
    end
    enumeration
        One(1)
    end
end

value = Code.One;
value.Value = 2;
)"
    );
    check(hasDiagnostic(result.diagnostics,
                        "value enumeration properties are immutable"),
          "value enumeration property mutation was not rejected");
}

void runHandleEnumerationSmoke() {
    const auto result = run(R"(classdef SharedState < handle
    properties
        Count
    end
    methods
        function obj = SharedState(count)
            obj.Count = count;
        end
        function value = bump(obj)
            obj.Count = obj.Count + 1;
            value = obj.Count;
        end
    end
    enumeration
        Only(1)
    end
end

first = SharedState.Only;
second = SharedState.Only;
afterFirst = first.bump();
afterSecond = second.bump();
shared = first.Count;
same = first == second;
)"
    );
    check(result.diagnostics.empty(),
          "handle enumeration diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "afterFirst", 2);
    checkNumber(result, "afterSecond", 3);
    checkNumber(result, "shared", 3);
    checkNumber(result, "same", 1);
}

void runPrivateConstructorAndContinuationSmoke() {
    const auto result = run(R"(classdef PrivateCode
    properties
        Value
    end
    methods (Access = private)
        function obj = PrivateCode(value)
            obj.Value = value;
        end
    end
    enumeration
        One(1), ...
        Two(2)
    end
end

one = PrivateCode.One;
two = PrivateCode.Two;
total = one.Value + two.Value;
)"
    );
    check(result.diagnostics.empty(),
          "private enum constructor diagnostics:\n" +
              diagnosticsText(result.diagnostics));
    checkNumber(result, "total", 3);
}

void runRecursiveAndInvalidConversionSmoke() {
    const auto recursive = run(R"(classdef RecursiveCode
    methods
        function obj = RecursiveCode(other)
        end
    end
    enumeration
        One(RecursiveCode.One)
    end
end

value = RecursiveCode.One;
)"
    );
    check(hasDiagnostic(recursive.diagnostics,
                        "recursive enumeration member evaluation"),
          "recursive enumeration initialization was not rejected");

    const auto invalid = run(R"(classdef SmallCode
    enumeration
        One, Two
    end
end

value = SmallCode('Missing');
)"
    );
    check(hasDiagnostic(invalid.diagnostics,
                        "enumeration member is not available: "
                        "SmallCode.Missing"),
          "invalid enum string conversion was not diagnosed");
}

void runSealedAndCollisionSmoke() {
    const auto sealed = run(R"(classdef BaseEnum
    enumeration
        Only
    end
end

classdef DerivedEnum < BaseEnum
end

value = DerivedEnum();
)"
    );
    check(hasDiagnostic(sealed.diagnostics,
                        "sealed class cannot be subclassed: BaseEnum"),
          "enumeration class was not treated as implicitly sealed");

    const auto conflicts = analyze(R"(classdef Conflict
    properties
        Red
    end
    events
        Green
    end
    methods
        function value = Blue(obj)
            value = 1;
        end
    end
    enumeration
        Red, Green, Blue, Conflict, Red
    end
end
)"
    );
    check(conflicts.diagnostics.size() >= 5,
          "enumeration member collision diagnostics are incomplete");
    check(hasDiagnostic(conflicts.diagnostics,
                        "enumeration member conflicts with another class "
                        "member"),
          "class-member enumeration collision was not diagnosed");
    check(hasDiagnostic(conflicts.diagnostics,
                        "enumeration member cannot have the class name"),
          "class-name enumeration collision was not diagnosed");
}

} // namespace

int main() {
    try {
        runFrontendAndBytecodeSmoke();
        runValueEnumerationSmoke();
        runSimpleEnumerationSmoke();
        runValueImmutabilitySmoke();
        runHandleEnumerationSmoke();
        runPrivateConstructorAndContinuationSmoke();
        runRecursiveAndInvalidConversionSmoke();
        runSealedAndCollisionSmoke();
        std::cout << "enumeration class smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
