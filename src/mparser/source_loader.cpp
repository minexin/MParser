#include "mparser/source_loader.h"

#include "mparser/filesystem_utf8.h"
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
                                 pathToUtf8(path));
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
    std::vector<std::string> parts;
    auto directory = sourcePath.parent_path();
    const std::string immediateFolder =
        pathToUtf8(directory.filename());
    if (immediateFolder.size() >= 2 && immediateFolder.front() == '@' &&
        isSimpleClassName(immediateFolder.substr(1))) {
        directory = directory.parent_path();
    }
    SourceNamespaceLocation result{directory, {}};
    while (!directory.empty()) {
        const std::string folder =
            pathToUtf8(directory.filename());
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

void recordRawDependency(std::map<std::string, bool>& dependencies,
                         const std::string& name,
                         bool functionsAllowed) {
    auto& existing = dependencies[name];
    existing = existing || functionsAllowed;
}

std::optional<std::string> staticStringLiteral(const SyntaxNode& node) {
    if (node.kind != SyntaxKind::StringLiteralExpr || node.raw.size() < 2) {
        return std::nullopt;
    }
    const char quote = node.raw.front();
    if ((quote != '\'' && quote != '"') || node.raw.back() != quote) {
        return std::nullopt;
    }
    std::string value;
    for (size_t index = 1; index + 1 < node.raw.size(); ++index) {
        if (node.raw[index] == quote && index + 1 < node.raw.size() - 1 &&
            node.raw[index + 1] == quote) {
            value.push_back(quote);
            ++index;
        } else {
            value.push_back(node.raw[index]);
        }
    }
    return value;
}

void collectRawDependencies(const SyntaxNode& node,
                            std::map<std::string, bool>& dependencies) {
    if (node.kind == SyntaxKind::ImportStatement ||
        node.kind == SyntaxKind::ImportItem) {
        return;
    }
    for (const auto& attribute : node.attributes) {
        for (const auto& className : attribute.metaClassNames) {
            if (!splitClassName(className).empty()) {
                recordRawDependency(dependencies, className, false);
            }
        }
    }

    if (node.kind == SyntaxKind::SuperclassList) {
        for (const auto& superclass : node.children) {
            if (!splitClassName(superclass->label).empty()) {
                recordRawDependency(dependencies, superclass->label, false);
            }
        }
    } else if (node.kind == SyntaxKind::PropertyDecl &&
               !splitClassName(node.property.className).empty()) {
        recordRawDependency(dependencies, node.property.className, false);
    } else if (node.kind == SyntaxKind::MetaClassExpr &&
               !splitClassName(node.label).empty()) {
        recordRawDependency(dependencies, node.label, false);
    } else if (node.kind == SyntaxKind::FunctionHandleExpr &&
               node.label != "@()" &&
               !splitClassName(node.label).empty()) {
        recordRawDependency(dependencies, node.label, true);
    } else if (node.kind == SyntaxKind::CallOrIndexExpr &&
               !node.children.empty()) {
        if (const auto name =
                dottedExpressionName(*node.children.front())) {
            recordRawDependency(dependencies, *name, true);
            if (*name == "enumeration" && node.children.size() == 2) {
                if (const auto className =
                        staticStringLiteral(*node.children[1]);
                    className && !splitClassName(*className).empty()) {
                    recordRawDependency(dependencies, *className, false);
                }
            } else if ((*name == "feval" || *name == "str2func") &&
                       node.children.size() >= 2) {
                if (const auto functionName =
                        staticStringLiteral(*node.children[1]);
                    functionName && !splitClassName(*functionName).empty()) {
                    recordRawDependency(dependencies, *functionName, true);
                }
            }
        }
    } else if (node.kind == SyntaxKind::MemberAccessExpr) {
        if (const auto name = dottedExpressionName(node)) {
            recordRawDependency(dependencies, *name, true);
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

    std::map<std::string, bool> rawDependencies;
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

    for (const auto& [dependency, rawFunctionsAllowed] : rawDependencies) {
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
            dependency,
            rawFunctionsAllowed ||
                dependency.find('.') != std::string::npos,
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
    for (const auto& path : options.searchPaths) {
        if (normalizedPath(path).filename() == "private") {
            continue;
        }
        append(path);
    }
    return result;
}

std::vector<std::filesystem::path> buildOrdinaryFunctionSearchPaths(
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
    for (const auto& path : options.searchPaths) {
        if (normalizedPath(path).filename() == "private") {
            continue;
        }
        append(path);
    }
    return result;
}

struct OrdinaryFunctionSource {
    std::filesystem::path path;
    bool privateFunction = false;
};

std::optional<std::string> classPrivateFunctionOwner(
    const std::filesystem::path& path) {
    const auto privateFolder = path.parent_path();
    if (privateFolder.filename() != "private") {
        return std::nullopt;
    }

    const auto classFolder = privateFolder.parent_path();
    const std::string folderName =
        pathToUtf8(classFolder.filename());
    if (folderName.size() < 2 || folderName.front() != '@' ||
        !isSimpleClassName(folderName.substr(1))) {
        return std::nullopt;
    }

    const std::string className = folderName.substr(1);
    const auto definitionPath = classFolder / (className + ".m");
    std::error_code error;
    if (!std::filesystem::is_regular_file(definitionPath, error) || error) {
        return std::nullopt;
    }

    const auto namespaceLocation = sourceNamespaceLocation(definitionPath);
    return qualifyClassName(namespaceLocation.name, className);
}

std::optional<OrdinaryFunctionSource> findOrdinaryFunctionSource(
    const std::string& functionName,
    const std::filesystem::path& sourcePath,
    const std::vector<std::filesystem::path>& searchPaths) {
    if (!isSimpleClassName(functionName)) {
        return std::nullopt;
    }

    std::vector<std::pair<std::filesystem::path, bool>> directories;
    const auto sourceDirectory = sourcePath.parent_path();
    if (sourceDirectory.filename() == "private") {
        directories.emplace_back(sourceDirectory, true);
    } else {
        directories.emplace_back(sourceDirectory / "private", true);
    }
    for (const auto& path : searchPaths) {
        directories.emplace_back(path, false);
    }

    std::set<std::filesystem::path> visited;
    for (const auto& [directory, privateFunction] : directories) {
        const auto normalizedDirectory = normalizedPath(directory);
        if (!visited.insert(normalizedDirectory).second) {
            continue;
        }
        if (!privateFunction &&
            normalizedDirectory.filename() == "private") {
            continue;
        }
        const auto candidate =
            normalizedDirectory / (functionName + ".m");
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return OrdinaryFunctionSource{normalizedPath(candidate),
                                          privateFunction};
        }
    }
    return std::nullopt;
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

std::filesystem::path classFolderSourceRelativePath(
    const std::string& symbolName) {
    const auto parts = splitClassName(symbolName);
    std::filesystem::path result;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        result /= "+" + parts[index];
    }
    if (!parts.empty()) {
        result /= "@" + parts.back();
        result /= parts.back() + ".m";
    }
    return result;
}

std::optional<std::filesystem::path> findSymbolSource(
    const std::string& symbolName,
    const std::filesystem::path& sourcePath,
    const std::vector<std::filesystem::path>& searchPaths) {
    const auto relative = symbolSourceRelativePath(symbolName);
    const auto classFolderRelative =
        classFolderSourceRelativePath(symbolName);
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
        error.clear();
        const auto classFolderCandidate =
            directory / classFolderRelative;
        if (std::filesystem::is_regular_file(classFolderCandidate, error) &&
            !error) {
            return normalizedPath(classFolderCandidate);
        }
    }
    return std::nullopt;
}

std::optional<std::string> classFolderOwnerForDefinition(
    const std::filesystem::path& path, std::string_view namespaceName,
    const InspectedSource& inspected) {
    const std::string folder =
        pathToUtf8(path.parent_path().filename());
    if (folder.size() < 2 || folder.front() != '@') {
        return std::nullopt;
    }
    const std::string className = folder.substr(1);
    if (!isSimpleClassName(className) ||
        pathToUtf8(path.stem()) != className ||
        !inspected.classes.contains(className)) {
        return std::nullopt;
    }
    return qualifyClassName(namespaceName, className);
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

SourceLoaderResult loadSourceGraph(
    const std::filesystem::path& entryPath,
    const SourceLoaderOptions& options,
    std::optional<std::string> inMemorySource) {
    const auto entry = normalizedPath(entryPath);
    if (!inMemorySource) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(entry, error) || error) {
            throw std::runtime_error("failed to open input file: " +
                                     pathToUtf8(entry));
        }
    }

    SourceLoaderResult result;
    std::vector<std::filesystem::path> sourcePaths;
    std::set<std::filesystem::path> loadedPaths;
    std::map<std::filesystem::path, std::string>
        functionIdentityByPath;
    std::set<std::string> knownClasses;
    std::set<std::string> knownFunctions;
    const auto searchPaths = buildSearchPaths(entry, options);
    const auto ordinaryFunctionSearchPaths =
        buildOrdinaryFunctionSearchPaths(entry, options);
    size_t nextExternalFunctionId = 0;

    const auto appendSource = [&](const std::filesystem::path& path,
                                  std::string content,
                                  InspectedSource inspected,
                                  std::string namespaceName,
                                  std::string functionIdentity = {},
                                  std::string classMethodOwner = {},
                                  std::string classPrivateOwner = {}) {
        loadedPaths.insert(path);
        if (classMethodOwner.empty()) {
            for (const auto& className : inspected.classes) {
                knownClasses.insert(
                    qualifyClassName(namespaceName, className));
            }
            for (const auto& functionName : inspected.functions) {
                const std::string identity = functionIdentity.empty()
                                                 ? qualifyClassName(
                                                       namespaceName,
                                                       functionName)
                                                 : functionIdentity;
                knownFunctions.insert(identity);
                if (!functionIdentity.empty()) {
                    functionIdentityByPath[path] = identity;
                }
            }
        }
        sourcePaths.push_back(path);
        result.sources.push_back(SourceUnit{
            pathToUtf8(path), std::move(content),
            std::move(namespaceName),
            std::move(functionIdentity), std::move(classMethodOwner),
            std::move(classPrivateOwner), {}});
    };

    const auto appendClassFolderMethods =
        [&](const std::filesystem::path& definitionPath,
            const std::string& owner) {
        std::vector<std::filesystem::path> methodPaths;
        std::error_code directoryError;
        for (std::filesystem::directory_iterator iterator(
                 definitionPath.parent_path(), directoryError), end;
             !directoryError && iterator != end;
             iterator.increment(directoryError)) {
            std::error_code fileError;
            if (!iterator->is_regular_file(fileError) || fileError) {
                continue;
            }
            const auto path = normalizedPath(iterator->path());
            if (path == definitionPath || path.extension() != ".m" ||
                !isSimpleClassName(pathToUtf8(path.stem()))) {
                continue;
            }
            methodPaths.push_back(path);
        }
        std::sort(methodPaths.begin(), methodPaths.end());

        for (const auto& methodPath : methodPaths) {
            if (loadedPaths.contains(methodPath)) {
                continue;
            }
            std::string content = readSourceFile(methodPath);
            auto inspected = inspectSource(content, result.sources.size());
            appendSource(methodPath, std::move(content),
                         std::move(inspected),
                         sourceNamespaceLocation(methodPath).name, {}, owner);
        }
    };

    const auto addFunctionBinding = [&](size_t sourceId,
                                        const std::string& alias,
                                        const std::string& target) {
        auto& bindings = result.sources[sourceId].functionBindings;
        const auto existing = std::find_if(
            bindings.begin(), bindings.end(),
            [&alias](const SourceFunctionBinding& binding) {
                return binding.alias == alias;
            });
        if (existing == bindings.end() || existing->target != target) {
            bindings.push_back(SourceFunctionBinding{alias, target});
        }
    };

    std::string entryContent = inMemorySource
                                   ? std::move(*inMemorySource)
                                   : readSourceFile(entry);
    const auto entryNamespace = sourceNamespaceLocation(entry).name;
    auto entryInspected =
        inspectSource(entryContent, result.sources.size());
    const auto entryClassFolderOwner = classFolderOwnerForDefinition(
        entry, entryNamespace, entryInspected);
    appendSource(entry, entryContent, std::move(entryInspected),
                 entryNamespace);
    if (entryClassFolderOwner) {
        appendClassFolderMethods(entry, *entryClassFolderOwner);
    }

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
            if (functionsAllowed &&
                dependency.find('.') == std::string::npos) {
                if (const auto ordinary = findOrdinaryFunctionSource(
                        dependency, sourcePaths[sourceId],
                        ordinaryFunctionSearchPaths)) {
                    if (const auto identity =
                            functionIdentityByPath.find(ordinary->path);
                        identity != functionIdentityByPath.end()) {
                        addFunctionBinding(sourceId, dependency,
                                           identity->second);
                        continue;
                    }

                    if (!loadedPaths.contains(ordinary->path)) {
                        std::string content =
                            readSourceFile(ordinary->path);
                        auto candidate = inspectSource(
                            content, result.sources.size());
                        const auto namespaceName =
                            sourceNamespaceLocation(ordinary->path).name;
                        if (namespaceName.empty() &&
                            candidate.functions.contains(dependency)) {
                            const std::string identity =
                                (ordinary->privateFunction
                                     ? "$private"
                                     : "$path") +
                                std::to_string(nextExternalFunctionId++) +
                                ">" + dependency;
                            appendSource(
                                ordinary->path, std::move(content),
                                std::move(candidate), namespaceName,
                                identity, {},
                                ordinary->privateFunction
                                    ? classPrivateFunctionOwner(
                                          ordinary->path)
                                          .value_or(std::string{})
                                    : std::string{});
                            addFunctionBinding(sourceId, dependency,
                                               identity);
                            continue;
                        }
                    }
                    if (ordinary->privateFunction) {
                        continue;
                    }
                }
            }

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
            const auto classFolderOwner = classFolderOwnerForDefinition(
                *path, namespaceName, candidate);
            appendSource(*path, std::move(content), std::move(candidate),
                         namespaceName);
            if (classFolderOwner) {
                appendClassFolderMethods(*path, *classFolderOwner);
            }
        }
    }

    return result;
}

} // namespace

SourceLoaderResult SourceLoader::load(
    const std::filesystem::path& entryPath,
    const SourceLoaderOptions& options) const {
    return loadSourceGraph(entryPath, options, std::nullopt);
}

SourceLoaderResult SourceLoader::loadSource(
    const std::filesystem::path& sourceName,
    std::string source,
    const SourceLoaderOptions& options) const {
    if (sourceName.empty()) {
        throw std::invalid_argument(
            "in-memory source name cannot be empty");
    }
    return loadSourceGraph(
        sourceName, options,
        std::optional<std::string>(std::move(source)));
}

} // namespace mparser
