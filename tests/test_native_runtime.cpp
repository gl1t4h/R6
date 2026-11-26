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
#include "r6/mips_interpreter.h"
#include "r6/native_runtime.h"
#include "r6/ram_snapshot.h"

namespace {

using r6::CpuState;
using r6::FunctionDispatcher;
using r6::Memory;
using r6::NullPlatformServices;

bool has_event(const r6::EventTrace& trace, const std::string& name);
void seed_pending_interrupt(r6::HardwareMap& hardware,
                            std::uint32_t bit);

class EventTestServices final : public r6::PlatformServices {
public:
    std::uint16_t read_controller(std::size_t) override { return 0xFFFF; }
    void submit_audio_samples(const std::int16_t*, std::size_t) override {}
    void present_frame(const std::uint32_t*, std::size_t) override {}
    std::uint32_t read_timer(std::size_t) override { return 0; }
    void write_gpu_register(std::uint32_t, std::uint32_t) override {}
    std::uint32_t read_gpu_status() override { return 0; }
    std::uint32_t read_gpu_data() override { return 0; }
    bool read_disc_sector(std::uint32_t, std::uint8_t*,
                          std::size_t) override {
        return false;
    }
    bool bios_call(std::uint32_t pc, CpuState& cpu, Memory& memory) override {
        r6::BiosTable table{};
        if (!r6::BiosServices::trap_table(pc, table)) return false;
        bios.dispatch(table, cpu, memory);
        return true;
    }
    std::uint32_t entry_int() override { return bios.entry_int(); }
    bool take_return_from_exception() override {
        return bios.take_return_from_exception();
    }

    r6::BiosServices bios;
};

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
    // The transfer holds the channel busy for its duration; the completion
    // flag appears once that time has passed.
    bus.hardware().advance_cycles(0x1000u);
    R6_CHECK((bus.hardware().read32(r6::hardware::kDmaInterrupt) &
              (1u << 27)) != 0u);
    R6_CHECK((bus.hardware().interrupt_status() &
              r6::hardware::kDmaInterruptBit) != 0u);
    R6_CHECK(has_event(bus.events(), "dma3"));

    r6::NativeBus dma4_bus(memory, services);
    dma4_bus.write32(r6::hardware::kDma4ChannelControl, (1u << 24) | 1u);
    R6_CHECK((dma4_bus.hardware().read32(r6::hardware::kDmaInterrupt) &
              (1u << 28)) != 0u);
    R6_CHECK(has_event(dma4_bus.events(), "dma4"));
}

void run_vblank_and_pad_event_trace_test() {
    Memory native_ram(2u * 1024u * 1024u, true);
    Memory oracle_ram(2u * 1024u * 1024u, true);
    EventTestServices native_services;
    EventTestServices oracle_services;
    r6::NativeBus native(native_ram, native_services);
    r6::MipsInterpreter oracle(oracle_ram, oracle_services);
    CpuState native_cpu{};
    native_cpu.pc = 0x80010000u;
    native_cpu.cop0[12] = 0x00000401u;
    oracle.cpu() = native_cpu;
    oracle.set_cop0(native_cpu.cop0);
    native_ram.write32(0x10000u, 0u);
    oracle_ram.write32(0x10000u, 0u);
    native.set_vblank_enabled(true);
    oracle.set_vblank_enabled(true);

    // Initialize both vblank schedules at cycle zero. The oracle executes the
    // nop, so its cycle counter advances one ahead of the event-only native
    // tick; align the counters before the field boundary.
    native.event_tick(native_cpu);
    oracle.run_event_step();
    native.advance_cycles(564480u);
    oracle.hardware().advance_cycles(564479u);
    native.event_tick(native_cpu);
    oracle.run_event_step();
    R6_CHECK((native.hardware().interrupt_status() & 1u) != 0u);
    R6_CHECK((oracle.hardware().interrupt_status() & 1u) != 0u);
    R6_CHECK(has_event(native.events(), "vblank"));
    R6_CHECK(has_event(oracle.events(), "vblank"));
    R6_CHECK(native.events().same_as(oracle.events()));

    // A pad transfer uses the same pending-interrupt path. Install a vector
    // and unmask only the pad bit, then let the fourth tick raise and deliver.
    native_ram.write32(0x80u, 0x401A7000u);
    oracle_ram.write32(0x80u, 0x401A7000u);
    native.hardware().write32(0x1F801074u, 0x80u);
    oracle.hardware().write32(0x1F801074u, 0x80u);
    native.hardware().pad_byte_received();
    oracle.hardware().pad_byte_received();
    for (int index = 0; index < 4; ++index) {
        // The event-only native tick does not execute the oracle's nop, so keep
        // the two PCs aligned for the exception's EPC.
        native_cpu.pc = oracle.cpu().pc;
        native.event_tick(native_cpu);
        oracle.run_event_step();
    }
    R6_CHECK(has_event(native.events(), "pad"));
    R6_CHECK(has_event(oracle.events(), "pad"));
    R6_CHECK(native_cpu.pc == 0x80000080u);
    R6_CHECK(oracle.cpu().pc == 0x80000080u);
    R6_CHECK(native.events().same_as(oracle.events()));
}

void run_rfe_event_trace_test() {
    Memory native_ram(2u * 1024u * 1024u, true);
    Memory oracle_ram(2u * 1024u * 1024u, true);
    EventTestServices native_services;
    EventTestServices oracle_services;
    r6::NativeBus native(native_ram, native_services);
    r6::MipsInterpreter oracle(oracle_ram, oracle_services);
    CpuState native_cpu{};
    native_cpu.pc = 0x80010000u;
    native_cpu.cop0[12] = 0x00000404u;
    oracle.cpu() = native_cpu;
    oracle.set_cop0(native_cpu.cop0);
    native_ram.write32(0x10000u, 0x42000010u);  // rfe
    oracle_ram.write32(0x10000u, 0x42000010u);

    native.rfe(native_cpu);
    native_cpu.pc += 4u;
    const auto result = oracle.run_event_step();
    R6_CHECK(result.reason == r6::StopReason::step_budget);
    R6_CHECK(native_cpu.pc == oracle.cpu().pc);
    R6_CHECK(native_cpu.cop0[12] == oracle.cop0(12));
    R6_CHECK(native.events().same_as(oracle.events()));
}

void run_cd_dma_interrupt_event_trace_test() {
    for (const auto bit : {1u << 2, 1u << 3}) {
        Memory native_ram(2u * 1024u * 1024u, true);
        Memory oracle_ram(2u * 1024u * 1024u, true);
        EventTestServices native_services;
        EventTestServices oracle_services;
        r6::NativeBus native(native_ram, native_services);
        r6::MipsInterpreter oracle(oracle_ram, oracle_services);
        CpuState native_cpu{};
        native_cpu.pc = 0x80010000u;
        native_cpu.cop0[12] = 0x00000401u;
        oracle.cpu() = native_cpu;
        oracle.set_cop0(native_cpu.cop0);
        native_ram.write32(0x80u, 0x401A7000u);
        oracle_ram.write32(0x80u, 0x401A7000u);
        seed_pending_interrupt(native.hardware(), bit);
        seed_pending_interrupt(oracle.hardware(), bit);
        R6_CHECK(native.event_tick(native_cpu));
        const auto result = oracle.run_event_step();
        R6_CHECK(result.reason == r6::StopReason::step_budget);
        R6_CHECK(native_cpu.pc == 0x80000080u);
        R6_CHECK(oracle.cpu().pc == 0x80000080u);
        R6_CHECK(native.events().same_as(oracle.events()));
    }
}

bool has_event(const r6::EventTrace& trace, const std::string& name) {
    for (const auto& event : trace.events()) {
        if (event == name) return true;
    }
    return false;
}

void seed_pending_interrupt(r6::HardwareMap& hardware,
                            std::uint32_t bit = 1u << 0) {
    hardware.write32(0x1F801074u, 0x7FFu);  // I_MASK
    hardware.raise_interrupt(bit);
}

void run_interrupt_event_trace_test() {
    Memory native_ram(2u * 1024u * 1024u, true);
    Memory oracle_ram(2u * 1024u * 1024u, true);
    EventTestServices native_services;
    EventTestServices oracle_services;
    r6::NativeBus native(native_ram, native_services);
    r6::MipsInterpreter oracle(oracle_ram, oracle_services);

    CpuState native_cpu{};
    native_cpu.pc = 0x80010000u;
    native_cpu.cop0[12] = 0x00000401u;
    native_cpu.gpr[4] = 0x1234u;
    auto& oracle_cpu = oracle.cpu();
    oracle_cpu = native_cpu;
    oracle.set_cop0(native_cpu.cop0);

    native_ram.write32(0x80u, 0x401A7000u);  // mfc0 k0, EPC
    oracle_ram.write32(0x80u, 0x401A7000u);
    seed_pending_interrupt(native.hardware());
    seed_pending_interrupt(oracle.hardware());

    R6_CHECK(native.event_tick(native_cpu));
    const auto result = oracle.run_event_step();
    R6_CHECK(result.reason == r6::StopReason::step_budget);
    R6_CHECK(native_cpu.pc == 0x80000080u);
    R6_CHECK(oracle_cpu.pc == 0x80000080u);
    R6_CHECK(native_cpu.cop0[14] == 0x80010000u);
    R6_CHECK(oracle.cop0(14) == 0x80010000u);
    R6_CHECK(native_cpu.cop0[12] == 0x00000404u);
    R6_CHECK(oracle.cop0(12) == 0x00000404u);
    R6_CHECK(native.events().same_as(oracle.events()));

    // B(17h) from the handler resumes at EPC and restores the saved context.
    native_cpu.gpr[9] = 0x17u;
    native_services.bios.dispatch(r6::BiosTable::b0, native_cpu, native_ram);
    R6_CHECK(native.take_bios_return(native_cpu));
    R6_CHECK(native_cpu.pc == 0x80010000u);
    R6_CHECK(native_cpu.cop0[12] == 0x00000401u);
    R6_CHECK(native_cpu.gpr[4] == 0x1234u);

    oracle_cpu.pc = 0xB0u;
    oracle_cpu.gpr[9] = 0x17u;
    const auto returned = oracle.run_event_step();
    R6_CHECK(returned.reason == r6::StopReason::step_budget);
    R6_CHECK(oracle_cpu.pc == 0x80010000u);
    R6_CHECK(oracle.cop0(12) == 0x00000401u);
    R6_CHECK(oracle_cpu.gpr[4] == 0x1234u);
    R6_CHECK(native.events().same_as(oracle.events()));
}

void run_hook_entry_int_event_trace_test() {
    Memory native_ram(2u * 1024u * 1024u, true);
    Memory oracle_ram(2u * 1024u * 1024u, true);
    EventTestServices native_services;
    EventTestServices oracle_services;
    r6::NativeBus native(native_ram, native_services);
    r6::MipsInterpreter oracle(oracle_ram, oracle_services);

    CpuState native_cpu{};
    native_cpu.pc = 0x80010000u;
    native_cpu.cop0[12] = 0x00000401u;
    auto& oracle_cpu = oracle.cpu();
    oracle_cpu = native_cpu;
    oracle.set_cop0(native_cpu.cop0);

    for (auto* ram : {&native_ram, &oracle_ram}) {
        ram->write32(0x20000u, 0x80002000u);  // EntryInt pc
        ram->write32(0x20004u, 0x80030000u);  // SP
        for (std::uint32_t index = 0; index < 10; ++index) {
            ram->write32(0x20008u + index * 4u, 0x80000000u + index);
        }
    }
    native_cpu.gpr[9] = 0x19u;
    native_cpu.gpr[4] = 0x20000u;
    oracle_cpu.gpr[9] = 0x19u;
    oracle_cpu.gpr[4] = 0x20000u;
    native_services.bios.dispatch(r6::BiosTable::b0, native_cpu, native_ram);
    oracle_services.bios.dispatch(r6::BiosTable::b0, oracle_cpu, oracle_ram);
    R6_CHECK(native_services.bios.entry_int() == 0x20000u);
    R6_CHECK(oracle_services.bios.entry_int() == 0x20000u);
    native_cpu.pc = 0x80010000u;
    oracle_cpu.pc = 0x80010000u;
    seed_pending_interrupt(native.hardware());
    seed_pending_interrupt(oracle.hardware());

    R6_CHECK(native.event_tick(native_cpu));
    const auto result = oracle.run_event_step();
    R6_CHECK(result.reason == r6::StopReason::step_budget);
    R6_CHECK(native_cpu.pc == 0x80002000u);
    R6_CHECK(oracle_cpu.pc == 0x80002000u);
    R6_CHECK(native_cpu.gpr[2] == 1u);
    R6_CHECK(oracle_cpu.gpr[2] == 1u);
    R6_CHECK(native_cpu.gpr[29] == 0x80030000u);
    R6_CHECK(oracle_cpu.gpr[29] == 0x80030000u);
    R6_CHECK(native.events().same_as(oracle.events()));
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
    run_interrupt_event_trace_test();
    run_hook_entry_int_event_trace_test();
    run_vblank_and_pad_event_trace_test();
    run_rfe_event_trace_test();
    run_cd_dma_interrupt_event_trace_test();
    run_ram_snapshot_round_trip();
}
