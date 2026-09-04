#include "mparser/embedding/compiled_module.h"

#include "mparser/semantic/argument_contract.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/io/filesystem_utf8.h"
#include "mparser/frontend/lexer.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/object_model/runtime_argument_validation.h"
#include "mparser/execution/jit/typed_ir.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mparser {

struct CompiledModuleData {
    std::vector<SourceUnit> sources;
    SemanticResult semantic;
    BytecodeProgram bytecode;
    BytecodeTypedIrModule staticTypedModule;
    std::vector<CompiledFunctionInfo> functions;
    std::vector<CompiledSourceInfo> sourceInfo;
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
                    pathToUtf8(pathFromUtf8(source.name).stem());
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

CompiledSourceInfo inspectCompiledSource(
    const SyntaxNode& root, const SourceUnit& source) {
    CompiledSourceInfo info;
    info.name = source.name;
    if (root.children.empty()) {
        info.kind = CompiledSourceKind::Script;
        return info;
    }

    const auto& first = *root.children.front();
    if (first.kind == SyntaxKind::FunctionDef) {
        info.kind = CompiledSourceKind::Function;
        info.primaryFunction = first.label;
    } else if (first.kind == SyntaxKind::ClassDef) {
        info.kind = CompiledSourceKind::Class;
    } else {
        info.kind = CompiledSourceKind::Script;
    }
    info.hasTopLevelStatements = std::any_of(
        root.children.begin(), root.children.end(),
        [](const std::unique_ptr<SyntaxNode>& child) {
            return child && child->kind != SyntaxKind::FunctionDef &&
                   child->kind != SyntaxKind::ClassDef;
        });
    return info;
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
        pathToUtf8(pathFromUtf8(source.name).stem());
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

ModuleSourcePosition projectSourcePosition(
    const SourcePosition& position) {
    return ModuleSourcePosition{
        position.offset, position.line, position.column};
}

ModuleSourceRange projectSourceRange(
    SourceSpan span, const std::vector<SourceUnit>& sources,
    std::string_view explicitSourceName = {}) {
    ModuleSourceRange result;
    if (span.begin.sourceId == kInvalidSourceId ||
        (explicitSourceName.empty() &&
         span.begin.sourceId >= sources.size())) {
        return result;
    }
    result.available = true;
    result.sourceName = explicitSourceName.empty()
                            ? sources[span.begin.sourceId].name
                            : std::string(explicitSourceName);
    result.begin = projectSourcePosition(span.begin);
    result.end = projectSourcePosition(span.end);
    return result;
}

ModuleOutputEvent projectOutputEvent(
    const RuntimeOutputEvent& event,
    const std::vector<SourceUnit>& sources) {
    return ModuleOutputEvent{
        event.kind == RuntimeOutputKind::Display
            ? ModuleOutputKind::Display
            : ModuleOutputKind::StandardOutput,
        event.text,
        projectSourceRange(event.span, sources, event.sourceName),
        event.sequence};
}

ModuleTopLevelExpression projectExpressionResult(
    RuntimeExpressionResult expression,
    const std::vector<SourceUnit>& sources) {
    return ModuleTopLevelExpression{
        std::move(expression.value),
        projectSourceRange(expression.span, sources),
        expression.outputSuppressed,
        expression.sequence,
        std::move(expression.displayText),
        expression.lineSpacing};
}

ModuleDiagnosticFrame projectDiagnosticFrame(
    const DiagnosticFrame& frame) {
    return ModuleDiagnosticFrame{
        frame.file, frame.name, frame.line};
}

ModuleDiagnosticCause projectDiagnosticCause(
    const DiagnosticCause& cause) {
    ModuleDiagnosticCause result;
    result.identifier = cause.identifier;
    result.message = cause.message;
    for (const auto& frame : cause.stack) {
        result.stack.push_back(projectDiagnosticFrame(frame));
    }
    for (const auto& nested : cause.causes) {
        result.causes.push_back(projectDiagnosticCause(nested));
    }
    return result;
}

ModuleDiagnostic projectDiagnostic(
    const Diagnostic& diagnostic, ModuleDiagnosticPhase phase,
    const std::vector<SourceUnit>& sources,
    std::string_view fallbackIdentifier) {
    ModuleDiagnostic result;
    result.phase = phase;
    result.severity =
        diagnostic.severity == DiagnosticSeverity::Warning
            ? ModuleDiagnosticSeverity::Warning
            : ModuleDiagnosticSeverity::Error;
    result.identifier = diagnostic.identifier.empty()
                            ? std::string(fallbackIdentifier)
                            : diagnostic.identifier;
    result.message = diagnostic.message;
    result.source = projectSourceRange(
        diagnostic.span, sources, diagnostic.sourceName);
    for (const auto& frame : diagnostic.stack) {
        result.stack.push_back(projectDiagnosticFrame(frame));
    }
    for (const auto& cause : diagnostic.causes) {
        result.causes.push_back(projectDiagnosticCause(cause));
    }
    return result;
}

std::vector<ModuleDiagnostic> projectDiagnostics(
    const std::vector<Diagnostic>& diagnostics,
    ModuleDiagnosticPhase phase,
    const std::vector<SourceUnit>& sources,
    std::string_view fallbackIdentifier) {
    std::vector<ModuleDiagnostic> result;
    result.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        result.push_back(projectDiagnostic(
            diagnostic, phase, sources, fallbackIdentifier));
    }
    return result;
}

Diagnostic requestDiagnostic(
    std::string message, std::string identifier) {
    return Diagnostic{
        SourceSpan{}, std::move(message), std::move(identifier)};
}

std::vector<Diagnostic> validateModuleInvocationRequest(
    const ModuleInvocationRequest& request) {
    std::vector<Diagnostic> diagnostics;
    switch (request.backend) {
    case ModuleExecutionBackend::Automatic:
    case ModuleExecutionBackend::Bytecode:
    case ModuleExecutionBackend::Portable:
    case ModuleExecutionBackend::Native:
        break;
    default:
        diagnostics.push_back(requestDiagnostic(
            "module execution backend is invalid",
            "MParser:InvalidExecutionBackend"));
        break;
    }

    if (request.limits.maxWallTime.count() < 0) {
        diagnostics.push_back(requestDiagnostic(
            "module wall-time limit cannot be negative",
            "MParser:InvalidExecutionLimits"));
    }

    if (request.entryFunction.empty()) {
        if (!request.arguments.empty()) {
            diagnostics.push_back(requestDiagnostic(
                "script invocation does not accept arguments",
                "MParser:ScriptArgumentsUnsupported"));
        }
        if (request.requestedOutputCount &&
            *request.requestedOutputCount != 0) {
            diagnostics.push_back(requestDiagnostic(
                "script invocation does not produce function outputs",
                "MParser:ScriptOutputsUnsupported"));
        }
    }

    for (size_t index = 0; index < request.arguments.size(); ++index) {
        const auto contract =
            validateRuntimeValueContract(request.arguments[index]);
        if (!contract.valid) {
            std::string message =
                "argument " + std::to_string(index + 1) +
                " violates the RuntimeValue contract";
            if (!contract.path.empty()) {
                message += " at " + contract.path;
            }
            if (!contract.error.empty()) {
                message += ": " + contract.error;
            }
            diagnostics.push_back(requestDiagnostic(
                std::move(message),
                "MParser:InvalidArgumentValue"));
            continue;
        }
        if (request.limits.maxArrayBytes != 0) {
            const auto bytes =
                runtimeValueArrayBytes(request.arguments[index]);
            if (!bytes ||
                *bytes > request.limits.maxArrayBytes) {
                diagnostics.push_back(requestDiagnostic(
                    "argument " + std::to_string(index + 1) +
                        " exceeds the array-byte limit of " +
                        std::to_string(
                            request.limits.maxArrayBytes),
                    "MParser:ArrayByteLimitExceeded"));
            }
        }
    }

    std::set<std::string> workspaceNames;
    for (size_t index = 0;
         index < request.initialWorkspace.size(); ++index) {
        const auto& variable = request.initialWorkspace[index];
        if (variable.name.empty()) {
            diagnostics.push_back(requestDiagnostic(
                "initial workspace variable name cannot be empty",
                "MParser:InvalidWorkspaceName"));
            continue;
        }
        if (!workspaceNames.insert(variable.name).second) {
            diagnostics.push_back(requestDiagnostic(
                "duplicate initial workspace variable: " +
                    variable.name,
                "MParser:DuplicateWorkspaceVariable"));
            continue;
        }
        const auto contract =
            validateRuntimeValueContract(variable.value);
        if (!contract.valid) {
            std::string message =
                "initial workspace variable " + variable.name +
                " violates the RuntimeValue contract";
            if (!contract.path.empty()) {
                message += " at " + contract.path;
            }
            if (!contract.error.empty()) {
                message += ": " + contract.error;
            }
            diagnostics.push_back(requestDiagnostic(
                std::move(message),
                "MParser:InvalidWorkspaceValue"));
            continue;
        }
        if (!runtimeValueIsStorable(variable.value)) {
            diagnostics.push_back(requestDiagnostic(
                "initial workspace variable is transient: " +
                    variable.name,
                "MParser:TransientWorkspaceValue"));
            continue;
        }
        if (request.limits.maxArrayBytes != 0) {
            const auto bytes =
                runtimeValueArrayBytes(variable.value);
            if (!bytes ||
                *bytes > request.limits.maxArrayBytes) {
                diagnostics.push_back(requestDiagnostic(
                    "initial workspace variable " +
                        variable.name +
                        " exceeds the array-byte limit of " +
                        std::to_string(
                            request.limits.maxArrayBytes),
                    "MParser:ArrayByteLimitExceeded"));
            }
        }
    }
    return diagnostics;
}

TypedRegionBackend typedBackendFor(
    ModuleExecutionBackend backend) {
    switch (backend) {
    case ModuleExecutionBackend::Portable:
        return TypedRegionBackend::Portable;
    case ModuleExecutionBackend::Native:
        return TypedRegionBackend::Native;
    case ModuleExecutionBackend::Automatic:
    case ModuleExecutionBackend::Bytecode:
        return TypedRegionBackend::Auto;
    }
    return TypedRegionBackend::Auto;
}

ModuleExecutionSummary summarizeExecution(
    const BytecodeVmResult& runtime,
    ModuleExecutionBackend requestedBackend) {
    ModuleExecutionSummary summary;
    summary.requestedBackend = requestedBackend;
    summary.profilingCollected = runtime.profile.collected;
    summary.executedInstructionCount =
        runtime.executedInstructionCount;
    summary.resourceControlsActive =
        runtime.execution.controlsActive;
    summary.optimizedExecutionSuppressed =
        runtime.execution.optimizedExecutionSuppressed;
    summary.stopReason = runtime.execution.stopReason;
    summary.maximumCallDepth =
        runtime.execution.maximumCallDepth;
    summary.maximumArrayBytes =
        runtime.execution.maximumArrayBytes;
    summary.maximumDiagnosticCount =
        runtime.execution.maximumDiagnosticCount;
    summary.elapsedNanoseconds =
        runtime.execution.elapsedNanoseconds;
    summary.typedRegionCount =
        runtime.typedRegionExecutions.size();

    bool usedPortable = false;
    bool usedNative = false;
    for (const auto& execution : runtime.typedRegionExecutions) {
        summary.typedRegionAttemptCount += execution.attemptCount;
        summary.typedRegionExecutionCount += execution.executionCount;
        summary.typedRegionFallbackCount += execution.fallbackCount;
        summary.nativeCompilationCount +=
            execution.nativeCompilationCount;
        summary.nativeCacheHitCount += execution.nativeCacheHitCount;
        summary.fallbackOccurred =
            summary.fallbackOccurred ||
            execution.lastFallbackKind != RuntimeFallbackKind::None ||
            execution.nativeFallbackKind != RuntimeFallbackKind::None;
        if (execution.executionCount == 0) {
            continue;
        }
        usedPortable = usedPortable || execution.backend == "portable";
        usedNative = usedNative || execution.backend == "native";
    }
    summary.fallbackOccurred =
        summary.fallbackOccurred ||
        summary.typedRegionFallbackCount != 0 ||
        (summary.optimizedExecutionSuppressed &&
         requestedBackend != ModuleExecutionBackend::Bytecode);
    if (usedPortable && usedNative) {
        summary.effectiveTier = ModuleExecutionTier::Mixed;
    } else if (usedNative) {
        summary.effectiveTier = ModuleExecutionTier::Native;
    } else if (usedPortable) {
        summary.effectiveTier = ModuleExecutionTier::Portable;
    }
    return summary;
}

ModuleExecutionSummary summarizeExecutionControl(
    const RuntimeExecutionControl& control,
    ModuleExecutionBackend requestedBackend) {
    BytecodeVmResult runtime;
    runtime.execution = control.snapshot();
    runtime.executedInstructionCount =
        runtime.execution.executedInstructionCount;
    return summarizeExecution(runtime, requestedBackend);
}

} // namespace

CompiledModule::CompiledModule()
    : data_(std::make_shared<CompiledModuleData>()),
      callableContext_(makeRuntimeCallableContext()) {
    callableContext_->lifetimeAnchor = data_;
}

CompiledModule CompiledModule::compile(std::string source) {
    return compile(std::move(source), {});
}

CompiledModule CompiledModule::compile(
    std::string source,
    const CompiledModuleCompileOptions& options) {
    std::vector<SourceUnit> sources;
    sources.push_back(SourceUnit{"<memory>", std::move(source)});
    return compile(std::move(sources), options);
}

CompiledModule CompiledModule::compile(
    std::vector<SourceUnit> sources) {
    return compile(std::move(sources), {});
}

CompiledModule CompiledModule::compile(
    std::vector<SourceUnit> sources,
    const CompiledModuleCompileOptions& options) {
    CompiledModule module;
    module.data_->sources = std::move(sources);
    if (module.data_->sources.empty()) {
        module.data_->diagnostics.push_back(Diagnostic{
            SourceSpan{}, "compiled module requires at least one source"});
        return module;
    }
    if (options.builtinRegistry &&
        !options.builtinRegistry->frozen()) {
        module.data_->diagnostics.push_back(Diagnostic{
            SourceSpan{},
            "custom builtin registry must be frozen before compilation",
            "MParser:MutableBuiltinRegistry"});
        return module;
    }

    auto root = std::make_unique<SyntaxNode>(SyntaxKind::CompilationUnit);
    module.data_->sourceInfo.reserve(module.data_->sources.size());
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
            module.data_->sourceInfo.push_back(CompiledSourceInfo{
                source.name, CompiledSourceKind::Unknown, {}, false});
            continue;
        }

        if (!source.classMethodOwner.empty()) {
            module.data_->sourceInfo.push_back(
                inspectCompiledSource(*parse.root, source));
            if (auto method = extractClassMethod(
                    *parse.root, source, *root,
                    module.data_->diagnostics)) {
                pendingClassMethods.push_back(std::move(*method));
            }
            continue;
        }

        applySourceIdentity(*parse.root, source);
        module.data_->sourceInfo.push_back(
            inspectCompiledSource(*parse.root, source));
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

    SemanticAnalyzer analyzer(options.builtinRegistry,
                              options.externalFunctionNames);
    module.data_->semantic =
        analyzer.analyze(
            *root, module.data_->sources,
            SemanticAnalysisOptions{
                options.allowTopLevelPersistentDeclarations});
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
    auto bytecodeValidation = validateBytecodeProgram(
        module.data_->bytecode, &module.data_->semantic);
    appendDiagnostics(module.data_->diagnostics,
                      bytecodeValidation.diagnostics);
    if (!bytecodeValidation.succeeded) {
        return module;
    }

    BytecodeOptimizationPlanner planner;
    BytecodeTypedIrBuilder builder;
    module.data_->staticTypedModule = builder.build(
        planner.planStaticRegions(
            module.data_->bytecode,
            module.data_->semantic.builtinRegistry));

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

const std::vector<CompiledSourceInfo>&
CompiledModule::sourceInfo() const {
    return data_->sourceInfo;
}

const CompiledSourceInfo* CompiledModule::sourceInfo(
    size_t sourceId) const {
    return !data_ || sourceId >= data_->sourceInfo.size()
               ? nullptr
               : &data_->sourceInfo[sourceId];
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
                               std::string(entryFunction),
                           "MParser:EntryFunctionNotFound"}};
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
                " values",
            "MParser:IncompleteRepeatingArguments"}};
    }
    if (argumentCountStatus == FunctionArgumentCountStatus::Mismatch) {
        return {Diagnostic{
            function->span,
            "function argument count mismatch for: " + function->name,
            "MParser:ArgumentCountMismatch"}};
    }
    if (requestedOutputCount &&
        !functionOutputCountIsValid(function->signature,
                                    *requestedOutputCount)) {
        return {Diagnostic{
            function->span,
            "function output count mismatch for: " + function->name,
            "MParser:OutputCountMismatch"}};
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
                               std::string(entryFunction),
                           "MParser:EntryFunctionNotFound"}};
    }
    const auto normalized = normalizeRuntimeInvocationArguments(
        function->signature, function->nameValueArguments, arguments);
    if (!normalized.succeeded) {
        return {Diagnostic{
            function->span,
            "function invocation failed for " + function->name + ": " +
                normalized.error,
            "MParser:ArgumentValidationFailed"}};
    }
    if (requestedOutputCount &&
        !functionOutputCountIsValid(function->signature,
                                    *requestedOutputCount)) {
        return {Diagnostic{
            function->span,
            "function output count mismatch for: " + function->name,
            "MParser:OutputCountMismatch"}};
    }
    return {};
}

BytecodeVmResult CompiledModule::invoke(
    const BytecodeVmOptions& options) const {
    return invokeInternal(options, false);
}

BytecodeVmResult CompiledModule::invokeInternal(
    const BytecodeVmOptions& options,
    bool enableTypedRegions) const {
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
    return enableTypedRegions
               ? vm.runValidated(
                     data_->bytecode, data_->semantic,
                     data_->staticTypedModule, runtimeOptions)
               : vm.runValidated(
                     data_->bytecode, data_->semantic, runtimeOptions);
}

ModuleInvocationResult CompiledModule::execute(
    const ModuleInvocationRequest& request) const {
    return execute(request, {});
}

ModuleInvocationResult CompiledModule::execute(
    const ModuleInvocationRequest& request,
    const std::shared_ptr<RuntimeSessionState>& state) const {
    ModuleInvocationResult result;
    result.entryFunction = request.entryFunction;
    result.requestedOutputCount =
        request.requestedOutputCount.value_or(0);
    result.execution.requestedBackend = request.backend;

    if (!valid()) {
        result.status = ModuleInvocationStatus::CompilationFailed;
        result.diagnostics = projectDiagnostics(
            data_->diagnostics, ModuleDiagnosticPhase::Compilation,
            data_->sources, "MParser:CompilationFailed");
        return result;
    }

    auto validation = validateModuleInvocationRequest(request);
    const auto invocationValidation = validateInvocation(
        request.entryFunction, request.arguments,
        request.requestedOutputCount);
    appendDiagnostics(validation, invocationValidation);
    if (!validation.empty()) {
        result.status = ModuleInvocationStatus::RequestRejected;
        result.diagnostics = projectDiagnostics(
            validation, ModuleDiagnosticPhase::Validation,
            data_->sources, "MParser:InvocationRejected");
        return result;
    }

    BytecodeVmOptions runtimeOptions;
    runtimeOptions.profiling =
        request.collectProfile
            ? BytecodeVmProfilingMode::Full
            : BytecodeVmProfilingMode::Disabled;
    runtimeOptions.callableContext = callableContext_;
    runtimeOptions.sessionState =
        state ? state
              : std::make_shared<RuntimeSessionState>(
                    request.systemContext);
    runtimeOptions.initialWorkspace = request.initialWorkspace;
    runtimeOptions.entryFunction = request.entryFunction;
    runtimeOptions.arguments = request.arguments;
    runtimeOptions.requestedOutputCount =
        request.requestedOutputCount;
    runtimeOptions.typedRegionBackend =
        typedBackendFor(request.backend);
    const auto executionControl =
        request.executionControl
            ? request.executionControl
            : std::make_shared<RuntimeExecutionControl>(
                  request.limits, request.cancellationToken);
    runtimeOptions.executionControl = executionControl;
    runtimeOptions.inheritedCallableInvoker =
        request.externalCallableInvoker;
    if (request.outputSink) {
        runtimeOptions.outputSink =
            [&sink = request.outputSink,
             &sources = data_->sources](
                const RuntimeOutputEvent& event) {
                return sink(projectOutputEvent(event, sources));
            };
    }

    BytecodeVmResult runtime;
    try {
        BytecodeVm vm;
        if (request.backend ==
            ModuleExecutionBackend::Bytecode) {
            runtime = vm.runValidated(
                data_->bytecode, data_->semantic, runtimeOptions);
        } else {
            runtime = vm.runValidated(
                data_->bytecode, data_->semantic,
                data_->staticTypedModule, runtimeOptions);
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& exception) {
        result.status = ModuleInvocationStatus::RuntimeFailed;
        result.execution = summarizeExecutionControl(
            *executionControl, request.backend);
        const std::vector<Diagnostic> diagnostics{
            Diagnostic{
                SourceSpan{},
                "host exception escaped module execution: " +
                    std::string(exception.what()),
                "MParser:HostExecutionException"}};
        result.diagnostics = projectDiagnostics(
            diagnostics, ModuleDiagnosticPhase::Execution,
            data_->sources, "MParser:RuntimeFailed");
        return result;
    } catch (...) {
        result.status = ModuleInvocationStatus::RuntimeFailed;
        result.execution = summarizeExecutionControl(
            *executionControl, request.backend);
        const std::vector<Diagnostic> diagnostics{
            Diagnostic{
                SourceSpan{},
                "unknown host exception escaped module execution",
                "MParser:HostExecutionException"}};
        result.diagnostics = projectDiagnostics(
            diagnostics, ModuleDiagnosticPhase::Execution,
            data_->sources, "MParser:RuntimeFailed");
        return result;
    }

    result.status =
        runtime.execution.stopReason !=
                RuntimeExecutionStopReason::None ||
            hasErrorDiagnostics(runtime.diagnostics)
            ? ModuleInvocationStatus::RuntimeFailed
            : ModuleInvocationStatus::Succeeded;
    result.entryFunction = runtime.entryFunction.empty()
                               ? request.entryFunction
                               : runtime.entryFunction;
    result.requestedOutputCount = runtime.requestedOutputCount;
    result.outputNames = std::move(runtime.outputNames);
    result.outputs = std::move(runtime.outputs);
    result.outputEvents.reserve(runtime.outputEvents.size());
    for (const auto& event : runtime.outputEvents) {
        result.outputEvents.push_back(
            projectOutputEvent(event, data_->sources));
    }
    result.topLevelExpressions.reserve(
        runtime.expressionResults.size());
    for (auto& expression : runtime.expressionResults) {
        result.topLevelExpressions.push_back(
            projectExpressionResult(
                std::move(expression), data_->sources));
    }
    result.variables = std::move(runtime.variables);
    result.diagnostics = projectDiagnostics(
        runtime.diagnostics, ModuleDiagnosticPhase::Execution,
        data_->sources, "MParser:RuntimeFailed");
    result.execution =
        summarizeExecution(runtime, request.backend);
    return result;
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

size_t CompiledModule::callableContextIdentity() const noexcept {
    return callableContext_ ? callableContext_->identity : 0;
}

RuntimeSourceCallableInvocationResult CompiledModule::invokeCallable(
    const RuntimeValue& callable,
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount,
    const std::shared_ptr<RuntimeSessionState>& state,
    const std::shared_ptr<RuntimeExecutionControl>& executionControl,
    ModuleExecutionBackend backend,
    const RuntimeSourceCallableInvoker& externalInvoker) const {
    RuntimeSourceCallableInvocationResult result;
    if (!valid()) {
        result.diagnostics = data_->diagnostics;
        return result;
    }

    BytecodeVmOptions options;
    options.profiling = BytecodeVmProfilingMode::Disabled;
    options.callableContext = callableContext_;
    options.sessionState = state;
    options.entryCallable = callable;
    options.arguments = arguments;
    options.requestedOutputCount = requestedOutputCount;
    options.typedRegionBackend = typedBackendFor(backend);
    options.executionControl = executionControl;
    options.inheritedCallableInvoker = externalInvoker;

    BytecodeVm vm;
    auto runtime = backend == ModuleExecutionBackend::Bytecode
                       ? vm.runValidated(
                             data_->bytecode, data_->semantic, options)
                       : vm.runValidated(
                             data_->bytecode, data_->semantic,
                             data_->staticTypedModule, options);
    for (auto& diagnostic : runtime.diagnostics) {
        if (diagnostic.sourceName.empty() &&
            diagnostic.span.begin.sourceId != kInvalidSourceId &&
            diagnostic.span.begin.sourceId < data_->sources.size()) {
            diagnostic.sourceName =
                data_->sources[diagnostic.span.begin.sourceId].name;
        }
    }
    for (auto& event : runtime.outputEvents) {
        if (event.sourceName.empty() &&
            event.span.begin.sourceId != kInvalidSourceId &&
            event.span.begin.sourceId < data_->sources.size()) {
            event.sourceName =
                data_->sources[event.span.begin.sourceId].name;
        }
    }
    result.outputs = std::move(runtime.outputs);
    result.diagnostics = std::move(runtime.diagnostics);
    result.outputEvents = std::move(runtime.outputEvents);
    result.succeeded =
        runtime.execution.stopReason == RuntimeExecutionStopReason::None &&
        !hasErrorDiagnostics(result.diagnostics);
    if (!result.succeeded) {
        result.outputs.clear();
    }
    return result;
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

ModuleInvocationResult CompiledModuleSession::execute(
    const ModuleInvocationRequest& request) const {
    return module_.execute(request, state_);
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
