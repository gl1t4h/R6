#include "test_support.h"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "r6/function_dispatch.h"
#include "r6/native_runtime.h"

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
}

}  // namespace

void run_native_runtime_tests() {
    run_dispatcher_test();
    run_unsupported_test();
    run_memory_snapshot_test();
}
