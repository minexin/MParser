#include "mparser/compiled_module.h"

#include "mparser/lexer.h"
#include "mparser/parser.h"

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

std::string firstDiagnosticMessage(
    const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) {
        return "compiled module invocation is invalid";
    }
    return diagnostics.front().message;
}

} // namespace

CompiledModule CompiledModule::compile(std::string source) {
    CompiledModule module;
    module.source_ = std::move(source);

    Lexer lexer(module.source_);
    Parser parser(lexer.lex());
    auto parse = parser.parse();
    appendDiagnostics(module.diagnostics_, parse.diagnostics);
    if (!parse.root || !parse.diagnostics.empty()) {
        return module;
    }

    SemanticAnalyzer analyzer;
    module.semantic_ = analyzer.analyze(*parse.root);
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
    return source_;
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
    if (function->signature.parameters.size() != argumentCount) {
        return {Diagnostic{
            function->span,
            "function argument count mismatch for: " + function->name}};
    }
    if (requestedOutputCount &&
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
