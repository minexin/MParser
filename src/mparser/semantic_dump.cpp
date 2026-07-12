#include "mparser/semantic_dump.h"

#include <cstddef>
#include <ostream>

namespace mparser {
namespace {

void indent(std::ostream& output, int depth) {
    for (int i = 0; i < depth; ++i) {
        output << "  ";
    }
}

const SemanticSymbol* symbolForBinding(const SemanticResult& result,
                                       BindingRef binding) {
    if (binding.symbolId < 0) {
        return nullptr;
    }

    const auto index = static_cast<size_t>(binding.symbolId);
    if (index >= result.symbols.size()) {
        return nullptr;
    }

    return &result.symbols[index];
}

void dumpNode(std::ostream& output, const SemanticResult& result,
              const HirNode& node, int depth) {
    indent(output, depth);
    output << hirKindName(node.kind);
    if (!node.label.empty()) {
        output << " " << node.label;
    }
    if (!node.raw.empty()) {
        output << " raw=\"" << node.raw << "\"";
    }
    if (node.binding.kind != BindingKind::Unresolved) {
        output << " binding=" << bindingKindName(node.binding.kind);
        if (const auto* symbol = symbolForBinding(result, node.binding)) {
            output << "(" << symbol->name;
            if (!symbol->typeName.empty()) {
                output << ":" << symbol->typeName;
            }
            output << ")";
        }
    }
    output << " @ " << node.span.begin.line << ":" << node.span.begin.column
           << "\n";

    for (const auto& child : node.children) {
        dumpNode(output, result, *child, depth + 1);
    }
}

} // namespace

void dumpSemanticTree(std::ostream& output, const SemanticResult& result) {
    output << "Scopes:\n";
    for (const auto& scope : result.scopes) {
        output << "  #" << scope.id << " " << scopeKindName(scope.kind);
        if (!scope.label.empty()) {
            output << " " << scope.label;
        }
        output << " parent=" << scope.parentId << "\n";
    }

    output << "Symbols:\n";
    for (const auto& symbol : result.symbols) {
        output << "  #" << symbol.id << " " << symbolKindName(symbol.kind)
               << " " << symbol.name;
        if (!symbol.typeName.empty()) {
            output << " type=" << symbol.typeName;
        }
        output << " scope=" << symbol.scopeId << "\n";
    }

    output << "HIR:\n";
    if (result.root) {
        dumpNode(output, result, *result.root, 1);
    }
}

} // namespace mparser
