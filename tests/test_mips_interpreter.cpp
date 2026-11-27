#include "test_support.h"

#include <cstdint>
#include <cstddef>
#include <vector>

#include "r6/bios_services.h"
#include "r6/hardware_map.h"
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

std::uint32_t lui(std::uint32_t rt, std::uint32_t immediate) {
    return (0x0Fu << 26) | (rt << 16) | (immediate & 0xFFFFu);
}

std::uint32_t ori(std::uint32_t rt, std::uint32_t rs, std::uint32_t immediate) {
    return (0x0Du << 26) | (rs << 21) | (rt << 16) |
           (immediate & 0xFFFFu);
}

std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::uint32_t immediate) {
    return (0x09u << 26) | (rs << 21) | (rt << 16) |
           (immediate & 0xFFFFu);
}

std::uint32_t sw(std::uint32_t rt, std::uint32_t offset, std::uint32_t rs) {
    return (0x2Bu << 26) | (rs << 21) | (rt << 16) |
           (offset & 0xFFFFu);
}

std::uint32_t bne(std::uint32_t rs, std::uint32_t rt, std::int32_t offset) {
    return (0x05u << 26) | (rs << 21) | (rt << 16) |
           (static_cast<std::uint32_t>(offset) & 0xFFFFu);
}

std::uint32_t sh(std::uint32_t rt, std::uint32_t offset, std::uint32_t rs) {
    return (0x29u << 26) | (rs << 21) | (rt << 16) |
           (offset & 0xFFFFu);
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

void instruction_charges_two_cycles() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    ram.write32(0x10000, 0x24080005u);   // addiu t0, zero, 5
    ram.write32(0x10004, 0x24090007u);   // addiu t1, zero, 7

    interpreter.run(2);
    // Compiled MIPS code retires about one instruction per two clocks. A
    // one-cycle clock shortened every guest delay loop, so libetc's VSync
    // wait expired inside a frame and the title never advanced its state.
    R6_CHECK(interpreter.hardware().cycles() ==
             2ull * r6::kCyclesPerInstruction);
}

void range_watch_counts_stores_only_inside_the_range() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;

    interpreter.watch_range(0x80020000u, 0x80020010u);
    ram.write32(0x10000, 0x3C088002u);   // lui t0, 0x8002
    ram.write32(0x10004, 0xA1000004u);   // sb zero, 4(t0): inside
    ram.write32(0x10008, 0xA1000020u);   // sb zero, 32(t0): outside

    interpreter.run(3);
    R6_CHECK(interpreter.range_watches()[0].stores == 1);
    R6_CHECK(interpreter.range_watches()[0].bytes == 1);
    R6_CHECK(interpreter.range_hot().size() == 1);
    R6_CHECK(interpreter.range_hot().begin()->first == 0x80010004u);
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

// The CD stream reads each sector as an 8-word header followed by a
// 504-word payload on channel 3. Only the payload may raise the completion
// interrupt, and CHCR must stay busy until the scheduled completion, so a
// driver polling it sees the transfer take time. Running the guest's CHCR
// store through MipsInterpreter exercises the gate that r6-run executes.
std::uint64_t start_dma3_transfer(Memory& ram, MipsInterpreter& interpreter,
                                  std::uint32_t words) {
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;
    std::uint32_t pc = 0x10000u;
    auto emit = [&](std::uint32_t instruction) {
        ram.write32(pc, instruction);
        pc += 4u;
    };
    emit(lui(8, 0x1F80));
    emit(ori(8, 8, 0x10F4));   // DICR
    emit(lui(9, 0x0088));
    emit(sw(9, 0, 8));         // master + channel 3 enable
    emit(addiu(8, 8, 0xFFBCu));  // DMA3 base 0x1F8010B0
    emit(lui(9, 0x0010));
    emit(sw(9, 0, 8));         // MADR = 0x00100000
    emit(addiu(9, 0, words));
    emit(sw(9, 4, 8));         // BCR
    emit(lui(9, 0x0100));
    emit(sw(9, 8, 8));         // CHCR: start the transfer
    interpreter.run(11);
    // run() also charges the triggering store, so the transfer began two
    // cycles before the current count.
    return interpreter.hardware().cycles() - r6::kCyclesPerInstruction;
}

void dma3_completion_splits_at_the_sector_payload() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();

    const auto busy = [&hardware]() {
        return (hardware.read32(r6::hardware::kDma3ChannelControl) &
                (1u << 24)) != 0u;
    };
    const auto publish_raised = [&hardware]() {
        return (hardware.interrupt_status() &
                r6::hardware::kDmaInterruptBit) != 0u;
    };
    const auto advance_to = [&hardware](std::uint64_t cycle) {
        const auto now = hardware.cycles();
        R6_CHECK(cycle >= now);
        hardware.advance_cycles(cycle - now);
    };

    // An 8-word sector header completes without pulling the interrupt.
    auto start = start_dma3_transfer(ram, interpreter, 8u);
    R6_CHECK(busy());
    advance_to(start + 15u);
    R6_CHECK(busy());
    advance_to(start + 16u);
    R6_CHECK(!busy());
    // The completion is still recorded as the channel's DICR flag.
    R6_CHECK((hardware.read32(r6::hardware::kDmaInterrupt) &
              (1u << 27)) != 0u);
    R6_CHECK(!publish_raised());
    R6_CHECK(hardware.take_dma_completion() == 0u);

    // A 504-word payload raises it, once the transfer completes.
    start = start_dma3_transfer(ram, interpreter, 504u);
    R6_CHECK(busy());
    R6_CHECK(hardware.take_dma_completion() == 0u);
    advance_to(start + 15u);
    R6_CHECK(busy());
    advance_to(start + 16u);
    R6_CHECK(!busy());
    R6_CHECK(publish_raised());
    R6_CHECK(hardware.take_dma_completion() == (1u << 3));
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

// Sony's library polls GetStat while a sector is still in the data FIFO. That
// command must not cost the driver the sector it has not collected yet: the
// data-ready interrupt and the sector bytes stay, the command's own answer is
// queued behind them, and the next sector waits so it cannot overwrite that
// answer before the guest reads it. Losing any of the three stops a streaming
// read, which is what an intro movie is.
void cdrom_status_command_keeps_the_pending_sector() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();
    auto& cdrom = hardware.cdrom();
    cdrom.set_command_delay(0);
    cdrom.set_sector_delay(1000);

    hardware.write_cdrom(0x1F801800u, 0);
    hardware.write_cdrom(0x1F801801u, 0x06);  // ReadN
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack INT3: arms the first sector
    hardware.advance_cycles(1000);
    R6_CHECK(cdrom.interrupt() == 1);  // INT1: a sector is waiting
    R6_CHECK((hardware.read_cdrom(0x1F801800u) & 0x40u) != 0);  // and its data
    // The drive's position has already moved past the sector it presented.
    const auto next_sector = cdrom.sector();

    // The driver polls the drive's status without collecting the sector first.
    hardware.write_cdrom(0x1F801800u, 0);
    hardware.write_cdrom(0x1F801801u, 0x01);  // GetStat

    // The command the guest is waiting on answers first; the sector data stays
    // in the fifo behind it.
    R6_CHECK(cdrom.interrupt() == 3);  // INT3 for the GetStat
    R6_CHECK((hardware.read_cdrom(0x1F801800u) & 0x40u) != 0);
    // 0x22 is the drive status: motor on, reading.
    R6_CHECK(hardware.read_cdrom(0x1F801801u) == 0x22);

    // Acknowledging the answer releases the sector's own data-ready interrupt
    // rather than losing it.
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);
    R6_CHECK(cdrom.interrupt() == 1);

    // Draining the sector arms the next one, which waits for the data-ready
    // interrupt to be acknowledged.
    for (int index = 0; index < 515; ++index) cdrom.read_word();
    hardware.advance_cycles(1000);
    R6_CHECK(cdrom.interrupt() == 1);
    R6_CHECK(cdrom.sector() == next_sector);  // nothing presented yet

    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack INT1
    hardware.advance_cycles(1000);
    R6_CHECK(cdrom.interrupt() == 1);  // the next sector
    R6_CHECK(cdrom.sector() == next_sector + 1);
}

// The driver acknowledges a sector before it collects the bytes. A command
// that lands between those two steps must not cost it the sector: the data is
// still in the fifo, so keeping it is what lets the read arm its next sector.
// Dropping it left the drive idle with a stream half read.
void cdrom_command_after_ack_keeps_the_sector() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);
    auto& hardware = interpreter.hardware();
    auto& cdrom = hardware.cdrom();
    cdrom.set_command_delay(0);
    cdrom.set_sector_delay(1000);

    hardware.write_cdrom(0x1F801800u, 0);
    hardware.write_cdrom(0x1F801801u, 0x06);  // ReadN
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack INT3: arms the first sector
    hardware.advance_cycles(1000);
    R6_CHECK(cdrom.interrupt() == 1);

    // Acknowledge the data-ready interrupt first, as libcd does.
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);
    R6_CHECK(cdrom.interrupt() == 0);

    // The driver polls the drive before it collects the sector.
    hardware.write_cdrom(0x1F801800u, 0);
    hardware.write_cdrom(0x1F801801u, 0x01);  // GetStat
    R6_CHECK((hardware.read_cdrom(0x1F801800u) & 0x40u) != 0);

    // Collecting the sector then arms the next one as usual, which presents
    // once the command's own answer has been acknowledged.
    for (int index = 0; index < 515; ++index) cdrom.read_word();
    hardware.write_cdrom(0x1F801800u, 1);
    hardware.write_cdrom(0x1F801803u, 0x07);  // ack the GetStat answer
    hardware.advance_cycles(1000);
    R6_CHECK(cdrom.interrupt() == 1);
    R6_CHECK(cdrom.sector() == 2);
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

void dma_interrupt_flags_are_write_one_to_clear() {
    TestServices services;
    r6::HardwareMap hardware(services);

    hardware.complete_dma(3);
    hardware.complete_dma(4);
    auto dicr = hardware.read32(0x1F8010F4u);
    R6_CHECK((dicr & (1u << 27)) != 0);
    R6_CHECK((dicr & (1u << 28)) != 0);

    // Acknowledge only channel 4.  Channel 3 must remain pending.
    hardware.write32(0x1F8010F4u, (1u << 28) | 0x00900000u);
    dicr = hardware.read32(0x1F8010F4u);
    R6_CHECK((dicr & (1u << 28)) == 0);
    R6_CHECK((dicr & (1u << 27)) != 0);
    R6_CHECK((dicr & 0x00FFFFFFu) == 0x00900000u);
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
    ram.write32(0x10010, 0x24040002u);  // addiu a0, zero, 2
    ram.write32(0x10014, 0x0000000Cu);  // syscall 2 again

    interpreter.run(2);
    R6_CHECK((interpreter.cop0_status() & 1u) == 0);
    R6_CHECK(cpu.gpr[2] == 1u);
    interpreter.run(2);
    R6_CHECK((interpreter.cop0_status() & 1u) != 0);

    // SYS(02h) is not a nesting counter: the retail BIOS always re-enables.
    interpreter.run(2);
    R6_CHECK((interpreter.cop0_status() & 1u) != 0);
}

}  // namespace

// Fills sound RAM through DMA4 and keys voice 0 on, so the voice model has
// real ADPCM headers to walk.
void start_spu_voice(Memory& ram, MipsInterpreter& interpreter,
                     std::uint32_t source, std::uint32_t control) {
    auto& cpu = interpreter.cpu();
    cpu.pc = 0x80010000u;
    std::uint32_t pc = 0x10000u;
    auto emit = [&](std::uint32_t instruction) {
        ram.write32(pc, instruction);
        pc += 4u;
    };
    emit(lui(8, 0x1F80));
    emit(addiu(8, 8, 0x10C0));     // DMA4 base
    emit(lui(9, 0x8000 | (source >> 16)));
    emit(ori(9, 9, source & 0xFFFFu));
    emit(sw(9, 0, 8));             // MADR
    emit(addiu(9, 0, 8));          // eight words
    emit(sw(9, 4, 8));             // BCR
    emit(lui(9, 0x0100));
    emit(ori(9, 9, 1));            // start, main RAM to device
    emit(sw(9, 8, 8));             // CHCR triggers the transfer
    emit(addiu(8, 8, 0xCC8));      // 0x1F801D88
    emit(addiu(9, 0, control));
    emit(sh(9, 0x22, 8));          // SPUCNT at 0x1F801DAA
    emit(sw(0, 0x1C, 8));          // IRQA = 0 at 0x1F801DA4
    emit(addiu(9, 0, 1));
    emit(sw(9, 0, 8));             // key on voice 0
    emit(addiu(10, 0, 20000));
    emit(bne(10, 0, 0));           // spin, decrementing in the delay slot
    emit(addiu(10, 10, 0xFFFF));
    emit(0);                       // park
    interpreter.run(200000);
}

void spu_voice_sets_endx_at_the_adpcm_end_flag() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);

    // One 16-byte block whose header flags byte is LOOP-END (bit 0).
    ram.write32(0x20000, 0x00000100u);
    ram.write32(0x20004, 0);
    ram.write32(0x20008, 0);
    ram.write32(0x2000C, 0);
    ram.write32(0x20010, 0);
    ram.write32(0x20014, 0);
    ram.write32(0x20018, 0);
    ram.write32(0x2001C, 0);
    start_spu_voice(ram, interpreter, 0x00020000u, 0x8000u);

    // 28 samples at 44.1 kHz last about 21,500 cycles, well inside the spin.
    R6_CHECK((interpreter.peek(0x1F801D9Cu) & 1u) != 0u);
    R6_CHECK(interpreter.spu_endx() == 1u);
    R6_CHECK(!interpreter.spu_voice_active(0));
}

void spu_voice_keeps_playing_without_an_end_flag() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);

    for (std::uint32_t word = 0; word < 8; ++word) {
        ram.write32(0x20000 + word * 4, 0);
    }
    start_spu_voice(ram, interpreter, 0x00020000u, 0x8000u);

    R6_CHECK(interpreter.spu_voice_active(0));
    R6_CHECK(interpreter.spu_endx() == 0u);
    // Current ADSR volume reports a live voice to a polling driver.
    R6_CHECK((interpreter.peek(0x1F801C0Cu) & 0xFFFFu) != 0u);
}

void spu_irq_raises_bit_nine_and_clears_on_acknowledge() {
    Memory ram(2u * 1024u * 1024u);
    TestServices services;
    MipsInterpreter interpreter(ram, services);

    ram.write32(0x20000, 0x00000100u);
    for (std::uint32_t word = 1; word < 8; ++word) {
        ram.write32(0x20000 + word * 4, 0);
    }
    // SPUCNT bit 15 enables the SPU and bit 6 the IRQ; voice 0 starts at 0,
    // which is also IRQA, so the level asserts on the first tick.
    start_spu_voice(ram, interpreter, 0x00020000u, 0x8040u);
    R6_CHECK((interpreter.irq_status() & 0x200u) != 0u);
    R6_CHECK((interpreter.peek(0x1F801DAEu) & 0x40u) != 0u);
}

void run_mips_interpreter_tests() {
    arithmetic_and_delay_slot();
    taken_branch_skips_following_instruction();
    instruction_charges_two_cycles();
    range_watch_counts_stores_only_inside_the_range();
    single_step_preserves_branch_target();
    call_and_return();
    load_and_store();
    unaligned_load_word_left();
    bios_trap_prints();
    unsupported_instruction_stops();
    break_stops_execution();
    irq_status_acknowledges_zero_bits();
    dma_start_reads_back_idle();
    dma3_completion_splits_at_the_sector_payload();
    timer_mode_reads_back();
    timer_clock_sources_match_the_hardware();
    gpu_status_reports_field_and_vblank();
    dma2_linked_list_reaches_the_gpu();
    cdrom_command_acknowledges_then_completes();
    cdrom_interrupt_line_honours_the_enable_mask();
    cdrom_command_answer_is_delayed();
    cdrom_read_sector_arrives_after_delay();
    cdrom_status_command_keeps_the_pending_sector();
    cdrom_command_after_ack_keeps_the_sector();
    interrupt_takes_the_exception_vector();
    interrupt_handler_returns_through_rfe();
    interrupt_without_a_vector_stays_pending();
    interrupt_falls_back_to_entry_int();
    interrupt_overrun_returns_to_epc();
    exception_vector_writes_are_traced();
    dma_interrupt_flags_are_write_one_to_clear();
    syscall_critical_section_toggles_interrupts();
    spu_voice_sets_endx_at_the_adpcm_end_flag();
    spu_voice_keeps_playing_without_an_end_flag();
    spu_irq_raises_bit_nine_and_clears_on_acknowledge();
}
