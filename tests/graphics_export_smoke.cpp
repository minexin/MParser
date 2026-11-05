#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/object_model/runtime_graphics_export.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/session/runtime_session_state.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

using namespace mparser;

namespace {

void require(bool value, const std::source_location location = std::source_location::current()) {
    if (!value) { throw std::runtime_error("graphics export invariant failed at line " + std::to_string(location.line())); }
}

class RecordingAdapter final : public RuntimeGraphicsExportAdapter {
public:
    mutable std::vector<RuntimeGraphicsExportRequest> requests;

    RuntimeGraphicsExportStatus write(const RuntimeGraphicsExportRequest& request) const override {
        requests.push_back(request);
        if (request.destination == "refused.png") { return RuntimeGraphicsExportStatus::DestinationRejected; }
        if (request.destination == "failed.pdf") { return RuntimeGraphicsExportStatus::Failed; }
        if (request.destination == "unsupported.pdf") { return RuntimeGraphicsExportStatus::UnsupportedFormat; }
        if (request.destination == "throw.pdf") { throw std::runtime_error("not public diagnostic text"); }
        // Transport-only positive control: no file or actual encoder is used.
        return RuntimeGraphicsExportStatus::Written;
    }
};

void run(std::optional<ModuleExecutionBackend> backend) {
    const auto adapter = std::make_shared<RecordingAdapter>();
    const auto execute = [&](const std::string& source, std::string_view expected = {}, bool attach = true) {
        auto module = CompiledModule::compile(source);
        require(module.valid());
        auto control = std::make_shared<RuntimeExecutionControl>(RuntimeExecutionLimits{}, std::nullopt,
            nullptr, RuntimeInputSource{}, RuntimeConsoleSink{}, attach ? adapter : nullptr);
        if (!backend) {
            InterpreterOptions options;
            options.executionControl = control;
            const auto result = Interpreter{}.run(module.semantic(), options);
            if (expected.empty()) { require(result.diagnostics.empty()); }
            else { require(!result.diagnostics.empty() && result.diagnostics.front().identifier == expected); }
            require(result.graphicsSnapshot.has_value());
            return result.graphicsSnapshot->json(true, true);
        }
        ModuleInvocationRequest request;
        request.backend = *backend;
        request.executionControl = control;
        const auto result = module.execute(request);
        require(result.succeeded() == expected.empty());
        if (!expected.empty()) { require(!result.diagnostics.empty() && result.diagnostics.front().identifier == expected); }
        require(result.outputs.empty() && result.graphicsSnapshot.has_value());
        return result.graphicsSnapshot->json(true, true);
    };
    const auto empty = execute("print('unused','-dpng');", "MParser:InvalidExportFigure");
    require(nlohmann::json::parse(empty).at("objects").empty() && adapter->requests.empty());

    const std::string setup = "f=figure(); f.Name='First'; a=gca(); l=plot(1:3); "
        "g=figure(); g.Name='Second'; plot([4,5]); ";
    const auto after = execute(setup + "saveas(l,'folder.with.dot/line','png'); saveas(a,'axes.pdf'); "
        "print('current','-dpdf'); print(f,'explicit.png','-dpng'); f.Name='Later'; close all;");
    require(nlohmann::json::parse(after).at("objects").empty() && adapter->requests.size() == 4);
    const auto verify = [&](size_t index, const char* path, RuntimeGraphicsExportFormat format, const char* name) {
        const auto& request = adapter->requests.at(index);
        require(request.destination == path && request.format == format);
        const auto scene = nlohmann::json::parse(request.sceneJson);
        require(scene.at("version") == 4);
        const auto& objects = scene.at("objects");
        const auto selected = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
            return object.at("objectIdentity") == std::to_string(request.figureIdentity);
        });
        require(selected != objects.end() && selected->at("type") == "Figure" && selected->at("properties").at("Name") == name);
        require(selected->at("layout").is_null()); // No inferred layout metrics.
    };
    verify(0, "folder.with.dot/line.png", RuntimeGraphicsExportFormat::Png, "First");
    verify(1, "axes.pdf", RuntimeGraphicsExportFormat::Pdf, "First");
    verify(2, "current.pdf", RuntimeGraphicsExportFormat::Pdf, "Second");
    verify(3, "explicit.png", RuntimeGraphicsExportFormat::Png, "First");
    require(adapter->requests[0].figureIdentity != adapter->requests[2].figureIdentity);
    const auto sent = adapter->requests.size();
    for (const auto& [source, identifier] : std::initializer_list<std::pair<const char*, const char*>>{
        {"print(l,'bad.png','-dpng');", "MParser:InvalidExportFigure"},
        {"saveas([f,g],'bad.png');", "MParser:InvalidExportFigure"},
        {"delete(a); saveas(l,'bad.png');", "MParser:InvalidExportFigure"},
        {"print(f,'bad.pdf','-dpng');", "MParser:GraphicsExportExtensionMismatch"},
        {"saveas(f,'bad.xyz','png');", "MParser:GraphicsExportExtensionMismatch"},
        {"saveas(f,'bad.PNG');", "MParser:UnsupportedExportFormat"},
        {"print('bad.png','-dPNG');", "MParser:UnsupportedExportFormat"},
        {"saveas(f,'bad');", "MParser:UnsupportedExportFormat"},
        {"saveas(f,'folder/','png');", "MParser:InvalidGraphicsExport"},
        {"saveas(f,'folder/..','png');", "MParser:InvalidGraphicsExport"},
        {"saveas(f,char([65,0,66]),'png');", "MParser:InvalidGraphicsExport"},
        {"print('-clipboard','-dpng');", "MParser:InvalidGraphicsExport"},
    }) {
        execute(setup + source, identifier);
        require(adapter->requests.size() == sent);
    }
    execute(setup + "saveas(l,'headless','pdf');", "MParser:GraphicsRendererUnavailable", false);
    require(adapter->requests.size() == sent);
    execute(setup + "saveas(f,'refused.png');", "MParser:GraphicsExportDestinationRejected");
    execute(setup + "saveas(f,'failed.pdf');", "MParser:GraphicsExportFailed");
    execute(setup + "saveas(f,'unsupported.pdf');", "MParser:UnsupportedExportFormat");
    execute(setup + "saveas(f,'throw.pdf');", "MParser:GraphicsExportFailed");
    require(adapter->requests.size() == sent + 4);
}

} // namespace

int main() {
    try {
        run(std::nullopt);
        for (auto backend : {ModuleExecutionBackend::Automatic, ModuleExecutionBackend::Bytecode,
                             ModuleExecutionBackend::Portable, ModuleExecutionBackend::Native}) {
            run(backend);
        }
        std::cout << "PASS HIR/VM graphics export targets, frozen requests, typed failures and headless refusal (no encoding)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
