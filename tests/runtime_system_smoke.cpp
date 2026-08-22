#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/runtime_execution_control.h"
#include "mparser/runtime/io/runtime_file_io.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_struct.h"
#include "mparser/runtime/io/runtime_system.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/runtime/core/runtime_warning.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Capability = mparser::RuntimeSystemCapability;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path virtualRoot() {
#ifdef _WIN32
    return std::filesystem::path("C:/virtual/root");
#else
    return std::filesystem::path("/virtual/root");
#endif
}

class FakeHostFile final : public mparser::RuntimeHostFile {
public:
    FakeHostFile(std::shared_ptr<std::string> storage, bool readable,
                 bool writable, bool append,
                 std::shared_ptr<size_t> closeCount)
        : storage_(std::move(storage)), readable_(readable),
          writable_(writable), append_(append),
          closeCount_(std::move(closeCount)),
          cursor_(append_ ? storage_->size() : 0) {}

    mparser::RuntimeSystemResult<size_t>
    write(std::string_view text) override {
        if (closed_ || !writable_) {
            return mparser::RuntimeSystemResult<size_t>::failure(
                closed_ ? "fake file is closed"
                        : "fake file is not writable");
        }
        if (append_) {
            cursor_ = storage_->size();
        }
        if (cursor_ > storage_->size()) {
            storage_->resize(cursor_, '\0');
        }
        const size_t replaced =
            std::min(text.size(), storage_->size() - cursor_);
        storage_->replace(cursor_, replaced, text);
        cursor_ += text.size();
        return mparser::RuntimeSystemResult<size_t>::success(text.size());
    }

    mparser::RuntimeSystemResult<mparser::RuntimeFileReadBuffer>
    readRemaining(size_t maximumBytes) override {
        if (closed_ || !readable_) {
            return mparser::RuntimeSystemResult<
                mparser::RuntimeFileReadBuffer>::failure(
                closed_ ? "fake file is closed"
                        : "fake file is not readable");
        }
        const size_t count = storage_->size() - cursor_;
        if (count > maximumBytes) {
            return mparser::RuntimeSystemResult<
                mparser::RuntimeFileReadBuffer>::failure(
                "fake file read exceeds the runtime limit");
        }
        std::string result = storage_->substr(cursor_);
        cursor_ = storage_->size();
        return mparser::RuntimeSystemResult<
            mparser::RuntimeFileReadBuffer>::success(
            {std::move(result), {}});
    }

    mparser::RuntimeSystemResult<std::int64_t> position() override {
        if (closed_) {
            return mparser::RuntimeSystemResult<std::int64_t>::failure(
                "fake file is closed");
        }
        if (cursor_ > static_cast<size_t>(
                          std::numeric_limits<std::int64_t>::max())) {
            return mparser::RuntimeSystemResult<std::int64_t>::failure(
                "fake file position is too large");
        }
        return mparser::RuntimeSystemResult<std::int64_t>::success(
            static_cast<std::int64_t>(cursor_));
    }

    mparser::RuntimeSystemStatus seek(
        std::int64_t offset,
        mparser::RuntimeFileSeekOrigin origin) override {
        if (closed_) {
            return mparser::RuntimeSystemStatus::failure(
                "fake file is closed");
        }
        std::int64_t base = 0;
        if (origin == mparser::RuntimeFileSeekOrigin::Current) {
            const auto current = position();
            if (!current.succeeded) {
                return mparser::RuntimeSystemStatus::failure(current.error);
            }
            base = current.value;
        } else if (origin == mparser::RuntimeFileSeekOrigin::End) {
            if (storage_->size() > static_cast<size_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
                return mparser::RuntimeSystemStatus::failure(
                    "fake file is too large");
            }
            base = static_cast<std::int64_t>(storage_->size());
        }
        if ((offset > 0 &&
             base > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 &&
             base < std::numeric_limits<std::int64_t>::min() - offset)) {
            return mparser::RuntimeSystemStatus::failure(
                "fake file seek target is too large");
        }
        const auto target = base + offset;
        if (target < 0 ||
            static_cast<std::uintmax_t>(target) >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<size_t>::max())) {
            return mparser::RuntimeSystemStatus::failure(
                "fake file seek target is invalid");
        }
        cursor_ = static_cast<size_t>(target);
        return mparser::RuntimeSystemStatus::success();
    }

    mparser::RuntimeSystemStatus close() override {
        if (closed_) {
            return mparser::RuntimeSystemStatus::failure(
                "fake file is already closed");
        }
        closed_ = true;
        ++*closeCount_;
        return mparser::RuntimeSystemStatus::success();
    }

private:
    std::shared_ptr<std::string> storage_;
    bool readable_ = false;
    bool writable_ = false;
    bool append_ = false;
    std::shared_ptr<size_t> closeCount_;
    bool closed_ = false;
    size_t cursor_ = 0;
};

class FakeHostAdapter final : public mparser::RuntimeHostAdapter {
public:
    FakeHostAdapter()
        : root(virtualRoot()), child(root / "child"), tools(root / "tools"),
          library(root / "library"), temporary(root / "temporary") {
        directories = {root, child, tools, library, temporary};
    }

    mparser::RuntimeSystemResult<std::filesystem::path>
    processCurrentDirectory() const override {
        ++currentDirectoryQueries;
        return mparser::RuntimeSystemResult<std::filesystem::path>::success(
            root);
    }

    mparser::RuntimeSystemResult<std::filesystem::path>
    temporaryDirectory() const override {
        return mparser::RuntimeSystemResult<std::filesystem::path>::success(
            temporary);
    }

    mparser::RuntimeSystemResult<std::filesystem::path>
    normalizeDirectory(const std::filesystem::path& base,
                       const std::filesystem::path& candidate) const override {
        const auto combined = normalize(
            candidate.is_absolute() ? candidate : base / candidate);
        if (!directories.contains(combined)) {
            return mparser::RuntimeSystemResult<
                std::filesystem::path>::failure(
                "fake directory does not exist");
        }
        return mparser::RuntimeSystemResult<std::filesystem::path>::success(
            combined);
    }

    mparser::RuntimeSystemResult<std::vector<mparser::RuntimeDirectoryEntry>>
    listDirectory(const std::filesystem::path& path) const override {
        lastListedDirectory = path;
        if (normalize(path) != child) {
            return mparser::RuntimeSystemResult<
                std::vector<mparser::RuntimeDirectoryEntry>>::failure(
                "fake directory is unavailable");
        }
        return mparser::RuntimeSystemResult<
            std::vector<mparser::RuntimeDirectoryEntry>>::success({
            {"nested", path.string(), "01-Jan-2026 01:02:03", 0, true},
            {"data.bin", path.string(), "01-Jan-2026 01:02:03", 17, false},
        });
    }

    mparser::RuntimeSystemResult<bool>
    regularFileExists(const std::filesystem::path& path) const override {
        const auto normalized = normalize(path);
        return mparser::RuntimeSystemResult<bool>::success(
            normalized == child / "data.bin" ||
            normalized == root / "script.m" ||
            normalized == root / "model.slx" ||
            normalized == root / "notes.txt" ||
            normalized == root / "application.mlapp" ||
            normalized == root / "live.mlx" ||
            normalized == tools / "packed.p" ||
            normalized == library / "native.mexw64" ||
            files.contains(normalized));
    }

    mparser::RuntimeSystemResult<bool>
    directoryExists(const std::filesystem::path& path) const override {
        return mparser::RuntimeSystemResult<bool>::success(
            directories.contains(normalize(path)));
    }

    mparser::RuntimeSystemResult<bool>
    createDirectories(const std::filesystem::path& path) const override {
        const auto normalized = normalize(path);
        if (files.contains(normalized)) {
            return mparser::RuntimeSystemResult<bool>::failure(
                "fake path is a file");
        }
        const bool created = directories.insert(normalized).second;
        auto parent = normalized.parent_path();
        while (!parent.empty() && isWithin(root, parent)) {
            directories.insert(parent);
            if (parent == root) {
                break;
            }
            parent = parent.parent_path();
        }
        return mparser::RuntimeSystemResult<bool>::success(created);
    }

    mparser::RuntimeSystemStatus removeDirectory(
        const std::filesystem::path& path,
        bool recursive) const override {
        const auto normalized = normalize(path);
        if (!directories.contains(normalized)) {
            return mparser::RuntimeSystemStatus::failure(
                "fake path is not a directory");
        }
        const auto hasChildDirectory = std::any_of(
            directories.begin(), directories.end(),
            [&normalized](const auto& candidate) {
                return candidate != normalized &&
                       isWithin(normalized, candidate);
            });
        const auto hasChildFile = std::any_of(
            files.begin(), files.end(),
            [&normalized](const auto& entry) {
                return isWithin(normalized, entry.first);
            });
        if (!recursive && (hasChildDirectory || hasChildFile)) {
            return mparser::RuntimeSystemStatus::failure(
                "fake directory is not empty");
        }
        std::erase_if(directories, [&normalized](const auto& candidate) {
            return candidate == normalized ||
                   isWithin(normalized, candidate);
        });
        std::erase_if(files, [&normalized](const auto& entry) {
            return isWithin(normalized, entry.first);
        });
        return mparser::RuntimeSystemStatus::success();
    }

    mparser::RuntimeSystemStatus copyPath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool force) const override {
        (void)force;
        const auto normalizedSource = normalize(source);
        auto normalizedDestination = normalize(destination);
        if (const auto found = files.find(normalizedSource);
            found != files.end()) {
            if (directories.contains(normalizedDestination)) {
                normalizedDestination /= normalizedSource.filename();
            }
            files[normalizedDestination] =
                std::make_shared<std::string>(*found->second);
            return mparser::RuntimeSystemStatus::success();
        }
        if (!directories.contains(normalizedSource)) {
            return mparser::RuntimeSystemStatus::failure(
                "fake copy source does not exist");
        }
        directories.insert(normalizedDestination);
        const auto originalDirectories = directories;
        const auto originalFiles = files;
        for (const auto& candidate : originalDirectories) {
            if (candidate != normalizedSource &&
                isWithin(normalizedSource, candidate)) {
                directories.insert(normalizedDestination /
                    candidate.lexically_relative(normalizedSource));
            }
        }
        for (const auto& [path, contents] : originalFiles) {
            if (isWithin(normalizedSource, path)) {
                files[normalizedDestination /
                      path.lexically_relative(normalizedSource)] =
                    std::make_shared<std::string>(*contents);
            }
        }
        return mparser::RuntimeSystemStatus::success();
    }

    mparser::RuntimeSystemStatus movePath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        bool force) const override {
        const auto normalizedSource = normalize(source);
        auto normalizedDestination = normalize(destination);
        if (directories.contains(normalizedDestination)) {
            normalizedDestination /= normalizedSource.filename();
        }
        auto copied = copyPath(normalizedSource, normalizedDestination, force);
        if (!copied.succeeded) {
            return copied;
        }
        if (files.erase(normalizedSource) != 0) {
            return mparser::RuntimeSystemStatus::success();
        }
        return removeDirectory(normalizedSource, true);
    }

    mparser::RuntimeSystemResult<std::shared_ptr<mparser::RuntimeHostFile>>
    openFile(const std::filesystem::path& path,
             const mparser::RuntimeFileOpenOptions& options) const override {
        const auto normalized = normalize(path);
        auto found = files.find(normalized);
        if (found == files.end()) {
            if (!options.writable ||
                (options.readable && !options.truncate && !options.append)) {
                return mparser::RuntimeSystemResult<
                    std::shared_ptr<mparser::RuntimeHostFile>>::failure(
                    "fake file does not exist");
            }
            found = files.emplace(
                normalized, std::make_shared<std::string>()).first;
        }
        if (options.truncate) {
            found->second->clear();
        }
        auto file = std::make_shared<FakeHostFile>(
            found->second, options.readable, options.writable,
            options.append, closeCount);
        return mparser::RuntimeSystemResult<
            std::shared_ptr<mparser::RuntimeHostFile>>::success(
            std::move(file));
    }

    mparser::RuntimeSystemResult<std::optional<std::string>>
    environment(std::string_view name) const override {
        return mparser::RuntimeSystemResult<
            std::optional<std::string>>::success(
            name == "MPARSER_TEST"
                ? std::optional<std::string>("adapter-value")
                : std::nullopt);
    }

    mparser::RuntimeSystemResult<mparser::RuntimeCalendarTime>
    localCalendarTime() const override {
        return mparser::RuntimeSystemResult<
            mparser::RuntimeCalendarTime>::success(
            {2026, 8, 11, 12, 34, 56.25});
    }

    mparser::RuntimeSystemStatus sleepFor(
        std::chrono::nanoseconds duration) const override {
        slept += duration;
        ++sleepCalls;
        return mparser::RuntimeSystemStatus::success();
    }

    mparser::RuntimeSystemResult<mparser::RuntimeProcessOutput>
    executeProcess(std::string_view command) const override {
        lastCommand = std::string(command);
        return mparser::RuntimeSystemResult<
            mparser::RuntimeProcessOutput>::success(
            {7, "captured output\n"});
    }

    const std::filesystem::path root;
    const std::filesystem::path child;
    const std::filesystem::path tools;
    const std::filesystem::path library;
    const std::filesystem::path temporary;
    mutable size_t currentDirectoryQueries = 0;
    mutable std::filesystem::path lastListedDirectory;
    mutable std::string lastCommand;
    mutable std::chrono::nanoseconds slept{};
    mutable size_t sleepCalls = 0;
    std::shared_ptr<size_t> closeCount = std::make_shared<size_t>(0);
    mutable std::map<std::filesystem::path,
                     std::shared_ptr<std::string>> files;
    mutable std::set<std::filesystem::path> directories;

private:
    static bool isWithin(const std::filesystem::path& rootPath,
                         const std::filesystem::path& candidate) {
        auto rootPart = rootPath.begin();
        auto candidatePart = candidate.begin();
        for (; rootPart != rootPath.end(); ++rootPart, ++candidatePart) {
            if (candidatePart == candidate.end() ||
                *rootPart != *candidatePart) {
                return false;
            }
        }
        return candidatePart != candidate.end();
    }

    static std::filesystem::path normalize(
        const std::filesystem::path& path) {
        auto result = path.lexically_normal();
        if (result.filename().empty() && result.has_relative_path()) {
            result = result.parent_path();
        }
        return result;
    }
};

Capability fullCapabilities() {
    return Capability::CurrentDirectory | Capability::SearchPaths |
           Capability::EnvironmentRead | Capability::FileSystemRead |
           Capability::FileSystemWrite |
           Capability::Process | Capability::Clock | Capability::Sleep |
           Capability::Random;
}

std::shared_ptr<mparser::RuntimeSystemContext>
makeContext(const std::shared_ptr<FakeHostAdapter>& adapter,
            std::uint64_t seed = 1234U,
            size_t maximumOpenFiles = 256,
            size_t maximumFileReadBytes = 16U * 1024U * 1024U) {
    mparser::RuntimeSystemContextOptions options;
    options.capabilities = fullCapabilities();
    options.hostAdapter = adapter;
    options.searchPaths = {adapter->tools};
    options.randomSeed = seed;
    options.maximumOpenFiles = maximumOpenFiles;
    options.maximumFileReadBytes = maximumFileReadBytes;
    return std::make_shared<mparser::RuntimeSystemContext>(
        std::move(options));
}

void permissionSmoke() {
    auto isolated = mparser::makeIsolatedRuntimeSystemContext();
    require(!isolated->currentDirectory().succeeded,
            "isolated context exposed the current directory");
    require(!isolated->searchPaths().succeeded,
            "isolated context exposed search paths");
    require(!isolated->environment("PATH").succeeded,
            "isolated context exposed the environment");
    require(!isolated->listDirectory(".").succeeded,
            "isolated context exposed the filesystem");
    mparser::RuntimeFileOpenOptions writeOptions;
    writeOptions.writable = true;
    writeOptions.truncate = true;
    require(!isolated->openFile("denied.txt", writeOptions).succeeded,
            "isolated context opened a writable file");
    require(!isolated->localCalendarTime().succeeded,
            "isolated context exposed the clock");
    require(!isolated->executeProcess("ignored").succeeded,
            "isolated context executed a process");
    require(!isolated->randomUniform(1).succeeded,
            "isolated context generated random values");
}

void pathAndHostSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    auto context = makeContext(adapter);
    require(adapter->currentDirectoryQueries == 1,
            "context did not initialize from the host directory");

    const auto original = context->currentDirectory();
    require(original.succeeded && original.value == adapter->root,
            "initial current directory mismatch");
    require(context->changeCurrentDirectory("child").succeeded,
            "relative current-directory change failed");
    require(context->currentDirectory().value == adapter->child,
            "current-directory change was not session-local");
    require(!context->changeCurrentDirectory("missing").succeeded,
            "missing current directory was accepted");

    auto paths = context->searchPaths();
    require(paths.succeeded && paths.value ==
                                   std::vector<std::filesystem::path>{
                                       adapter->tools},
            "initial search path mismatch");
    require(context->addSearchPaths({adapter->library}, true).succeeded,
            "search path prepend failed");
    paths = context->searchPaths();
    require(paths.value == std::vector<std::filesystem::path>{
                               adapter->library, adapter->tools},
            "search path order mismatch");
    require(context->addSearchPaths({adapter->tools}, false).succeeded,
            "search path move failed");
    paths = context->searchPaths();
    require(paths.value == std::vector<std::filesystem::path>{
                               adapter->library, adapter->tools},
            "search path de-duplication mismatch");
    require(context->removeSearchPaths({adapter->library}).succeeded,
            "search path removal failed");

    const auto childListing = context->listDirectory({});
    require(childListing.succeeded && childListing.value.size() == 2 &&
                adapter->lastListedDirectory.lexically_normal() ==
                    adapter->child,
            "empty directory listing did not use the session directory");
    require(context->regularFileExists("data.bin").value,
            "relative file query mismatch");
    require(context->directoryExists(".").value,
            "relative directory query mismatch");
    require(context->temporaryDirectory().value == adapter->temporary,
            "temporary directory mismatch");

    const auto environment = context->environment("MPARSER_TEST");
    require(environment.succeeded && environment.value == "adapter-value",
            "environment adapter result mismatch");
    require(!context->environment("ABSENT").value.has_value(),
            "absent environment variable was fabricated");

    const auto calendar = context->localCalendarTime();
    require(calendar.succeeded && calendar.value.year == 2026 &&
                calendar.value.month == 8 && calendar.value.day == 11 &&
                std::fabs(calendar.value.second - 56.25) < 1e-12,
            "calendar adapter result mismatch");

    const auto process = context->executeProcess("fake command");
    require(process.succeeded && process.value.status == 7 &&
                process.value.output == "captured output\n" &&
                adapter->lastCommand == "fake command",
            "process adapter result mismatch");
}

void fileContextSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    auto context = makeContext(adapter, 1234U, 2, 64);

    mparser::RuntimeFileOpenOptions writeOptions;
    writeOptions.writable = true;
    writeOptions.truncate = true;
    writeOptions.permission = "w";
    const auto first = context->openFile("first.txt", writeOptions);
    const auto second = context->openFile("second.txt", writeOptions);
    require(first.succeeded && second.succeeded && first.value >= 3 &&
                second.value > first.value,
            "file identifiers were not allocated monotonically");
    require(!context->openFile("third.txt", writeOptions).succeeded,
            "open-file resource limit was ignored");
    const auto written = context->writeFile(first.value, "one two three");
    require(written.succeeded && written.value == 13,
            "file write count mismatch");
    require(context->closeFile(first.value).succeeded,
            "file close failed");
    require(!context->closeFile(first.value).succeeded,
            "closed file identifier remained valid");
    require(context->closeAllFiles().succeeded &&
                context->openFileIdentifiers().value.empty(),
            "close-all did not release every file identifier");

    mparser::RuntimeFileOpenOptions readOptions;
    readOptions.readable = true;
    readOptions.permission = "r";
    const auto reader = context->openFile("first.txt", readOptions);
    require(reader.succeeded, "written fake file could not be reopened");
    const auto input = context->readFileRemaining(reader.value);
    require(input.succeeded && input.value == "one two three",
            "file read payload mismatch");
    require(context->restoreUnreadFileData(reader.value, "two three")
                .succeeded &&
                context->filePosition(reader.value).value == 4 &&
                context
                    ->seekFile(reader.value, 0,
                               mparser::RuntimeFileSeekOrigin::Current)
                    .succeeded &&
                context->filePosition(reader.value).value == 4 &&
                context->readFileRemaining(reader.value).value == "two three",
            "unread file buffer or logical position was not preserved");
    require(context->closeFile(reader.value).succeeded,
            "reader close failed");

    mparser::RuntimeFileOpenOptions updateOptions;
    updateOptions.readable = true;
    updateOptions.writable = true;
    updateOptions.permission = "r+b";
    const auto update = context->openFile("first.txt", updateOptions);
    require(update.succeeded, "update stream could not be opened");
    require(context->readFileRemaining(update.value).value ==
                "one two three" &&
                context->restoreUnreadFileData(update.value, "two three")
                    .succeeded &&
                !context->writeFile(update.value, "TWO").succeeded,
            "update stream allowed a read-to-write switch without fseek");
    require(context
                ->seekFile(update.value, 0,
                           mparser::RuntimeFileSeekOrigin::Current)
                .succeeded &&
                context->filePosition(update.value).value == 4 &&
                context->writeFile(update.value, "TWO").succeeded &&
                context->filePosition(update.value).value == 7 &&
                !context->readFileRemaining(update.value).succeeded,
            "update stream did not synchronize a read-to-write switch");
    require(context
                ->seekFile(update.value, 0,
                           mparser::RuntimeFileSeekOrigin::Beginning)
                .succeeded &&
                context->readFileRemaining(update.value).value ==
                    "one TWO three" &&
                context
                    ->seekFile(update.value, -5,
                               mparser::RuntimeFileSeekOrigin::End)
                    .succeeded &&
                context->filePosition(update.value).value == 8 &&
                !context
                     ->seekFile(update.value, -1,
                                mparser::RuntimeFileSeekOrigin::Beginning)
                     .succeeded &&
                context->closeFile(update.value).succeeded,
            "absolute/end-relative file positioning mismatch");

    auto small = makeContext(adapter, 1234U, 2, 4);
    const auto limited = small->openFile("first.txt", readOptions);
    require(limited.succeeded &&
                !small->readFileRemaining(limited.value).succeeded,
            "file read byte limit was ignored");

    auto lifetimeAdapter = std::make_shared<FakeHostAdapter>();
    {
        auto lifetimeContext = makeContext(lifetimeAdapter);
        require(lifetimeContext->openFile("lifetime.txt", writeOptions)
                    .succeeded,
                "lifetime file could not be opened");
    }
    require(*lifetimeAdapter->closeCount == 1,
            "context destruction did not close its host files");
}

void formattedScannerSmoke() {
    auto scanned = mparser::runtimeScanFormattedText(
        "hello42 17\nabc", "%s");
    require(scanned.succeeded && scanned.matchedCount == 3 &&
                scanned.consumedBytes == 14 &&
                mparser::runtimeTextScalarUtf8(scanned.value) ==
                    std::optional<std::string>{"hello4217abc"},
            "repeated string scan diverged from MATLAB behavior");

    mparser::RuntimeFileScanSize firstThree;
    firstThree.maximumMatches = 3;
    firstThree.scalarRequested = true;
    scanned = mparser::runtimeScanFormattedText(
        "1 2 3 4 5 tail", "%d", firstThree);
    require(scanned.succeeded && scanned.matchedCount == 3 &&
                scanned.consumedBytes == 5 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({3, 1}) &&
                mparser::runtimeNumericElement(scanned.value, 2) == 3.0,
            "bounded numeric scan or cursor mismatch");

    mparser::RuntimeFileScanSize paddedScalar;
    paddedScalar.maximumMatches = 4;
    paddedScalar.scalarRequested = true;
    scanned = mparser::runtimeScanFormattedText(
        "1 2", "%d", paddedScalar);
    require(scanned.succeeded && scanned.matchedCount == 2 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({4, 1}) &&
                mparser::runtimeNumericElement(scanned.value, 2) == 0.0 &&
                mparser::runtimeNumericElement(scanned.value, 3) == 0.0,
            "finite scalar scan size did not zero-pad its output");

    mparser::RuntimeFileScanSize matrix;
    matrix.maximumMatches = 4;
    matrix.matrixRequested = true;
    matrix.rows = 2;
    matrix.columns = 2;
    scanned = mparser::runtimeScanFormattedText("1 2 3", "%d", matrix);
    require(scanned.succeeded && scanned.matchedCount == 3 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({2, 2}) &&
                mparser::runtimeNumericElement(scanned.value, 2) == 3.0 &&
                mparser::runtimeNumericElement(scanned.value, 3) == 0.0,
            "partial matrix scan did not use column-major zero padding");

    mparser::RuntimeFileScanSize paddedMatrix;
    paddedMatrix.maximumMatches = 6;
    paddedMatrix.matrixRequested = true;
    paddedMatrix.rows = 2;
    paddedMatrix.columns = 3;
    scanned = mparser::runtimeScanFormattedText("1 2 3", "%d",
                                                paddedMatrix);
    require(scanned.succeeded && scanned.matchedCount == 3 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({2, 3}) &&
                mparser::runtimeNumericElement(scanned.value, 5) == 0.0,
            "finite matrix scan size did not retain its requested shape");

    scanned = mparser::runtimeScanFormattedText(
        "skip=9 value=2.5", "skip=%*d value=%f");
    require(scanned.succeeded && scanned.matchedCount == 1 &&
                mparser::runtimeNumericElement(scanned.value, 0) == 2.5,
            "literal or suppressed formatted scan mismatch");

    mparser::RuntimeFileScanSize oneField;
    oneField.maximumMatches = 1;
    oneField.scalarRequested = true;
    scanned = mparser::runtimeScanFormattedText(
        "alphabet tail", "%s", oneField);
    require(scanned.succeeded && scanned.matchedCount == 1 &&
                mparser::runtimeTextScalarUtf8(scanned.value) ==
                    std::optional<std::string>{"alphabet"},
            "bounded string scan truncated a complete text field");

    scanned = mparser::runtimeScanFormattedText(
        "7 word", "%*d %s", oneField);
    require(scanned.succeeded && scanned.matchedCount == 1 &&
                scanned.value.numericClass ==
                    mparser::RuntimeNumericClass::Double &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({4, 1}) &&
                mparser::runtimeNumericElement(scanned.value, 0) ==
                    static_cast<double>('w'),
            "suppressed numeric conversion did not force mixed double output");

    scanned = mparser::runtimeScanFormattedText(
        "9223372036854775807 -9223372036854775808", "%ld %li");
    const auto signedMaximum =
        mparser::runtimeNumericElementValue(scanned.value, 0);
    const auto signedMinimum =
        mparser::runtimeNumericElementValue(scanned.value, 1);
    require(scanned.succeeded && scanned.matchedCount == 2 &&
                scanned.value.numericClass ==
                    mparser::RuntimeNumericClass::Int64 &&
                signedMaximum && signedMinimum &&
                signedMaximum->integerRealBits ==
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) &&
                signedMinimum->integerRealBits ==
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::min()),
            "signed 64-bit formatted scan lost exact integer values");

    scanned = mparser::runtimeScanFormattedText(
        "18446744073709551615 ffffffffffffffff", "%lu %lx");
    const auto unsignedDecimal =
        mparser::runtimeNumericElementValue(scanned.value, 0);
    const auto unsignedHex =
        mparser::runtimeNumericElementValue(scanned.value, 1);
    require(scanned.succeeded && scanned.matchedCount == 2 &&
                scanned.value.numericClass ==
                    mparser::RuntimeNumericClass::UInt64 &&
                unsignedDecimal && unsignedHex &&
                unsignedDecimal->integerRealBits ==
                    std::numeric_limits<std::uint64_t>::max() &&
                unsignedHex->integerRealBits ==
                    std::numeric_limits<std::uint64_t>::max(),
            "unsigned 64-bit formatted scan lost exact integer values");

    scanned = mparser::runtimeScanFormattedText("-1", "%u");
    require(scanned.succeeded && scanned.matchedCount == 1 &&
                mparser::runtimeNumericElement(scanned.value, 0) ==
                    static_cast<double>(
                        std::numeric_limits<std::uint32_t>::max()),
            "unsigned 32-bit scan did not apply two's-complement conversion");

    mparser::RuntimeFileScanSize paddedCharacters;
    paddedCharacters.maximumMatches = 3;
    paddedCharacters.scalarRequested = true;
    scanned = mparser::runtimeScanFormattedText(
        "ab", "%c", paddedCharacters);
    require(scanned.succeeded && scanned.matchedCount == 2 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({1, 3}) &&
                mparser::runtimeCharacterElement(scanned.value, 0) == u'a' &&
                mparser::runtimeCharacterElement(scanned.value, 1) == u'b' &&
                mparser::runtimeCharacterElement(scanned.value, 2) == u'\0',
            "finite character scan size did not null-pad its output");
    mparser::RuntimeFileScanSize zeroRows;
    zeroRows.matrixRequested = true;
    zeroRows.rows = 0;
    zeroRows.maximumMatches = 0;
    scanned = mparser::runtimeScanFormattedText("1 2 3", "%d", zeroRows);
    require(scanned.succeeded && scanned.matchedCount == 0 &&
                scanned.consumedBytes == 0 &&
                mparser::runtimeDimensions(scanned.value) ==
                    std::vector<size_t>({0, 0}),
            "zero-row formatted scan consumed input or changed shape");
    require(!mparser::runtimeScanFormattedText("1", "%q").succeeded,
            "unsupported formatted scan conversion was accepted");
    mparser::RuntimeFileScanSize excessive;
    excessive.maximumMatches = std::numeric_limits<size_t>::max();
    excessive.scalarRequested = true;
    require(!mparser::runtimeScanFormattedText("1", "%d", excessive)
                 .succeeded,
            "excessive formatted scan output size was accepted");
}

void sleepSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    auto context = makeContext(adapter);
    mparser::RuntimeExecutionControl control;
    require(context->sleepFor(std::chrono::milliseconds(125), &control)
                .succeeded,
            "chunked sleep failed");
    require(adapter->sleepCalls == 3 &&
                adapter->slept == std::chrono::milliseconds(125),
            "sleep was not chunked through the host adapter");
    require(!context->sleepFor(std::chrono::nanoseconds(-1), nullptr)
                 .succeeded,
            "negative sleep was accepted");

    mparser::RuntimeCancellationToken cancellation;
    cancellation.requestCancellation();
    mparser::RuntimeExecutionControl cancelled({}, cancellation);
    const size_t callsBefore = adapter->sleepCalls;
    require(!context->sleepFor(std::chrono::seconds(1), &cancelled)
                 .succeeded &&
                adapter->sleepCalls == callsBefore,
            "cancelled sleep reached the host adapter");
}

void randomSmoke() {
    auto firstAdapter = std::make_shared<FakeHostAdapter>();
    auto secondAdapter = std::make_shared<FakeHostAdapter>();
    auto first = makeContext(firstAdapter, 42U);
    auto second = makeContext(secondAdapter, 42U);

    const auto firstUniform = first->randomUniform(32);
    const auto secondUniform = second->randomUniform(32);
    require(firstUniform.succeeded && secondUniform.succeeded &&
                firstUniform.value == secondUniform.value,
            "equal random seeds were not reproducible");
    for (double value : firstUniform.value) {
        require(value >= 0.0 && value < 1.0,
                "uniform random value escaped [0,1)");
    }

    require(first->reseedRandom(42U).succeeded,
            "random reseed failed");
    require(first->randomUniform(32).value == firstUniform.value,
            "random reseed did not restore the sequence");

    require(first->reseedRandom(9U).succeeded,
            "normal random reseed failed");
    const auto saved = first->randomState();
    const auto normal = first->randomNormal(5);
    require(normal.succeeded && normal.value.size() == 5,
            "normal random generation failed");
    require(saved.succeeded, "random state snapshot failed");
    require(first->restoreRandomState(saved.value).succeeded,
            "random state restore rejected its own snapshot");
    require(first->randomNormal(5).value == normal.value,
            "saved random state did not restore the sequence position");

    first->setPauseEnabled(false);
    require(!first->pauseEnabled(), "pause state was not session-local");
}

void existBuiltinSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    auto system = makeContext(adapter);
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    std::map<std::string, mparser::RuntimeValue> variables{
        {"shadowed", mparser::makeRuntimeNumberValue(1.0)},
        {"sin", mparser::makeRuntimeNumberValue(2.0)},
    };
    mparser::BuiltinWorkspaceAccess workspace;
    workspace.variables = &variables;
    workspace.functionExists = [](std::string_view name) {
        return name == "local_fn" || name == "child";
    };
    workspace.classExists = [](std::string_view name) {
        return name == "LoadedClass";
    };
    mparser::BuiltinCallContext callContext;
    callContext.workspace = &workspace;
    callContext.systemContext = system.get();
    callContext.registry = registry.get();

    const auto query = [&](std::string name,
                           std::optional<std::string> searchType =
                               std::nullopt) {
        std::vector<mparser::RuntimeValue> arguments{
            mparser::makeRuntimeCharacterVectorUtf8(name)};
        if (searchType) {
            arguments.push_back(
                mparser::makeRuntimeCharacterVectorUtf8(*searchType));
        }
        const auto result = registry->invoke(
            "exist", mparser::BuiltinCall{
                         arguments, 1, {}, &callContext});
        require(result.succeeded && result.outputs.size() == 1 &&
                    result.outputs.front().kind ==
                        mparser::RuntimeValueKind::Number,
                "exist builtin query failed");
        return result.outputs.front().number;
    };

    require(query("shadowed") == 1.0,
            "exist did not prioritize a workspace variable");
    require(query("sin") == 1.0 &&
                query("sin", "BuIlTiN") == 5.0 &&
                query("sin", "var") == 1.0,
            "exist variable/builtin filters mismatch");
    require(query("LoadedClass") == 8.0 &&
                query("LoadedClass", "CLASS") == 8.0,
            "exist class query mismatch");
    require(query("local_fn") == 2.0 &&
                query("local_fn", "file") == 2.0,
            "exist loaded-function query mismatch");
    require(query("child") == 7.0 &&
                query("child", "dir") == 7.0 &&
                query("child", "file") == 7.0,
            "exist did not prioritize a folder over a loaded function");
    require(query("script") == 2.0 &&
                query("notes.txt", "file") == 2.0 &&
                query("application") == 2.0 &&
                query("live") == 2.0,
            "exist ordinary-file classification mismatch");
    require(query("packed") == 6.0,
            "exist P-code classification mismatch");
    require(query("model") == 4.0,
            "exist model classification mismatch");
    require(query((adapter->library / "native.mexw64").string(),
                  "file") == 3.0,
            "exist absolute MEX classification mismatch");
    require(query("definitely_absent") == 0.0,
            "exist fabricated an absent target");

    std::vector<mparser::RuntimeValue> invalidArguments{
        mparser::makeRuntimeCharacterVectorUtf8("sin"),
        mparser::makeRuntimeCharacterVectorUtf8("unknown")};
    const auto invalid = registry->invoke(
        "exist", mparser::BuiltinCall{
                     invalidArguments, 1, {}, &callContext});
    require(!invalid.succeeded && invalid.diagnostics.size() == 1 &&
                invalid.diagnostics.front().identifier ==
                    "MParser:InvalidExistSearchType",
            "exist accepted an invalid search type");
}

void fileBuiltinSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    auto system = makeContext(adapter);
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    std::vector<mparser::RuntimeOutputEvent> outputEvents;
    mparser::RuntimeOutputSink outputSink =
        [&outputEvents](const mparser::RuntimeOutputEvent& event) {
            outputEvents.push_back(event);
            return true;
        };
    mparser::BuiltinCallContext context;
    context.systemContext = system.get();
    context.outputSink = &outputSink;
    context.registry = registry.get();
    const auto text = [](std::string_view value) {
        return mparser::makeRuntimeCharacterVectorUtf8(value);
    };
    const auto invoke = [&](std::string_view name,
                            std::vector<mparser::RuntimeValue> arguments,
                            size_t outputs) {
        return registry->invoke(
            name, mparser::BuiltinCall{arguments, outputs, {}, &context});
    };
    const auto outputNumber = [](const mparser::BuiltinResult& result,
                                 size_t index) {
        require(result.succeeded && result.outputs.size() > index,
                "file builtin omitted an output");
        const auto value =
            mparser::runtimeNumericElement(result.outputs[index], 0);
        require(value.has_value(), "file builtin output is not numeric");
        return *value;
    };

    const std::string separator(
        1, static_cast<char>(
               std::filesystem::path::preferred_separator));
    auto result = invoke("fullfile", {text("alpha"), text("beta"),
                                       text("data.txt")}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs.front()) ==
                    std::optional<std::string>{
                        "alpha" + separator + "beta" + separator +
                        "data.txt"},
            "fullfile scalar composition mismatch");
    result = invoke(
        "fullfile",
        {mparser::makeRuntimeStringArray(
             {1, 2}, {{u"left", false}, {u"right", false}}),
         mparser::makeRuntimeStringScalarUtf8("tail")},
        1);
    require(result.succeeded &&
                mparser::runtimeDimensions(result.outputs.front()) ==
                    std::vector<size_t>({1, 2}) &&
                mparser::runtimeUtf16ToUtf8(
                    mparser::runtimeStringElement(
                        result.outputs.front(), 1)->value) ==
                    "right" + separator + "tail",
            "fullfile string-array scalar expansion mismatch");

    result = invoke(
        "fullfile",
        {mparser::makeRuntimeStringArray(
             {1, 2}, {{u"left", false}, {u"", true}}),
         mparser::makeRuntimeStringScalarUtf8("tail")},
        1);
    const auto* emptyComponent =
        result.succeeded && !result.outputs.empty()
            ? mparser::runtimeStringElement(result.outputs.front(), 1)
            : nullptr;
    require(result.succeeded && emptyComponent &&
                !emptyComponent->missing &&
                mparser::runtimeUtf16ToUtf8(emptyComponent->value) ==
                    "tail",
            "fullfile did not treat a missing string as an empty component");

    result = invoke("fullfile", {text("alpha"), text("/rooted")}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs.front()) ==
                    std::optional<std::string>{
                        "alpha" + separator + "rooted"},
            "fullfile replaced its prefix with a later rooted component");

    result = invoke("filesep", {}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs.front()) ==
                    std::optional<std::string>{separator},
            "filesep did not report the native separator");
    result = invoke("pathsep", {}, 1);
#ifdef _WIN32
    const std::string expectedPathSeparator = ";";
#else
    const std::string expectedPathSeparator = ":";
#endif
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs.front()) ==
                    std::optional<std::string>{expectedPathSeparator},
            "pathsep did not report the native path-list separator");
    result = invoke("fullfile", {mparser::makeRuntimeMissingValue(),
                                  text("tail")}, 1);
    require(!result.succeeded,
            "fullfile accepted a bare missing value as a path component");

    result = invoke("fopen", {text("session.txt"), text("w")}, 2);
    const int writer = static_cast<int>(outputNumber(result, 0));
    require(writer >= 3 &&
                mparser::runtimeTextScalarUtf8(result.outputs[1]) ==
                    std::optional<std::string>{""},
            "fopen write result mismatch");
    result = invoke("fopen", {mparser::makeRuntimeNumberValue(writer)}, 4);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"session.txt"} &&
                mparser::runtimeTextScalarUtf8(result.outputs[1]) ==
                    std::optional<std::string>{"wb"} &&
                mparser::runtimeTextScalarUtf8(result.outputs[3]) ==
                    std::optional<std::string>{"UTF-8"},
            "fopen identifier query mismatch");
    result = invoke("fprintf",
                    {mparser::makeRuntimeNumberValue(writer), text("hello%d"),
                     mparser::makeRuntimeNumberValue(42)},
                    1);
    require(outputNumber(result, 0) == 7.0,
            "file fprintf count mismatch");
    require(outputEvents.empty(),
            "file fprintf leaked into the console sink");
    require(outputNumber(
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(writer)}, 1),
                0) == 0.0,
            "fclose status mismatch");

    result = invoke("fopen", {text("position.txt"), text("w+")}, 1);
    const int updater = static_cast<int>(outputNumber(result, 0));
    require(invoke("fprintf",
                   {mparser::makeRuntimeNumberValue(updater),
                    text("10 20 30")},
                   0).succeeded &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 8.0 &&
                !invoke("fscanf",
                        {mparser::makeRuntimeNumberValue(updater),
                         text("%d"), mparser::makeRuntimeNumberValue(1)},
                        2).succeeded,
            "update stream did not require positioning after writing");
    require(invoke("frewind",
                   {mparser::makeRuntimeNumberValue(updater)}, 0)
                .succeeded &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 0.0 &&
                !invoke("fscanf",
                        {mparser::makeRuntimeNumberValue(updater),
                         text("%q")},
                        1).succeeded &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 0.0,
            "frewind or failed-scan position rollback mismatch");
    result = invoke("fscanf",
                    {mparser::makeRuntimeNumberValue(updater), text("%d"),
                     mparser::makeRuntimeNumberValue(1)},
                    2);
    require(result.succeeded && outputNumber(result, 0) == 10.0 &&
                outputNumber(result, 1) == 1.0 &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 2.0 &&
                !invoke("fprintf",
                        {mparser::makeRuntimeNumberValue(updater),
                         text("99")},
                        0).succeeded,
            "fscanf did not expose its logical byte position");
    require(outputNumber(
                invoke("fseek",
                       {mparser::makeRuntimeNumberValue(updater),
                        mparser::makeRuntimeNumberValue(1), text("cof")},
                       1),
                0) == 0.0 &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 3.0 &&
                invoke("fprintf",
                       {mparser::makeRuntimeNumberValue(updater), text("99")},
                       0).succeeded &&
                outputNumber(
                    invoke("fseek",
                           {mparser::makeRuntimeNumberValue(updater),
                            mparser::makeRuntimeNumberValue(0),
                            mparser::makeRuntimeNumberValue(-1)},
                           1),
                    0) == 0.0,
            "fseek did not synchronize the update stream");
    result = invoke("fscanf",
                    {mparser::makeRuntimeNumberValue(updater), text("%d"),
                     mparser::makeRuntimeNumberValue(3)},
                    2);
    require(result.succeeded && outputNumber(result, 1) == 3.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 0) == 10.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 1) == 99.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 2) == 30.0 &&
                outputNumber(
                    invoke("fseek",
                           {mparser::makeRuntimeNumberValue(updater),
                            mparser::makeRuntimeNumberValue(-2),
                            mparser::makeRuntimeNumberValue(1)},
                           1),
                    0) == 0.0 &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(updater)}, 1),
                    0) == 6.0,
            "positioned update read/write results mismatch");
    result = invoke("fseek",
                    {mparser::makeRuntimeNumberValue(updater),
                     mparser::makeRuntimeNumberValue(-1), text("bof")},
                    1);
    require(outputNumber(result, 0) == -1.0 &&
                !invoke("fseek",
                        {mparser::makeRuntimeNumberValue(updater),
                         mparser::makeRuntimeNumberValue(0), text("invalid")},
                        1).succeeded &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(updater)}, 0)
                    .succeeded,
            "file-position failures did not follow the builtin contract");

    result = invoke("fopen", {text("position.txt"), text("a+")}, 1);
    const int appendUpdater = static_cast<int>(outputNumber(result, 0));
    require(outputNumber(
                invoke("fseek",
                       {mparser::makeRuntimeNumberValue(appendUpdater),
                        mparser::makeRuntimeNumberValue(0), text("bof")},
                       1),
                0) == 0.0 &&
                invoke("fprintf",
                       {mparser::makeRuntimeNumberValue(appendUpdater),
                        text("|")},
                       0).succeeded &&
                invoke("frewind",
                       {mparser::makeRuntimeNumberValue(appendUpdater)}, 0)
                    .succeeded,
            "a+ did not preserve append writes across positioning");
    result = invoke("fscanf",
                    {mparser::makeRuntimeNumberValue(appendUpdater),
                     text("%c")},
                    2);
    require(result.succeeded && outputNumber(result, 1) == 9.0 &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"10 99 30|"} &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(appendUpdater)}, 0)
                    .succeeded,
            "a+ append/readback semantics mismatch");

    result = invoke("fopen", {text("session.txt"), text("a")}, 1);
    const int appender = static_cast<int>(outputNumber(result, 0));
    require(invoke("fprintf",
                   {mparser::makeRuntimeNumberValue(appender), text("!")},
                   0).succeeded &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(appender)}, 0)
                    .succeeded,
            "append-mode write failed");

    result = invoke("fopen", {text("session.txt"), text("r")}, 1);
    const int reader = static_cast<int>(outputNumber(result, 0));
    result = invoke("fopen", {text("second.txt"), text("w")}, 1);
    const int secondWriter = static_cast<int>(outputNumber(result, 0));
    const auto openIdentifiers = invoke("fopen", {text("all")}, 1);
    require(openIdentifiers.succeeded &&
                mparser::runtimeDimensions(
                    openIdentifiers.outputs.front()) ==
                    std::vector<size_t>({1, 2}) &&
                mparser::runtimeShapeElementCount(
                    openIdentifiers.outputs.front()) == 2 &&
                mparser::runtimeNumericElement(
                    openIdentifiers.outputs.front(), 0) ==
                    static_cast<double>(reader) &&
                mparser::runtimeNumericElement(
                    openIdentifiers.outputs.front(), 1) ==
                    static_cast<double>(secondWriter),
            "fopen all did not report the session handle");
    result = invoke("fscanf",
                    {mparser::makeRuntimeNumberValue(reader), text("%s")}, 2);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"hello42!"} &&
                outputNumber(result, 1) == 1.0,
            "fscanf string result mismatch");
    require(invoke("fclose", {text("all")}, 1).succeeded &&
                system->openFileIdentifiers().value.empty(),
            "fclose all did not release the session handle");

    result = invoke("fopen", {text("absent.txt"), text("r")}, 2);
    require(result.succeeded && outputNumber(result, 0) == -1.0 &&
                !mparser::runtimeTextScalarUtf8(result.outputs[1])
                     ->empty(),
            "fopen failure did not return -1 and a message");
    result = invoke("fclose", {mparser::makeRuntimeNumberValue(999)}, 1);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier ==
                    "MParser:InvalidFileIdentifier",
            "fclose accepted an invalid file identifier");

    result = invoke("fopen", {text("lines.txt"), text("wb")}, 1);
    const int lineWriter = static_cast<int>(outputNumber(result, 0));
    require(invoke("fprintf",
                   {mparser::makeRuntimeNumberValue(lineWriter),
                    text("alpha\r\nbeta\nlast")},
                   0).succeeded &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(lineWriter)}, 0)
                    .succeeded,
            "line fixture write failed");
    result = invoke("fopen", {text("lines.txt"), text("rb")}, 1);
    const int lineReader = static_cast<int>(outputNumber(result, 0));
    require(outputNumber(
                invoke("feof",
                       {mparser::makeRuntimeNumberValue(lineReader)}, 1),
                0) == 0.0,
            "feof was set before a read reached the end");
    result = invoke("fgetl",
                    {mparser::makeRuntimeNumberValue(lineReader)}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"alpha"} &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(lineReader)}, 1),
                    0) == 7.0,
            "fgetl did not remove a CRLF terminator");
    result = invoke("fgets",
                    {mparser::makeRuntimeNumberValue(lineReader),
                     mparser::makeRuntimeNumberValue(3)},
                    2);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"bet"} &&
                outputNumber(result, 1) == 0.0,
            "bounded fgets consumed beyond its character limit");
    result = invoke("fgets",
                    {mparser::makeRuntimeNumberValue(lineReader)}, 2);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"a\n"} &&
                outputNumber(result, 1) == 10.0,
            "fgets did not preserve and report the line terminator");
    result = invoke("fgetl",
                    {mparser::makeRuntimeNumberValue(lineReader)}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{"last"} &&
                outputNumber(
                    invoke("feof",
                           {mparser::makeRuntimeNumberValue(lineReader)}, 1),
                    0) == 1.0 &&
                outputNumber(
                    invoke("fgetl",
                           {mparser::makeRuntimeNumberValue(lineReader)}, 1),
                    0) == -1.0,
            "line reads did not expose the logical end of file");
    require(invoke("frewind",
                   {mparser::makeRuntimeNumberValue(lineReader)}, 0)
                .succeeded &&
                outputNumber(
                    invoke("feof",
                           {mparser::makeRuntimeNumberValue(lineReader)}, 1),
                    0) == 0.0 &&
                outputNumber(
                    invoke("fseek",
                           {mparser::makeRuntimeNumberValue(lineReader),
                            mparser::makeRuntimeNumberValue(-1), text("bof")},
                           1),
                    0) == -1.0,
            "frewind did not clear EOF or failed seek status");
    result = invoke("ferror",
                    {mparser::makeRuntimeNumberValue(lineReader)}, 2);
    require(result.succeeded &&
                !mparser::runtimeTextScalarUtf8(result.outputs[0])->empty() &&
                outputNumber(result, 1) == -1.0,
            "ferror did not report the preceding seek failure");
    result = invoke("ferror",
                    {mparser::makeRuntimeNumberValue(lineReader),
                     text("clear")},
                    2);
    require(result.succeeded &&
                !mparser::runtimeTextScalarUtf8(result.outputs[0])->empty() &&
                outputNumber(result, 1) == -1.0,
            "ferror clear did not return the preceding error");
    result = invoke("ferror",
                    {mparser::makeRuntimeNumberValue(lineReader)}, 2);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    std::optional<std::string>{""} &&
                outputNumber(result, 1) == 0.0 &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(lineReader)}, 0)
                    .succeeded,
            "ferror clear did not reset the stream error indicator");

    result = invoke("fopen",
                    {text("binary.dat"), text("w+"), text("ieee-be"),
                     text("UTF-8")},
                    1);
    const int binary = static_cast<int>(outputNumber(result, 0));
    result = invoke("fopen", {mparser::makeRuntimeNumberValue(binary)}, 4);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[2]) ==
                    std::optional<std::string>{"ieee-be"} &&
                mparser::runtimeTextScalarUtf8(result.outputs[3]) ==
                    std::optional<std::string>{"UTF-8"},
            "fopen did not retain machine format and encoding metadata");
    result = invoke("fopen",
                    {mparser::makeRuntimeNumberValue(binary), text("r")}, 1);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier ==
                    "MParser:InvalidFileQuery",
            "fopen identifier query silently ignored extra inputs");
    auto binaryValues = mparser::runtimeNumericValueFromLogicalOrder(
        {1, 3}, {1.0, 258.0, 65535.0},
        mparser::RuntimeNumericClass::UInt16);
    require(binaryValues.has_value() &&
                outputNumber(
                    invoke("fwrite",
                           {mparser::makeRuntimeNumberValue(binary),
                            *binaryValues, text("uint16")},
                           1),
                    0) == 3.0 &&
                outputNumber(
                    invoke("ftell",
                           {mparser::makeRuntimeNumberValue(binary)}, 1),
                    0) == 6.0 &&
                !invoke("fread",
                        {mparser::makeRuntimeNumberValue(binary)}, 1)
                     .succeeded &&
                invoke("frewind",
                       {mparser::makeRuntimeNumberValue(binary)}, 0)
                    .succeeded,
            "big-endian fwrite or update-stream barrier mismatch");
    result = invoke(
        "fread",
        {mparser::makeRuntimeNumberValue(binary),
         mparser::makeRuntimeVectorValue({2.0, 2.0}),
         text("uint16=>uint16")},
        2);
    require(result.succeeded && outputNumber(result, 1) == 3.0 &&
                result.outputs[0].numericClass ==
                    mparser::RuntimeNumericClass::UInt16 &&
                mparser::runtimeDimensions(result.outputs[0]) ==
                    std::vector<size_t>({2, 2}) &&
                mparser::runtimeNumericElement(result.outputs[0], 0) == 1.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 1) == 258.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 2) ==
                    65535.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 3) == 0.0 &&
                outputNumber(
                    invoke("feof",
                           {mparser::makeRuntimeNumberValue(binary)}, 1),
                    0) == 1.0,
            "fread shape, class, padding, or EOF mismatch");
    require(invoke("frewind",
                   {mparser::makeRuntimeNumberValue(binary)}, 0)
                .succeeded,
            "binary rewind failed");
    result = invoke("fread",
                    {mparser::makeRuntimeNumberValue(binary),
                     mparser::makeRuntimeNumberValue(1), text("uint16"),
                     text("ieee-le")},
                    2);
    require(result.succeeded && outputNumber(result, 0) == 256.0 &&
                outputNumber(result, 1) == 1.0 &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(binary)}, 0)
                    .succeeded,
            "fread machine-format override mismatch");

    result = invoke("fopen",
                    {text("skip.dat"), text("w+"), text("ieee-le")}, 1);
    const int skipped = static_cast<int>(outputNumber(result, 0));
    auto byteValues = mparser::runtimeNumericValueFromLogicalOrder(
        {1, 4}, {1.0, 2.0, 3.0, 4.0},
        mparser::RuntimeNumericClass::UInt8);
    require(byteValues.has_value() &&
                outputNumber(
                    invoke("fwrite",
                           {mparser::makeRuntimeNumberValue(skipped),
                            *byteValues, text("2*uint8"),
                            mparser::makeRuntimeNumberValue(2)},
                           1),
                    0) == 4.0 &&
                invoke("frewind",
                       {mparser::makeRuntimeNumberValue(skipped)}, 0)
                    .succeeded,
            "repeated fwrite precision or skip failed");
    result = invoke("fread",
                    {mparser::makeRuntimeNumberValue(skipped),
                     mparser::makeRuntimeNumberValue(
                         std::numeric_limits<double>::infinity()),
                     text("2*uint8=>uint8"),
                     mparser::makeRuntimeNumberValue(2)},
                    2);
    require(result.succeeded && outputNumber(result, 1) == 4.0 &&
                result.outputs[0].numericClass ==
                    mparser::RuntimeNumericClass::UInt8 &&
                mparser::runtimeNumericElement(result.outputs[0], 0) == 1.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 1) == 2.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 2) == 3.0 &&
                mparser::runtimeNumericElement(result.outputs[0], 3) == 4.0 &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(skipped)}, 0)
                    .succeeded,
            "repeated fread precision or skip failed");

    mparser::RuntimeNumericElementValue exactElement;
    exactElement.numericClass = mparser::RuntimeNumericClass::UInt64;
    exactElement.integerRealBits = std::numeric_limits<std::uint64_t>::max();
    exactElement.real = static_cast<double>(exactElement.integerRealBits);
    auto exactValue = mparser::runtimeNumericValueFromElements(
        {1, 1}, {exactElement}, mparser::RuntimeNumericClass::UInt64);
    result = invoke("fopen", {text("exact.dat"), text("w+")}, 1);
    const int exactFile = static_cast<int>(outputNumber(result, 0));
    require(exactValue.has_value() &&
                outputNumber(
                    invoke("fwrite",
                           {mparser::makeRuntimeNumberValue(exactFile),
                            *exactValue, text("uint64")},
                           1),
                    0) == 1.0 &&
                invoke("frewind",
                       {mparser::makeRuntimeNumberValue(exactFile)}, 0)
                    .succeeded,
            "exact uint64 fixture write failed");
    result = invoke("fread",
                    {mparser::makeRuntimeNumberValue(exactFile),
                     mparser::makeRuntimeNumberValue(1), text("*uint64")},
                    2);
    const auto exactOutput =
        result.succeeded
            ? mparser::runtimeNumericElementValue(result.outputs[0], 0)
            : std::nullopt;
    require(result.succeeded && exactOutput &&
                exactOutput->numericClass ==
                    mparser::RuntimeNumericClass::UInt64 &&
                exactOutput->integerRealBits ==
                    std::numeric_limits<std::uint64_t>::max() &&
                invoke("fclose",
                       {mparser::makeRuntimeNumberValue(exactFile)}, 0)
                    .succeeded,
            "fread/fwrite lost exact uint64 payload bits");

    result = invoke("fopen",
                    {text("encoding.txt"), text("w"), text("native"),
                     text("windows-1252")},
                    1);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier ==
                    "MParser:UnsupportedFileEncoding",
            "fopen silently accepted an unsupported text encoding");

    result = invoke("fprintf", {text("console=%d"),
                                 mparser::makeRuntimeNumberValue(9)}, 1);
    require(result.succeeded && outputNumber(result, 0) == 9.0 &&
                outputEvents.size() == 1 &&
                outputEvents.front().text == "console=9",
            "console fprintf routing regressed");
}

void filesystemManagementBuiltinSmoke() {
    auto adapter = std::make_shared<FakeHostAdapter>();
    adapter->files[adapter->root / "alpha.txt"] =
        std::make_shared<std::string>("alpha contents");
    adapter->files[adapter->tools / "lookup.txt"] =
        std::make_shared<std::string>("path contents");
    auto system = makeContext(adapter);
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    mparser::RuntimeWarningContext warningContext;
    mparser::BuiltinCallContext context;
    context.systemContext = system.get();
    context.registry = registry.get();
    context.warningContext = &warningContext;
    const auto text = [](std::string_view value) {
        return mparser::makeRuntimeCharacterVectorUtf8(value);
    };
    const auto invoke = [&](std::string_view name,
                            std::vector<mparser::RuntimeValue> arguments,
                            size_t outputs) {
        return registry->invoke(
            name, mparser::BuiltinCall{arguments, outputs, {}, &context});
    };
    const auto numeric = [](const mparser::RuntimeValue& value,
                            size_t index = 0) {
        const auto result = mparser::runtimeNumericElement(value, index);
        require(result.has_value(),
                "filesystem builtin output is not numeric");
        return *result;
    };

    auto result = invoke(
        "isfile",
        {mparser::makeRuntimeStringArray(
            {1, 3}, {{u"alpha.txt", false}, {u"", true},
                     {u"child", false}})},
        1);
    require(result.succeeded && result.outputs.size() == 1 &&
                mparser::runtimeDimensions(result.outputs[0]) ==
                    std::vector<size_t>({1, 3}) &&
                numeric(result.outputs[0], 0) == 1.0 &&
                numeric(result.outputs[0], 1) == 0.0 &&
                numeric(result.outputs[0], 2) == 0.0,
            "isfile did not preserve string-array shape or missing values");
    result = invoke(
        "isfolder",
        {mparser::makeRuntimeCellValue(
            {2, 2}, {text("child"), text("alpha.txt"),
                     text("tools"), text("absent")})},
        1);
    require(result.succeeded &&
                mparser::runtimeDimensions(result.outputs[0]) ==
                    std::vector<size_t>({2, 2}) &&
                numeric(result.outputs[0], 0) == 1.0 &&
                numeric(result.outputs[0], 1) == 0.0 &&
                numeric(result.outputs[0], 2) == 1.0 &&
                numeric(result.outputs[0], 3) == 0.0,
            "isfolder did not preserve cell-array shape");

    result = invoke(
        "fileparts",
        {mparser::makeRuntimeStringArray(
            {1, 2}, {{u"folder/archive.tar.gz", false},
                     {u".profile", false}})},
        3);
    const auto stringAt = [](const mparser::RuntimeValue& value,
                             size_t index) {
        const auto* element = mparser::runtimeStringElement(value, index);
        return element && !element->missing
                   ? std::optional<std::string>(
                         mparser::runtimeUtf16ToUtf8(element->value))
                   : std::nullopt;
    };
    require(result.succeeded && result.outputs.size() == 3 &&
                stringAt(result.outputs[0], 0) == "folder" &&
                stringAt(result.outputs[1], 0) == "archive.tar" &&
                stringAt(result.outputs[2], 0) == ".gz" &&
                stringAt(result.outputs[1], 1) == "" &&
                stringAt(result.outputs[2], 1) == ".profile",
            "fileparts string-array projection mismatch");
    result = invoke(
        "fileparts",
        {mparser::makeRuntimeCellValue(
            {1, 2}, {text("one.m"), text("dir/two.txt")})},
        3);
    require(result.succeeded &&
                result.outputs[0].kind == mparser::RuntimeValueKind::Cell &&
                mparser::runtimeTextScalarUtf8(
                    result.outputs[0].cells[1]) == "dir" &&
                mparser::runtimeTextScalarUtf8(
                    result.outputs[1].cells[1]) == "two",
            "fileparts did not preserve cell-array output type");
    result = invoke(
        "fileparts",
        {mparser::makeRuntimeStringArray(
            {1, 1}, {{u"", true}})},
        3);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics[0].identifier ==
                    "MParser:MissingFileName",
            "fileparts accepted a missing string");

    result = invoke("fileread", {text("alpha.txt")}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    "alpha contents",
            "fileread current-directory lookup mismatch");
    result = invoke("fileread", {text("lookup.txt")}, 1);
    require(result.succeeded &&
                mparser::runtimeTextScalarUtf8(result.outputs[0]) ==
                    "path contents",
            "fileread search-path lookup mismatch");

    const auto firstTemporary = invoke("tempname", {}, 1);
    const auto secondTemporary = invoke("tempname", {}, 1);
    const auto relativeTemporary = invoke(
        "tempname", {text("child")}, 1);
    const auto firstName = firstTemporary.succeeded
                               ? mparser::runtimeTextScalarUtf8(
                                     firstTemporary.outputs[0])
                               : std::nullopt;
    const auto secondName = secondTemporary.succeeded
                                ? mparser::runtimeTextScalarUtf8(
                                      secondTemporary.outputs[0])
                                : std::nullopt;
    const auto relativeName = relativeTemporary.succeeded
                                  ? mparser::runtimeTextScalarUtf8(
                                        relativeTemporary.outputs[0])
                                  : std::nullopt;
    require(firstName && secondName && relativeName &&
                *firstName != *secondName &&
                std::filesystem::path(*firstName).parent_path() ==
                    adapter->temporary &&
                std::filesystem::path(*relativeName).parent_path() ==
                    std::filesystem::path("child"),
            "tempname uniqueness or directory projection mismatch");

    result = invoke("mkdir", {text("managed/deep")}, 3);
    require(result.succeeded && result.outputs.size() == 3 &&
                numeric(result.outputs[0]) == 1.0 &&
                adapter->directories.contains(
                    adapter->root / "managed" / "deep") &&
                mparser::runtimeTextScalarUtf8(result.outputs[1]) == "",
            "mkdir recursive creation or status outputs mismatch");
    result = invoke("mkdir", {text("managed/deep")}, 3);
    require(result.succeeded && numeric(result.outputs[0]) == 1.0 &&
                mparser::runtimeTextScalarUtf8(result.outputs[2]) ==
                    "MATLAB:MKDIR:DirectoryExists",
            "mkdir existing-directory status mismatch");
    result = invoke("mkdir", {text("managed/deep")}, 0);
    require(result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics[0].severity ==
                    mparser::DiagnosticSeverity::Warning &&
                warningContext.snapshot().lastIdentifier ==
                    "MATLAB:MKDIR:DirectoryExists",
            "mkdir no-output warning contract mismatch");
    const auto disabledWarning = warningContext.warning(
        {text("off"), text("MATLAB:MKDIR:DirectoryExists")}, 0);
    require(disabledWarning.succeeded,
            "mkdir warning identifier could not be disabled");
    result = invoke("mkdir", {text("managed/deep")}, 0);
    require(result.succeeded && result.diagnostics.empty() &&
                warningContext.snapshot().lastIdentifier ==
                    "MATLAB:MKDIR:DirectoryExists",
            "mkdir warning did not honor session warning state");

    result = invoke("copyfile",
                    {text("alpha.txt"), text("child/copied.txt")}, 3);
    require(result.succeeded && numeric(result.outputs[0]) == 1.0 &&
                adapter->files.contains(adapter->child / "copied.txt") &&
                *adapter->files[adapter->child / "copied.txt"] ==
                    "alpha contents",
            "copyfile file-copy status mismatch");
    result = invoke("movefile",
                    {text("child/copied.txt"), text("library")}, 3);
    require(result.succeeded && numeric(result.outputs[0]) == 1.0 &&
                !adapter->files.contains(adapter->child / "copied.txt") &&
                adapter->files.contains(adapter->library / "copied.txt"),
            "movefile directory destination mismatch");

    adapter->directories.insert(adapter->root / "tree");
    adapter->files[adapter->root / "tree" / "inside.txt"] =
        std::make_shared<std::string>("inside");
    result = invoke("rmdir", {text("tree")}, 3);
    require(result.succeeded && numeric(result.outputs[0]) == 0.0 &&
                mparser::runtimeTextScalarUtf8(result.outputs[2]) ==
                    "MATLAB:RMDIR:DirectoryNotRemoved" &&
                adapter->directories.contains(adapter->root / "tree"),
            "rmdir nonempty status mismatch");
    result = invoke("rmdir", {text("tree"), text("s")}, 3);
    require(result.succeeded && numeric(result.outputs[0]) == 1.0 &&
                !adapter->directories.contains(adapter->root / "tree") &&
                !adapter->files.contains(
                    adapter->root / "tree" / "inside.txt"),
            "rmdir recursive removal mismatch");

    auto limited = makeContext(adapter, 1234U, 256, 4);
    context.systemContext = limited.get();
    result = invoke("fileread", {text("alpha.txt")}, 1);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics[0].identifier ==
                    "MParser:FileReadFailed" &&
                limited->openFileIdentifiers().succeeded &&
                limited->openFileIdentifiers().value.empty(),
            "fileread limit failure leaked an open file");

    auto isolated = mparser::makeIsolatedRuntimeSystemContext();
    context.systemContext = isolated.get();
    result = invoke("isfile", {text("alpha.txt")}, 1);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics[0].identifier ==
                    "MParser:SystemOperationFailed",
            "filesystem query bypassed an isolated system context");
}

void formatBuiltinSmoke() {
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    mparser::RuntimeDisplayFormat state;
    mparser::BuiltinDisplayFormatAccess display;
    display.current = [&state] { return state; };
    display.replace = [&state](mparser::RuntimeDisplayFormat next) {
        const auto previous = state;
        state = next;
        return previous;
    };
    mparser::BuiltinCallContext context;
    context.displayFormat = &display;

    const auto invoke = [&](std::vector<mparser::RuntimeValue> arguments,
                            size_t outputs) {
        return registry->invoke(
            "format", mparser::BuiltinCall{
                          arguments, outputs, {}, &context});
    };
    const auto fieldText = [](const mparser::RuntimeValue& value,
                              std::string_view name) {
        const auto* field = mparser::runtimeStructField(value, name);
        return field ? mparser::runtimeTextScalarUtf8(*field)
                     : std::optional<std::string>{};
    };

    auto result = invoke({}, 1);
    require(result.succeeded && result.outputs.size() == 1 &&
                fieldText(result.outputs.front(), "NumericFormat") ==
                    std::optional<std::string>{"short"} &&
                fieldText(result.outputs.front(), "LineSpacing") ==
                    std::optional<std::string>{"loose"},
            "format query did not return the default state");

    result = invoke(
        {mparser::makeRuntimeCharacterVectorUtf8("long")}, 1);
    require(result.succeeded && result.outputs.size() == 1 &&
                fieldText(result.outputs.front(), "NumericFormat") ==
                    std::optional<std::string>{"short"} &&
                state.numeric ==
                    mparser::RuntimeNumericDisplayFormat::Long,
            "format did not atomically return and replace state");
    const auto original = result.outputs.front();

    result = invoke(
        {mparser::makeRuntimeCharacterVectorUtf8("compact")}, 0);
    require(result.succeeded &&
                state.numeric ==
                    mparser::RuntimeNumericDisplayFormat::Long &&
                state.spacing == mparser::RuntimeLineSpacing::Compact,
            "format compact changed the numeric style");
    result = invoke({}, 1);
    require(result.succeeded &&
                fieldText(result.outputs.front(), "NumericFormat") ==
                    std::optional<std::string>{"long"} &&
                fieldText(result.outputs.front(), "LineSpacing") ==
                    std::optional<std::string>{"compact"},
            "format query lost compact state");

    result = invoke({original}, 0);
    require(result.succeeded && state == mparser::RuntimeDisplayFormat{},
            "format rejected its own state structure");
    result = invoke(
        {mparser::makeRuntimeCharacterVectorUtf8("short"),
         mparser::makeRuntimeCharacterVectorUtf8("E")},
        0);
    require(result.succeeded &&
                state.numeric ==
                    mparser::RuntimeNumericDisplayFormat::ShortE,
            "format two-token style mismatch");

    const auto beforeInvalid = state;
    result = invoke(
        {mparser::makeRuntimeCharacterVectorUtf8("unsupported")}, 0);
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier ==
                    "MParser:InvalidDisplayFormat" &&
                state == beforeInvalid,
            "invalid format style partially modified state");

    std::vector<mparser::RuntimeValue> noArguments;
    result = registry->invoke(
        "format", mparser::BuiltinCall{
                      noArguments, 0, {}, nullptr});
    require(!result.succeeded && result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier ==
                    "MParser:MissingBuiltinContext",
            "format accepted a missing display-state context");
}

} // namespace

int main() {
    try {
        permissionSmoke();
        pathAndHostSmoke();
        fileContextSmoke();
        formattedScannerSmoke();
        sleepSmoke();
        randomSmoke();
        existBuiltinSmoke();
        fileBuiltinSmoke();
        filesystemManagementBuiltinSmoke();
        formatBuiltinSmoke();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Runtime system smoke failure: " << error.what()
                  << '\n';
        return 1;
    }
}
