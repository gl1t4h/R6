#include "test_support.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "r6/bios_services.h"
#include "r6/cdrom_device.h"
#include "r6/function_dispatch.h"
#include "r6/gte.h"
#include "r6/hardware_map.h"
#include "r6/native_runtime.h"
#include "r6/ram_snapshot.h"

namespace {

using r6::CpuState;
using r6::FunctionDispatcher;
using r6::Memory;
using r6::NullPlatformServices;

std::uint32_t add_block(
    CpuState& cpu,
    Memory& memory,
    r6::PlatformServices& services) {
    (void)memory;
    (void)services;
    cpu.gpr[2] = cpu.gpr[4] + cpu.gpr[5];
    cpu.pc = 0x00001008u;
    return cpu.pc;
}

std::uint32_t tail_block(
    CpuState& cpu,
    Memory& memory,
    r6::PlatformServices& services) {
    (void)memory;
    (void)services;
    cpu.gpr[3] = 1;
    cpu.pc = 0x00002000u;
    return cpu.pc;
}

std::uint32_t persistent_block(CpuState& cpu, r6::NativeBus& bus) {
    bus.advance_cycles(1);
    cpu.gpr[2] = static_cast<std::uint32_t>(bus.hardware().cycles());
    cpu.pc = 0x00003000u;
    return cpu.pc;
}

void run_dispatcher_test() {
    Memory memory(64 * 1024, true);
    NullPlatformServices services;
    FunctionDispatcher dispatcher;
    dispatcher.register_function(0x1000u, &add_block);
    dispatcher.register_function(0x1008u, &tail_block);

    CpuState cpu{};
    cpu.pc = 0x1000u;
    cpu.gpr[4] = 5;
    cpu.gpr[5] = 7;

    R6_CHECK(dispatcher.contains(0x1000u));
    R6_CHECK(!dispatcher.contains(0x1004u));

    cpu.pc = dispatcher.step(cpu, memory, services);
    R6_CHECK(cpu.pc == 0x1008u);
    R6_CHECK(cpu.gpr[2] == 12u);

    cpu.pc = dispatcher.step(cpu, memory, services);
    R6_CHECK(cpu.pc == 0x2000u);
    R6_CHECK(cpu.gpr[3] == 1u);
}

void run_persistent_bus_test() {
    Memory memory(64 * 1024, true);
    NullPlatformServices services;
    r6::NativeBus bus(memory, services);
    r6::NativeBlockDispatcher dispatcher;
    dispatcher.register_function(0x3000u, &persistent_block);

    CpuState cpu{};
    cpu.pc = 0x3000u;
    cpu.pc = dispatcher.step(cpu, bus);
    R6_CHECK(cpu.gpr[2] == 1u);
    cpu.pc = dispatcher.step(cpu, bus);
    R6_CHECK(cpu.gpr[2] == 2u);
    R6_CHECK(bus.hardware().cycles() == 2u);
}

void run_unsupported_test() {
    CpuState cpu{};
    bool threw = false;
    try {
        r6::unsupported_instruction(cpu, 0x00001000u, 0xDEADBEEFu);
    } catch (const std::runtime_error& error) {
        threw = true;
        const std::string message = error.what();
        R6_CHECK(message.find("0x00001000") != std::string::npos);
        R6_CHECK(message.find("0xDEADBEEF") != std::string::npos);
    }
    R6_CHECK(threw);
}

void run_memory_snapshot_test() {
    Memory memory(4096, true);
    memory.write32(0x100u, 0x12345678u);
    const auto saved = memory.snapshot();
    memory.write32(0x100u, 0u);
    R6_CHECK(memory.read32(0x100u) == 0u);
    memory.restore(saved);
    R6_CHECK(memory.read32(0x100u) == 0x12345678u);

    Memory same(4096, true);
    same.restore(saved);
    R6_CHECK(memory.same_data(same));
    same.write32(0x100u, 1u);
    R6_CHECK(!memory.same_data(same));
}

void run_device_state_test() {
    NullPlatformServices services;

    r6::HardwareMap hardware_a(services);
    r6::HardwareMap hardware_b(services);
    R6_CHECK(hardware_a.same_state(hardware_b));
    hardware_a.advance_cycles(1);
    R6_CHECK(hardware_a.state_difference(hardware_b) == "cycles");
    hardware_b.advance_cycles(1);
    R6_CHECK(hardware_a.same_state(hardware_b));

    hardware_a.complete_dma(4);
    R6_CHECK(hardware_a.state_difference(hardware_b) == "dma interrupt");
    hardware_b.complete_dma(4);
    R6_CHECK(hardware_a.same_state(hardware_b));

    r6::CdromDevice cd_a(services);
    r6::CdromDevice cd_b(services);
    R6_CHECK(cd_a.same_state(cd_b));
    cd_a.write(0, 1);
    R6_CHECK(!cd_a.same_state(cd_b));
    cd_b.write(0, 1);
    R6_CHECK(cd_a.same_state(cd_b));

    r6::Gte gte_a;
    r6::Gte gte_b;
    R6_CHECK(gte_a.same_state(gte_b));
    gte_a.write_data(r6::Gte::kVxy0, 0x12345678u);
    R6_CHECK(!gte_a.same_state(gte_b));
    gte_b.write_data(r6::Gte::kVxy0, 0x12345678u);
    R6_CHECK(gte_a.same_state(gte_b));

    Memory memory(2 * 1024 * 1024, true);
    r6::BiosServices bios_a;
    r6::BiosServices bios_b;
    CpuState cpu_a{};
    CpuState cpu_b{};
    cpu_a.gpr[9] = 0x3Du;
    cpu_a.gpr[4] = 'A';
    cpu_b.gpr[9] = 0x3Du;
    cpu_b.gpr[4] = 'A';
    bios_a.dispatch(r6::BiosTable::b0, cpu_a, memory);
    R6_CHECK(!bios_a.same_state(bios_b));
    bios_b.dispatch(r6::BiosTable::b0, cpu_b, memory);
    R6_CHECK(bios_a.same_state(bios_b));
}

void run_scratchpad_and_dma_test() {
    Memory memory(2 * 1024 * 1024, true);
    NullPlatformServices services;

    r6::NativeBus bus(memory, services);
    bus.write32(0x1F800000u, 0x12345678u);
    R6_CHECK(bus.scratchpad()[0] == 0x78u);
    R6_CHECK(bus.scratchpad()[1] == 0x56u);
    R6_CHECK(bus.scratchpad()[2] == 0x34u);
    R6_CHECK(bus.scratchpad()[3] == 0x12u);

    memory.write32(0x000B4394u, 1u);
    bus.write32(r6::hardware::kDma3BaseAddress, 0x00100000u);
    bus.write32(r6::hardware::kDma3BaseAddress + 4u, 1u);
    bus.write32(r6::hardware::kDma3ChannelControl, 1u << 24);
    R6_CHECK((bus.hardware().read32(r6::hardware::kDmaInterrupt) &
              (1u << 27)) != 0u);
    R6_CHECK((bus.hardware().interrupt_status() &
              r6::hardware::kDmaInterruptBit) != 0u);

    r6::NativeBus dma4_bus(memory, services);
    dma4_bus.write32(r6::hardware::kDma4ChannelControl, (1u << 24) | 1u);
    R6_CHECK((dma4_bus.hardware().read32(r6::hardware::kDmaInterrupt) &
              (1u << 28)) != 0u);
}

void run_ram_snapshot_round_trip() {
    Memory memory(4096, true);
    memory.write32(0x100u, 0x12345678u);
    CpuState cpu{};
    cpu.gpr[1] = 5u;
    cpu.gpr[29] = 0x80000800u;
    cpu.hi = 7u;
    cpu.lo = 9u;
    cpu.pc = 0x80000100u;

    const auto path = std::filesystem::temp_directory_path() /
                      "r6_ram_snapshot_test.bin";
    R6_CHECK(r6::write_snapshot(path, cpu, memory));

    Memory restored(4096, true);
    CpuState restored_cpu{};
    R6_CHECK(r6::read_snapshot(path, restored_cpu, restored));
    R6_CHECK(restored_cpu.gpr[1] == 5u);
    R6_CHECK(restored_cpu.gpr[29] == 0x80000800u);
    R6_CHECK(restored_cpu.hi == 7u);
    R6_CHECK(restored_cpu.lo == 9u);
    R6_CHECK(restored_cpu.pc == 0x80000100u);
    R6_CHECK(restored.read32(0x100u) == 0x12345678u);

    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

void run_native_runtime_tests() {
    run_dispatcher_test();
    run_persistent_bus_test();
    run_unsupported_test();
    run_memory_snapshot_test();
    run_device_state_test();
    run_scratchpad_and_dma_test();
    run_ram_snapshot_round_trip();
}
