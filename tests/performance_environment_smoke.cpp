#include "performance_environment.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void expectModel(const std::string& cpuInfo,
                 const std::string& expected) {
    std::istringstream input(cpuInfo);
    assert(mparser::performance::parseLinuxCpuModel(input) == expected);
}

} // namespace

int main() {
    expectModel(
        "processor : 0\n"
        "model name : AMD EPYC 7763 64-Core Processor\n",
        "AMD EPYC 7763 64-Core Processor");
    expectModel(
        "processor : 0\n"
        "CPU implementer : 0x41\n"
        "CPU architecture: 8\n"
        "CPU variant : 0x1\n"
        "CPU part : 0xd49\n"
        "CPU revision : 0\n"
        "processor : 1\n"
        "CPU implementer : 0x41\n"
        "CPU part : 0xd49\n",
        "ARM (implementer=0x41, architecture=8, variant=0x1, "
        "part=0xd49, revision=0)");
    expectModel(
        "Model\t: Raspberry Pi 5 Model B Rev 1.0  \n",
        "Raspberry Pi 5 Model B Rev 1.0");
    expectModel(
        "Processor : AArch64 Processor rev 1\n"
        "CPU implementer : 0x41\n",
        "AArch64 Processor rev 1");
    expectModel("processor : 0\nBogoMIPS : 100.00\n", "unknown");

    std::cout << "MParser performance environment validated: Linux x86 "
                 "model, ARM board/MIDR identity, unknown fallback\n";
    return 0;
}
