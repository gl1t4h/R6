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
    bool take_return_from_exception() override {
        return bios.take_return_from_exception();
    }

    BiosServices bios;
};

class EntryIntServices final : public TestServices {
public:
    std::uint32_t entry_int() override { return kEntryInt; }

    static constexpr std::uint32_t kEntryInt = 0x80020000u;
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

void single_step_preserves_branch_target() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, beq(0, 0, 3));    // taken, target 0x80010010
    ram.write32(0x10004, 0x00000000u);     // delay slot
    ram.write32(0x10008, 0x24080099u);     // skipped fallthrough
    ram.write32(0x10010, 0x24080001u);     // t0 = 1

    auto result = interpreter.run(1);
    R6_CHECK(result.reason == StopReason::step_budget);
    R6_CHECK(cpu.pc == 0x80010004u);
    result = interpreter.run(1);
    R6_CHECK(result.reason == StopReason::step_budget);
    // The pending branch target must survive the run() boundary.
    R6_CHECK(cpu.pc == 0x80010010u);
    interpreter.run(1);
    R6_CHECK(cpu.gpr[8] == 1u);
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

// Hardware timer model (psx-spx; DuckStation implements the same table):
// timer 0 counts sysclk or dot clock, timer 1 sysclk or hblank, and timer 2
// sysclk or sysclk/8 (bit 1 of the selector).
void timer_clock_sources_match_the_hardware() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();

    hardware.write32(0x1F801110u, 0);        // timer 1 counter
    hardware.write32(0x1F801114u, 0u << 8);  // source 0: sysclk
    hardware.advance_cycles(2146 * 2);
    R6_CHECK(hardware.read32(0x1F801110u) == 2146 * 2);

    hardware.write32(0x1F801110u, 0);
    hardware.write32(0x1F801114u, 1u << 8);  // source 1: hblank
    hardware.advance_cycles(2146 * 2);
    R6_CHECK(hardware.read32(0x1F801110u) == 2);

    hardware.write32(0x1F801120u, 0);        // timer 2 counter
    hardware.write32(0x1F801124u, 1u << 8);  // source 1: sysclk
    hardware.advance_cycles(80);
    R6_CHECK(hardware.read32(0x1F801120u) == 80);

    hardware.write32(0x1F801120u, 0);
    hardware.write32(0x1F801124u, 2u << 8);  // source 2: sysclk/8
    hardware.advance_cycles(80);
    R6_CHECK(hardware.read32(0x1F801120u) == 10);

    // A mode write restarts the counter.
    hardware.advance_cycles(40);
    hardware.write32(0x1F801124u, 2u << 8);
    hardware.advance_cycles(8);
    R6_CHECK(hardware.read32(0x1F801120u) == 1);
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
    // This check is about the acknowledgement/completion handshake, not
    // about how long the drive takes to accept the command.
    interpreter.hardware().cdrom().set_command_delay(0);
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
    R6_CHECK(cpu.gpr[2] == 0x03);  // INT3 is the type value 3, not a bit
    R6_CHECK(cpu.gpr[3] == 0x02);  // completion reported after the ack
    R6_CHECK(cpu.gpr[4] == 2);     // response starts with the drive status
}

// The controller drives I_STAT bit 2 only while a pending flag is enabled in
// its own interrupt mask, and a bank-0 +3 request-register write must not
// disturb that mask: libcd sets BFRD there just before it drains a sector.
void cdrom_interrupt_line_honours_the_enable_mask() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();
    hardware.cdrom().set_command_delay(0);

    hardware.write_cdrom(0x1F801800u, 0);     // bank 0
    hardware.write_cdrom(0x1F801801u, 0x01);  // GetStat: INT3 pending
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);

    hardware.write_cdrom(0x1F801800u, 1);     // bank 1
    hardware.write_cdrom(0x1F801802u, 0x00);  // mask every CD interrupt
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);

    hardware.write_cdrom(0x1F801800u, 1);     // bank 1: interrupt enable
    hardware.write_cdrom(0x1F801802u, 0x07);  // enable the response type
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);

    // Banks 2/3 +2/+3 are CD audio volumes. A driver that sets them right
    // before sending a command must not lose the interrupt it then waits on.
    hardware.write_cdrom(0x1F801800u, 3);
    hardware.write_cdrom(0x1F801802u, 0x00);  // right-to-right volume
    hardware.write_cdrom(0x1F801803u, 0x20);  // right-to-left volume
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);

    hardware.write_cdrom(0x1F801800u, 0);     // bank 0 +3: request register
    hardware.write_cdrom(0x1F801803u, 0x80);  // BFRD, not the mask
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);

    hardware.write_cdrom(0x1F801800u, 1);     // bank 1: acknowledge INT3
    hardware.write_cdrom(0x1F801803u, 0x07);
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);
}

// A command's acknowledgement arrives a little after the store. Raising it
// immediately would let the interrupt handler run before the driver reaches
// its wait loop; the driver then clears the state byte the handler set and
// waits forever.
void cdrom_command_answer_is_delayed() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();

    hardware.write_cdrom(0x1F801800u, 0);     // bank 0
    hardware.write_cdrom(0x1F801801u, 0x01);  // GetStat
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);

    hardware.advance_cycles(r6::CdromDevice::kCommandDelayCycles - 1);
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);

    hardware.advance_cycles(1);
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);
}

// A read sector only becomes visible after the drive's sector time, so the
// guest's interrupt loop can return between sectors instead of draining a
// whole file inside one handler.
void cdrom_read_sector_arrives_after_delay() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();
    auto& cdrom = hardware.cdrom();
    cdrom.set_command_delay(0);
    cdrom.set_sector_delay(1000);

    hardware.write_cdrom(0x1F801800u, 0);
    hardware.write_cdrom(0x1F801801u, 0x06);  // ReadN
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);  // INT3

    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack INT3, arms first sector
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);
    hardware.advance_cycles(999);
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);
    hardware.advance_cycles(1);
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);  // INT1

    // Drain the 2060-byte FIFO, then acknowledge. The next sector is armed but
    // must wait for its own delay rather than appear during the handler.
    for (int index = 0; index < 515; ++index) cdrom.read_word();
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack INT1
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);
    hardware.advance_cycles(999);
    R6_CHECK((hardware.interrupt_status() & 0x04u) == 0);
    hardware.advance_cycles(1);
    R6_CHECK((hardware.interrupt_status() & 0x04u) != 0);
}

// GPUSTAT bit 31 is the drawing field: zero through vertical blank and the
// field polarity afterwards. Bit 22 is colour depth, so it must not follow
// the field.
void gpu_status_reports_field_and_vblank() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();
    constexpr std::uint32_t kStatus = 0x1F801814u;

    R6_CHECK((hardware.read32(kStatus) & 0x80400000u) == 0);

    hardware.toggle_gpu_field();  // odd field starts in vertical blank
    R6_CHECK((hardware.read32(kStatus) & 0x80000000u) == 0);
    R6_CHECK((hardware.read32(kStatus) & 0x00400000u) == 0);

    hardware.advance_cycles(2146u * 20u);  // vblank ends
    R6_CHECK((hardware.read32(kStatus) & 0x80000000u) != 0);
    R6_CHECK((hardware.read32(kStatus) & 0x00400000u) == 0);

    hardware.toggle_gpu_field();  // even field, vblank again
    R6_CHECK((hardware.read32(kStatus) & 0x80000000u) == 0);
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

// Games never install a handler at 0x80000080 because the BIOS owns it;
// they hook EntryInt instead, and libetc dispatches IRQs from there.
void interrupt_falls_back_to_entry_int() {
    Memory ram(2u * 1024u * 1024u);
    EntryIntServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    // HookEntryInt jmp_buf: RA, SP, FP, S0-S7 and GP as A(13h) saves them.
    ram.write32(0x20000, 0x80002000u);  // resume at the dispatcher
    ram.write32(0x20004, 0x80030000u);  // SP
    for (std::uint32_t index = 0; index < 10; ++index) {
        ram.write32(0x20008 + index * 4, 0x80000000u + index);
    }

    ram.write32(0x2000, 0x24100BADu);   // addiu s0, zero, 0xBAD
    ram.write32(0x2004, 0x240A0055u);   // addiu t2, zero, 0x55
    ram.write32(0x2008, 0x3C088002u);   // lui t0, 0x8002
    ram.write32(0x200C, 0xAD0A0040u);   // sw t2, 0x40(t0)
    ram.write32(0x2010, 0x24090017u);   // addiu t1, zero, 0x17
    ram.write32(0x2014, jal(0xB0));     // ReturnFromException
    ram.write32(0x2018, 0x00000000u);   // delay slot

    ram.write32(0x10000, 0x24080001u);  // addiu t0, zero, 1
    ram.write32(0x10004, 0x0000000Du);  // break
    cpu.gpr[16] = 0x1234u;              // interrupted callee-saved value
    cpu.gpr[29] = 0x801FF000u;          // interrupted stack pointer
    cpu.gpr[31] = 0x80010040u;          // interrupted return address
    raise_pending_interrupt(interpreter);

    const auto result = interpreter.run(12);
    R6_CHECK(interpreter.interrupts() == 1);
    R6_CHECK(interpreter.missing_vectors() == 0);
    R6_CHECK(ram.read32(0x20040) == 0x55u);  // the dispatcher ran
    // B(17h) restores the interrupted register file, so the handler's stack
    // and callee-saved clobbers do not leak into the guest.
    R6_CHECK(cpu.gpr[16] == 0x1234u);
    R6_CHECK(cpu.gpr[29] == 0x801FF000u);
    R6_CHECK(cpu.gpr[31] == 0x80010040u);
    R6_CHECK(cpu.gpr[10] == 0u);
    R6_CHECK(cpu.gpr[8] == 1u);  // then the interrupted stream resumed
    R6_CHECK(result.reason == StopReason::guest_break);
}

void interrupt_overrun_returns_to_epc() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    interpreter.set_interrupt_budget(50000);
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

void syscall_critical_section_toggles_interrupts() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x24040001u);  // addiu a0, zero, 1
    ram.write32(0x10004, 0x0000000Cu);  // syscall 1: enter critical section
    ram.write32(0x10008, 0x24040002u);  // addiu a0, zero, 2
    ram.write32(0x1000C, 0x0000000Cu);  // syscall 2: exit critical section

    const auto result = interpreter.run(4);
    R6_CHECK(result.reason == StopReason::step_budget);
    R6_CHECK((interpreter.cop0_status() & 1u) != 0);
}

}  // namespace

void run_mips_interpreter_tests() {
    arithmetic_and_delay_slot();
    taken_branch_skips_following_instruction();
    single_step_preserves_branch_target();
    call_and_return();
    load_and_store();
    unaligned_load_word_left();
    bios_trap_prints();
    unsupported_instruction_stops();
    break_stops_execution();
    irq_status_acknowledges_zero_bits();
    dma_start_reads_back_idle();
    timer_mode_reads_back();
    timer_clock_sources_match_the_hardware();
    gpu_status_reports_field_and_vblank();
    dma2_linked_list_reaches_the_gpu();
    cdrom_command_acknowledges_then_completes();
    cdrom_interrupt_line_honours_the_enable_mask();
    cdrom_command_answer_is_delayed();
    cdrom_read_sector_arrives_after_delay();
    interrupt_takes_the_exception_vector();
    interrupt_handler_returns_through_rfe();
    interrupt_without_a_vector_stays_pending();
    interrupt_falls_back_to_entry_int();
    interrupt_overrun_returns_to_epc();
    exception_vector_writes_are_traced();
    syscall_critical_section_toggles_interrupts();
}
