#include "mparser/syntax_dump.h"

#include <cstddef>
#include <ostream>

namespace mparser {
namespace {

void indent(std::ostream& output, int depth) {
    for (int i = 0; i < depth; ++i) {
        output << "  ";
    }
}

void dumpAttributes(std::ostream& output,
                    const std::vector<AttributeSyntax>& attributes) {
    if (attributes.empty()) {
        return;
    }

    output << " attrs=[";
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) {
            output << ", ";
        }
        if (attributes[i].negated) {
            output << "~";
        }
        output << attributes[i].name;
        if (!attributes[i].value.empty()) {
            output << "=" << attributes[i].value;
        }
        if (attributes[i].name.empty()) {
            output << attributes[i].raw;
        }
    }
    output << "]";
}

void dumpPropertySpec(std::ostream& output, const PropertySpec& property) {
    if (!property.dimensions.empty()) {
        output << " size=(";
        for (size_t index = 0; index < property.dimensions.size(); ++index) {
            if (index != 0) {
                output << ",";
            }
            output << property.dimensions[index].text;
        }
        output << ")";
    }
    if (!property.className.empty()) {
        output << " type=" << property.className;
    }
    if (!property.validators.empty()) {
        output << " validators={";
        for (size_t index = 0; index < property.validators.size(); ++index) {
            if (index != 0) {
                output << ",";
            }
            output << property.validators[index].raw;
        }
        output << "}";
    }
    if (property.hasExplicitDefault) {
        output << " default=explicit";
    }
}

void dumpNode(std::ostream& output, const SyntaxNode& node, int depth) {
    indent(output, depth);
    output << syntaxKindName(node.kind);
    if (!node.label.empty()) {
        output << " " << node.label;
    }
    dumpAttributes(output, node.attributes);
    if (node.kind == SyntaxKind::ArgumentsBlock) {
        output << " group=" << argumentBlockKindName(node.argumentBlock.kind);
    }
    if (node.kind == SyntaxKind::PropertyDecl ||
        node.kind == SyntaxKind::ArgumentDecl) {
        dumpPropertySpec(output, node.property);
    }
    if (!node.raw.empty()) {
        output << " raw=\"" << node.raw << "\"";
    }
    output << " @ " << node.span.begin.line << ":" << node.span.begin.column
           << "\n";

    for (const auto& child : node.children) {
        dumpNode(output, *child, depth + 1);
    }
}

} // namespace

void dumpSyntaxTree(std::ostream& output, const SyntaxNode& node) {
    dumpNode(output, node, 0);
}

} // namespace mparser
