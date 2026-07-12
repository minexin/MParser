#pragma once

#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/bytecode.h"
#include "mparser/function_signature.h"

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace mparser {

struct CompiledFunctionInfo {
    std::string name;
    FunctionSignature signature;
    SourceSpan span;
};

class CompiledModule {
public:
    static CompiledModule compile(std::string source);

    bool valid() const;
    std::string_view source() const;
    const std::vector<Diagnostic>& diagnostics() const;
    const SemanticResult& semantic() const;
    const BytecodeProgram& bytecode() const;
    const std::vector<CompiledFunctionInfo>& functions() const;
    const CompiledFunctionInfo* findFunction(std::string_view name) const;

    std::vector<Diagnostic> validateInvocation(
        std::string_view entryFunction,
        size_t argumentCount,
        std::optional<size_t> requestedOutputCount = std::nullopt) const;
    BytecodeVmResult invoke(const BytecodeVmOptions& options = {}) const;
    AdaptiveBytecodeVmSession createAdaptiveSession(
        const AdaptiveBytecodeVmOptions& options = {}) const;

private:
    CompiledModule() = default;

    std::string source_;
    SemanticResult semantic_;
    BytecodeProgram bytecode_;
    std::vector<CompiledFunctionInfo> functions_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace mparser
