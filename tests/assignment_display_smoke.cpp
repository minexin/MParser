#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/execution/jit/typed_ir.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace mparser;
enum class Engine { Interpreter, Bytecode, Portable, Native, Automatic };

void require(bool condition, std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

BytecodeVmResult run(std::string_view source, Engine engine, bool capture = true) {
    Lexer lexer(source, 0);
    Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.diagnostics.empty(), "display test must parse");
    SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root, {SourceUnit{"display_probe.m", std::string(source)}});
    require(semantic.diagnostics.empty(), "display test must analyze");
    if (engine == Engine::Interpreter) {
        InterpreterOptions options;
        options.captureAssignmentDisplays = capture;
        auto interpreted = Interpreter{}.run(semantic, options);
        BytecodeVmResult result;
        result.variables = std::move(interpreted.variables);
        result.assignmentDisplays = std::move(interpreted.assignmentDisplays);
        result.outputEvents = std::move(interpreted.outputEvents);
        result.expressionResults = std::move(interpreted.expressionResults);
        result.diagnostics = std::move(interpreted.diagnostics);
        return result;
    }
    auto program = BytecodeLowerer{}.lower(semantic);
    const auto verified = validateBytecodeProgram(program, &semantic);
    require(verified.succeeded, "display metadata must verify");
    BytecodeVmOptions options;
    options.captureAssignmentDisplays = capture;
    if (engine == Engine::Bytecode) { return BytecodeVm{}.run(program, semantic, options); }
    options.typedRegionBackend = engine == Engine::Portable ? TypedRegionBackend::Portable :
        engine == Engine::Native ? TypedRegionBackend::Native : TypedRegionBackend::Auto;
    const auto typed = BytecodeTypedIrBuilder{}.build(
        BytecodeOptimizationPlanner{}.planStaticRegions(program, semantic.builtinRegistry));
    auto result = BytecodeVm{}.run(program, semantic, typed, options);
    if (capture && !typed.regions.empty()) {
        require(result.execution.optimizedExecutionSuppressed, "recording must disclose optimized suppression");
        require(std::all_of(result.typedRegionExecutions.begin(), result.typedRegionExecutions.end(),
            [](const auto& profile) { return profile.executionCount == 0; }),
            "optimized regions must not skip recorded commits");
    }
    return result;
}

const RuntimeValue& variable(const BytecodeVmResult& result, std::string_view name) {
    const auto found = std::find_if(result.variables.begin(), result.variables.end(),
        [&](const auto& value) { return value.name == name; });
    require(found != result.variables.end(), "expected final variable");
    return found->value;
}

void display(const RuntimeAssignmentDisplay& event, std::string_view label,
             const RuntimeValue& value, bool suppressed = false,
             RuntimeDisplayFormat format = {}) {
    require(event.label == label, "display uses actual assignment root label");
    require(event.text == runtimeFormatConsoleValue(value, format), "display freezes execution-time content");
    require(event.outputSuppressed == suppressed, "display preserves separator suppression");
    require(event.sourceName == "display_probe.m", "display retains source identity");
    require(event.span.begin.sourceId == 0 && event.span.end.offset > event.span.begin.offset,
        "display retains a real nonempty source span");
    require(event.lineSpacing == format.spacing, "display freezes spacing");
}

void checkEngine(Engine engine) {
    const auto number = [](double value) { return makeRuntimeNumberValue(value); };
    {
        auto result = run("a=1\na=2;\na=3 %;\na=4\na=5", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 5, "all assignments recorded");
        for (size_t index = 0; index < 5; ++index) {
            display(result.assignmentDisplays[index], "a", number(static_cast<double>(index + 1)), index == 1);
            require(result.assignmentDisplays[index].sequence == index, "assignment order preserved");
            require(result.assignmentDisplays[index].span.begin.line == static_cast<int>(index + 1),
                "assignment line preserved");
        }
        require(variable(result,"a").number == 5, "workspace still has final value only");
    }
    {
        auto result = run("a=6\ndisp('between')\na=7", engine);
        require(result.assignmentDisplays.size() == 2 && result.outputEvents.size() == 1,
            "interleaved output recorded");
        display(result.assignmentDisplays[0], "a", number(6));
        display(result.assignmentDisplays[1], "a", number(7));
        require(result.assignmentDisplays[0].sequence == 0 && result.outputEvents[0].sequence == 1 &&
                result.assignmentDisplays[1].sequence == 2, "text and assignments share sequence");
    }
    {
        auto result = run("a=1\n2+3\nfprintf('middle')\na=4;", engine);
        require(result.assignmentDisplays.size() == 2 && result.expressionResults.size() == 1 &&
                result.outputEvents.size() == 1, "mixed output families retained");
        require(result.assignmentDisplays[0].sequence == 0 && result.expressionResults[0].sequence == 1 &&
                result.outputEvents[0].sequence == 2 && result.assignmentDisplays[1].sequence == 3,
                "expression/text/assignment chronological order");
    }
    {
        auto result = run("a=8\nerror('DisplayProbe:Expected','stop')\na=9", engine);
        require(hasErrorDiagnostics(result.diagnostics) && result.assignmentDisplays.size() == 1,
            "failure retains earlier committed display only");
        display(result.assignmentDisplays.front(), "a", number(8));
    }
    {
        auto result = run("a=7;\na=error('DisplayProbe:Expected','stop');\nb=9", engine);
        require(hasErrorDiagnostics(result.diagnostics) && result.assignmentDisplays.size() == 1 &&
                variable(result,"a").number == 7, "failed RHS must neither replace old value nor publish a commit");
        require(std::none_of(result.variables.begin(),result.variables.end(),
            [](const auto& item) { return item.name == "b"; }), "unhandled error stops later statements");
    }
    {
        auto result = run("a=1\ntry\na=2\nerror('DisplayProbe:Expected','stop')\na=999\n"
            "catch caught\na=3\nend\na=4", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 4,
            "catch preserves earlier history and resumes only in the catch arm");
        for (size_t index = 0; index < 4; ++index) {
            display(result.assignmentDisplays[index], "a", number(static_cast<double>(index + 1)));
        }
    }
    {
        auto result = run("a=1\nanswer=failing();\na=999\nfunction y=failing()\ny=2\n"
            "error('DisplayProbe:Expected','stop')\ny=999\nend", engine);
        require(hasErrorDiagnostics(result.diagnostics) && result.assignmentDisplays.size() == 2,
            "function failure retains callee history without a failed caller assignment");
        display(result.assignmentDisplays[0], "a", number(1));
        display(result.assignmentDisplays[1], "y", number(2));
    }
    {
        auto result = run("a=[1,2]; a(2)=9\na(1)=7;", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3, "indexed commits captured");
        display(result.assignmentDisplays[0], "a", makeRuntimeVectorValue({1,2}), true);
        display(result.assignmentDisplays[1], "a", makeRuntimeVectorValue({1,9}));
        display(result.assignmentDisplays[2], "a", makeRuntimeVectorValue({7,9}), true);
    }
    {
        auto result = run("s=struct('v',[1,2]); s.v(2)=9\ns.v=[5,6];", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3, "nested writes captured");
        display(result.assignmentDisplays[1], "s", makeRuntimeStructValue({{"v",makeRuntimeVectorValue({1,9})}}));
    }
    {
        auto result = run("c={1,2}; c{1}=5\nc{2}=9;", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3, "cell writes captured");
        display(result.assignmentDisplays[1], "c", makeRuntimeCellValue({number(5),number(2)}));
    }
    const std::string pair = "\nfunction [left,right]=pair()\nleft=11;\nright=12;\nend\n";
    {
        auto result = run("[a,a]=pair()" + pair, engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 4,
            "function-body and repeated-target records retained");
        require(variable(result,"a").number == 12, "multi-output stores must commit left to right");
        display(result.assignmentDisplays[2], "a", number(12));
        display(result.assignmentDisplays[3], "a", number(12));
        auto disabled = run("[a,a]=pair();" + pair, engine, false);
        require(disabled.assignmentDisplays.empty() && variable(disabled,"a").number == 12,
            "correct store order is not conditional on recording");
    }
    {
        auto result = run("[left,right]=pair()" + pair, engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 4, "multi-output captured");
        display(result.assignmentDisplays[2], "left", number(11));
        display(result.assignmentDisplays[3], "right", number(12));
        auto ignored = run("[~,right]=pair()" + pair, engine);
        require(ignored.diagnostics.empty() && ignored.assignmentDisplays.size() == 3, "ignored target not displayed");
        display(ignored.assignmentDisplays.back(), "right", number(12));
    }
    {
        auto result = run("a=[1,2]; [a(1),a(1)]=pair()" + pair, engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 5, "indexed repeated outputs captured");
        display(result.assignmentDisplays[3], "a", makeRuntimeVectorValue({12,2}));
        display(result.assignmentDisplays[4], "a", makeRuntimeVectorValue({12,2}));
    }
    {
        auto result = run("for k=1:3\na=k\nend", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3,
            "loop assignments recorded without inventing header display");
        for (size_t index = 0; index < 3; ++index) {
            display(result.assignmentDisplays[index], "a", number(static_cast<double>(index + 1)));
        }
    }
    {
        auto result = run("answer=inner();\nfunction y=inner()\ny=41\ny=42;\nend", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3, "function-scope display captured");
        display(result.assignmentDisplays[0], "y", number(41));
        display(result.assignmentDisplays[1], "y", number(42), true);
        display(result.assignmentDisplays[2], "answer", number(42), true);
    }
    {
        auto result = run("global calls; calls=0; a=[1,2]; a(next_index())=9\n"
            "function n=next_index()\nglobal calls; calls=calls+1; n=2;\nend", engine);
        require(result.diagnostics.empty() && variable(result,"calls").number == 1,
            "capture must not reevaluate an index or execute extra source");
        display(result.assignmentDisplays.back(), "a", makeRuntimeVectorValue({1,9}));
    }
    {
        auto result = run("a=1,b=2; c=3 %;\r\nd=4;", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 4, "comma/comment/CRLF parsed");
        require(!result.assignmentDisplays[0].outputSuppressed && result.assignmentDisplays[1].outputSuppressed &&
                !result.assignmentDisplays[2].outputSuppressed && result.assignmentDisplays[3].outputSuppressed,
                "only real semicolon separators suppress output");
        require(result.assignmentDisplays[3].span.begin.line == 2 &&
                result.assignmentDisplays[3].span.begin.offset == 17, "CRLF span uses original byte offsets");
    }
    {
        auto result = run("% \xe8\xb7\xaf\xe5\xbe\x84\r\na=1", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 1 &&
                result.assignmentDisplays[0].span.begin.offset == 10 &&
                result.assignmentDisplays[0].span.begin.line == 2, "UTF-8 and CRLF offsets are original bytes");
    }
    {
        auto result = run("format short; a=1/7\nformat long; a=1/7;\nformat compact; b=2", engine);
        require(result.diagnostics.empty() && result.assignmentDisplays.size() == 3, "format changes recorded");
        display(result.assignmentDisplays[0], "a", number(1.0/7.0));
        RuntimeDisplayFormat longFormat;
        longFormat.numeric = RuntimeNumericDisplayFormat::Long;
        display(result.assignmentDisplays[1], "a", number(1.0/7.0), true, longFormat);
        longFormat.spacing = RuntimeLineSpacing::Compact;
        display(result.assignmentDisplays[2], "b", number(2), false, longFormat);
    }
    {
        auto recorded = run("a=1; a=2;", engine);
        auto ordinary = run("a=1; a=2;", engine, false);
        if (engine == Engine::Bytecode) {
            require(recorded.executedInstructionCount == ordinary.executedInstructionCount,
                "recording adds no hidden source or VM instructions");
        }
    }
    require(run("a=1", engine, false).assignmentDisplays.empty(), "legacy default collection stays off");
}

void verifierGuards() {
    Lexer lexer("[a,b]=pair();\nfunction [a,b]=pair()\na=1;b=2;end",0);
    Parser parser(lexer.lex());
    auto parsed = parser.parse();
    auto semantic = SemanticAnalyzer{}.analyze(*parsed.root);
    auto program = BytecodeLowerer{}.lower(semantic);
    auto reverse = std::find_if(program.instructions.begin(),program.instructions.end(),
        [](const auto& instruction) { return instruction.op == BytecodeOp::ReverseOutputs; });
    require(reverse != program.instructions.end(), "ordered output stack instruction emitted");
    reverse->operandCount = 1000000;
    require(!validateBytecodeProgram(program,&semantic).succeeded, "verifier rejects oversized reversal");
    auto malformed = BytecodeLowerer{}.lower(semantic);
    malformed.instructions.front().assignmentDisplayNames = {"a"};
    require(!validateBytecodeProgram(malformed,&semantic).succeeded, "verifier rejects unbound display roots");
}
}

int main() {
    try {
        for (const auto engine : {Engine::Interpreter, Engine::Bytecode, Engine::Portable, Engine::Native, Engine::Automatic}) {
            std::cout << "requested engine " << static_cast<int>(engine) << '\n';
            checkEngine(engine);
        }
        verifierGuards();
        std::cout << "assignment display foundation: five requested engines, committed text, suppression, order, failure and roots passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
