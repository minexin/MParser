#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

mparser::BytecodeVmResult run(std::string_view source) {
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

    mparser::BytecodeVm vm;
    return vm.run(bytecode, semantic);
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

void runClassRuntimeSmoke() {
    const auto result = run(R"(classdef Meter
    properties
        Value
    end
    methods
        function obj = Meter(value)
            obj.Value = value;
        end
        function result = scale(obj, factor)
            result = obj.Value * factor;
        end
    end
    methods (Static)
        function obj = make(value)
            obj = Meter(value);
        end
    end
end

meter = Meter(4);
meter.Value = 5;
scaled = meter.scale(3);
created = Meter.make(2);
created_value = created.Value;
)");

    for (const auto& diagnostic : result.diagnostics) {
        std::cerr << diagnostic.span.begin.line << ":"
                  << diagnostic.span.begin.column << ": "
                  << diagnostic.message << "\n";
    }
    assert(result.diagnostics.empty());
    const auto* meter = findVariable(result, "meter");
    assert(meter != nullptr);
    assert(meter->kind == mparser::RuntimeValueKind::Object);
    assert(meter->className == "Meter");
    assertNumber(result, "scaled", 15);
    assertNumber(result, "created_value", 2);
}

void runStaticDispatchSmoke() {
    const auto result = run(R"(classdef Meter
    methods
        function result = instanceOnly(obj)
            result = 1;
        end
    end
end

bad = Meter.instanceOnly();
)");

    assert(result.diagnostics.size() == 1);
    assert(result.diagnostics[0].message ==
           "class method is not available: Meter.instanceOnly");
}

} // namespace

int main() {
    runClassRuntimeSmoke();
    runStaticDispatchSmoke();
    std::cout << "class runtime smoke tests passed\n";
    return 0;
}
