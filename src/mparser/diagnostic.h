#pragma once

#include "mparser/source.h"

#include <string>
#include <utility>
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

    Diagnostic() = default;

    Diagnostic(SourceSpan sourceSpan, std::string diagnosticMessage,
               std::string diagnosticIdentifier = {},
               DiagnosticSeverity diagnosticSeverity =
                   DiagnosticSeverity::Error,
               std::vector<DiagnosticFrame> diagnosticStack = {},
               std::vector<DiagnosticCause> diagnosticCauses = {})
        : span(std::move(sourceSpan)),
          message(std::move(diagnosticMessage)),
          identifier(std::move(diagnosticIdentifier)),
          severity(diagnosticSeverity),
          stack(std::move(diagnosticStack)),
          causes(std::move(diagnosticCauses)) {}
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
