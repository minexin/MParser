#pragma once

#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/bytecode.h"
#include "mparser/function_signature.h"
#include "mparser/source.h"

#include <optional>
#include <string>
#include <string_view>
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
    static CompiledModule compile(std::vector<SourceUnit> sources);

    bool valid() const;
    std::string_view source() const;
    const std::vector<SourceUnit>& sources() const;
    std::string_view sourceName(size_t sourceId) const;
    std::string_view sourceName(SourceSpan span) const;
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

    std::vector<SourceUnit> sources_;
    SemanticResult semantic_;
    BytecodeProgram bytecode_;
    std::vector<CompiledFunctionInfo> functions_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace mparser
