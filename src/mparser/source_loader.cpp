#include "mparser/source_loader.h"

#include "mparser/lexer.h"
#include "mparser/parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mparser {
namespace {

struct InspectedSource {
    std::set<std::string> classes;
    std::set<std::string> functions;
    std::map<std::string, bool> dependencies;
};

struct ImportSpec {
    std::string target;
    bool wildcard = false;
};

struct SourceNamespaceLocation {
    std::filesystem::path root;
    std::string name;
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

std::vector<std::string> splitClassName(std::string_view name) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= name.size()) {
        const size_t end = name.find('.', begin);
        const auto part = name.substr(
            begin, end == std::string_view::npos ? name.size() - begin
                                                 : end - begin);
        if (!isSimpleClassName(std::string(part))) {
            return {};
        }
        parts.emplace_back(part);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

std::string joinClassName(const std::vector<std::string>& parts,
                          size_t count) {
    std::string result;
    for (size_t index = 0; index < count; ++index) {
        if (!result.empty()) {
            result += '.';
        }
        result += parts[index];
    }
    return result;
}

std::string qualifyClassName(std::string_view namespaceName,
                             std::string_view className) {
    if (namespaceName.empty()) {
        return std::string(className);
    }
    return std::string(namespaceName) + "." + std::string(className);
}

SourceNamespaceLocation sourceNamespaceLocation(
    const std::filesystem::path& sourcePath) {
    SourceNamespaceLocation result{sourcePath.parent_path(), {}};
    std::vector<std::string> parts;
    auto directory = sourcePath.parent_path();
    while (!directory.empty()) {
        const std::string folder = directory.filename().string();
        if (folder.size() < 2 || folder.front() != '+' ||
            !isSimpleClassName(folder.substr(1))) {
            break;
        }
        parts.push_back(folder.substr(1));
        directory = directory.parent_path();
    }

    if (parts.empty()) {
        return result;
    }
    std::reverse(parts.begin(), parts.end());
    result.root = directory;
    result.name = joinClassName(parts, parts.size());
    return result;
}

std::optional<std::string> dottedExpressionName(const SyntaxNode& node) {
    if (node.kind == SyntaxKind::IdentifierExpr &&
        isSimpleClassName(node.label)) {
        return node.label;
    }
    if (node.kind != SyntaxKind::MemberAccessExpr ||
        node.children.empty() || !isSimpleClassName(node.label)) {
        return std::nullopt;
    }
    auto prefix = dottedExpressionName(*node.children.front());
    if (!prefix) {
        return std::nullopt;
    }
    return *prefix + "." + node.label;
}

void appendSymbolNameCandidates(
    std::string_view name, bool functionsAllowed,
    std::map<std::string, bool>& dependencies) {
    const auto parts = splitClassName(name);
    for (size_t count = parts.size(); count > 0; --count) {
        auto& allowFunctions = dependencies[joinClassName(parts, count)];
        allowFunctions = allowFunctions || functionsAllowed;
    }
}

void collectDefinedSymbols(const SyntaxNode& root,
                           InspectedSource& inspected) {
    for (const auto& child : root.children) {
        if (child->kind == SyntaxKind::ClassDef &&
            isSimpleClassName(child->label)) {
            inspected.classes.insert(child->label);
        }
    }
    if (!root.children.empty() &&
        root.children.front()->kind == SyntaxKind::FunctionDef &&
        isSimpleClassName(root.children.front()->label)) {
        inspected.functions.insert(root.children.front()->label);
    }
}

void collectImports(const SyntaxNode& node,
                    std::vector<ImportSpec>& imports) {
    if (node.kind == SyntaxKind::ImportStatement) {
        for (const auto& item : node.children) {
            if (item->kind != SyntaxKind::ImportItem ||
                item->label.empty()) {
                continue;
            }
            ImportSpec spec;
            spec.target = item->label;
            if (spec.target.ends_with(".*")) {
                spec.target.resize(spec.target.size() - 2);
                spec.wildcard = true;
            }
            if (!splitClassName(spec.target).empty()) {
                imports.push_back(std::move(spec));
            }
        }
        return;
    }

    for (const auto& child : node.children) {
        collectImports(*child, imports);
    }
}

void collectRawDependencies(const SyntaxNode& node,
                            std::set<std::string>& dependencies) {
    if (node.kind == SyntaxKind::ImportStatement ||
        node.kind == SyntaxKind::ImportItem) {
        return;
    }
    for (const auto& attribute : node.attributes) {
        for (const auto& className : attribute.metaClassNames) {
            if (!splitClassName(className).empty()) {
                dependencies.insert(className);
            }
        }
    }

    if (node.kind == SyntaxKind::SuperclassList) {
        for (const auto& superclass : node.children) {
            if (!splitClassName(superclass->label).empty()) {
                dependencies.insert(superclass->label);
            }
        }
    } else if (node.kind == SyntaxKind::PropertyDecl &&
               !splitClassName(node.property.className).empty()) {
        dependencies.insert(node.property.className);
    } else if (node.kind == SyntaxKind::MetaClassExpr &&
               !splitClassName(node.label).empty()) {
        dependencies.insert(node.label);
    } else if (node.kind == SyntaxKind::CallOrIndexExpr &&
               !node.children.empty()) {
        if (const auto name =
                dottedExpressionName(*node.children.front())) {
            dependencies.insert(*name);
        }
    } else if (node.kind == SyntaxKind::MemberAccessExpr) {
        if (const auto name = dottedExpressionName(node)) {
            dependencies.insert(*name);
        }
    }

    for (const auto& child : node.children) {
        collectRawDependencies(*child, dependencies);
    }
}

std::string finalNameSegment(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    return std::string(dot == std::string_view::npos
                           ? name
                           : name.substr(dot + 1));
}

InspectedSource inspectSource(std::string_view source, size_t sourceId) {
    Lexer lexer(source, sourceId);
    Parser parser(lexer.lex());
    auto parsed = parser.parse();
    InspectedSource inspected;
    if (!parsed.root) {
        return inspected;
    }
    collectDefinedSymbols(*parsed.root, inspected);

    std::set<std::string> rawDependencies;
    collectRawDependencies(*parsed.root, rawDependencies);
    std::vector<ImportSpec> imports;
    collectImports(*parsed.root, imports);

    std::vector<ImportSpec> explicitImports;
    std::vector<std::string> wildcardImports;
    for (const auto& import : imports) {
        if (import.wildcard) {
            wildcardImports.push_back(import.target);
            continue;
        }
        explicitImports.push_back(import);
        appendSymbolNameCandidates(import.target, true,
                                   inspected.dependencies);
    }

    for (const auto& dependency : rawDependencies) {
        for (const auto& import : explicitImports) {
            const std::string alias = finalNameSegment(import.target);
            if (dependency == alias ||
                dependency.starts_with(alias + ".")) {
                appendSymbolNameCandidates(
                    import.target + dependency.substr(alias.size()), true,
                    inspected.dependencies);
            }
        }
        appendSymbolNameCandidates(
            dependency, dependency.find('.') != std::string::npos,
            inspected.dependencies);
        for (const auto& wildcard : wildcardImports) {
            appendSymbolNameCandidates(wildcard + "." + dependency, true,
                                       inspected.dependencies);
        }
    }
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

    append(sourceNamespaceLocation(entry).root);
    for (const auto& path : options.classPaths) {
        append(path);
    }
    return result;
}

std::filesystem::path symbolSourceRelativePath(
    const std::string& symbolName) {
    const auto parts = splitClassName(symbolName);
    std::filesystem::path result;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        result /= "+" + parts[index];
    }
    if (!parts.empty()) {
        result /= parts.back() + ".m";
    }
    return result;
}

std::optional<std::filesystem::path> findSymbolSource(
    const std::string& symbolName,
    const std::filesystem::path& sourcePath,
    const std::vector<std::filesystem::path>& searchPaths) {
    const auto relative = symbolSourceRelativePath(symbolName);
    if (relative.empty()) {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> directories;
    directories.push_back(
        normalizedPath(sourceNamespaceLocation(sourcePath).root));
    directories.insert(directories.end(), searchPaths.begin(),
                       searchPaths.end());

    std::set<std::filesystem::path> visited;
    for (const auto& directory : directories) {
        if (!visited.insert(directory).second) {
            continue;
        }
        const auto candidate = directory / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return normalizedPath(candidate);
        }
    }
    return std::nullopt;
}

size_t symbolNameDepth(std::string_view name) {
    return static_cast<size_t>(
               std::count(name.begin(), name.end(), '.')) +
           1;
}

bool coveredByKnownSymbol(const std::string& dependency,
                          bool functionsAllowed,
                          const std::set<std::string>& knownClasses,
                          const std::set<std::string>& knownFunctions) {
    const auto covered = [&dependency](const auto& knownSymbols) {
        for (const auto& known : knownSymbols) {
            if (known == dependency ||
                (known.size() > dependency.size() &&
                 known.starts_with(dependency + "."))) {
                return true;
            }
        }
        return false;
    };
    return covered(knownClasses) ||
           (functionsAllowed && covered(knownFunctions));
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
    std::set<std::string> knownFunctions;
    const auto searchPaths = buildSearchPaths(entry, options);

    const auto appendSource = [&](const std::filesystem::path& path,
                                  std::string content,
                                  InspectedSource inspected,
                                  std::string namespaceName) {
        loadedPaths.insert(path);
        for (const auto& className : inspected.classes) {
            knownClasses.insert(
                qualifyClassName(namespaceName, className));
        }
        for (const auto& functionName : inspected.functions) {
            knownFunctions.insert(
                qualifyClassName(namespaceName, functionName));
        }
        sourcePaths.push_back(path);
        result.sources.push_back(SourceUnit{
            path.string(), std::move(content), std::move(namespaceName)});
    };

    std::string entryContent = readSourceFile(entry);
    const auto entryNamespace = sourceNamespaceLocation(entry).name;
    appendSource(entry, entryContent,
                 inspectSource(entryContent, result.sources.size()),
                 entryNamespace);

    for (size_t sourceId = 0; sourceId < result.sources.size(); ++sourceId) {
        const auto inspected =
            inspectSource(result.sources[sourceId].content, sourceId);
        std::vector<std::pair<std::string, bool>> dependencies(
            inspected.dependencies.begin(), inspected.dependencies.end());
        std::sort(dependencies.begin(), dependencies.end(),
                  [](const auto& left, const auto& right) {
                      const size_t leftDepth = symbolNameDepth(left.first);
                      const size_t rightDepth = symbolNameDepth(right.first);
                      return leftDepth != rightDepth
                                 ? leftDepth > rightDepth
                                 : left.first < right.first;
                  });
        for (const auto& [dependency, functionsAllowed] : dependencies) {
            if (coveredByKnownSymbol(dependency, functionsAllowed,
                                     knownClasses, knownFunctions)) {
                continue;
            }
            const auto path = findSymbolSource(
                dependency, sourcePaths[sourceId], searchPaths);
            if (!path || loadedPaths.contains(*path)) {
                continue;
            }

            std::string content = readSourceFile(*path);
            auto candidate = inspectSource(content, result.sources.size());
            const auto namespaceName =
                sourceNamespaceLocation(*path).name;
            bool definesDependency = false;
            for (const auto& className : candidate.classes) {
                if (qualifyClassName(namespaceName, className) ==
                    dependency) {
                    definesDependency = true;
                    break;
                }
            }
            if (!definesDependency && functionsAllowed) {
                for (const auto& functionName : candidate.functions) {
                    if (qualifyClassName(namespaceName, functionName) ==
                        dependency) {
                        definesDependency = true;
                        break;
                    }
                }
            }
            if (!definesDependency) {
                continue;
            }
            appendSource(*path, std::move(content), std::move(candidate),
                         namespaceName);
        }
    }

    return result;
}

} // namespace mparser
