#include "mparser/cpp_api.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

class TemporaryRoot {
public:
    TemporaryRoot() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path() /
               ("mparser-sdk-demo-" + std::to_string(stamp));
        std::filesystem::create_directories(path / "tmp");
    }

    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

} // namespace

int main() {
    using namespace mparser::sdk;

    TemporaryRoot root;
    SystemContextOptions systemOptions;
    systemOptions.capabilities =
        SystemCapability::CurrentDirectory |
        SystemCapability::FileSystemRead |
        SystemCapability::FileSystemWrite |
        SystemCapability::Random;
    systemOptions.rootDirectory = root.path.string();
    systemOptions.temporaryDirectory = "tmp";
    systemOptions.randomSeed = 7;
    systemOptions.maximumOpenFiles = 4;
    systemOptions.maximumFileReadBytes = 1024;

    auto systemContext = SystemContext::rootedNative(systemOptions);
    const auto module = Module::compile(R"MATLAB(
function total = run_demo()
    sample = rand();
    assert(sample >= 0 && sample < 1);
    filename = fullfile(tempdir(), 'values.bin');
    fid = fopen(filename, 'wb');
    assert(fid >= 3);
    assert(fwrite(fid, uint8([3 4 5]), 'uint8') == 3);
    assert(fclose(fid) == 0);
    fid = fopen(filename, 'rb');
    [values, count] = fread(fid, [1 3], 'uint8=>uint8');
    assert(fclose(fid) == 0);
    assert(count == 3);
    total = double(sum(values));
end
)MATLAB", "cpp_system_context_demo.m");
    if (!module.isValid()) {
        std::cerr << "could not compile system-context demo\n";
        return 1;
    }

    auto session = module.createSession(systemContext);
    systemContext = {};
    Invocation invocation;
    invocation.entryFunction = "run_demo";
    invocation.requestedOutputCount = 1;
    const auto result = session.execute(invocation);
    if (!result.succeeded()) {
        std::cerr << "system-context demo execution failed\n";
        return 1;
    }
    const auto output = result.output(0);
    const auto values = output.numericData<double>();
    if (values.size() != 1 || values.front() != 12.0) {
        std::cerr << "system-context demo returned an unexpected value\n";
        return 1;
    }

    std::cout << "system context sdk = rooted-session,12\n";
    return 0;
}
