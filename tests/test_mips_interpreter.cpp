#include "test_support.h"

#include <cstdint>
#include <cstddef>
#include <vector>

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

class TestServices : public PlatformServices {
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

class CaptureServices final : public TestServices {
public:
    void write_gpu_register(std::uint32_t address,
                            std::uint32_t value) override {
        if (address == 0x1F801810u) gp0.push_back(value);
    }

    std::vector<std::uint32_t> gp0;
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

void irq_status_acknowledges_zero_bits() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    // Two pending sources: vblank (bit 0) and CD-ROM (bit 2).
    interpreter.hardware().raise_interrupt(1u << 0);
    interpreter.hardware().raise_interrupt(1u << 2);

    ram.write32(0x10000, 0x3C081F80u);   // lui t0, 0x1F80
    ram.write32(0x10004, 0x35081070u);   // ori t0, t0, 0x1070 (I_STAT)
    ram.write32(0x10008, 0x2409FFFEu);   // addiu t1, zero, -2  (~bit 0)
    ram.write32(0x1000Cu, 0xA5090000u);  // sh t1, 0(t0)

    interpreter.run(4);
    // PS1 I_STAT clears bits written as zero, so only vblank is acknowledged
    // and the CD source survives.
    R6_CHECK(interpreter.hardware().interrupt_status() == (1u << 2));
}

void dma_start_reads_back_idle() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C081F80u);  // lui t0, 0x1F80
    ram.write32(0x10004, 0x350810A8u);  // ori t0, t0, 0x10A8 (DMA2 CHCR)
    ram.write32(0x10008, 0x3C090100u);  // lui t1, 0x0100
    ram.write32(0x1000Cu, 0x35290010u);  // ori t1, t1, 0x0010
    ram.write32(0x10010, 0xAD090000u);   // sw t1, 0(t0)  start + trigger
    ram.write32(0x10014, 0x8D020000u);   // lw v0, 0(t0)

    interpreter.run(6);
    // The transfer is already finished, so busy (bit 24) and trigger (bit 28)
    // must read back clear or the guest would wait on them forever.
    R6_CHECK((cpu.gpr[2] & 0x11000000u) == 0u);
}

void timer_mode_reads_back() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C081F80u);  // lui t0, 0x1F80
    ram.write32(0x10004, 0x35081114u);  // ori t0, t0, 0x1114 (timer 1 mode)
    ram.write32(0x10008, 0x3C090040u);  // lui t1, 0x0040
    ram.write32(0x1000Cu, 0x35290002u);  // ori t1, t1, 0x0002
    ram.write32(0x10010, 0xAD090000u);   // sw t1, 0(t0)
    ram.write32(0x10014, 0x8D020000u);   // lw v0, 0(t0)

    interpreter.run(6);
    R6_CHECK(cpu.gpr[2] == 0x00400002u);
}

void dma2_linked_list_reaches_the_gpu() {
    Memory ram(2u * 1024u * 1024u);
    CaptureServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C081F80u);    // lui t0, 0x1F80
    ram.write32(0x10004, 0x350810A0u);    // ori t0, t0, 0x10A0 (DMA2 MADR)
    ram.write32(0x10008, 0x3C098002u);    // lui t1, 0x8002
    ram.write32(0x1000Cu, 0x35290000u);   // ori t1, t1, 0x0000 (list address)
    ram.write32(0x10010, 0xAD090000u);    // sw t1, 0(t0)
    ram.write32(0x10014, 0x25080008u);    // addiu t0, t0, 8 (DMA2 CHCR)
    ram.write32(0x10018, 0x3C090100u);    // lui t1, 0x0100
    ram.write32(0x1001Cu, 0x35290401u);   // ori t1: start, linked list, to GPU
    ram.write32(0x10020, 0xAD090000u);    // sw t1, 0(t0)

    // Two-word packet, terminated by the end marker.
    ram.write32(0x20000, 0x02FFFFFFu);
    ram.write32(0x20004, 0xAAAAAAAAu);
    ram.write32(0x20008, 0xBBBBBBBBu);

    interpreter.run(9);
    R6_CHECK(interpreter.dma_lists() == 1);
    R6_CHECK(interpreter.dma_words() == 2);
    R6_CHECK(services.gp0.size() == 2);
    if (services.gp0.size() == 2) {
        R6_CHECK(services.gp0[0] == 0xAAAAAAAAu);
        R6_CHECK(services.gp0[1] == 0xBBBBBBBBu);
    }
}

void cdrom_command_acknowledges_then_completes() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C081F80u);  // lui t0, 0x1F80
    ram.write32(0x10004, 0x35081800u);  // ori t0, t0, 0x1800 (CD index)
    ram.write32(0x10008, 0xA1000000u);  // sb zero, 0(t0) -> bank 0
    ram.write32(0x1000C, 0x2409000Au);  // addiu t1, zero, 0x0A (Init)
    ram.write32(0x10010, 0xA1090001u);  // sb t1, 1(t0) -> command
    ram.write32(0x10014, 0x24090001u);  // addiu t1, zero, 1
    ram.write32(0x10018, 0xA1090000u);  // sb t1, 0(t0) -> bank 1 (flags)
    ram.write32(0x1001C, 0x91020003u);  // lbu v0, 3(t0) -> interrupt flags
    ram.write32(0x10020, 0x24090007u);  // addiu t1, zero, 7 (acknowledge)
    ram.write32(0x10024, 0xA1090003u);  // sb t1, 3(t0)
    ram.write32(0x10028, 0x91030003u);  // lbu v1, 3(t0)
    ram.write32(0x1002Cu, 0x91040001u);  // lbu a0, 1(t0) -> response byte

    interpreter.run(12);
    R6_CHECK(cpu.gpr[2] == 3);  // command acknowledged
    R6_CHECK(cpu.gpr[3] == 2);  // completion reported after the ack
    R6_CHECK(cpu.gpr[4] == 2);  // the response starts with the drive status
}

// Raises bit 0 (vblank) with every source unmasked, so an interrupt is
// pending as soon as the interpreter polls.
void raise_pending_interrupt(MipsInterpreter& interpreter) {
    interpreter.hardware().write32(0x1F801074u, 0x7FFu);  // I_MASK
    interpreter.hardware().raise_interrupt(1u << 0);       // I_STAT vblank
}

void interrupt_takes_the_exception_vector() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x80, 0x401A7000u);  // mfc0 k0, EPC: marks the vector as code
    raise_pending_interrupt(interpreter);

    interpreter.run(1);
    // The interrupted PC is kept, not executed: EPC holds it for the return.
    R6_CHECK(interpreter.cop0(14) == 0x80010000u);
    R6_CHECK((interpreter.cop0(13) & 0x7Cu) == 0);  // ExcCode 0 (interrupt)
    R6_CHECK((interpreter.cop0(13) & 0x400u) != 0);  // CAUSE.IP2
    // SR pushes IEc into IEp and clears IEc, so the handler cannot be entered
    // again before RFE.
    R6_CHECK(interpreter.cop0(12) == 0x404u);
    R6_CHECK(interpreter.interrupts() == 1);
    R6_CHECK(cpu.pc == 0x80000084u);
}

void interrupt_handler_returns_through_rfe() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x80, 0x401A7000u);      // mfc0 k0, EPC
    ram.write32(0x84, 0x42000010u);      // rfe
    ram.write32(0x88, 0x03400008u);      // jr k0
    ram.write32(0x8C, 0x00000000u);      // delay slot
    ram.write32(0x10000, 0x24080001u);   // addiu t0, zero, 1
    ram.write32(0x10004, 0x0000000Du);   // break
    raise_pending_interrupt(interpreter);

    const auto result = interpreter.run(6);
    // Four handler instructions, then the interrupted addiu runs.
    R6_CHECK(cpu.gpr[8] == 1);
    R6_CHECK(cpu.pc == 0x80010004u);
    R6_CHECK(interpreter.cop0(12) == 0x401u);
    R6_CHECK(interpreter.cop0(14) == 0x80010000u);
    R6_CHECK(result.reason == StopReason::guest_break);
}

void interrupt_without_a_vector_stays_pending() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x24080001u);  // addiu t0, zero, 1
    ram.write32(0x10004, 0x24080002u);
    ram.write32(0x10008, 0x24080003u);
    raise_pending_interrupt(interpreter);

    interpreter.run(3);
    // Zeroed RAM at the vector is not a handler: nothing jumps there.
    R6_CHECK(interpreter.interrupts() == 0);
    R6_CHECK(interpreter.missing_vectors() == 1);
    R6_CHECK(cpu.gpr[8] == 3);
    R6_CHECK(cpu.pc == 0x8001000Cu);
}

void interrupt_overrun_returns_to_epc() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x80, 0x1000FFFFu);      // b . (handler that never returns)
    ram.write32(0x10000, 0x24080001u);   // addiu t0, zero, 1
    ram.write32(0x10004, 0x0000000Du);   // break
    raise_pending_interrupt(interpreter);

    const auto result = interpreter.run(60000);
    // The handler budget expired: the exception is unwound to EPC with the SR
    // that was pushed, and the interrupted stream continues.
    R6_CHECK(interpreter.interrupt_overruns() == 1);
    R6_CHECK(interpreter.cop0(12) == 0x401u);
    R6_CHECK(cpu.gpr[8] == 1);
    R6_CHECK(result.reason == StopReason::guest_break);
}

void exception_vector_writes_are_traced() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x3C088000u);  // lui t0, 0x8000
    ram.write32(0x10004, 0x35080090u);  // ori t0, t0, 0x0090
    ram.write32(0x10008, 0x3C09401Au);  // lui t1, 0x401A
    ram.write32(0x1000C, 0x35297000u);  // ori t1, t1, 0x7000
    ram.write32(0x10010, 0xAD090000u);  // sw t1, 0(t0)
    ram.write32(0x10014, 0x24080001u);  // addiu t0, zero, 1 (not the vector)

    interpreter.run(6);
    const auto& writes = interpreter.vector_writes();
    R6_CHECK(writes.size() == 1);
    if (writes.size() == 1) {
        R6_CHECK(writes[0].address == 0x80000090u);
        R6_CHECK(writes[0].value == 0x401A7000u);
        R6_CHECK(writes[0].pc == 0x80010010u);
        R6_CHECK(writes[0].width == 32);
    }
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
    irq_status_acknowledges_zero_bits();
    dma_start_reads_back_idle();
    timer_mode_reads_back();
    dma2_linked_list_reaches_the_gpu();
    cdrom_command_acknowledges_then_completes();
    interrupt_takes_the_exception_vector();
    interrupt_handler_returns_through_rfe();
    interrupt_without_a_vector_stays_pending();
    interrupt_overrun_returns_to_epc();
    exception_vector_writes_are_traced();
}
