#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kAbstractHierarchy = R"(classdef AbstractMeasure
    properties (Abstract, SetAccess = protected)
        Scale(1,1) double {mustBePositive}
    end
    methods (Abstract, Access = protected)
        value = transform(obj, input)
    end
    methods
        function obj = AbstractMeasure(scale)
            obj.Scale = scale;
        end

        function value = evaluate(obj, input)
            value = obj.transform(input) + obj.Scale;
        end
    end
    methods (Sealed)
        function value = kindCode(obj)
            value = 9;
        end
    end
end

classdef (Sealed) LinearMeasure < AbstractMeasure
    properties (SetAccess = protected)
        Scale = 2
    end
    methods
        function obj = LinearMeasure(scale)
            obj = obj@AbstractMeasure(scale);
        end
        function output = transform(self, input)
            output = input * self.Scale;
        end
    end
end
)";

void executeConcreteAbstractImplementationSmoke() {
    const auto result = run(std::string(kAbstractHierarchy) + R"(
obj = LinearMeasure(3);
value = obj.evaluate(4);
scale = obj.Scale;
code = obj.kindCode();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "value", 15);
    assertNumber(result, "scale", 3);
    assertNumber(result, "code", 9);
}

void rejectAbstractInstantiationSmoke() {
    const auto result = run(std::string(kAbstractHierarchy) + R"(
obj = AbstractMeasure();
)");

    assert(hasDiagnostic(result, "abstract class cannot be instantiated"));
    assert(hasDiagnostic(result, "method transform"));
    assert(hasDiagnostic(result, "property Scale"));
}

void rejectIncompleteSubclassInstantiationSmoke() {
    const auto result = run(R"(classdef AbstractSource
    methods (Abstract)
        value = read(obj)
    end
end

classdef IncompleteSource < AbstractSource
end

obj = IncompleteSource();
)");

    assert(hasDiagnostic(result, "abstract class cannot be instantiated"));
    assert(hasDiagnostic(result, "method read"));
}

void executeSubclassOfExplicitAbstractClassSmoke() {
    auto result = run(R"(classdef (Abstract) AbstractMarker
    methods
        function value = markerCode(obj)
            value = 4;
        end
    end
end

classdef MarkerImplementation < AbstractMarker
end

obj = MarkerImplementation();
value = obj.markerCode();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "value", 4);

    result = run(R"(classdef (Abstract) AbstractMarker
end

obj = AbstractMarker();
)");
    assert(hasDiagnostic(result, "abstract class cannot be instantiated"));
}

void inheritAbstractValidationSmoke() {
    const auto result = run(std::string(kAbstractHierarchy) + R"(
obj = LinearMeasure(-2);
)");

    assert(hasDiagnostic(result, "value must be positive"));
}

void enforceAbstractPropertyCompatibilitySmoke() {
    auto result = run(R"(classdef AbstractAccess
    properties (Abstract, SetAccess = protected)
        Value
    end
end

classdef BadAccess < AbstractAccess
    properties (SetAccess = private)
        Value = 1
    end
end
)");
    assert(hasDiagnostic(
        result,
        "abstract property implementation must preserve GetAccess and "
        "SetAccess"));

    result = run(R"(classdef AbstractValidated
    properties (Abstract)
        Value(1,1) double {mustBePositive}
    end
end

classdef BadValidation < AbstractValidated
    properties
        Value(1,1) double = 1
    end
end
)");
    assert(hasDiagnostic(
        result,
        "abstract property implementation cannot redefine inherited "
        "validation"));
}

void satisfyAbstractMethodAcrossBasesSmoke() {
    const auto result = run(R"(classdef AbstractOperation
    methods (Abstract)
        output = compute(obj, input)
    end
end

classdef ConcreteOperation
    methods
        function output = compute(obj, input)
            output = input + 5;
        end
    end
end

classdef CombinedOperation < AbstractOperation & ConcreteOperation
end

obj = CombinedOperation();
value = obj.compute(7);
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "value", 12);
}

void rejectSealedInheritanceAndOverrideSmoke() {
    auto result = run(R"(classdef (Sealed) FinalBase
end

classdef InvalidChild < FinalBase
end
)");
    assert(hasDiagnostic(result, "sealed class cannot be subclassed"));

    result = run(R"(classdef MethodBase
    methods (Sealed)
        function value = identity(obj)
            value = 1;
        end
    end
end

classdef MethodChild < MethodBase
    methods
        function value = identity(obj)
            value = 2;
        end
    end
end
)");
    assert(hasDiagnostic(result, "sealed method cannot be redefined"));
}

void rejectInvalidAbstractDeclarationsSmoke() {
    auto result = run(R"(classdef MissingAbstract
    methods
        output = compute(obj)
    end
end
)");
    assert(hasDiagnostic(result,
                         "method prototype must be declared Abstract"));

    result = run(R"(classdef ImplementedAbstract
    methods (Abstract)
        function output = compute(obj)
            output = 1;
        end
    end
end
)" );
    assert(hasDiagnostic(result,
                         "abstract method must be declared as a prototype"));

    result = run(R"(classdef (Sealed) SealedAbstract
    methods (Abstract)
        output = compute(obj)
    end
end
)" );
    assert(hasDiagnostic(
        result, "sealed class cannot define or inherit abstract members"));
}

} // namespace

int main() {
    executeConcreteAbstractImplementationSmoke();
    rejectAbstractInstantiationSmoke();
    rejectIncompleteSubclassInstantiationSmoke();
    executeSubclassOfExplicitAbstractClassSmoke();
    inheritAbstractValidationSmoke();
    enforceAbstractPropertyCompatibilitySmoke();
    satisfyAbstractMethodAcrossBasesSmoke();
    rejectSealedInheritanceAndOverrideSmoke();
    rejectInvalidAbstractDeclarationsSmoke();
    std::cout << "class contract smoke tests passed\n";
    return 0;
}
