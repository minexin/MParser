#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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

bool logicalScalar(const Value& value) {
    assert(value.kind() == ValueKind::Numeric);
    assert(value.numericClass() == mparser::sdk::NumericClass::Logical);
    const auto data = value.numericData<std::uint8_t>();
    assert(data.size() == 1);
    return data.front() != 0;
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

function score = filesystem_management()
    [made, message, message_id] = mkdir('managed/deep/');
    assert(made && isempty(message) && isempty(message_id));
    filename = fullfile('managed', 'deep', 'value.txt');
    fid = fopen(filename, 'w');
    assert(fid >= 3);
    assert(fprintf(fid, '%s', 'rooted-data') == 11);
    assert(fclose(fid) == 0);
    assert(isfile(filename) && isfolder('managed/deep'));
    assert(strcmp(fileread(filename), 'rooted-data'));
    [copied, ignored, ignored_id] = copyfile(filename, 'managed/copy.txt');
    assert(copied && isempty(ignored) && isempty(ignored_id));
    [moved, ignored, ignored_id] = movefile('managed/copy.txt', 'sub');
    assert(moved && isfile('sub/copy.txt'));
    [removed_nonempty, ignored, ignored_id] = rmdir('managed');
    assert(~removed_nonempty);
    [removed, ignored, ignored_id] = rmdir('managed', 's');
    assert(removed && ~isfolder('managed'));
    candidate = tempname('tmp');
    assert(strcmp(fileparts(candidate), 'tmp') && ~isfile(candidate));
    score = made + copied + moved + removed + length(fileread('sub/copy.txt'));
end

function score = file_metadata_management()
    [made, message, message_id] = mkdir('metadata');
    assert(made && isempty(message_id));
    first = fullfile('metadata', 'alpha.txt');
    second = fullfile('metadata', 'beta.txt');
    doomed = fullfile('metadata', 'delete-me.tmp');
    fid = fopen(first, 'w'); fprintf(fid, '%s', 'alpha'); fclose(fid);
    fid = fopen(second, 'w'); fprintf(fid, '%s', 'beta'); fclose(fid);
    fid = fopen(doomed, 'w'); fprintf(fid, '%s', 'doomed'); fclose(fid);

    [status, attributes] = fileattrib('metadata/*.txt');
    assert(status && numel(attributes) == 2);
    assert(isfield(attributes, 'UserWrite'));
    [status, message, message_id] = fileattrib(first, '-w');
    assert(status && isempty(message) && isempty(message_id));
    [status, attributes] = fileattrib(first);
    assert(status && attributes.UserWrite == 0);
    [status, message, message_id] = fileattrib(first, '+w');
    assert(status && isempty(message) && isempty(message_id));

    entries = dir('metadata/*.txt');
    assert(numel(entries) == 2 && entries(1).datenum > 0);
    delete('metadata/delete-*.tmp');
    assert(~isfile(doomed));
    [removed, ignored, ignored_id] = rmdir('metadata', 's');
    assert(removed && isempty(ignored) && isempty(ignored_id));
    score = numel(attributes) + numel(entries) + removed;
end

function [status, message] = escape_mkdir()
    [status, message] = mkdir('../outside/new-directory');
end

function [status, message] = escape_copy()
    [status, message] = copyfile('tmp/context.bin', '../outside/copied.bin');
end

function [status, message] = escape_move()
    [status, message] = movefile('tmp/context.bin', '../outside/moved.bin');
end

function [status, message] = escape_fileattrib()
    [status, message] = fileattrib('../outside', '-w');
end

function escape_delete()
    delete('../outside/protected.txt');
end

function [status, message] = remove_root()
    [status, message] = rmdir('.', 's');
end

function [status, message] = copy_into_self()
    [status, message] = copyfile('sub', 'sub/nested-copy');
end

function [status, message] = move_into_self()
    [status, message] = movefile('sub', 'sub/nested-move');
end

function status = force_copy()
    [status, message, message_id] = copyfile( ...
        'force-copy.txt', 'restricted-copy', 'f');
    assert(status && isempty(message) && isempty(message_id));
    assert(strcmp(fileread('restricted-copy/force-copy.txt'), ...
                  'copy-force'));
end

function status = force_move()
    [status, message, message_id] = movefile( ...
        'force-move.txt', 'restricted-move', 'f');
    assert(status && isempty(message) && isempty(message_id));
    assert(strcmp(fileread('restricted-move/force-move.txt'), ...
                  'move-force'));
end

function [status, message] = link_mkdir()
    [status, message] = mkdir('internal-link/new-directory');
end

function [fid, message] = link_open()
    [fid, message] = fopen('internal-link/new.bin', 'wb');
end

function [status, message] = link_copy()
    [status, message] = copyfile( ...
        'tmp/context.bin', 'internal-link/copied.bin');
end

function [status, message] = link_move()
    [status, message] = movefile('internal-link', 'moved-link');
end

function [status, message] = link_remove()
    [status, message] = rmdir('internal-link', 's');
end

function [status, message] = link_fileattrib()
    [status, message] = fileattrib('internal-link', '-w');
end

function link_delete()
    delete('internal-link/link-protected.txt');
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

    auto managed = module.execute(
        request("filesystem_management", 1), context);
    if (managed.status() != InvocationStatus::Succeeded) {
        printDiagnostics(managed);
    }
    assert(managed.status() == InvocationStatus::Succeeded);
    assert(scalar(managed.output(0)) == 15.0);
    assert(std::filesystem::is_regular_file(tree.root / "sub" / "copy.txt"));
    assert(!std::filesystem::exists(tree.root / "managed"));

    auto metadata = module.execute(
        request("file_metadata_management", 1), context);
    if (metadata.status() != InvocationStatus::Succeeded) {
        printDiagnostics(metadata);
    }
    assert(metadata.status() == InvocationStatus::Succeeded);
    assert(scalar(metadata.output(0)) == 4.0);
    assert(!std::filesystem::exists(tree.root / "metadata"));

    const auto restrictedCopy = tree.root / "restricted-copy";
    const auto restrictedMove = tree.root / "restricted-move";
    std::filesystem::create_directories(restrictedCopy);
    std::filesystem::create_directories(restrictedMove);
    {
        std::ofstream(tree.root / "force-copy.txt") << "copy-force";
        std::ofstream(tree.root / "force-move.txt") << "move-force";
        std::ofstream(restrictedCopy / "force-copy.txt") << "old-copy";
        std::ofstream(restrictedMove / "force-move.txt") << "old-move";
    }
    std::error_code permissionError;
    std::filesystem::permissions(
        restrictedCopy, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::remove, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedMove, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::remove, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedCopy / "force-copy.txt",
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::remove, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedMove / "force-move.txt",
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::remove, permissionError);
    assert(!permissionError);
    const auto copyPermissions =
        std::filesystem::status(restrictedCopy).permissions();
    const auto movePermissions =
        std::filesystem::status(restrictedMove).permissions();
    const auto copyTargetPermissions = std::filesystem::status(
        restrictedCopy / "force-copy.txt").permissions();
    const auto moveTargetPermissions = std::filesystem::status(
        restrictedMove / "force-move.txt").permissions();

    auto forceCopy = module.execute(request("force_copy", 1), context);
    if (forceCopy.status() != InvocationStatus::Succeeded) {
        printDiagnostics(forceCopy);
    }
    assert(forceCopy.status() == InvocationStatus::Succeeded);
    assert(logicalScalar(forceCopy.output(0)));
    assert(std::filesystem::status(restrictedCopy).permissions() ==
           copyPermissions);
    assert(std::filesystem::status(
               restrictedCopy / "force-copy.txt").permissions() ==
           copyTargetPermissions);

    auto forceMove = module.execute(request("force_move", 1), context);
    if (forceMove.status() != InvocationStatus::Succeeded) {
        printDiagnostics(forceMove);
    }
    assert(forceMove.status() == InvocationStatus::Succeeded);
    assert(logicalScalar(forceMove.output(0)));
    assert(std::filesystem::status(restrictedMove).permissions() ==
           movePermissions);
    assert(std::filesystem::status(
               restrictedMove / "force-move.txt").permissions() ==
           moveTargetPermissions);
    assert(!std::filesystem::exists(tree.root / "force-move.txt"));
    std::filesystem::permissions(
        restrictedCopy, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedMove, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedCopy / "force-copy.txt",
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, permissionError);
    assert(!permissionError);
    std::filesystem::permissions(
        restrictedMove / "force-move.txt",
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, permissionError);
    assert(!permissionError);

    for (const auto* entry : {"escape_mkdir", "escape_copy",
                              "escape_move", "escape_fileattrib",
                              "remove_root",
                              "copy_into_self", "move_into_self"}) {
        auto operation = module.execute(request(entry, 2), context);
        assert(operation.status() == InvocationStatus::Succeeded);
        assert(!logicalScalar(operation.output(0)));
        assert(!asciiText(operation.output(1)).empty());
    }
    assert(!std::filesystem::exists(tree.outside / "new-directory"));
    assert(!std::filesystem::exists(tree.outside / "copied.bin"));
    assert(!std::filesystem::exists(tree.outside / "moved.bin"));
    assert(!std::filesystem::exists(tree.root / "sub" / "nested-copy"));
    assert(!std::filesystem::exists(tree.root / "sub" / "nested-move"));
    assert(std::filesystem::is_directory(tree.root));
    assert(std::filesystem::is_regular_file(tree.root / "tmp" /
                                            "context.bin"));
    {
        std::ofstream protectedFile(tree.outside / "protected.txt");
        protectedFile << "protected";
    }
    auto deleteEscape = module.execute(request("escape_delete", 0), context);
    assert(deleteEscape.status() == InvocationStatus::RuntimeFailed);
    assert(std::filesystem::is_regular_file(tree.outside /
                                            "protected.txt"));

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

    linkError.clear();
    {
        std::ofstream protectedFile(tree.root / "sub" /
                                    "link-protected.txt");
        protectedFile << "protected";
    }
    std::filesystem::create_directory_symlink(
        tree.root / "sub", tree.root / "internal-link", linkError);
    if (!linkError) {
        for (const auto* entry : {"link_mkdir", "link_copy", "link_move",
                                  "link_remove", "link_fileattrib"}) {
            auto operation = module.execute(request(entry, 2), context);
            assert(operation.status() == InvocationStatus::Succeeded);
            assert(!logicalScalar(operation.output(0)));
            assert(asciiText(operation.output(1)).find("symbolic links") !=
                   std::string::npos);
        }
        auto linkOpen = module.execute(request("link_open", 2), context);
        assert(linkOpen.status() == InvocationStatus::Succeeded);
        assert(scalar(linkOpen.output(0)) == -1.0);
        assert(asciiText(linkOpen.output(1)).find("symbolic links") !=
               std::string::npos);
        auto linkDelete = module.execute(request("link_delete", 0), context);
        assert(linkDelete.status() == InvocationStatus::RuntimeFailed);
        assert(std::filesystem::is_regular_file(
            tree.root / "sub" / "link-protected.txt"));
        assert(std::filesystem::is_directory(tree.root / "sub"));
        assert(std::filesystem::is_symlink(tree.root / "internal-link"));
        assert(!std::filesystem::exists(tree.root / "sub" / "new-directory"));
        assert(!std::filesystem::exists(tree.root / "sub" / "new.bin"));
        assert(!std::filesystem::exists(tree.root / "sub" / "copied.bin"));
        assert(!std::filesystem::exists(tree.root / "moved-link"));
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
