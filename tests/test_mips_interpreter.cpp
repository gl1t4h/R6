#include "test_support.h"

#include <cstdint>
#include <cstddef>

#include "r6/bios_services.h"
#include "r6/mips_interpreter.h"

namespace {

using r6::BiosServices;
using r6::BiosTable;
using r6::CpuState;
using r6::Memory;
using r6::MipsInterpreter;
using r6::PlatformServices;
using r6::StopReason;

class TestServices final : public PlatformServices {
public:
    std::uint16_t read_controller(std::size_t) override { return 0xFFFF; }
    void submit_audio_samples(const std::int16_t*, std::size_t) override {}
    void present_frame(const std::uint32_t*, std::size_t) override {}
    std::uint32_t read_timer(std::size_t) override { return 0; }
    void write_gpu_register(std::uint32_t, std::uint32_t) override {}
    std::uint32_t read_gpu_status() override { return 0; }
    std::uint32_t read_gpu_data() override { return 0; }
    bool read_disc_sector(std::uint32_t, std::uint8_t*, std::size_t) override {
        return false;
    }
    bool bios_call(std::uint32_t pc, CpuState& cpu, Memory& memory) override {
        BiosTable table{};
        if (!BiosServices::trap_table(pc, table)) return false;
        bios.dispatch(table, cpu, memory);
        return true;
    }

    BiosServices bios;
};

std::uint32_t beq(std::uint32_t rs, std::uint32_t rt, std::int32_t offset) {
    return (0x04u << 26) | (rs << 21) | (rt << 16) |
           (static_cast<std::uint32_t>(offset) & 0xFFFFu);
}

std::uint32_t jal(std::uint32_t target) {
    return 0x0C000000u | ((target & 0x0FFFFFFFu) >> 2);
}

void arithmetic_and_delay_slot() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x24080005u);        // addiu t0, zero, 5
    ram.write32(0x10004, beq(8, 0, 2));       // not taken
    ram.write32(0x10008, 0x24090007u);        // delay slot: t1 = 7
    ram.write32(0x1000C, 0x240A0009u);        // t2 = 9

    const auto result = interpreter.run(4);
    R6_CHECK(result.reason == StopReason::step_budget);
    R6_CHECK(cpu.gpr[8] == 5);
    R6_CHECK(cpu.gpr[9] == 7);
    R6_CHECK(cpu.gpr[10] == 9);
}

void taken_branch_skips_following_instruction() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, beq(0, 0, 2));   // taken, target 0x1000C
    ram.write32(0x10004, 0x24090007u);    // delay slot: t1 = 7
    ram.write32(0x10008, 0x240A0009u);    // skipped
    ram.write32(0x1000C, 0x240B000Bu);    // t3 = 11

    interpreter.run(3);
    R6_CHECK(cpu.gpr[9] == 7);
    R6_CHECK(cpu.gpr[10] == 0);
    R6_CHECK(cpu.gpr[11] == 11);
}

void call_and_return() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, jal(0x80010010u));
    ram.write32(0x10004, 0x00000000u);
    ram.write32(0x10010, 0x24020001u);  // v0 = 1
    ram.write32(0x10014, 0x03E00008u);  // jr ra
    ram.write32(0x10018, 0x00000000u);

    interpreter.run(5);
    R6_CHECK(cpu.gpr[2] == 1);
    R6_CHECK(cpu.gpr[31] == 0x80010008u);
    R6_CHECK(cpu.pc == 0x80010008u);
}

void load_and_store() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C088002u);  // lui t0, 0x8002
    ram.write32(0x10004, 0x3C091122u);  // lui t1, 0x1122
    ram.write32(0x10008, 0x35293344u);  // ori t1, t1, 0x3344
    ram.write32(0x1000C, 0xAD090000u);  // sw t1, 0(t0)
    ram.write32(0x10010, 0x8D0A0000u);  // lw t2, 0(t0)

    interpreter.run(5);
    R6_CHECK(ram.read32(0x20000) == 0x11223344u);
    R6_CHECK(cpu.gpr[10] == 0x11223344u);
}

void unaligned_load_word_left() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x20000, 0x11223344u);
    ram.write32(0x10000, 0x3C088002u);  // lui t0, 0x8002
    ram.write32(0x10004, 0x89090002u);  // lwl t1, 2(t0)

    interpreter.run(2);
    R6_CHECK(cpu.gpr[9] == 0x22334400u);
}

void bios_trap_prints() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x2409003Cu);  // addiu t1, zero, 0x3C (putchar)
    ram.write32(0x10004, 0x24040048u);  // addiu a0, zero, 'H'
    ram.write32(0x10008, jal(0x800000A0u));
    ram.write32(0x1000C, 0x00000000u);

    interpreter.run(5);
    R6_CHECK(services.bios.tty_output() == "H");
    R6_CHECK(cpu.gpr[2] == static_cast<std::uint32_t>('H'));
    R6_CHECK(cpu.pc == 0x80010010u);
}

void unsupported_instruction_stops() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    interpreter.cpu().pc = 0x80010000u;

    ram.write32(0x10000, 0xFFFFFFFFu);
    const auto result = interpreter.run(4);
    R6_CHECK(result.reason == StopReason::unsupported_instruction);
    R6_CHECK(result.steps == 1);
}

void break_stops_execution() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    interpreter.cpu().pc = 0x80010000u;

    ram.write32(0x10000, 0x24080001u);  // addiu t0, zero, 1
    ram.write32(0x10004, 0x0000000Du);  // break
    ram.write32(0x10008, 0x24080002u);  // addiu t0, zero, 2 (not executed)

    const auto result = interpreter.run(8);
    R6_CHECK(result.reason == StopReason::guest_break);
    R6_CHECK(result.pc == 0x80010004u);
    R6_CHECK(interpreter.cpu().gpr[8] == 1);
}

}  // namespace

void run_mips_interpreter_tests() {
    arithmetic_and_delay_slot();
    taken_branch_skips_following_instruction();
    call_and_return();
    load_and_store();
    unaligned_load_word_left();
    bios_trap_prints();
    unsupported_instruction_stops();
    break_stops_execution();
}
