#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::ApiError;
using mparser::sdk::Invocation;
using mparser::sdk::InvocationStatus;
using mparser::sdk::Module;
using mparser::sdk::Result;
using mparser::sdk::Session;
using mparser::sdk::SystemCapability;
using mparser::sdk::SystemContext;
using mparser::sdk::SystemContextOptions;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

class TemporaryTree {
public:
    TemporaryTree() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        base = std::filesystem::temp_directory_path() /
               ("mparser-system-context-" + std::to_string(stamp));
        root = base / "root";
        outside = base / "outside";
        std::filesystem::create_directories(root / "tmp");
        std::filesystem::create_directories(root / "sub");
        std::filesystem::create_directories(root / "lib");
        std::filesystem::create_directories(outside);
    }

    ~TemporaryTree() {
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }

    std::filesystem::path base;
    std::filesystem::path root;
    std::filesystem::path outside;
};

double scalar(const Value& value) {
    assert(value.kind() == ValueKind::Numeric);
    const auto data = value.numericData<double>();
    assert(data.size() == 1);
    return data.front();
}

std::string asciiText(const Value& value) {
    assert(value.kind() == ValueKind::Character);
    std::string result;
    for (const auto unit : value.characterData()) {
        assert(unit <= 0x7fU);
        result.push_back(static_cast<char>(unit));
    }
    return result;
}

Invocation request(std::string entry, std::size_t outputs) {
    Invocation invocation;
    invocation.entryFunction = std::move(entry);
    invocation.requestedOutputCount = outputs;
    return invocation;
}

bool hasDisabledDiagnostic(const Result& result) {
    const auto diagnostics = result.diagnostics();
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find("disabled by the runtime system") !=
                   std::string::npos;
        });
}

void printDiagnostics(const Result& result) {
    for (const auto& diagnostic : result.diagnostics()) {
        std::cerr << diagnostic.identifier << ": "
                  << diagnostic.message << '\n';
    }
}

SystemContextOptions fullOptions(const TemporaryTree& tree) {
    SystemContextOptions options;
    options.capabilities =
        SystemCapability::CurrentDirectory |
        SystemCapability::SearchPaths |
        SystemCapability::FileSystemRead |
        SystemCapability::FileSystemWrite |
        SystemCapability::Random |
        SystemCapability::DynamicEvaluation;
    options.rootDirectory = tree.root.string();
    options.currentDirectory = ".";
    options.temporaryDirectory = "tmp";
    options.searchPaths = {"lib"};
    options.randomSeed = 123456U;
    options.maximumOpenFiles = 8;
    options.maximumFileReadBytes = 1024;
    return options;
}

const char* kSource = R"MATLAB(
function value = next_random()
    value = rand();
end

function [fid, message, filename] = probe_open()
    filename = fullfile(tempdir(), 'probe.bin');
    [fid, message] = fopen(filename, 'wb', 'ieee-be');
    if fid >= 3
        fclose(fid);
    end
end

function [count, total] = file_roundtrip()
    filename = fullfile(tempdir(), 'context.bin');
    fid = fopen(filename, 'wb', 'ieee-be');
    assert(fid >= 3);
    written = fwrite(fid, uint16([1 513]), 'uint16');
    assert(fclose(fid) == 0);
    fid = fopen(filename, 'rb', 'ieee-be');
    [values, count] = fread(fid, [1 2], 'uint16=>uint16');
    assert(fclose(fid) == 0);
    total = double(sum(values)) + double(written);
end

function [fid, message] = escape_root()
    [fid, message] = fopen('../outside/escaped.bin', 'wb');
end

function outside_path()
    addpath('..');
end

function here = move_to_sub()
    cd('sub');
    here = pwd();
end

function here = current_path()
    here = pwd();
end

function here = temporary_path()
    here = tempdir();
end

function value = eval_random()
    value = eval('rand()');
end

function [first, second] = open_limit()
    first = fopen(fullfile(tempdir(), 'one.bin'), 'wb');
    [second, ignored] = fopen(fullfile(tempdir(), 'two.bin'), 'wb');
    assert(length(ignored) > 0);
    assert(fclose(first) == 0);
end

function [fid, message] = escape_link()
    [fid, message] = fopen('link/escaped.bin', 'wb');
end
)MATLAB";

} // namespace

int main() {
    TemporaryTree tree;
    const Module module = Module::compile(kSource, "system_context_api.m");
    assert(module.isValid());

    bool missingRootRejected = false;
    try {
        SystemContextOptions missing;
        missing.rootDirectory = (tree.base / "missing").string();
        (void)SystemContext::rootedNative(missing);
    } catch (const ApiError& error) {
        missingRootRejected =
            error.status() == MPARSER_API_STATUS_SYSTEM_CONTEXT_FAILED;
    }
    assert(missingRootRejected);

    bool escapedCurrentRejected = false;
    try {
        auto escaped = fullOptions(tree);
        escaped.currentDirectory = "..";
        (void)SystemContext::rootedNative(escaped);
    } catch (const ApiError& error) {
        escapedCurrentRejected =
            error.status() == MPARSER_API_STATUS_SYSTEM_CONTEXT_FAILED;
    }
    assert(escapedCurrentRejected);

#ifdef _WIN32
    auto caseOptions = fullOptions(tree);
    if (caseOptions.rootDirectory.size() >= 2 &&
        caseOptions.rootDirectory[1] == ':') {
        char& drive = caseOptions.rootDirectory[0];
        drive = drive >= 'A' && drive <= 'Z'
                    ? static_cast<char>(drive - 'A' + 'a')
                    : static_cast<char>(drive - 'a' + 'A');
        caseOptions.temporaryDirectory =
            (tree.root / "tmp").string();
        auto caseContext = SystemContext::rootedNative(caseOptions);
        auto caseResult = module.execute(
            request("temporary_path", 1), caseContext);
        assert(caseResult.status() == InvocationStatus::Succeeded);
    }
#endif

    auto context = SystemContext::rootedNative(fullOptions(tree));
    assert(mparser::sdk::hasSystemCapability(
        context.capabilities(), SystemCapability::Random));
    assert(mparser::sdk::hasSystemCapability(
        context.capabilities(), SystemCapability::FileSystemWrite));

    auto firstRandom = module.execute(
        request("next_random", 1), context);
    auto secondRandom = module.execute(
        request("next_random", 1), context);
    assert(firstRandom.status() == InvocationStatus::Succeeded);
    assert(secondRandom.status() == InvocationStatus::Succeeded);
    const double first = scalar(firstRandom.output(0));
    const double second = scalar(secondRandom.output(0));
    assert(first >= 0.0 && first < 1.0);
    assert(second >= 0.0 && second < 1.0);
    assert(first != second);

    auto opened = module.execute(request("probe_open", 3), context);
    assert(opened.status() == InvocationStatus::Succeeded);
    if (scalar(opened.output(0)) < 3.0) {
        std::cerr << "probe_open path: " << asciiText(opened.output(2))
                  << "\nprobe_open: " << asciiText(opened.output(1))
                  << '\n';
    }
    assert(scalar(opened.output(0)) >= 3.0);

    auto roundtrip = module.execute(
        request("file_roundtrip", 2), context);
    if (roundtrip.status() != InvocationStatus::Succeeded) {
        printDiagnostics(roundtrip);
    }
    assert(roundtrip.status() == InvocationStatus::Succeeded);
    assert(scalar(roundtrip.output(0)) == 2.0);
    assert(scalar(roundtrip.output(1)) == 516.0);
    assert(std::filesystem::file_size(tree.root / "tmp" / "context.bin") ==
           4);

    auto escaped = module.execute(request("escape_root", 2), context);
    assert(escaped.status() == InvocationStatus::Succeeded);
    assert(scalar(escaped.output(0)) == -1.0);
    assert(asciiText(escaped.output(1)).find("outside the rooted") !=
           std::string::npos);
    assert(!std::filesystem::exists(tree.outside / "escaped.bin"));

    auto pathEscape = module.execute(request("outside_path", 0), context);
    assert(pathEscape.status() == InvocationStatus::RuntimeFailed);

    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        tree.outside, tree.root / "link", linkError);
    if (!linkError) {
        auto linkEscape = module.execute(
            request("escape_link", 2), context);
        assert(linkEscape.status() == InvocationStatus::Succeeded);
        assert(scalar(linkEscape.output(0)) == -1.0);
        assert(!std::filesystem::exists(tree.outside / "escaped.bin"));
    }

    Session retainedSession;
    {
        auto retainedContext = context;
        retainedSession = module.createSession(retainedContext);
    }
    auto temporary = retainedSession.execute(request("temporary_path", 1));
    assert(temporary.status() == InvocationStatus::Succeeded);
    assert(std::filesystem::weakly_canonical(asciiText(temporary.output(0))) ==
           std::filesystem::weakly_canonical(tree.root / "tmp"));
    auto moved = retainedSession.execute(request("move_to_sub", 1));
    assert(moved.status() == InvocationStatus::Succeeded);
    assert(std::filesystem::weakly_canonical(asciiText(moved.output(0))) ==
           std::filesystem::weakly_canonical(tree.root / "sub"));
    auto current = retainedSession.execute(request("current_path", 1));
    assert(current.status() == InvocationStatus::Succeeded);
    assert(asciiText(current.output(0)) == asciiText(moved.output(0)));
    auto evaluated = retainedSession.execute(request("eval_random", 1));
    assert(evaluated.status() == InvocationStatus::Succeeded);
    assert(scalar(evaluated.output(0)) >= 0.0);

    SystemContextOptions deniedOptions;
    deniedOptions.capabilities = SystemCapability::Random;
    deniedOptions.rootDirectory = tree.root.string();
    auto denied = SystemContext::rootedNative(deniedOptions);
    auto deniedResult = module.execute(request("current_path", 1), denied);
    assert(deniedResult.status() == InvocationStatus::RuntimeFailed);
    assert(hasDisabledDiagnostic(deniedResult));

    auto limitedOptions = fullOptions(tree);
    limitedOptions.maximumOpenFiles = 1;
    auto limited = SystemContext::rootedNative(limitedOptions);
    auto limitedResult = module.execute(request("open_limit", 2), limited);
    assert(limitedResult.status() == InvocationStatus::Succeeded);
    assert(scalar(limitedResult.output(0)) >= 3.0);
    assert(scalar(limitedResult.output(1)) == -1.0);

    std::cout << "system context api smoke = 516,rooted,retained\n";
    return 0;
}
