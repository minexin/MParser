#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kSeed = 0x4D505253u;
constexpr size_t kGeneratedCaseCount = 128;
constexpr size_t kMaxSourceBytes = 768;
constexpr size_t kMaxMutationsPerCase = 6;
constexpr size_t kMinimumSemanticCases = 32;

void appendString(std::ostream& output, std::string_view value) {
    output << value.size() << ':';
    output.write(value.data(),
                 static_cast<std::streamsize>(value.size()));
    output << ';';
}

void appendPosition(std::ostream& output,
                    const mparser::SourcePosition& position) {
    output << position.offset << ',' << position.line << ','
           << position.column << ',' << position.sourceId << ';';
}

void appendSpan(std::ostream& output, const mparser::SourceSpan& span) {
    appendPosition(output, span.begin);
    appendPosition(output, span.end);
}

void appendStringVector(std::ostream& output,
                        const std::vector<std::string>& values) {
    output << values.size() << ';';
    for (const auto& value : values) {
        appendString(output, value);
    }
}

void appendDiagnosticFrame(std::ostream& output,
                           const mparser::DiagnosticFrame& frame) {
    appendString(output, frame.file);
    appendString(output, frame.name);
    output << frame.line << ';';
}

void appendDiagnosticCause(std::ostream& output,
                           const mparser::DiagnosticCause& cause) {
    appendString(output, cause.identifier);
    appendString(output, cause.message);
    output << cause.stack.size() << ';';
    for (const auto& frame : cause.stack) {
        appendDiagnosticFrame(output, frame);
    }
    output << cause.causes.size() << ';';
    for (const auto& nested : cause.causes) {
        appendDiagnosticCause(output, nested);
    }
}

std::string diagnosticFingerprint(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::ostringstream output;
    output << diagnostics.size() << ';';
    for (const auto& diagnostic : diagnostics) {
        appendSpan(output, diagnostic.span);
        appendString(output, diagnostic.message);
        appendString(output, diagnostic.identifier);
        output << static_cast<int>(diagnostic.severity) << ';';
        output << diagnostic.stack.size() << ';';
        for (const auto& frame : diagnostic.stack) {
            appendDiagnosticFrame(output, frame);
        }
        output << diagnostic.causes.size() << ';';
        for (const auto& cause : diagnostic.causes) {
            appendDiagnosticCause(output, cause);
        }
    }
    return output.str();
}

std::string tokenFingerprint(const std::vector<mparser::Token>& tokens) {
    std::ostringstream output;
    output << tokens.size() << ';';
    for (const auto& token : tokens) {
        output << static_cast<int>(token.kind) << ';';
        appendString(output, token.text);
        appendSpan(output, token.span);
        output << token.leadingTrivia.size() << ';';
        for (const auto& trivia : token.leadingTrivia) {
            output << static_cast<int>(trivia.kind) << ';';
            appendString(output, trivia.text);
            appendSpan(output, trivia.span);
        }
    }
    return output.str();
}

void appendArgumentBlock(std::ostream& output,
                         const mparser::ArgumentBlockSpec& block) {
    output << static_cast<int>(block.kind) << ','
           << block.explicitInput << ',' << block.explicitOutput << ','
           << block.repeating << ',' << block.valid << ';';
}

void appendAttribute(std::ostream& output,
                     const mparser::AttributeSyntax& attribute) {
    appendString(output, attribute.name);
    appendString(output, attribute.value);
    appendString(output, attribute.raw);
    output << attribute.negated << ',' << attribute.hasMetaClassList << ';';
    appendStringVector(output, attribute.metaClassNames);
    appendSpan(output, attribute.span);
}

void appendProperty(std::ostream& output,
                    const mparser::PropertySpec& property) {
    output << property.dimensions.size() << ';';
    for (const auto& dimension : property.dimensions) {
        appendString(output, dimension.text);
        appendSpan(output, dimension.span);
    }
    appendString(output, property.className);
    appendSpan(output, property.classSpan);
    output << property.validators.size() << ';';
    for (const auto& validator : property.validators) {
        appendString(output, validator.name);
        appendString(output, validator.raw);
        appendSpan(output, validator.span);
        appendStringVector(output, validator.arguments);
    }
    output << property.hasExplicitDefault << ';';
}

void appendSyntaxNode(std::ostream& output,
                      const mparser::SyntaxNode& node) {
    output << static_cast<int>(node.kind) << ';';
    appendString(output, node.label);
    appendString(output, node.raw);
    appendSpan(output, node.span);
    output << node.attributes.size() << ';';
    for (const auto& attribute : node.attributes) {
        appendAttribute(output, attribute);
    }
    appendArgumentBlock(output, node.argumentBlock);
    appendString(output, node.nameValueSourceClass);
    appendSpan(output, node.nameValueSourceSpan);
    appendProperty(output, node.property);
    output << node.children.size() << ';';
    for (const auto& child : node.children) {
        appendSyntaxNode(output, *child);
    }
}

std::string syntaxFingerprint(const mparser::SyntaxNode& root) {
    std::ostringstream output;
    appendSyntaxNode(output, root);
    return output.str();
}

void appendBinding(std::ostream& output,
                   const mparser::BindingRef& binding) {
    output << static_cast<int>(binding.kind) << ',' << binding.symbolId
           << ';';
}

void appendHirNode(std::ostream& output, const mparser::HirNode& node) {
    output << static_cast<int>(node.kind) << ';';
    appendString(output, node.label);
    appendString(output, node.raw);
    appendString(output, node.lexicalClassName);
    appendSpan(output, node.span);
    appendBinding(output, node.binding);
    output << node.attributes.size() << ';';
    for (const auto& attribute : node.attributes) {
        appendAttribute(output, attribute);
    }
    appendArgumentBlock(output, node.argumentBlock);
    appendString(output, node.nameValueSourceClass);
    appendSpan(output, node.nameValueSourceSpan);
    appendStringVector(output, node.superclasses);
    appendProperty(output, node.property);
    output << node.children.size() << ';';
    for (const auto& child : node.children) {
        appendHirNode(output, *child);
    }
}

std::string semanticFingerprint(const mparser::SemanticResult& result) {
    std::ostringstream output;
    output << result.scopes.size() << ';';
    for (const auto& scope : result.scopes) {
        output << scope.id << ',' << scope.parentId << ','
               << static_cast<int>(scope.kind) << ';';
        appendString(output, scope.label);
        output << scope.symbols.size() << ';';
        for (const auto symbol : scope.symbols) {
            output << symbol << ';';
        }
    }

    output << result.symbols.size() << ';';
    for (const auto& symbol : result.symbols) {
        output << symbol.id << ',' << static_cast<int>(symbol.kind) << ','
               << symbol.scopeId << ';';
        appendString(output, symbol.name);
        appendString(output, symbol.typeName);
        appendSpan(output, symbol.span);
    }

    output << result.sources.size() << ';';
    for (const auto& source : result.sources) {
        appendString(output, source.name);
        appendString(output, source.namespaceName);
    }

    output << static_cast<bool>(result.builtinRegistry) << ';';
    if (result.root) {
        output << "root;";
        appendHirNode(output, *result.root);
    } else {
        output << "no-root;";
    }
    return output.str();
}

struct PipelineSnapshot {
    std::string tokens;
    std::string syntax;
    std::string parseDiagnostics;
    std::string semantic;
    std::string semanticDiagnostics;
    size_t tokenCount = 0;
    size_t parseDiagnosticCount = 0;
    size_t semanticDiagnosticCount = 0;
    bool semanticRan = false;
};

PipelineSnapshot runPipeline(std::string_view source) {
    PipelineSnapshot snapshot;

    mparser::Lexer lexer(source, 0);
    auto tokens = lexer.lex();
    snapshot.tokenCount = tokens.size();
    snapshot.tokens = tokenFingerprint(tokens);

    mparser::Parser parser(std::move(tokens));
    auto parsed = parser.parse();
    snapshot.parseDiagnosticCount = parsed.diagnostics.size();
    snapshot.parseDiagnostics =
        diagnosticFingerprint(parsed.diagnostics);
    if (!parsed.root) {
        throw std::runtime_error("parser returned a null syntax root");
    }
    snapshot.syntax = syntaxFingerprint(*parsed.root);

    // Match the production pipeline: semantic analysis only sees accepted
    // syntax trees, while malformed inputs still exercise Lexer and Parser.
    if (parsed.diagnostics.empty()) {
        mparser::SourceUnit unit;
        unit.name = "<parser-semantic-fuzz>";
        unit.content = std::string(source);

        mparser::SemanticAnalyzer analyzer;
        auto semantic = analyzer.analyze(*parsed.root, {unit});
        snapshot.semanticRan = true;
        snapshot.semanticDiagnosticCount =
            semantic.diagnostics.size();
        snapshot.semanticDiagnostics =
            diagnosticFingerprint(semantic.diagnostics);
        snapshot.semantic = semanticFingerprint(semantic);
    }

    return snapshot;
}

std::string_view firstMismatch(const PipelineSnapshot& first,
                               const PipelineSnapshot& second) {
    if (first.tokens != second.tokens ||
        first.tokenCount != second.tokenCount) {
        return "Lexer token stream";
    }
    if (first.syntax != second.syntax) {
        return "Parser syntax tree";
    }
    if (first.parseDiagnostics != second.parseDiagnostics ||
        first.parseDiagnosticCount != second.parseDiagnosticCount) {
        return "Parser diagnostics";
    }
    if (first.semanticRan != second.semanticRan) {
        return "Semantic execution decision";
    }
    if (first.semantic != second.semantic) {
        return "Semantic HIR, scope, or symbol state";
    }
    if (first.semanticDiagnostics != second.semanticDiagnostics ||
        first.semanticDiagnosticCount !=
            second.semanticDiagnosticCount) {
        return "Semantic diagnostics";
    }
    return {};
}

size_t nextBounded(std::mt19937& engine, size_t bound) {
    if (bound == 0) {
        throw std::logic_error("fuzz generator received an empty range");
    }
    return static_cast<size_t>(engine()) % bound;
}

std::vector<std::string> corpusSources() {
    return {
        "",
        "% comment-only compilation unit\n",
        R"(x = [1 2; 3 4];
y = sin(x(1, end)) + pi;
)",
        R"(function y = bounded_loop(x)
y = 0;
for i = 1:4
    if x > i
        y = y + i;
    else
        y = y - 1;
    end
end
end
)",
        R"(function [y, values] = handles_and_cells(x)
h = @(t) t.^2 + x;
y = h(3);
values = {x, y, h};
end
)",
        R"(classdef FuzzBox < handle
    properties (SetAccess = private)
        Value (1,1) double = 0
    end
    methods
        function obj = FuzzBox(v)
            obj.Value = v;
        end
        function y = scale(obj, x)
            y = obj.Value * x;
        end
    end
end
)",
        R"(function y = branching(mode)
try
    switch mode
        case 1
            y = 10;
        otherwise
            y = -1;
    end
catch err
    y = 0;
end
end
)",
        R"(global fuzzShared
function y = workspace_case(x)
persistent count
global fuzzShared
count = count + 1;
fuzzShared = x;
y = count + fuzzShared;
end
)",
        R"(text = "double quoted";
legacy = 'single quoted';
matrix = [1, 2; 3, 4]';
cellValue = {text, legacy, matrix};
)",
        "function y = missing_end(x)\ny = x + 1;\n",
        "classdef Broken\nproperties\nValue (1,1 double\nend\n",
        "x = ([1, 2};\ny = @() ?pkg.Type;\n",
    };
}

std::string structuredSource(std::mt19937& engine, size_t index) {
    const auto suffix =
        std::to_string(index) + "_" +
        std::to_string(nextBounded(engine, 1000));
    const auto firstValue = 1 + nextBounded(engine, 9);
    const auto secondValue = 1 + nextBounded(engine, 9);
    std::ostringstream source;

    switch (index % 6) {
    case 0:
        source << "value" << suffix << " = [" << firstValue << " "
               << secondValue << "; " << secondValue << " "
               << firstValue << "];\n"
               << "result" << suffix << " = value" << suffix
               << "(1, end) + sin(" << firstValue << ");\n";
        break;
    case 1:
        source << "function y = fuzzFunction" << suffix << "(x)\n"
               << "y = 0;\n"
               << "for i = 1:" << firstValue << "\n"
               << "    if x >= i\n"
               << "        y = y + i;\n"
               << "    else\n"
               << "        y = y - " << secondValue << ";\n"
               << "    end\n"
               << "end\n"
               << "end\n";
        break;
    case 2:
        source << "function [y, values] = fuzzHandle" << suffix
               << "(x)\n"
               << "h = @(t) t.^2 + x;\n"
               << "y = h(" << firstValue << ");\n"
               << "values = {x, y, h};\n"
               << "end\n";
        break;
    case 3:
        source << "classdef FuzzClass" << suffix << " < handle\n"
               << "    properties (SetAccess = private)\n"
               << "        Value (1,1) double = " << firstValue << "\n"
               << "    end\n"
               << "    methods\n"
               << "        function obj = FuzzClass" << suffix
               << "(v)\n"
               << "            obj.Value = v;\n"
               << "        end\n"
               << "        function y = scale(obj, x)\n"
               << "            y = obj.Value * x;\n"
               << "        end\n"
               << "    end\n"
               << "end\n";
        break;
    case 4:
        source << "function y = fuzzSwitch" << suffix << "(mode)\n"
               << "try\n"
               << "    switch mode\n"
               << "        case " << firstValue << "\n"
               << "            y = " << secondValue << ";\n"
               << "        otherwise\n"
               << "            y = -1;\n"
               << "    end\n"
               << "catch err\n"
               << "    y = 0;\n"
               << "end\n"
               << "end\n";
        break;
    default:
        source << "global shared" << suffix << "\n"
               << "function y = fuzzWorkspace" << suffix << "(x)\n"
               << "persistent count" << suffix << "\n"
               << "global shared" << suffix << "\n"
               << "count" << suffix << " = count" << suffix
               << " + 1;\n"
               << "shared" << suffix << " = x;\n"
               << "y = count" << suffix << " + shared" << suffix
               << ";\n"
               << "end\n";
        break;
    }

    auto result = source.str();
    result.resize(std::min(result.size(), kMaxSourceBytes));
    return result;
}

void insertBounded(std::string& source, size_t position,
                   std::string_view fragment) {
    if (source.size() >= kMaxSourceBytes || fragment.empty()) {
        return;
    }
    const auto count =
        std::min(fragment.size(), kMaxSourceBytes - source.size());
    source.insert(position, fragment.data(), count);
}

std::string mutatedSource(std::mt19937& engine,
                          const std::vector<std::string>& corpus) {
    static constexpr std::array<std::string_view, 28> fragments{
        "\n",       " ",       ";",          ",",
        "end",      "function", "classdef",   "properties",
        "methods",  "if",      "else",       "for",
        "try",      "catch",   "(",          ")",
        "[",        "]",       "{",          "}",
        "'",        "\"",      "% fuzz\n",   "...",
        "=",        "~=",      "@(x)",       "?pkg.Type",
    };
    static constexpr std::string_view replacementCharacters =
        "abcXYZ019_+-*/\\^=()[]{}.,;:'\"@?% \n";

    std::string source = corpus[nextBounded(engine, corpus.size())];
    const auto mutationCount =
        1 + nextBounded(engine, kMaxMutationsPerCase);
    for (size_t mutation = 0; mutation < mutationCount; ++mutation) {
        switch (nextBounded(engine, 6)) {
        case 0: {
            const auto fragment =
                fragments[nextBounded(engine, fragments.size())];
            insertBounded(source,
                          nextBounded(engine, source.size() + 1),
                          fragment);
            break;
        }
        case 1:
            if (source.empty()) {
                insertBounded(source, 0, "\n");
            } else {
                const auto begin =
                    nextBounded(engine, source.size());
                const auto available =
                    std::min<size_t>(16, source.size() - begin);
                const auto count =
                    1 + nextBounded(engine, available);
                source.erase(begin, count);
            }
            break;
        case 2:
            if (source.empty()) {
                insertBounded(source, 0, "x");
            } else {
                source[nextBounded(engine, source.size())] =
                    replacementCharacters[nextBounded(
                        engine, replacementCharacters.size())];
            }
            break;
        case 3:
            if (!source.empty() &&
                source.size() < kMaxSourceBytes) {
                const auto begin =
                    nextBounded(engine, source.size());
                const auto available =
                    std::min<size_t>(24, source.size() - begin);
                const auto count =
                    1 + nextBounded(engine, available);
                const auto copy = source.substr(begin, count);
                insertBounded(
                    source,
                    nextBounded(engine, source.size() + 1), copy);
            }
            break;
        case 4:
            insertBounded(source, 0,
                          fragments[nextBounded(engine,
                                                fragments.size())]);
            break;
        default:
            insertBounded(
                source, source.size(),
                fragments[nextBounded(engine, fragments.size())]);
            break;
        }
        source.resize(std::min(source.size(), kMaxSourceBytes));
    }
    return source;
}

std::vector<std::string> generateCases(std::uint32_t seed) {
    auto cases = corpusSources();
    const auto corpus = cases;
    std::mt19937 engine(seed);
    for (size_t index = 0; index < kGeneratedCaseCount; ++index) {
        if ((index % 2) == 0) {
            cases.push_back(structuredSource(engine, index));
        } else {
            cases.push_back(mutatedSource(engine, corpus));
        }
    }
    return cases;
}

void reportFailure(std::uint32_t seed, size_t caseIndex,
                   std::string_view reason, std::string_view source) {
    std::cerr << "parser/semantic fuzz failure\n"
              << "seed=0x" << std::hex << std::uppercase << seed
              << std::dec << "\n"
              << "case=" << caseIndex << "\n"
              << "reason=" << reason << "\n"
              << "--- source begin ---\n";
    std::cerr.write(source.data(),
                    static_cast<std::streamsize>(source.size()));
    if (source.empty() || source.back() != '\n') {
        std::cerr << '\n';
    }
    std::cerr << "--- source end ---\n";
}

} // namespace

int main() {
    std::vector<std::string> cases;
    try {
        cases = generateCases(kSeed);
        const auto repeatedCases = generateCases(kSeed);
        if (cases.size() != repeatedCases.size()) {
            reportFailure(kSeed, 0, "source generator size changed", {});
            return 1;
        }
        for (size_t index = 0; index < cases.size(); ++index) {
            if (cases[index] != repeatedCases[index]) {
                reportFailure(kSeed, index,
                              "source generator is nondeterministic",
                              cases[index]);
                return 1;
            }
        }
    } catch (const std::exception& error) {
        reportFailure(kSeed, 0, error.what(), {});
        return 1;
    }

    size_t semanticCases = 0;
    size_t parseDiagnosticCases = 0;
    size_t tokenCount = 0;
    for (size_t index = 0; index < cases.size(); ++index) {
        try {
            const auto first = runPipeline(cases[index]);
            const auto second = runPipeline(cases[index]);
            if (const auto mismatch = firstMismatch(first, second);
                !mismatch.empty()) {
                reportFailure(kSeed, index,
                              std::string("nondeterministic ") +
                                  std::string(mismatch),
                              cases[index]);
                return 1;
            }

            semanticCases += first.semanticRan ? 1 : 0;
            parseDiagnosticCases +=
                first.parseDiagnosticCount != 0 ? 1 : 0;
            tokenCount += first.tokenCount;
        } catch (const std::exception& error) {
            reportFailure(kSeed, index, error.what(), cases[index]);
            return 1;
        } catch (...) {
            reportFailure(kSeed, index, "unknown exception",
                          cases[index]);
            return 1;
        }
    }

    if (semanticCases < kMinimumSemanticCases) {
        reportFailure(kSeed, 0,
                      "too few accepted inputs reached Semantic",
                      cases.front());
        return 1;
    }
    if (parseDiagnosticCases == 0) {
        reportFailure(kSeed, 0,
                      "no malformed input produced Parser diagnostics",
                      cases.front());
        return 1;
    }

    std::cout << "parser/semantic deterministic fuzz tests passed: "
              << "seed=0x" << std::hex << std::uppercase << kSeed
              << std::dec << ", cases=" << cases.size()
              << ", semantic=" << semanticCases
              << ", parse-diagnostic=" << parseDiagnosticCases
              << ", tokens=" << tokenCount
              << ", max-source-bytes=" << kMaxSourceBytes << "\n";
    return 0;
}
