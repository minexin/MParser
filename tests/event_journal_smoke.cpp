#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/execution/jit/typed_ir.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/io/runtime_system.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace mparser;
enum class Engine { Hir, Bytecode, Portable, Native, Automatic };
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
std::shared_ptr<RuntimeSessionState> session() {
    RuntimeSystemContextOptions options;
    options.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    return std::make_shared<RuntimeSessionState>(std::make_shared<RuntimeSystemContext>(options));
}
BytecodeVmResult run(const std::string& source, Engine engine,
    std::shared_ptr<RuntimeExecutionControl> control = {}, bool capture = true) {
    auto module = CompiledModule::compile(std::vector<SourceUnit>{{"journal_probe.m",source}});
    require(module.valid(),"journal fixture must compile");
    if (engine == Engine::Hir) {
        InterpreterOptions options;
        options.sessionState=session(); options.executionControl=control; options.captureAssignmentDisplays=capture;
        auto interpreted=Interpreter{}.run(module.semantic(),options);
        BytecodeVmResult result;
        result.variables=std::move(interpreted.variables);
        result.outputEvents=std::move(interpreted.outputEvents);
        result.assignmentDisplays=std::move(interpreted.assignmentDisplays);
        result.journalEvents=std::move(interpreted.journalEvents);
        result.diagnostics=std::move(interpreted.diagnostics);
        return result;
    }
    BytecodeVmOptions options;
    options.sessionState=session(); options.executionControl=control; options.captureAssignmentDisplays=capture;
    if (engine == Engine::Bytecode) { return BytecodeVm{}.run(module.bytecode(),module.semantic(),options); }
    options.typedRegionBackend=engine==Engine::Portable ? TypedRegionBackend::Portable :
        engine==Engine::Native ? TypedRegionBackend::Native : TypedRegionBackend::Auto;
    const auto typed=BytecodeTypedIrBuilder{}.build(BytecodeOptimizationPlanner{}.planStaticRegions(module.bytecode()));
    return BytecodeVm{}.run(module.bytecode(),module.semantic(),typed,options);
}
const RuntimeValue& variable(const BytecodeVmResult& result,const std::string& name) {
    const auto found=std::find_if(result.variables.begin(),result.variables.end(),[&](const auto& item){return item.name==name;});
    require(found!=result.variables.end(),"expected journal result variable"); return found->value;
}
void ordered(const std::vector<RuntimeJournalEvent>& events) {
    for (size_t index=0;index<events.size();++index) {
        require(events[index].sequence==index,"journal must be contiguous in actual emission order");
        require(events[index].sourceContext!=0,"each producer has a source context");
        require(events[index].disposition!=RuntimeJournalDisposition::Raised,"outer results have no unresolved diagnostic disposition");
    }
}
size_t count(const std::vector<RuntimeJournalEvent>& events,RuntimeJournalKind kind) {
    return static_cast<size_t>(std::count_if(events.begin(),events.end(),[&](const auto& event){return event.kind==kind;}));
}
void checkEngine(Engine engine) {
    {
        auto result=run("a=1\ndisp('middle');\nwarning('Journal:Warning','notice');\n2+3\na=2;\n"
            "error('Journal:Stop','stop');\na=99",engine);
        auto& events=result.journalEvents;
        require(hasErrorDiagnostics(result.diagnostics)&&events.size()==6,"all pre-failure event families recorded");
        ordered(events);
        const std::vector<RuntimeJournalKind> kinds{RuntimeJournalKind::Assignment,RuntimeJournalKind::Text,
            RuntimeJournalKind::Warning,RuntimeJournalKind::Expression,RuntimeJournalKind::Assignment,RuntimeJournalKind::Error};
        for(size_t index=0;index<kinds.size();++index) { require(events[index].kind==kinds[index],"mixed family ordering"); }
        require(std::get<RuntimeAssignmentDisplay>(events[4].payload).outputSuppressed,"suppression stays explicit");
        require(events[5].disposition==RuntimeJournalDisposition::Unhandled,"unhandled error status retained");
        require(std::get<Diagnostic>(events[2].payload).sourceName=="journal_probe.m" &&
            std::get<Diagnostic>(events[2].payload).span.begin.line==3,"warning provenance retained");
    }
    {
        auto result=run("a=1\ntry\na=2\nerror('Journal:Caught','inside');\na=99\n"
            "catch caught\na=3\nend\ndisp('after');\na=4",engine);
        require(result.diagnostics.empty()&&result.journalEvents.size()==6,"caught failure keeps history and resumes");
        ordered(result.journalEvents);
        require(result.journalEvents[2].kind==RuntimeJournalKind::Error &&
            result.journalEvents[2].disposition==RuntimeJournalDisposition::Caught,"caught error is not a final error");
        auto rendered=runtimeRenderEventJournal(result.journalEvents,0);
        require(rendered.succeeded&&rendered.text.find("inside")==std::string::npos&&rendered.text.find("a = 99")==std::string::npos,
            "caught control-flow errors do not become console errors or later writes");
    }
    {
        auto result=run("a=1\neval('inner=2; disp(''inside''); warning(''Journal:Inner'',''inner warning'');');\na=3",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"eval fixture executes");
        ordered(result.journalEvents);
        require(result.journalEvents.size()==5&&count(result.journalEvents,RuntimeJournalKind::Text)==1 &&
            count(result.journalEvents,RuntimeJournalKind::Warning)==1,"eval aggregate is not recorded a second time");
        require(result.journalEvents[0].sourceContext!=result.journalEvents[1].sourceContext &&
            std::get<RuntimeAssignmentDisplay>(result.journalEvents[1].payload).sourceName=="<eval>",
            "temporary module retains its own source context");
    }
    {
        auto result=run("text=evalc('a=6; disp(''middle''); a=7');",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"evalc fixture executes");
        ordered(result.journalEvents);
        const auto captured = runtimeTextScalarUtf8(variable(result,"text"));
        require(captured==runtimeFormatDisplay(makeRuntimeCharacterVectorUtf8("middle")).text + "a = 7\n\n",
            "captured transcript includes real unsuppressed assignments in order");
        require(result.outputEvents.empty() && result.journalEvents.front().captureDepth==1 &&
            result.journalEvents.back().captureDepth==0,"evalc output is captured without leaking to caller console");
    }
    {
        auto result=run("outer=evalc('x=2; nested=evalc(''y=3; disp(''''hidden'''');''); disp(''outer'');');",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"nested evalc fixture executes");
        ordered(result.journalEvents);
        require(runtimeTextScalarUtf8(variable(result,"outer"))==runtimeFormatDisplay(makeRuntimeCharacterVectorUtf8("outer")).text &&
            runtimeTextScalarUtf8(variable(result,"nested"))==runtimeFormatDisplay(makeRuntimeCharacterVectorUtf8("hidden")).text,
            "nested captures remain separate");
        require(std::any_of(result.journalEvents.begin(),result.journalEvents.end(),
            [](const auto& event){return event.captureDepth==2;}),"capture depth is not flattened to one boolean");
    }
    {
        auto result=run("answer=eval('2+3');",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"returning eval fixture executes");
        require(result.journalEvents.size()==1 &&
            std::get<RuntimeAssignmentDisplay>(result.journalEvents[0].payload).label=="answer",
            "compiler-created eval output bindings never become user display events");
    }
    {
        auto result=run("eval('local_parent()');\nfunction local_parent()\nx=1\ndisp('parent');\nend",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"dynamic inherited callback executes");
        ordered(result.journalEvents);
        require(result.journalEvents.size()==2 && count(result.journalEvents,RuntimeJournalKind::Text)==1,
            "inherited callback publishes each actual event once");
        require(std::get<RuntimeAssignmentDisplay>(result.journalEvents.front().payload).sourceName=="journal_probe.m",
            "inherited callback keeps original source provenance");
    }
    {
        auto result=run("eval('a=1; error(''Journal:Fallback'',''failed'');','a=2;'); a=3;",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"eval catch expression recovers");
        ordered(result.journalEvents);
        require(count(result.journalEvents,RuntimeJournalKind::Error)==1,"fallback retains one original error record");
        const auto error=std::find_if(result.journalEvents.begin(),result.journalEvents.end(),
            [](const auto& event){return event.kind==RuntimeJournalKind::Error;});
        require(error->disposition==RuntimeJournalDisposition::Caught,"eval catch expression marks the primary error caught");
    }
    {
        auto result=run("eval('error(''Journal:Original'',''primary'');',123);",engine);
        require(hasErrorDiagnostics(result.diagnostics)&&count(result.journalEvents,RuntimeJournalKind::Error)==1,
            "invalid catch argument is rejected before executing the source");
        ordered(result.journalEvents);
        require(result.journalEvents.front().disposition==RuntimeJournalDisposition::Unhandled &&
            std::get<Diagnostic>(result.journalEvents.front().payload).identifier!="Journal:Original",
            "argument validation must not invent an unexecuted primary-source error");
    }
    {
        auto result=run("a=1; eval('b=;','a=2;'); a=3;",engine);
        require(!hasErrorDiagnostics(result.diagnostics),"dynamic compile failure is recovered by catch expression");
        ordered(result.journalEvents);
        require(result.journalEvents.size()==4&&result.journalEvents[1].kind==RuntimeJournalKind::Error &&
            result.journalEvents[1].disposition==RuntimeJournalDisposition::Caught,
            "dynamic compile diagnostics retain their actual place before recovery");
        require(std::get<Diagnostic>(result.journalEvents[1].payload).sourceName=="<eval>",
            "dynamic compile diagnostic keeps original submitted-source provenance");
    }
    {
        auto result=run("text=evalc('a=1; error(''Journal:Escape'',''failed'');');",engine);
        require(hasErrorDiagnostics(result.diagnostics),"error escapes evalc");
        ordered(result.journalEvents);
        const auto error=std::find_if(result.journalEvents.begin(),result.journalEvents.end(),
            [](const auto& event){return event.kind==RuntimeJournalKind::Error;});
        require(error!=result.journalEvents.end()&&error->captureDepth==1&&
            error->disposition==RuntimeJournalDisposition::Unhandled,"captured-origin errors still escape as unhandled");
    }
    {
        auto control=std::make_shared<RuntimeExecutionControl>();
        auto first=run("a=1",engine,control);
        auto second=run("a=2",engine,control);
        require(!control->eventJournal() && first.journalEvents.size()==1&&second.journalEvents.size()==1 &&
            second.journalEvents[0].sequence==0,"sequential runs do not leak the previous journal");
        require(run("a=3",engine,control,false).journalEvents.empty(),"default mode still has no journal");
        require(std::get<RuntimeAssignmentDisplay>(first.journalEvents[0].payload).valueSnapshot.node().numeric[0].real==1,
            "retained first transcript stays immutable across later runs");
    }
    {
        auto result=run("warning('off','Journal:Hidden'); warning('Journal:Hidden','hidden'); a=1;",engine);
        require(!hasErrorDiagnostics(result.diagnostics)&&count(result.journalEvents,RuntimeJournalKind::Warning)==0,
            "suppressed warnings are not invented by the journal");
    }
}

void crossModule() {
    auto producer=CompiledModule::compile(std::vector<SourceUnit>{{"producer.m",
        "function h=make_handle()\nh=@foreign;\nend\nfunction y=foreign(x)\ny=x+1\n"
        "disp('foreign');\nwarning('Journal:Foreign','foreign warning');\nend"}});
    ModuleInvocationRequest seed; seed.entryFunction="make_handle"; seed.requestedOutputCount=1;
    const auto exported=producer.execute(seed);
    require(exported.succeeded()&&exported.outputs.size()==1,"cross-module callback fixture exports a real handle");
    auto caller=CompiledModule::compile(std::vector<SourceUnit>{{"caller.m","a=1\nb=callback(2)\na=4"}});
    for(const auto backend:{ModuleExecutionBackend::Bytecode,ModuleExecutionBackend::Portable,
            ModuleExecutionBackend::Native,ModuleExecutionBackend::Automatic}) {
        auto state=session(); auto control=std::make_shared<RuntimeExecutionControl>();
        ModuleInvocationRequest request; request.backend=backend; request.captureEventJournal=true;
        request.executionControl=control;
        request.initialWorkspace={{"callback",exported.outputs[0]}};
        request.externalCallableInvoker=[&](const RuntimeValue& callable,const std::vector<RuntimeValue>& args,
            size_t outputs,SourceSpan,RuntimeWorkspace*) {
            return producer.invokeCallable(callable,args,outputs,state,control,backend,{});
        };
        const auto result=caller.execute(request);
        require(result.succeeded()&&result.journalEvents.size()==6,"escaped-module events share the caller journal");
        ordered(result.journalEvents);
        require(result.journalEvents[1].sourceContext!=result.journalEvents[0].sourceContext &&
            std::get<RuntimeAssignmentDisplay>(result.journalEvents[1].payload).sourceName=="producer.m",
            "escaped module source IDs are not rebound to caller source IDs");
        require(count(result.journalEvents,RuntimeJournalKind::Text)==1&&count(result.journalEvents,RuntimeJournalKind::Warning)==1,
            "forwarded escaped-module text and warnings are not duplicated");
        require(!control->eventJournal(),"cross-module ownership ends at outer invocation exit");
    }
}

void rejectionAndCancellation() {
    auto rejected=std::make_shared<RuntimeExecutionControl>(RuntimeExecutionLimits{},std::nullopt,nullptr,
        RuntimeInputSource{},[](std::string_view){return false;});
    auto result=run("a=1\ndisp('rejected');\na=99",Engine::Bytecode,rejected);
    require(hasErrorDiagnostics(result.diagnostics)&&result.journalEvents.size()==3,
        "console rejection keeps generated text and pre-failure history");
    require(result.journalEvents.back().disposition==RuntimeJournalDisposition::Unhandled,
        "rejection error remains a real failure");
    RuntimeCancellationToken token; token.requestCancellation();
    auto cancelled=std::make_shared<RuntimeExecutionControl>(RuntimeExecutionLimits{},token);
    result=run("a=1",Engine::Bytecode,cancelled);
    require(hasErrorDiagnostics(result.diagnostics)&&count(result.journalEvents,RuntimeJournalKind::Assignment)==0&&
        count(result.journalEvents,RuntimeJournalKind::Error)==1,"pre-cancellation produces no fake assignments");
}

void scopeAndForwarding() {
    RuntimeExecutionControl control;
    std::vector<RuntimeJournalEvent> retained;
    {
        RuntimeEventJournalScope outer(&control,true);
        auto journal=outer.journal();
        RuntimeOutputEvent event; event.text="one";
        journal->text(event,outer.sourceContext(),0,"outer.m");
        {
            RuntimeEventJournalScope inner(&control,false);
            require(inner.journal()==journal,"child inherits recording even with its local option off");
            journal->text(event,inner.sourceContext(),0,"inner.m");
            require(journal->size()==1,"forwarded event identity prevents duplicate append");
            RuntimeOutputEvent replay; replay.text="synthetic transcript"; replay.transcriptReplay=true;
            journal->text(replay,inner.sourceContext(),0,"inner.m");
            require(journal->size()==1,"synthetic transcript is not a second generated event");
        }
        Diagnostic error{{},"failure","Journal:Unit"};
        journal->diagnostic(error,outer.sourceContext(),0,"outer.m");
        auto before=outer.snapshot();
        journal->caught(error);
        retained=outer.snapshot();
        require(before.back().disposition==RuntimeJournalDisposition::Raised &&
            retained.back().disposition==RuntimeJournalDisposition::Caught,"snapshots do not mutate when later catch policy changes");
        bool rejected=false;
        RuntimeAssignmentDisplay display;
        try { journal->display(display,RuntimeJournalKind::Warning,outer.sourceContext(),0); }
        catch(const std::invalid_argument&) { rejected=true; }
        require(rejected&&journal->size()==2,"invalid display kind changes no recorded state");
    }
    require(!control.eventJournal()&&retained.size()==2,"outer scope detaches without invalidating retained events");
    try { RuntimeEventJournalScope scope(&control,true); throw std::runtime_error("unwind"); }
    catch(const std::runtime_error&) {}
    require(!control.eventJournal(),"exception unwinding also detaches the journal");
}

void hostExceptions() {
    const auto module=CompiledModule::compile(std::vector<SourceUnit>{{"host_throw.m","a=1\ndisp('before throw');\na=99"}});
    for (const auto backend:{ModuleExecutionBackend::Bytecode,ModuleExecutionBackend::Portable,
            ModuleExecutionBackend::Native,ModuleExecutionBackend::Automatic}) {
        ModuleInvocationRequest request; request.backend=backend; request.captureEventJournal=true;
        request.executionControl=std::make_shared<RuntimeExecutionControl>();
        request.outputSink=[](const ModuleOutputEvent&)->bool {throw std::runtime_error("host sink throw");};
        const auto result=module.execute(request);
        require(!result.succeeded() && result.journalEvents.size()==3,
            "module boundary preserves pre-exception journal and typed host failure");
        ordered(result.journalEvents);
        require(result.journalEvents[0].kind==RuntimeJournalKind::Assignment &&
            result.journalEvents[1].kind==RuntimeJournalKind::Text &&
            std::get<Diagnostic>(result.journalEvents[2].payload).identifier=="MParser:BuiltinHostException",
            "builtin-mapped host exception follows the actual generated assignment/text");
        require(!request.executionControl->eventJournal(),"host failure leaves no active journal behind");
    }
    const auto producer=CompiledModule::compile("function h=make_handle()\nh=@foreign;\nend\nfunction y=foreign(x)\ny=x;\nend");
    ModuleInvocationRequest seed; seed.entryFunction="make_handle"; seed.requestedOutputCount=1;
    const auto exported=producer.execute(seed);
    require(exported.succeeded()&&exported.outputs.size()==1,"throwing invoker fixture exports real handle");
    const auto caller=CompiledModule::compile(std::vector<SourceUnit>{{"invoker_throw.m",
        "a=1\ndisp('before invoker');\nb=callback(2);\na=99"}});
    ModuleInvocationRequest request; request.captureEventJournal=true;
    request.initialWorkspace={{"callback",exported.outputs[0]}};
    request.executionControl=std::make_shared<RuntimeExecutionControl>();
    request.externalCallableInvoker=[](const RuntimeValue&,const std::vector<RuntimeValue>&,size_t,SourceSpan,RuntimeWorkspace*)
        ->RuntimeSourceCallableInvocationResult {throw std::runtime_error("external invoker throw");};
    const auto result=caller.execute(request);
    require(!result.succeeded()&&result.journalEvents.size()==3,"module exception boundary retains earlier events");
    ordered(result.journalEvents);
    require(std::get<Diagnostic>(result.journalEvents.back().payload).identifier=="MParser:HostExecutionException" &&
        !request.executionControl->eventJournal(),"unmapped host exception is recorded and scope detached");
}
} // namespace
int main() {
    try {
        for(const auto engine:{Engine::Hir,Engine::Bytecode,Engine::Portable,Engine::Native,Engine::Automatic}) {
            std::cout<<"requested engine "<<static_cast<int>(engine)<<'\n'; checkEngine(engine);
        }
        crossModule(); rejectionAndCancellation(); scopeAndForwarding(); hostExceptions();
        std::cout<<"shared event journal: actual order, modules, dynamic capture, caught errors and failure history passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
