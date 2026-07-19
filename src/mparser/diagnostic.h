#pragma once

#include "mparser/source.h"

#include <string>
#include <vector>

namespace mparser {

enum class DiagnosticSeverity {
    Error,
    Warning,
};

struct DiagnosticFrame {
    std::string file;
    std::string name;
    int line = 1;
};

struct DiagnosticCause {
    std::string identifier;
    std::string message;
    std::vector<DiagnosticFrame> stack;
    std::vector<DiagnosticCause> causes;
};

struct Diagnostic {
    SourceSpan span;
    std::string message;
    std::string identifier;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::vector<DiagnosticFrame> stack;
    std::vector<DiagnosticCause> causes;
};

inline bool isErrorDiagnostic(const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
}

inline bool hasErrorDiagnostics(
    const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (isErrorDiagnostic(diagnostic)) {
            return true;
        }
    }
    return false;
}

} // namespace mparser
