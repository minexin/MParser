#include "mparser/compiled_module.h"

#include "mparser/lexer.h"
#include "mparser/parser.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mparser {
namespace {

void collectInvocableFunctions(
    const HirNode* node, std::vector<CompiledFunctionInfo>& functions,
    std::vector<Diagnostic>& diagnostics) {
    if (!node || node->kind == HirKind::Class) {
        return;
    }

    if (node->kind == HirKind::Function) {
        for (const auto& function : functions) {
            if (function.name == node->label) {
                diagnostics.push_back(Diagnostic{
                    node->span,
                    "duplicate top-level function: " + node->label});
                return;
            }
        }

        functions.push_back(CompiledFunctionInfo{
            node->label, parseFunctionSignature(*node), node->span});
        return;
    }

    for (const auto& child : node->children) {
        collectInvocableFunctions(child.get(), functions, diagnostics);
    }
}

void appendDiagnostics(std::vector<Diagnostic>& destination,
                       const std::vector<Diagnostic>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
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

CompiledModule CompiledModule::compile(std::string source) {
    std::vector<SourceUnit> sources;
    sources.push_back(SourceUnit{"<memory>", std::move(source)});
    return compile(std::move(sources));
}

CompiledModule CompiledModule::compile(std::vector<SourceUnit> sources) {
    CompiledModule module;
    module.sources_ = std::move(sources);
    if (module.sources_.empty()) {
        module.diagnostics_.push_back(Diagnostic{
            SourceSpan{}, "compiled module requires at least one source"});
        return module;
    }

    auto root = std::make_unique<SyntaxNode>(SyntaxKind::CompilationUnit);
    bool rootSpanInitialized = false;
    std::map<std::string, SourceSpan> classDefinitions;
    for (size_t sourceId = 0; sourceId < module.sources_.size(); ++sourceId) {
        auto& source = module.sources_[sourceId];
        if (source.name.empty()) {
            source.name = "<source:" + std::to_string(sourceId) + ">";
        }

        Lexer lexer(source.content, sourceId);
        Parser parser(lexer.lex());
        auto parse = parser.parse();
        appendDiagnostics(module.diagnostics_, parse.diagnostics);
        if (!parse.root) {
            continue;
        }

        collectTopLevelClasses(*parse.root, classDefinitions,
                               module.diagnostics_);
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

    if (!module.diagnostics_.empty()) {
        return module;
    }

    SemanticAnalyzer analyzer;
    module.semantic_ = analyzer.analyze(*root);
    appendDiagnostics(module.diagnostics_, module.semantic_.diagnostics);
    if (!module.semantic_.root || !module.semantic_.diagnostics.empty()) {
        return module;
    }

    BytecodeLowerer lowerer;
    module.bytecode_ = lowerer.lower(module.semantic_);
    appendDiagnostics(module.diagnostics_, module.bytecode_.diagnostics);
    if (!module.bytecode_.diagnostics.empty()) {
        return module;
    }

    collectInvocableFunctions(module.semantic_.root.get(), module.functions_,
                              module.diagnostics_);
    return module;
}

bool CompiledModule::valid() const {
    return semantic_.root != nullptr && diagnostics_.empty();
}

std::string_view CompiledModule::source() const {
    return sources_.empty() ? std::string_view{}
                            : std::string_view{sources_.front().content};
}

const std::vector<SourceUnit>& CompiledModule::sources() const {
    return sources_;
}

std::string_view CompiledModule::sourceName(size_t sourceId) const {
    if (sourceId >= sources_.size()) {
        return {};
    }
    return sources_[sourceId].name;
}

std::string_view CompiledModule::sourceName(SourceSpan span) const {
    return sourceName(span.begin.sourceId);
}

const std::vector<Diagnostic>& CompiledModule::diagnostics() const {
    return diagnostics_;
}

const SemanticResult& CompiledModule::semantic() const {
    return semantic_;
}

const BytecodeProgram& CompiledModule::bytecode() const {
    return bytecode_;
}

const std::vector<CompiledFunctionInfo>& CompiledModule::functions() const {
    return functions_;
}

const CompiledFunctionInfo* CompiledModule::findFunction(
    std::string_view name) const {
    for (const auto& function : functions_) {
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
        return diagnostics_;
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
    if (argumentCount < function->signature.parameters.size() ||
        (!function->signature.hasVarargin &&
         function->signature.parameters.size() != argumentCount)) {
        return {Diagnostic{
            function->span,
            "function argument count mismatch for: " + function->name}};
    }
    if (!function->signature.hasVarargout && requestedOutputCount &&
        *requestedOutputCount > function->signature.outputs.size()) {
        return {Diagnostic{
            function->span,
            "function output count mismatch for: " + function->name}};
    }
    return {};
}

BytecodeVmResult CompiledModule::invoke(
    const BytecodeVmOptions& options) const {
    const auto validation =
        validateInvocation(options.entryFunction, options.arguments.size(),
                           options.requestedOutputCount);
    if (!validation.empty()) {
        BytecodeVmResult result;
        result.diagnostics = validation;
        return result;
    }

    BytecodeVm vm;
    return vm.run(bytecode_, semantic_, options);
}

AdaptiveBytecodeVmSession CompiledModule::createAdaptiveSession(
    const AdaptiveBytecodeVmOptions& options) const {
    const auto validation =
        validateInvocation(options.entryFunction, options.arguments.size(),
                           options.requestedOutputCount);
    if (!validation.empty()) {
        throw std::invalid_argument(firstDiagnosticMessage(validation));
    }
    return AdaptiveBytecodeVmSession(bytecode_, semantic_, options);
}

} // namespace mparser
