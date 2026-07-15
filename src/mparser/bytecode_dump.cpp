#include "mparser/bytecode_dump.h"

#include <cstddef>
#include <iomanip>
#include <ostream>

namespace mparser {
namespace {

const SemanticSymbol* symbolForBinding(const SemanticResult& semantic,
                                       BindingRef binding) {
    if (binding.symbolId < 0) {
        return nullptr;
    }

    const auto index = static_cast<size_t>(binding.symbolId);
    if (index >= semantic.symbols.size()) {
        return nullptr;
    }

    return &semantic.symbols[index];
}

void dumpBinding(std::ostream& output, const SemanticResult& semantic,
                 BindingRef binding) {
    if (binding.kind == BindingKind::Unresolved) {
        return;
    }

    output << " binding=" << bindingKindName(binding.kind);
    if (const auto* symbol = symbolForBinding(semantic, binding)) {
        output << "(" << symbol->name;
        if (!symbol->typeName.empty()) {
            output << ":" << symbol->typeName;
        }
        output << ")";
    }
}

} // namespace

void dumpBytecode(std::ostream& output, const BytecodeProgram& program,
                  const SemanticResult& semantic) {
    output << "Bytecode:\n";
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& instruction = program.instructions[i];
        output << "  " << std::setw(4) << i << " "
               << bytecodeOpName(instruction.op);
        if (!instruction.operand.empty()) {
            output << " " << instruction.operand;
        }
        if (instruction.operandCount > 0) {
            output << " argc=" << instruction.operandCount;
        }
        if (instruction.resultCount != 1) {
            output << " results=" << instruction.resultCount;
        }
        if (!instruction.calleeName.empty()) {
            output << " callee=" << instruction.calleeName;
        }
        if (instruction.nullAssignment) {
            output << " null-assignment";
        }
        if (instruction.nondeterministicAssignment) {
            output << " nondeterministic-assignment";
        }
        if (instruction.target >= 0) {
            output << " target=" << instruction.target;
        }
        dumpBinding(output, semantic, instruction.binding);
        output << " @ " << instruction.span.begin.line << ":"
               << instruction.span.begin.column << "\n";
    }
}

} // namespace mparser
