#include <cstdint>
#include <iostream>

#include "r6/bios_services.h"
#include "r6/software_gpu.h"

int main() {
    r6::SoftwareGpu gpu;
    gpu.submit_gp0(0x020000FFu);  // fill rectangle, red
    gpu.submit_gp0(0x00000000u);  // at 0,0
    gpu.submit_gp0(0x00010010u);  // 16x1

    std::cout << "R6 native recompilation scaffold\n"
              << "No game assets are bundled with this build.\n"
              << "GP0 textured quad packet: " << r6::SoftwareGpu::gp0_word_count(0x3C)
              << " words\n"
              << "GPUSTAT: 0x" << std::hex << gpu.status() << std::dec << "\n"
              << "VRAM halfword at (0,0): 0x" << std::hex << gpu.read_vram(0, 0)
              << std::dec << "\n";

    r6::BiosTable table{};
    const bool a0_trap = r6::BiosServices::trap_table(0x800000A0u, table);
    std::cout << "BIOS A0 trap recognised: " << (a0_trap ? "yes" : "no") << "\n";
    return 0;
}
