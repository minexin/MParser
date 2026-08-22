#include "mparser/embedding/compiled_module.h"
#include "mparser/frontend/lexer.h"
#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/frontend/parser.h"
#include "performance_environment.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif
#endif

#ifndef MPARSER_BASELINE_PROJECT_VERSION
#define MPARSER_BASELINE_PROJECT_VERSION "unknown"
#endif

#ifndef MPARSER_BASELINE_BUILD_TYPE
#define MPARSER_BASELINE_BUILD_TYPE "unknown"
#endif

#ifndef MPARSER_BASELINE_COMPILER_ID
#define MPARSER_BASELINE_COMPILER_ID "unknown"
#endif

#ifndef MPARSER_BASELINE_COMPILER_VERSION
#define MPARSER_BASELINE_COMPILER_VERSION "unknown"
#endif

#ifndef MPARSER_BASELINE_SYSTEM_NAME
#define MPARSER_BASELINE_SYSTEM_NAME "unknown"
#endif

#ifndef MPARSER_BASELINE_SYSTEM_VERSION
#define MPARSER_BASELINE_SYSTEM_VERSION "unknown"
#endif

namespace allocation_tracking {

std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> allocationCount{0};
std::atomic<std::uint64_t> requestedBytes{0};

void record(std::size_t size) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    requestedBytes.fetch_add(
        static_cast<std::uint64_t>(size), std::memory_order_relaxed);
}

void* allocate(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (void* memory = std::malloc(size)) {
        record(size);
        return memory;
    }
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        size = 1;
    }
#if defined(_WIN32)
    if (void* memory = _aligned_malloc(size, alignment)) {
        record(size);
        return memory;
    }
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) == 0) {
        record(size);
        return memory;
    }
#endif
    throw std::bad_alloc();
}

void freeAligned(void* memory) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace allocation_tracking

void* operator new(std::size_t size) {
    return allocation_tracking::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_tracking::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_tracking::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_tracking::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_tracking::allocateAligned(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_tracking::allocateAligned(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return allocation_tracking::allocateAligned(
            size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return allocation_tracking::allocateAligned(
            size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    allocation_tracking::freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    allocation_tracking::freeAligned(memory);
}

void operator delete(void* memory, std::size_t,
                     std::align_val_t) noexcept {
    allocation_tracking::freeAligned(memory);
}

void operator delete[](void* memory, std::size_t,
                       std::align_val_t) noexcept {
    allocation_tracking::freeAligned(memory);
}

void operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept {
    allocation_tracking::freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept {
    allocation_tracking::freeAligned(memory);
}

namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;

struct AllocationActivity {
    std::uint64_t count = 0;
    std::uint64_t requestedBytes = 0;
};

struct AllocationMeasurements {
    AllocationActivity total;
    std::vector<std::uint64_t> countSamples;
    std::vector<std::uint64_t> requestedByteSamples;
};

class AllocationScope {
public:
    AllocationScope() {
        allocation_tracking::allocationCount.store(
            0, std::memory_order_relaxed);
        allocation_tracking::requestedBytes.store(
            0, std::memory_order_relaxed);
        allocation_tracking::enabled.store(
            true, std::memory_order_release);
    }

    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    ~AllocationScope() {
        allocation_tracking::enabled.store(
            false, std::memory_order_release);
    }

    AllocationActivity stop() {
        allocation_tracking::enabled.store(
            false, std::memory_order_release);
        return AllocationActivity{
            allocation_tracking::allocationCount.load(
                std::memory_order_relaxed),
            allocation_tracking::requestedBytes.load(
                std::memory_order_relaxed)};
    }
};

struct Options {
    std::string collectorPath;
    std::string cliPath;
    std::string libraryPath;
    std::string sourcePath;
    std::string outputPath;
    std::string workloadId = "custom-v1";
    std::string revision = "working-tree";
    std::string resultVariable = "baseline_result";
    std::vector<std::string> childPrefix;
    size_t parseWarmup = 3;
    size_t parseIterations = 20;
    size_t compileWarmup = 2;
    size_t compileIterations = 10;
    size_t runtimeWarmup = 3;
    size_t runtimeIterations = 20;
    size_t processIterations = 5;
};

struct Measurement {
    std::string status = "measured";
    std::string reason;
    std::string boundary;
    size_t warmupIterations = 0;
    std::vector<std::uint64_t> hostWallSamples;
    std::vector<std::uint64_t> engineSamples;
    std::optional<AllocationMeasurements> allocations;
};

struct RuntimeTotals {
    std::set<std::string> effectiveTiers;
    std::uint64_t fallbackIterations = 0;
    std::uint64_t executedInstructionCount = 0;
    std::uint64_t typedRegionCount = 0;
    std::uint64_t typedRegionAttemptCount = 0;
    std::uint64_t typedRegionExecutionCount = 0;
    std::uint64_t typedRegionFallbackCount = 0;
    std::uint64_t nativeCompilationCount = 0;
    std::uint64_t nativeCacheHitCount = 0;
    std::uint64_t maximumCallDepth = 0;
    std::uint64_t maximumArrayBytes = 0;
    std::uint64_t maximumDiagnosticCount = 0;
};

struct RuntimeMeasurement {
    Measurement measurement;
    RuntimeTotals totals;
    double observedResult = 0.0;
};

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

class Sha256 {
public:
    void update(const void* data, size_t size) {
        require(size <=
                    std::numeric_limits<std::uint64_t>::max() -
                        totalBytes_,
                "SHA-256 input exceeds uint64 byte count");
        totalBytes_ += static_cast<std::uint64_t>(size);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        while (size > 0) {
            const size_t copied =
                std::min(size, block_.size() - blockSize_);
            std::copy_n(bytes, copied, block_.begin() + blockSize_);
            blockSize_ += copied;
            bytes += copied;
            size -= copied;
            if (blockSize_ == block_.size()) {
                compress(block_);
                blockSize_ = 0;
            }
        }
    }

    std::string finish() {
        require(!finished_, "SHA-256 digest was already finalized");
        require(totalBytes_ <=
                    std::numeric_limits<std::uint64_t>::max() / 8u,
                "SHA-256 input bit length exceeds uint64");
        const std::uint64_t bitLength = totalBytes_ * 8u;

        block_[blockSize_++] = 0x80u;
        if (blockSize_ > 56) {
            std::fill(block_.begin() + blockSize_, block_.end(),
                      std::uint8_t{0});
            compress(block_);
            blockSize_ = 0;
        }
        std::fill(block_.begin() + blockSize_,
                  block_.begin() + 56, std::uint8_t{0});
        for (size_t index = 0; index < 8; ++index) {
            block_[56 + index] = static_cast<std::uint8_t>(
                bitLength >> (56u - 8u * index));
        }
        compress(block_);
        finished_ = true;

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    static constexpr std::array<std::uint32_t, 64> constants_{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    void compress(const std::array<std::uint8_t, 64>& block) {
        std::array<std::uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index) {
            const size_t offset = index * 4;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24u) |
                (static_cast<std::uint32_t>(block[offset + 1])
                 << 16u) |
                (static_cast<std::uint32_t>(block[offset + 2])
                 << 8u) |
                static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t sigma0 =
                std::rotr(words[index - 15], 7) ^
                std::rotr(words[index - 15], 18) ^
                (words[index - 15] >> 3u);
            const std::uint32_t sigma1 =
                std::rotr(words[index - 2], 17) ^
                std::rotr(words[index - 2], 19) ^
                (words[index - 2] >> 10u);
            words[index] = words[index - 16] + sigma0 +
                           words[index - 7] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                std::rotr(e, 6) ^ std::rotr(e, 11) ^
                std::rotr(e, 25);
            const std::uint32_t choose =
                (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + constants_[index] + words[index];
            const std::uint32_t sum0 =
                std::rotr(a, 2) ^ std::rotr(a, 13) ^
                std::rotr(a, 22);
            const std::uint32_t majority =
                (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> block_{};
    size_t blockSize_ = 0;
    std::uint64_t totalBytes_ = 0;
    bool finished_ = false;
};

size_t parseCount(std::string_view value, std::string_view option) {
    static constexpr size_t maximumIterationCount = 100000;
    size_t result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }
    if (result > maximumIterationCount) {
        throw std::runtime_error(
            std::string(option) + " cannot exceed " +
            std::to_string(maximumIterationCount));
    }
    return result;
}

bool takeValue(std::string_view argument, std::string_view prefix,
               std::string& output) {
    if (!argument.starts_with(prefix)) {
        return false;
    }
    output = std::string(argument.substr(prefix.size()));
    return true;
}

bool takeCount(std::string_view argument, std::string_view prefix,
               size_t& output) {
    if (!argument.starts_with(prefix)) {
        return false;
    }
    output = parseCount(argument.substr(prefix.size()), prefix);
    return true;
}

bool pathsAlias(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) {
        return false;
    }
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) {
        return true;
    }
    error.clear();
    const auto absoluteLeft =
        std::filesystem::absolute(left, error).lexically_normal();
    if (error) {
        return false;
    }
    const auto absoluteRight =
        std::filesystem::absolute(right, error).lexically_normal();
    return !error && absoluteLeft == absoluteRight;
}

void printUsage() {
    std::cout
        << "Usage: mparser_performance_baseline "
           "--cli=<mparser> --library=<mparser_c> --source=<script.m> "
           "[options]\n"
        << "Options:\n"
        << "  --output=<report.json>\n"
        << "  --workload-id=<id>\n"
        << "  --revision=<revision>\n"
        << "  --result-variable=<name>\n"
        << "  --child-prefix=<launcher-argument> (repeatable)\n"
        << "  --parse-warmup=<count>\n"
        << "  --parse-iterations=<count>\n"
        << "  --compile-warmup=<count>\n"
        << "  --compile-iterations=<count>\n"
        << "  --runtime-warmup=<count>\n"
        << "  --runtime-iterations=<count>\n"
        << "  --process-iterations=<count>\n"
        << "  --quick\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;
    if (argc > 0) {
        options.collectorPath = argv[0];
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            printUsage();
            std::exit(0);
        }
        if (argument == "--quick") {
            options.parseWarmup = 0;
            options.parseIterations = 2;
            options.compileWarmup = 0;
            options.compileIterations = 2;
            options.runtimeWarmup = 1;
            options.runtimeIterations = 2;
            options.processIterations = 1;
            continue;
        }
        std::string value;
        if (takeValue(argument, "--cli=", options.cliPath) ||
            takeValue(argument, "--library=", options.libraryPath) ||
            takeValue(argument, "--source=", options.sourcePath) ||
            takeValue(argument, "--output=", options.outputPath) ||
            takeValue(argument, "--workload-id=", options.workloadId) ||
            takeValue(argument, "--revision=", options.revision) ||
            takeValue(argument, "--result-variable=",
                      options.resultVariable)) {
            continue;
        }
        if (takeValue(argument, "--child-prefix=", value)) {
            options.childPrefix.push_back(std::move(value));
            continue;
        }
        if (takeCount(argument, "--parse-warmup=",
                      options.parseWarmup) ||
            takeCount(argument, "--parse-iterations=",
                      options.parseIterations) ||
            takeCount(argument, "--compile-warmup=",
                      options.compileWarmup) ||
            takeCount(argument, "--compile-iterations=",
                      options.compileIterations) ||
            takeCount(argument, "--runtime-warmup=",
                      options.runtimeWarmup) ||
            takeCount(argument, "--runtime-iterations=",
                      options.runtimeIterations) ||
            takeCount(argument, "--process-iterations=",
                      options.processIterations)) {
            continue;
        }
        throw std::runtime_error(
            "unknown performance baseline option: " +
            std::string(argument));
    }

    require(!options.cliPath.empty(), "--cli is required");
    require(!options.libraryPath.empty(), "--library is required");
    require(!options.sourcePath.empty(), "--source is required");
    require(!options.workloadId.empty(), "--workload-id cannot be empty");
    require(!options.revision.empty(), "--revision cannot be empty");
    require(!options.resultVariable.empty(),
            "--result-variable cannot be empty");
    require(options.parseIterations > 0,
            "--parse-iterations must be positive");
    require(options.compileIterations > 0,
            "--compile-iterations must be positive");
    require(options.runtimeIterations > 0,
            "--runtime-iterations must be positive");
    require(options.processIterations > 0,
            "--process-iterations must be positive");
    require(options.childPrefix.size() <= 16,
            "--child-prefix cannot contain more than 16 arguments");
    if (!options.outputPath.empty()) {
        require(!pathsAlias(options.outputPath, options.sourcePath),
                "--output cannot overwrite the source workload");
        require(!pathsAlias(options.outputPath, options.cliPath),
                "--output cannot overwrite the mparser CLI");
        require(!pathsAlias(options.outputPath, options.libraryPath),
                "--output cannot overwrite the C API library");
        require(!pathsAlias(options.outputPath,
                            options.collectorPath),
                "--output cannot overwrite the baseline collector");
    }
    return options;
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open source: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string sha256(std::string_view value) {
    Sha256 digest;
    digest.update(value.data(), value.size());
    return digest.finish();
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size());
}

std::string sha256File(const std::filesystem::path& path,
                       std::string_view label) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input),
            "failed to open " + std::string(label) +
                " for SHA-256: " + pathUtf8(path));
    Sha256 digest;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            digest.update(buffer.data(),
                          static_cast<size_t>(count));
        }
    }
    require(input.eof(),
            "failed to read " + std::string(label) +
                " for SHA-256: " + pathUtf8(path));
    return digest.finish();
}

std::uint64_t elapsedNanoseconds(Clock::time_point begin,
                                 Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin)
            .count());
}

void addChecked(std::uint64_t& total, std::uint64_t value,
                std::string_view label) {
    require(value <= std::numeric_limits<std::uint64_t>::max() - total,
            std::string(label) + " overflowed uint64");
    total += value;
}

void addAllocation(AllocationActivity& total,
                   const AllocationActivity& sample) {
    addChecked(total.count, sample.count, "allocation request count");
    addChecked(total.requestedBytes, sample.requestedBytes,
               "allocation requested bytes");
}

void recordAllocation(AllocationMeasurements& measurements,
                      const AllocationActivity& sample) {
    addAllocation(measurements.total, sample);
    measurements.countSamples.push_back(sample.count);
    measurements.requestedByteSamples.push_back(
        sample.requestedBytes);
}

mparser::ParseResult parseSource(std::string_view source) {
    mparser::Lexer lexer(source, 0);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.root != nullptr,
            "Parser returned a null root during baseline");
    require(parsed.diagnostics.empty(),
            "Parser reported diagnostics during baseline");
    return parsed;
}

mparser::CompiledModule compileSource(const std::string& source) {
    auto module = mparser::CompiledModule::compile(source);
    require(module.valid(),
            "CompiledModule rejected the baseline workload");
    require(module.diagnostics().empty(),
            "CompiledModule reported diagnostics for the baseline workload");
    return module;
}

Measurement measureParse(const std::string& source,
                         const Options& options) {
    for (size_t index = 0; index < options.parseWarmup; ++index) {
        parseSource(source);
    }

    Measurement result;
    result.boundary =
        "Lexer construction through Parser::parse return; in-memory source, "
        "syntax destruction excluded";
    result.warmupIterations = options.parseWarmup;
    result.hostWallSamples.reserve(options.parseIterations);
    AllocationMeasurements allocations;
    for (size_t index = 0; index < options.parseIterations; ++index) {
        AllocationScope allocationScope;
        const auto begin = Clock::now();
        auto parsed = parseSource(source);
        const auto end = Clock::now();
        recordAllocation(allocations, allocationScope.stop());
        (void)parsed;
        result.hostWallSamples.push_back(
            elapsedNanoseconds(begin, end));
    }
    result.allocations = allocations;
    return result;
}

Measurement measureCompile(const std::string& source,
                           const Options& options) {
    for (size_t index = 0; index < options.compileWarmup; ++index) {
        (void)compileSource(source);
    }

    Measurement result;
    result.boundary =
        "collector compile wrapper entry through immutable "
        "CompiledModule return; includes source ownership copy into "
        "CompiledModule::compile, while module destruction and source "
        "file I/O are excluded";
    result.warmupIterations = options.compileWarmup;
    result.hostWallSamples.reserve(options.compileIterations);
    AllocationMeasurements allocations;
    for (size_t index = 0; index < options.compileIterations; ++index) {
        AllocationScope allocationScope;
        const auto begin = Clock::now();
        auto module = compileSource(source);
        const auto end = Clock::now();
        recordAllocation(allocations, allocationScope.stop());
        (void)module;
        result.hostWallSamples.push_back(
            elapsedNanoseconds(begin, end));
    }
    result.allocations = allocations;
    return result;
}

const mparser::RuntimeValue* findVariable(
    const mparser::ModuleInvocationResult& result,
    std::string_view name) {
    const auto found = std::find_if(
        result.variables.begin(), result.variables.end(),
        [name](const mparser::RuntimeVariable& variable) {
            return variable.name == name;
        });
    return found == result.variables.end() ? nullptr : &found->value;
}

double checkedResult(const mparser::ModuleInvocationResult& result,
                     std::string_view variable) {
    require(result.succeeded(),
            "runtime baseline invocation did not succeed");
    const auto* value = findVariable(result, variable);
    require(value != nullptr,
            "runtime baseline result variable is missing: " +
                std::string(variable));
    require(value->kind == mparser::RuntimeValueKind::Number,
            "runtime baseline result must be a scalar number");
    require(std::isfinite(value->number),
            "runtime baseline result must be finite");
    return value->number;
}

void mergeRuntimeSummary(
    RuntimeTotals& totals,
    const mparser::ModuleExecutionSummary& summary) {
    totals.effectiveTiers.insert(
        std::string(mparser::moduleExecutionTierName(
            summary.effectiveTier)));
    addChecked(totals.fallbackIterations,
               summary.fallbackOccurred ? 1u : 0u,
               "runtime fallback iterations");
    addChecked(totals.executedInstructionCount,
               summary.executedInstructionCount,
               "executed instruction count");
    addChecked(totals.typedRegionCount, summary.typedRegionCount,
               "typed region count");
    addChecked(totals.typedRegionAttemptCount,
               summary.typedRegionAttemptCount,
               "typed region attempt count");
    addChecked(totals.typedRegionExecutionCount,
               summary.typedRegionExecutionCount,
               "typed region execution count");
    addChecked(totals.typedRegionFallbackCount,
               summary.typedRegionFallbackCount,
               "typed region fallback count");
    addChecked(totals.nativeCompilationCount,
               summary.nativeCompilationCount,
               "native compilation count");
    addChecked(totals.nativeCacheHitCount,
               summary.nativeCacheHitCount,
               "native cache hit count");
    totals.maximumCallDepth =
        std::max<std::uint64_t>(
            totals.maximumCallDepth, summary.maximumCallDepth);
    totals.maximumArrayBytes =
        std::max<std::uint64_t>(
            totals.maximumArrayBytes, summary.maximumArrayBytes);
    totals.maximumDiagnosticCount =
        std::max<std::uint64_t>(
            totals.maximumDiagnosticCount,
            summary.maximumDiagnosticCount);
}

mparser::ModuleInvocationResult executeModule(
    const mparser::CompiledModule& module,
    mparser::ModuleExecutionBackend backend) {
    mparser::ModuleInvocationRequest request;
    request.backend = backend;
    request.collectProfile = false;
    return module.execute(request);
}

RuntimeMeasurement measureRuntime(
    const mparser::CompiledModule& module,
    mparser::ModuleExecutionBackend backend,
    std::string boundary, size_t warmupIterations,
    size_t measuredIterations, std::string_view resultVariable,
    std::optional<double> expectedResult) {
    for (size_t index = 0; index < warmupIterations; ++index) {
        const auto warmup = executeModule(module, backend);
        const double value = checkedResult(warmup, resultVariable);
        if (expectedResult) {
            require(value == *expectedResult,
                    "runtime warmup output differs from bytecode reference");
        }
    }

    RuntimeMeasurement result;
    result.measurement.boundary = std::move(boundary);
    result.measurement.warmupIterations = warmupIterations;
    result.measurement.hostWallSamples.reserve(measuredIterations);
    result.measurement.engineSamples.reserve(measuredIterations);
    AllocationMeasurements allocations;
    std::optional<double> firstResult;
    for (size_t index = 0; index < measuredIterations; ++index) {
        AllocationScope allocationScope;
        const auto begin = Clock::now();
        const auto invocation = executeModule(module, backend);
        const auto end = Clock::now();
        recordAllocation(allocations, allocationScope.stop());

        const double value = checkedResult(invocation, resultVariable);
        if (!firstResult) {
            firstResult = value;
        }
        require(value == *firstResult,
                "runtime output changed between measured iterations");
        if (expectedResult) {
            require(value == *expectedResult,
                    "runtime output differs from bytecode reference");
        }
        result.measurement.hostWallSamples.push_back(
            elapsedNanoseconds(begin, end));
        result.measurement.engineSamples.push_back(
            invocation.execution.elapsedNanoseconds);
        mergeRuntimeSummary(result.totals, invocation.execution);
    }
    result.measurement.allocations = allocations;
    result.observedResult = firstResult.value_or(0.0);
    return result;
}

RuntimeMeasurement unavailableRuntime(std::string reason,
                                      std::string boundary) {
    RuntimeMeasurement result;
    result.measurement.status = "unavailable";
    result.measurement.reason = std::move(reason);
    result.measurement.boundary = std::move(boundary);
    return result;
}

#if defined(_WIN32)

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    require(value.size() <=
                static_cast<size_t>(std::numeric_limits<int>::max()),
            "Windows child argument is too long");
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    require(required > 0,
            "Windows child argument is not valid UTF-8");
    std::wstring result(static_cast<size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required);
    require(converted == required,
            "failed to convert Windows child argument to UTF-16");
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
    if (!value.empty() &&
        value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

int runChildProcess(const std::vector<std::string>& arguments) {
    require(!arguments.empty(), "child process command is empty");
    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(L' ');
        }
        commandLine += quoteWindowsArgument(utf8ToWide(argument));
    }
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE nullHandle = CreateFileW(
        L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(nullHandle != INVALID_HANDLE_VALUE,
            "failed to open NUL for child output");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullHandle;
    startup.hStdOutput = nullHandle;
    startup.hStdError = nullHandle;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(nullHandle);
    require(created != FALSE,
            "failed to create mparser child process");

    const DWORD waitResult =
        WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = std::numeric_limits<DWORD>::max();
    const BOOL readExit =
        GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    require(waitResult == WAIT_OBJECT_0 && readExit != FALSE,
            "failed while waiting for mparser child process");
    return static_cast<int>(exitCode);
}

#else

int runChildProcess(const std::vector<std::string>& arguments) {
    require(!arguments.empty(), "child process command is empty");
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed for mparser child process");
    }
    if (child == 0) {
        const int nullFd = open("/dev/null", O_RDWR);
        if (nullFd >= 0) {
            (void)dup2(nullFd, STDIN_FILENO);
            (void)dup2(nullFd, STDOUT_FILENO);
            (void)dup2(nullFd, STDERR_FILENO);
            if (nullFd > STDERR_FILENO) {
                close(nullFd);
            }
        }
        std::vector<char*> childArguments;
        childArguments.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            childArguments.push_back(
                const_cast<char*>(argument.c_str()));
        }
        childArguments.push_back(nullptr);
        execvp(childArguments.front(), childArguments.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error(
                "waitpid failed for mparser child process");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128;
}

#endif

Measurement measureProcessColdStart(const Options& options) {
    std::vector<std::string> command = options.childPrefix;
    command.push_back(options.cliPath);
    command.push_back("--run");
    command.push_back("--jit=off");
    command.push_back(options.sourcePath);

    Measurement result;
    result.boundary =
        "fresh child process launch request through child exit; includes "
        "CLI startup, source file I/O, compile, one --run --jit=off "
        "execution, and shutdown; child output discarded";
    result.hostWallSamples.reserve(options.processIterations);
    for (size_t index = 0; index < options.processIterations; ++index) {
        const auto begin = Clock::now();
        const int exitCode = runChildProcess(command);
        const auto end = Clock::now();
        require(exitCode == 0,
                "mparser cold-start child exited with code " +
                    std::to_string(exitCode));
        result.hostWallSamples.push_back(
            elapsedNanoseconds(begin, end));
    }
    return result;
}

#if defined(_WIN32) || defined(__APPLE__)
std::string trim(std::string value) {
    const auto notSpace = [](unsigned char character) {
        return character != ' ' && character != '\t' &&
               character != '\r' && character != '\n';
    };
    const auto begin = std::find_if(
        value.begin(), value.end(), notSpace);
    const auto end = std::find_if(
        value.rbegin(), value.rend(), notSpace)
                         .base();
    return begin < end ? std::string(begin, end) : std::string{};
}
#endif

std::string architectureName() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

std::string cpuModel() {
#if defined(_WIN32)
    std::vector<char> value(32768);
    const DWORD length = GetEnvironmentVariableA(
        "PROCESSOR_IDENTIFIER", value.data(),
        static_cast<DWORD>(value.size()));
    if (length > 0 && length < value.size()) {
        return trim(std::string(value.data(), length));
    }
    return "unknown";
#elif defined(__APPLE__)
    size_t size = 0;
    if (sysctlbyname("machdep.cpu.brand_string", nullptr, &size,
                     nullptr, 0) == 0 &&
        size > 1) {
        std::string value(size, '\0');
        if (sysctlbyname("machdep.cpu.brand_string", value.data(),
                         &size, nullptr, 0) == 0) {
            value.resize(size > 0 ? size - 1 : 0);
            return trim(std::move(value));
        }
    }
    return "unknown";
#else
    std::ifstream input("/proc/cpuinfo");
    return mparser::performance::parseLinuxCpuModel(input);
#endif
}

std::uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullTotalPhys;
    }
    return 0;
#elif defined(__APPLE__)
    std::uint64_t value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname("hw.memsize", &value, &size, nullptr, 0) == 0) {
        return value;
    }
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0) {
        return 0;
    }
    const auto pageCount = static_cast<std::uint64_t>(pages);
    const auto bytesPerPage = static_cast<std::uint64_t>(pageSize);
    if (pageCount >
        std::numeric_limits<std::uint64_t>::max() / bytesPerPage) {
        return 0;
    }
    return pageCount * bytesPerPage;
#endif
}

std::uint64_t peakResidentBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<std::uint64_t>(
            counters.PeakWorkingSetSize);
    }
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
}

std::string utcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::filesystem::path executablePath(std::string_view fallback) {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(
            std::wstring(buffer.data(), length));
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) == -1 && size > 0) {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            std::error_code error;
            const auto resolved =
                std::filesystem::weakly_canonical(buffer.data(), error);
            if (!error) {
                return resolved;
            }
            return std::filesystem::path(buffer.data());
        }
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const ssize_t length =
        readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length > 0 &&
        static_cast<size_t>(length) < buffer.size()) {
        return std::filesystem::path(
            std::string(buffer.data(), static_cast<size_t>(length)));
    }
#endif
    return std::filesystem::path(fallback);
}

std::uint64_t fileSize(const std::filesystem::path& path,
                       std::string_view label) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && size > 0,
            "failed to read " + std::string(label) +
                " binary size: " + pathUtf8(path));
    return static_cast<std::uint64_t>(size);
}

Json binaryArtifactJson(const std::filesystem::path& path,
                        std::string_view reportedPath,
                        std::string_view label) {
    return Json{
        {"path", reportedPath},
        {"size_bytes", fileSize(path, label)},
        {"sha256", sha256File(path, label)}};
}

Json timingJson(const std::vector<std::uint64_t>& samples) {
    require(!samples.empty(), "timing sample set is empty");
    std::vector<std::uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t total = 0;
    for (const auto sample : samples) {
        addChecked(total, sample, "timing sample total");
    }
    const size_t count = sorted.size();
    const double mean =
        static_cast<double>(total) / static_cast<double>(count);
    const double median =
        count % 2 == 0
            ? (static_cast<double>(sorted[count / 2 - 1]) +
               static_cast<double>(sorted[count / 2])) /
                  2.0
            : static_cast<double>(sorted[count / 2]);
    const size_t p95Index =
        std::min(count - 1, (count * 95 + 99) / 100 - 1);

    return Json{
        {"unit", "nanoseconds"},
        {"samples_ns", samples},
        {"total_ns", total},
        {"mean_ns", mean},
        {"median_ns", median},
        {"minimum_ns", sorted.front()},
        {"p95_ns", sorted[p95Index]},
        {"maximum_ns", sorted.back()}};
}

Json allocationJson(const AllocationMeasurements& measurements) {
    return Json{{"aggregation", "sum-across-measured-iterations"},
                {"successful_request_count",
                 measurements.total.count},
                {"requested_bytes",
                 measurements.total.requestedBytes},
                {"successful_request_samples",
                 measurements.countSamples},
                {"requested_byte_samples",
                 measurements.requestedByteSamples},
                {"boundary",
                 "successful global operator new/new[] requests in the "
                 "baseline process for each measured call; totals sum "
                 "all measured iterations, while deallocation and "
                 "child-process allocations are excluded"}};
}

Json runtimeTotalsJson(const RuntimeTotals& totals) {
    return Json{
        {"effective_tiers",
         std::vector<std::string>(
             totals.effectiveTiers.begin(),
             totals.effectiveTiers.end())},
        {"fallback_iterations", totals.fallbackIterations},
        {"executed_instruction_count",
         totals.executedInstructionCount},
        {"typed_region_count", totals.typedRegionCount},
        {"typed_region_attempt_count",
         totals.typedRegionAttemptCount},
        {"typed_region_execution_count",
         totals.typedRegionExecutionCount},
        {"typed_region_fallback_count",
         totals.typedRegionFallbackCount},
        {"native_compilation_count",
         totals.nativeCompilationCount},
        {"native_cache_hit_count", totals.nativeCacheHitCount},
        {"maximum_call_depth", totals.maximumCallDepth},
        {"maximum_array_bytes", totals.maximumArrayBytes},
        {"maximum_diagnostic_count",
         totals.maximumDiagnosticCount}};
}

Json measurementJson(const Measurement& measurement,
                     const RuntimeTotals* runtimeTotals = nullptr) {
    Json result{
        {"status", measurement.status},
        {"reason", measurement.reason},
        {"boundary", measurement.boundary},
        {"warmup_iterations", measurement.warmupIterations},
        {"measured_iterations",
         measurement.hostWallSamples.size()}};
    result["host_wall"] =
        measurement.hostWallSamples.empty()
            ? Json(nullptr)
            : timingJson(measurement.hostWallSamples);
    result["engine_elapsed"] =
        measurement.engineSamples.empty()
            ? Json(nullptr)
            : timingJson(measurement.engineSamples);
    result["allocation_activity"] =
        measurement.allocations
            ? allocationJson(*measurement.allocations)
            : Json(nullptr);
    result["execution_summary"] =
        runtimeTotals ? runtimeTotalsJson(*runtimeTotals)
                      : Json(nullptr);
    return result;
}

Json runtimeMeasurementJson(const RuntimeMeasurement& measurement) {
    return measurementJson(
        measurement.measurement,
        measurement.measurement.status == "measured"
            ? &measurement.totals
            : nullptr);
}

Json cacheStatisticsJson(
    const mparser::NativeScalarJitCacheStatistics& statistics) {
    return Json{
        {"limits",
         {{"max_entries", statistics.limits.maxEntries},
          {"max_code_bytes", statistics.limits.maxCodeBytes}}},
        {"entry_count", statistics.entryCount},
        {"code_bytes", statistics.codeBytes},
        {"lookup_count", statistics.lookupCount},
        {"hit_count", statistics.hitCount},
        {"miss_count", statistics.missCount},
        {"compilation_count", statistics.compilationCount},
        {"compilation_failure_count",
         statistics.compilationFailureCount},
        {"insertion_count", statistics.insertionCount},
        {"duplicate_compilation_count",
         statistics.duplicateCompilationCount},
        {"bypass_count", statistics.bypassCount},
        {"eviction_count", statistics.evictionCount},
        {"evicted_code_bytes", statistics.evictedCodeBytes},
        {"clear_count", statistics.clearCount},
        {"cleared_entry_count", statistics.clearedEntryCount},
        {"cleared_code_bytes", statistics.clearedCodeBytes}};
}

size_t lineCount(std::string_view source) {
    if (source.empty()) {
        return 0;
    }
    return static_cast<size_t>(
               std::count(source.begin(), source.end(), '\n')) +
           (source.back() == '\n' ? 0 : 1);
}

Json buildReport(const Options& options) {
    const std::string source = readFile(options.sourcePath);
    const auto parse = measureParse(source, options);
    const auto compile = measureCompile(source, options);
    const auto module = compileSource(source);

    const auto bytecode = measureRuntime(
        module, mparser::ModuleExecutionBackend::Bytecode,
        "CompiledModule::execute(Bytecode) call entry through projected "
        "ModuleInvocationResult return; compile and result destruction "
        "excluded",
        options.runtimeWarmup, options.runtimeIterations,
        options.resultVariable, std::nullopt);
    const double referenceResult = bytecode.observedResult;

    const auto portable = measureRuntime(
        module, mparser::ModuleExecutionBackend::Portable,
        "CompiledModule::execute(Portable) call entry through projected "
        "ModuleInvocationResult return; static typed planning and compile "
        "excluded",
        options.runtimeWarmup, options.runtimeIterations,
        options.resultVariable, referenceResult);

    mparser::clearNativeScalarJitCache();
    mparser::resetNativeScalarJitCacheStatistics();
    const auto cacheBefore =
        mparser::nativeScalarJitCacheStatistics();
    const bool nativeAvailable = mparser::nativeScalarJitAvailable();
    RuntimeMeasurement nativeCold;
    RuntimeMeasurement nativeWarm;
    mparser::NativeScalarJitCacheStatistics cacheAfterCold;
    if (nativeAvailable) {
        nativeCold = measureRuntime(
            module, mparser::ModuleExecutionBackend::Native,
            "first CompiledModule::execute(Native) call after explicit "
            "native cache clear; includes native kernel compilation",
            0, 1, options.resultVariable, referenceResult);
        cacheAfterCold =
            mparser::nativeScalarJitCacheStatistics();
        nativeWarm = measureRuntime(
            module, mparser::ModuleExecutionBackend::Native,
            "CompiledModule::execute(Native) call entry through projected "
            "ModuleInvocationResult return after native cache population",
            options.runtimeWarmup, options.runtimeIterations,
            options.resultVariable, referenceResult);
    } else {
        nativeCold = unavailableRuntime(
            "native scalar JIT is not compiled or unavailable on this "
            "platform",
            "native code-cache cold invocation");
        nativeWarm = unavailableRuntime(
            "native scalar JIT is not compiled or unavailable on this "
            "platform",
            "native code-cache warm invocation");
        cacheAfterCold =
            mparser::nativeScalarJitCacheStatistics();
    }
    const auto cacheAfterWarm =
        mparser::nativeScalarJitCacheStatistics();
    const auto processCold = measureProcessColdStart(options);

    const auto toolPath = executablePath(options.collectorPath);
    const bool portableMatches =
        portable.observedResult == referenceResult;
    const bool nativeMatches =
        !nativeAvailable ||
        (nativeCold.observedResult == referenceResult &&
         nativeWarm.observedResult == referenceResult);
    Json report{
        {"protocol",
         {{"name", "mparser.performance-baseline"},
          {"major", 1},
          {"minor", 0}}},
        {"generated_at_utc", utcTimestamp()},
        {"revision", options.revision},
        {"environment",
         {{"os", MPARSER_BASELINE_SYSTEM_NAME},
          {"os_version", MPARSER_BASELINE_SYSTEM_VERSION},
          {"architecture", architectureName()},
          {"cpu_model", cpuModel()},
          {"logical_cpu_count",
           std::thread::hardware_concurrency()},
          {"physical_memory_bytes", physicalMemoryBytes()},
          {"emulated", !options.childPrefix.empty()}}},
        {"build",
         {{"project_version", MPARSER_BASELINE_PROJECT_VERSION},
          {"build_type", MPARSER_BASELINE_BUILD_TYPE},
          {"compiler_id", MPARSER_BASELINE_COMPILER_ID},
          {"compiler_version", MPARSER_BASELINE_COMPILER_VERSION},
          {"native_jit_available", nativeAvailable},
          {"native_jit_platform",
           std::string(mparser::nativeScalarJitPlatform())}}},
        {"workload",
         {{"id", options.workloadId},
          {"source_path", options.sourcePath},
          {"source_sha256", sha256(source)},
          {"source_bytes", source.size()},
          {"source_lines", lineCount(source)},
          {"result_variable", options.resultVariable}}},
        {"settings",
         {{"parse_warmup", options.parseWarmup},
          {"parse_iterations", options.parseIterations},
          {"compile_warmup", options.compileWarmup},
          {"compile_iterations", options.compileIterations},
          {"runtime_warmup", options.runtimeWarmup},
          {"runtime_iterations", options.runtimeIterations},
          {"process_iterations", options.processIterations}}},
        {"measurements",
         {{"parse", measurementJson(parse)},
          {"compile", measurementJson(compile)},
          {"process_cold_start", measurementJson(processCold)},
          {"bytecode", runtimeMeasurementJson(bytecode)},
          {"portable", runtimeMeasurementJson(portable)},
          {"native_cold", runtimeMeasurementJson(nativeCold)},
          {"native_warm", runtimeMeasurementJson(nativeWarm)}}},
        {"resources",
         {{"peak_resident_bytes", peakResidentBytes()},
          {"binary_artifacts",
           {{"baseline_tool",
             binaryArtifactJson(toolPath, options.collectorPath,
                                "baseline tool")},
            {"mparser_cli",
             binaryArtifactJson(options.cliPath, options.cliPath,
                                "mparser CLI")},
            {"mparser_c_api",
             binaryArtifactJson(options.libraryPath,
                                options.libraryPath,
                                "mparser C API")}}},
          {"native_cache",
           {{"before", cacheStatisticsJson(cacheBefore)},
            {"after_cold", cacheStatisticsJson(cacheAfterCold)},
            {"after_warm",
             cacheStatisticsJson(cacheAfterWarm)}}}}},
        {"correctness",
         {{"result_variable", options.resultVariable},
          {"reference_value", referenceResult},
          {"bytecode_matches", true},
          {"portable_matches", portableMatches},
          {"native_matches", nativeMatches},
          {"process_exit_success_count",
           processCold.hostWallSamples.size()},
          {"all_runtime_results_match",
           portableMatches && nativeMatches}}}};
    return report;
}

void writeReport(const Json& report, const std::string& outputPath) {
    if (outputPath.empty()) {
        std::cout << report.dump(2) << "\n";
        return;
    }
    const std::filesystem::path path(outputPath);
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "failed to open baseline output: " + outputPath);
    }
    output << report.dump(2) << "\n";
    if (!output) {
        throw std::runtime_error(
            "failed to write baseline output: " + outputPath);
    }
    std::cout << "MParser performance baseline written: "
              << outputPath << "\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        writeReport(buildReport(options), options.outputPath);
        return 0;
    } catch (const std::exception& error) {
        allocation_tracking::enabled.store(
            false, std::memory_order_release);
        std::cerr << "MParser performance baseline failed: "
                  << error.what() << "\n";
        return 1;
    }
}
