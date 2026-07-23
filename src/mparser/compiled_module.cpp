#include "mparser/compiled_module.h"

#include "mparser/argument_contract.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_argument_validation.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mparser {

struct CompiledModuleData {
    std::vector<SourceUnit> sources;
    SemanticResult semantic;
    BytecodeProgram bytecode;
    std::vector<CompiledFunctionInfo> functions;
    std::vector<Diagnostic> diagnostics;
};

namespace {

void collectInvocableFunctions(
    const HirNode* node, std::vector<CompiledFunctionInfo>& functions,
    std::vector<Diagnostic>& diagnostics,
    const ArgumentContractCatalog& argumentCatalog) {
    if (!node || node->kind == HirKind::Class) {
        return;
    }

    if (node->kind == HirKind::Function) {
        if (node->label.find('>') != std::string::npos) {
            return;
        }
        for (const auto& function : functions) {
            if (function.name == node->label) {
                diagnostics.push_back(Diagnostic{
                    node->span,
                    "duplicate top-level function: " + node->label});
                return;
            }
        }

        const auto resolution =
            resolveArgumentContracts(*node, argumentCatalog);
        diagnostics.insert(diagnostics.end(),
                           resolution.diagnostics.begin(),
                           resolution.diagnostics.end());
        std::vector<std::string> nameValueArguments;
        for (const auto& contract : resolution.contracts) {
            if (contract.blockKind == ArgumentBlockKind::Input &&
                contract.name.find('.') != std::string::npos) {
                nameValueArguments.push_back(contract.name);
            }
        }
        functions.push_back(CompiledFunctionInfo{
            node->label, parseFunctionSignature(*node),
            std::move(nameValueArguments), node->span});
        return;
    }

    for (const auto& child : node->children) {
        collectInvocableFunctions(child.get(), functions, diagnostics,
                                  argumentCatalog);
    }
}

void appendDiagnostics(std::vector<Diagnostic>& destination,
                       const std::vector<Diagnostic>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

void applySourceIdentity(SyntaxNode& root, const SourceUnit& source) {
    const std::string_view namespaceName = source.namespaceName;
    for (auto& child : root.children) {
        if (!namespaceName.empty() &&
            child->kind == SyntaxKind::ClassDef &&
            !child->label.empty()) {
            child->label = std::string(namespaceName) + "." + child->label;
        }
    }

    if (root.children.empty()) {
        return;
    }
    const bool functionFile =
        root.children.front()->kind == SyntaxKind::FunctionDef;
    if (namespaceName.empty() &&
        (!functionFile || source.primaryFunctionIdentity.empty())) {
        return;
    }

    std::string owner;
    if (functionFile) {
        owner = source.primaryFunctionIdentity;
        if (owner.empty()) {
            owner = std::string(namespaceName) + "." +
                    root.children.front()->label;
        }
    } else {
        for (const auto& child : root.children) {
            if (child->kind == SyntaxKind::ClassDef) {
                owner = child->label;
                break;
            }
        }
        if (owner.empty()) {
            owner = std::string(namespaceName) + "." +
                    std::filesystem::path(source.name).stem().string();
        }
    }

    for (auto& child : root.children) {
        if (child->kind != SyntaxKind::FunctionDef ||
            child->label.empty()) {
            continue;
        }
        if (functionFile && child.get() == root.children.front().get()) {
            child->label = owner;
        } else {
            child->label = owner + ">" + child->label;
        }
    }
}

struct PendingClassMethod {
    std::string owner;
    std::unique_ptr<SyntaxNode> implementation;
};

std::string unqualifiedName(std::string_view name) {
    const size_t separator = name.find_last_of('.');
    return std::string(separator == std::string_view::npos
                           ? name
                           : name.substr(separator + 1));
}

std::optional<PendingClassMethod> extractClassMethod(
    SyntaxNode& sourceRoot, const SourceUnit& source,
    SyntaxNode& moduleRoot, std::vector<Diagnostic>& diagnostics) {
    if (sourceRoot.children.empty() ||
        sourceRoot.children.front()->kind != SyntaxKind::FunctionDef) {
        diagnostics.push_back(Diagnostic{
            sourceRoot.span,
            "class method file must begin with a function: " +
                source.name});
        return std::nullopt;
    }

    auto implementation = std::move(sourceRoot.children.front());
    const std::string fileMethodName =
        std::filesystem::path(source.name).stem().string();
    if (implementation->label != fileMethodName) {
        diagnostics.push_back(Diagnostic{
            implementation->span,
            "class method file name must match its primary function: " +
                fileMethodName});
        return std::nullopt;
    }
    if (implementation->label == unqualifiedName(source.classMethodOwner)) {
        diagnostics.push_back(Diagnostic{
            implementation->span,
            "class constructor must be defined in the classdef file: " +
                source.classMethodOwner});
        return std::nullopt;
    }

    const std::string helperOwner = source.classMethodOwner + "." +
                                    implementation->label;
    for (size_t index = 1; index < sourceRoot.children.size(); ++index) {
        auto& child = sourceRoot.children[index];
        if (!child) {
            continue;
        }
        if (child->kind != SyntaxKind::FunctionDef || child->label.empty()) {
            diagnostics.push_back(Diagnostic{
                child->span,
                "class method file may contain only function definitions: " +
                    source.name});
            continue;
        }
        child->label = helperOwner + ">" + child->label;
        moduleRoot.children.push_back(std::move(child));
    }

    return PendingClassMethod{source.classMethodOwner,
                              std::move(implementation)};
}

SyntaxNode* findClassDefinition(SyntaxNode& root,
                                std::string_view owner) {
    for (const auto& child : root.children) {
        if (child->kind == SyntaxKind::ClassDef && child->label == owner) {
            return child.get();
        }
    }
    return nullptr;
}

bool signaturesMatch(const FunctionSignature& declaration,
                     const FunctionSignature& implementation) {
    return declaration.outputs == implementation.outputs &&
           declaration.parameters == implementation.parameters &&
           declaration.hasVarargout == implementation.hasVarargout &&
           declaration.hasVarargin == implementation.hasVarargin;
}

void attachClassMethod(SyntaxNode& moduleRoot,
                       PendingClassMethod method,
                       std::vector<Diagnostic>& diagnostics) {
    SyntaxNode* classNode =
        findClassDefinition(moduleRoot, method.owner);
    if (!classNode) {
        diagnostics.push_back(Diagnostic{
            method.implementation->span,
            "class method owner is not available: " + method.owner});
        return;
    }

    std::unique_ptr<SyntaxNode>* declaration = nullptr;
    SyntaxNode* defaultMethodsBlock = nullptr;
    for (auto& child : classNode->children) {
        if (child->kind != SyntaxKind::MethodsBlock) {
            continue;
        }
        if (child->attributes.empty() && !defaultMethodsBlock) {
            defaultMethodsBlock = child.get();
        }
        for (auto& candidate : child->children) {
            if (candidate->label != method.implementation->label) {
                continue;
            }
            if (candidate->kind == SyntaxKind::FunctionDef) {
                diagnostics.push_back(Diagnostic{
                    method.implementation->span,
                    "duplicate method implementation: " + method.owner +
                        "." + method.implementation->label});
                return;
            }
            if (candidate->kind == SyntaxKind::MethodPrototype) {
                if (declaration) {
                    diagnostics.push_back(Diagnostic{
                        candidate->span,
                        "duplicate method declaration: " + method.owner +
                            "." + method.implementation->label});
                    return;
                }
                declaration = &candidate;
            }
        }
    }

    if (declaration) {
        const auto expected = parseFunctionSignature(
            (*declaration)->raw, (*declaration)->label);
        const auto actual = parseFunctionSignature(
            method.implementation->raw, method.implementation->label);
        if (!signaturesMatch(expected, actual)) {
            diagnostics.push_back(Diagnostic{
                method.implementation->span,
                "separate method signature does not match its declaration: " +
                    method.owner + "." + method.implementation->label});
            return;
        }
        method.implementation->attributes = (*declaration)->attributes;
        *declaration = std::move(method.implementation);
        return;
    }

    if (!defaultMethodsBlock) {
        auto block = std::make_unique<SyntaxNode>(SyntaxKind::MethodsBlock);
        block->span = method.implementation->span;
        defaultMethodsBlock = block.get();
        classNode->children.push_back(std::move(block));
    }
    defaultMethodsBlock->children.push_back(
        std::move(method.implementation));
}

void collectTopLevelClasses(
    const SyntaxNode& root, std::map<std::string, SourceSpan>& definitions,
    std::vector<Diagnostic>& diagnostics) {
    for (const auto& child : root.children) {
        if (child->kind != SyntaxKind::ClassDef || child->label.empty()) {
            continue;
        }
        const bool inserted =
            definitions.try_emplace(child->label, child->span).second;
        if (!inserted) {
            diagnostics.push_back(Diagnostic{
                child->span,
                "duplicate top-level class: " + child->label});
        }
    }
}

std::string firstDiagnosticMessage(
    const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) {
        return "compiled module invocation is invalid";
    }
    return diagnostics.front().message;
}

} // namespace

CompiledModule::CompiledModule()
    : data_(std::make_shared<CompiledModuleData>()),
      callableContext_(makeRuntimeCallableContext()) {
    callableContext_->lifetimeAnchor = data_;
}

CompiledModule CompiledModule::compile(std::string source) {
    std::vector<SourceUnit> sources;
    sources.push_back(SourceUnit{"<memory>", std::move(source)});
    return compile(std::move(sources));
}

CompiledModule CompiledModule::compile(std::vector<SourceUnit> sources) {
    CompiledModule module;
    module.data_->sources = std::move(sources);
    if (module.data_->sources.empty()) {
        module.data_->diagnostics.push_back(Diagnostic{
            SourceSpan{}, "compiled module requires at least one source"});
        return module;
    }

    auto root = std::make_unique<SyntaxNode>(SyntaxKind::CompilationUnit);
    bool rootSpanInitialized = false;
    std::map<std::string, SourceSpan> classDefinitions;
    std::vector<PendingClassMethod> pendingClassMethods;
    for (size_t sourceId = 0;
         sourceId < module.data_->sources.size(); ++sourceId) {
        auto& source = module.data_->sources[sourceId];
        if (source.name.empty()) {
            source.name = "<source:" + std::to_string(sourceId) + ">";
        }

        Lexer lexer(source.content, sourceId);
        Parser parser(lexer.lex());
        auto parse = parser.parse();
        appendDiagnostics(module.data_->diagnostics,
                          parse.diagnostics);
        if (!parse.root) {
            continue;
        }

        if (!source.classMethodOwner.empty()) {
            if (auto method = extractClassMethod(
                    *parse.root, source, *root,
                    module.data_->diagnostics)) {
                pendingClassMethods.push_back(std::move(*method));
            }
            continue;
        }

        applySourceIdentity(*parse.root, source);
        collectTopLevelClasses(*parse.root, classDefinitions,
                               module.data_->diagnostics);
        if (!rootSpanInitialized) {
            root->span = parse.root->span;
            rootSpanInitialized = true;
        } else {
            root->span.end = parse.root->span.end;
        }
        for (auto& child : parse.root->children) {
            root->children.push_back(std::move(child));
        }
    }

    for (auto& method : pendingClassMethods) {
        attachClassMethod(*root, std::move(method),
                          module.data_->diagnostics);
    }

    if (!module.data_->diagnostics.empty()) {
        return module;
    }

    SemanticAnalyzer analyzer;
    module.data_->semantic =
        analyzer.analyze(*root, module.data_->sources);
    appendDiagnostics(module.data_->diagnostics,
                      module.data_->semantic.diagnostics);
    if (!module.data_->semantic.root ||
        !module.data_->semantic.diagnostics.empty()) {
        return module;
    }

    BytecodeLowerer lowerer;
    module.data_->bytecode =
        lowerer.lower(module.data_->semantic);
    appendDiagnostics(module.data_->diagnostics,
                      module.data_->bytecode.diagnostics);
    if (!module.data_->bytecode.diagnostics.empty()) {
        return module;
    }

    const ArgumentContractCatalog argumentCatalog =
        buildArgumentContractCatalog(*module.data_->semantic.root);
    collectInvocableFunctions(
        module.data_->semantic.root.get(),
        module.data_->functions, module.data_->diagnostics,
        argumentCatalog);
    return module;
}

bool CompiledModule::valid() const {
    return data_ && data_->semantic.root != nullptr &&
           data_->diagnostics.empty();
}

std::string_view CompiledModule::source() const {
    return !data_ || data_->sources.empty()
               ? std::string_view{}
               : std::string_view{
                     data_->sources.front().content};
}

const std::vector<SourceUnit>& CompiledModule::sources() const {
    return data_->sources;
}

std::string_view CompiledModule::sourceName(size_t sourceId) const {
    if (!data_ || sourceId >= data_->sources.size()) {
        return {};
    }
    return data_->sources[sourceId].name;
}

std::string_view CompiledModule::sourceName(SourceSpan span) const {
    return sourceName(span.begin.sourceId);
}

const std::vector<Diagnostic>& CompiledModule::diagnostics() const {
    return data_->diagnostics;
}

const SemanticResult& CompiledModule::semantic() const {
    return data_->semantic;
}

const BytecodeProgram& CompiledModule::bytecode() const {
    return data_->bytecode;
}

const std::vector<CompiledFunctionInfo>& CompiledModule::functions() const {
    return data_->functions;
}

const CompiledFunctionInfo* CompiledModule::findFunction(
    std::string_view name) const {
    for (const auto& function : data_->functions) {
        if (function.name == name) {
            return &function;
        }
    }
    return nullptr;
}

std::vector<Diagnostic> CompiledModule::validateInvocation(
    std::string_view entryFunction, size_t argumentCount,
    std::optional<size_t> requestedOutputCount) const {
    if (!valid()) {
        return data_->diagnostics;
    }
    if (entryFunction.empty()) {
        return {};
    }

    const auto* function = findFunction(entryFunction);
    if (!function) {
        return {Diagnostic{SourceSpan{},
                           "entry function is not available: " +
                               std::string(entryFunction)}};
    }
    const auto argumentCountStatus =
        functionArgumentCountStatus(function->signature, argumentCount);
    if (argumentCountStatus ==
        FunctionArgumentCountStatus::IncompleteRepeatingGroup) {
        return {Diagnostic{
            function->span,
            "incomplete repeating argument group for " + function->name +
                ": expected a multiple of " +
                std::to_string(functionRepeatingParameterCount(
                    function->signature)) +
                " values"}};
    }
    if (argumentCountStatus == FunctionArgumentCountStatus::Mismatch) {
        return {Diagnostic{
            function->span,
            "function argument count mismatch for: " + function->name}};
    }
    if (requestedOutputCount &&
        !functionOutputCountIsValid(function->signature,
                                    *requestedOutputCount)) {
        return {Diagnostic{
            function->span,
            "function output count mismatch for: " + function->name}};
    }
    return {};
}

std::vector<Diagnostic> CompiledModule::validateInvocation(
    std::string_view entryFunction,
    const std::vector<RuntimeValue>& arguments,
    std::optional<size_t> requestedOutputCount) const {
    if (!valid()) {
        return data_->diagnostics;
    }
    if (entryFunction.empty()) {
        return {};
    }

    const auto* function = findFunction(entryFunction);
    if (!function) {
        return {Diagnostic{SourceSpan{},
                           "entry function is not available: " +
                               std::string(entryFunction)}};
    }
    const auto normalized = normalizeRuntimeInvocationArguments(
        function->signature, function->nameValueArguments, arguments);
    if (!normalized.succeeded) {
        return {Diagnostic{
            function->span,
            "function invocation failed for " + function->name + ": " +
                normalized.error}};
    }
    if (requestedOutputCount &&
        !functionOutputCountIsValid(function->signature,
                                    *requestedOutputCount)) {
        return {Diagnostic{
            function->span,
            "function output count mismatch for: " + function->name}};
    }
    return {};
}

BytecodeVmResult CompiledModule::invoke(
    const BytecodeVmOptions& options) const {
    const auto validation =
        validateInvocation(options.entryFunction, options.arguments,
                           options.requestedOutputCount);
    if (!validation.empty()) {
        BytecodeVmResult result;
        result.diagnostics = validation;
        return result;
    }

    BytecodeVm vm;
    BytecodeVmOptions runtimeOptions = options;
    runtimeOptions.callableContext = callableContext_;
    return vm.run(data_->bytecode, data_->semantic,
                  runtimeOptions);
}

AdaptiveBytecodeVmSession CompiledModule::createAdaptiveSession(
    const AdaptiveBytecodeVmOptions& options) const {
    const auto validation =
        validateInvocation(options.entryFunction, options.arguments,
                           options.requestedOutputCount);
    if (!validation.empty()) {
        throw std::invalid_argument(firstDiagnosticMessage(validation));
    }
    AdaptiveBytecodeVmOptions runtimeOptions = options;
    runtimeOptions.callableContext = callableContext_;
    return AdaptiveBytecodeVmSession(
        data_->bytecode, data_->semantic, runtimeOptions);
}

CompiledModuleSession CompiledModule::createSession(
    std::shared_ptr<RuntimeSessionState> state) const {
    if (!state) {
        state = std::make_shared<RuntimeSessionState>();
    }
    return CompiledModuleSession(*this, std::move(state));
}

CompiledModuleSession::CompiledModuleSession(
    CompiledModule module,
    std::shared_ptr<RuntimeSessionState> state)
    : module_(std::move(module)), state_(std::move(state)) {}

BytecodeVmResult CompiledModuleSession::invoke(
    const BytecodeVmOptions& options) const {
    BytecodeVmOptions runtimeOptions = options;
    runtimeOptions.sessionState = state_;
    return module_.invoke(runtimeOptions);
}

std::shared_ptr<RuntimeSessionState>
CompiledModuleSession::state() const {
    return state_;
}

std::vector<RuntimeVariable>
CompiledModuleSession::globals() const {
    return state_->globals();
}

std::vector<RuntimePersistentVariable>
CompiledModuleSession::persistentVariables() const {
    return state_->persistentVariables(
        module_.callableContext_->identity);
}

bool CompiledModuleSession::clearGlobal(
    std::string_view name) {
    return state_->clearGlobal(name);
}

size_t CompiledModuleSession::clearFunction(
    std::string_view function) {
    return state_->clearFunction(
        module_.callableContext_->identity, function);
}

void CompiledModuleSession::clearGlobals() {
    state_->clearGlobals();
}

void CompiledModuleSession::reset() {
    state_->reset();
}

} // namespace mparser
