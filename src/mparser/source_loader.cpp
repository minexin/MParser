#include "mparser/source_loader.h"

#include "mparser/lexer.h"
#include "mparser/parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mparser {
namespace {

struct InspectedSource {
    std::set<std::string> classes;
    std::set<std::string> dependencies;
};

std::filesystem::path normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
        error.clear();
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical;
}

std::string readSourceFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open input file: " +
                                 path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool isSimpleClassName(const std::string& name) {
    if (name.empty() ||
        !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 ||
          name.front() == '_')) {
        return false;
    }
    return std::all_of(
        name.begin() + 1, name.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '_';
        });
}

void collectDefinedClasses(const SyntaxNode& root,
                           std::set<std::string>& classes) {
    for (const auto& child : root.children) {
        if (child->kind == SyntaxKind::ClassDef &&
            isSimpleClassName(child->label)) {
            classes.insert(child->label);
        }
    }
}

void collectDependencies(const SyntaxNode& node,
                         std::set<std::string>& dependencies) {
    for (const auto& attribute : node.attributes) {
        for (const auto& className : attribute.metaClassNames) {
            if (isSimpleClassName(className)) {
                dependencies.insert(className);
            }
        }
    }

    if (node.kind == SyntaxKind::SuperclassList) {
        for (const auto& superclass : node.children) {
            if (isSimpleClassName(superclass->label)) {
                dependencies.insert(superclass->label);
            }
        }
    } else if (node.kind == SyntaxKind::PropertyDecl &&
               isSimpleClassName(node.property.className)) {
        dependencies.insert(node.property.className);
    } else if (node.kind == SyntaxKind::MetaClassExpr &&
               isSimpleClassName(node.label)) {
        dependencies.insert(node.label);
    } else if ((node.kind == SyntaxKind::CallOrIndexExpr ||
                node.kind == SyntaxKind::MemberAccessExpr) &&
               !node.children.empty() &&
               node.children.front()->kind == SyntaxKind::IdentifierExpr &&
               isSimpleClassName(node.children.front()->label)) {
        dependencies.insert(node.children.front()->label);
    }

    for (const auto& child : node.children) {
        collectDependencies(*child, dependencies);
    }
}

InspectedSource inspectSource(std::string_view source, size_t sourceId) {
    Lexer lexer(source, sourceId);
    Parser parser(lexer.lex());
    auto parsed = parser.parse();
    InspectedSource inspected;
    if (!parsed.root) {
        return inspected;
    }
    collectDefinedClasses(*parsed.root, inspected.classes);
    collectDependencies(*parsed.root, inspected.dependencies);
    return inspected;
}

std::vector<std::filesystem::path> buildSearchPaths(
    const std::filesystem::path& entry,
    const SourceLoaderOptions& options) {
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> seen;
    const auto append = [&](const std::filesystem::path& path) {
        const auto normalized = normalizedPath(path);
        if (seen.insert(normalized).second) {
            result.push_back(normalized);
        }
    };

    append(entry.parent_path());
    for (const auto& path : options.classPaths) {
        append(path);
    }
    return result;
}

std::optional<std::filesystem::path> findClassSource(
    const std::string& className,
    const std::filesystem::path& sourceDirectory,
    const std::vector<std::filesystem::path>& searchPaths) {
    std::vector<std::filesystem::path> directories;
    directories.push_back(normalizedPath(sourceDirectory));
    directories.insert(directories.end(), searchPaths.begin(),
                       searchPaths.end());

    std::set<std::filesystem::path> visited;
    for (const auto& directory : directories) {
        if (!visited.insert(directory).second) {
            continue;
        }
        const auto candidate = directory / (className + ".m");
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return normalizedPath(candidate);
        }
    }
    return std::nullopt;
}

} // namespace

SourceLoaderResult SourceLoader::load(
    const std::filesystem::path& entryPath,
    const SourceLoaderOptions& options) const {
    const auto entry = normalizedPath(entryPath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(entry, error) || error) {
        throw std::runtime_error("failed to open input file: " +
                                 entry.string());
    }

    SourceLoaderResult result;
    std::vector<std::filesystem::path> sourcePaths;
    std::set<std::filesystem::path> loadedPaths;
    std::set<std::string> knownClasses;
    const auto searchPaths = buildSearchPaths(entry, options);

    const auto appendSource = [&](const std::filesystem::path& path,
                                  std::string content,
                                  InspectedSource inspected) {
        loadedPaths.insert(path);
        knownClasses.insert(inspected.classes.begin(),
                            inspected.classes.end());
        sourcePaths.push_back(path);
        result.sources.push_back(
            SourceUnit{path.string(), std::move(content)});
    };

    std::string entryContent = readSourceFile(entry);
    appendSource(entry, entryContent,
                 inspectSource(entryContent, result.sources.size()));

    for (size_t sourceId = 0; sourceId < result.sources.size(); ++sourceId) {
        const auto inspected =
            inspectSource(result.sources[sourceId].content, sourceId);
        for (const auto& dependency : inspected.dependencies) {
            if (knownClasses.contains(dependency)) {
                continue;
            }
            const auto path = findClassSource(
                dependency, sourcePaths[sourceId].parent_path(), searchPaths);
            if (!path || loadedPaths.contains(*path)) {
                continue;
            }

            std::string content = readSourceFile(*path);
            auto candidate = inspectSource(content, result.sources.size());
            if (!candidate.classes.contains(dependency)) {
                continue;
            }
            appendSource(*path, std::move(content), std::move(candidate));
        }
    }

    return result;
}

} // namespace mparser
