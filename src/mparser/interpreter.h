#pragma once

#include "mparser/diagnostic.h"
#include "mparser/semantic.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mparser {

enum class RuntimeValueKind {
    Missing,
    Number,
    String,
    Vector,
    Matrix,
};

struct RuntimeValue {
    RuntimeValueKind kind = RuntimeValueKind::Missing;
    double number = 0.0;
    std::string text;
    std::vector<double> elements;
    size_t rows = 0;
    size_t columns = 0;
};

struct RuntimeVariable {
    std::string name;
    RuntimeValue value;
};

struct InterpreterResult {
    std::vector<RuntimeVariable> variables;
    std::vector<Diagnostic> diagnostics;
};

class Interpreter {
public:
    InterpreterResult run(const SemanticResult& semantic);
};

std::string runtimeValueToString(const RuntimeValue& value);

} // namespace mparser
