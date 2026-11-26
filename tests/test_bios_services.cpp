#include "test_support.h"

#include <cstdint>
#include <string>

#include "r6/bios_services.h"

namespace {

using r6::BiosServices;
using r6::BiosTable;
using r6::CpuState;
using r6::Memory;

void write_string(Memory& memory, std::uint32_t address, const char* text) {
    std::uint32_t cursor = address;
    for (const char* character = text; *character != '\0'; ++character) {
        memory.write8(cursor, static_cast<std::uint8_t>(*character));
        ++cursor;
    }
    memory.write8(cursor, 0);
}

void trap_recognition() {
    BiosTable table{};
    R6_CHECK(BiosServices::trap_table(0x800000A0u, table));
    R6_CHECK(table == BiosTable::a0);
    R6_CHECK(BiosServices::trap_table(0x000000B0u, table));
    R6_CHECK(table == BiosTable::b0);
    R6_CHECK(BiosServices::trap_table(0xA00000C0u, table));
    R6_CHECK(table == BiosTable::c0);
    R6_CHECK(!BiosServices::trap_table(0x80001000u, table));
}

void tty_output() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    cpu.gpr[9] = 0x3C;  // putchar
    cpu.gpr[4] = 'A';
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == static_cast<std::uint32_t>('A'));

    write_string(memory, 0x100, "hello");
    cpu.gpr[9] = 0x3E;  // puts
    cpu.gpr[4] = 0x100;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(bios.tty_output() == "Ahello");
    R6_CHECK(cpu.gpr[2] == 1);
}

void printf_formatting() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    write_string(memory, 0x200, "x=%d s=%s h=%04x %%");
    write_string(memory, 0x300, "ok");
    cpu.gpr[9] = 0x3F;
    cpu.gpr[4] = 0x200;
    cpu.gpr[5] = 42;
    cpu.gpr[6] = 0x300;
    cpu.gpr[7] = 0xBEEFu;

    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(bios.tty_output() == "x=42 s=ok h=beef %");
}

void printf_reads_stack_arguments() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    write_string(memory, 0x200, "%d %d %d %d");
    memory.write32(0x900, 4000);
    cpu.gpr[9] = 0x3F;
    cpu.gpr[4] = 0x200;
    cpu.gpr[5] = 1000;
    cpu.gpr[6] = 2000;
    cpu.gpr[7] = 3000;
    cpu.gpr[29] = 0x8F0;  // sp + 0x10 == 0x900

    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(bios.tty_output() == "1000 2000 3000 4000");
}

void exit_sets_state() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    cpu.gpr[9] = 0x3A;
    cpu.gpr[4] = 7;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(bios.exited());
    R6_CHECK(bios.exit_code() == 7);
    R6_CHECK(cpu.gpr[2] == 7);
}

void event_slots() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    cpu.gpr[9] = 0x08;  // OpenEvent(class, spec, mode, handler)
    cpu.gpr[4] = 0xF0000001u;
    cpu.gpr[5] = 0x1234;
    cpu.gpr[6] = 1;
    cpu.gpr[7] = 0;
    bios.dispatch(BiosTable::b0, cpu, memory);
    const auto handle = cpu.gpr[2];
    R6_CHECK(handle == 1);

    cpu.gpr[9] = 0x0B;  // TestEvent
    cpu.gpr[4] = handle;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0);

    cpu.gpr[9] = 0x07;  // DeliverEvent
    cpu.gpr[4] = 0xF0000001u;
    cpu.gpr[5] = 0x1234;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);

    cpu.gpr[9] = 0x0B;
    cpu.gpr[4] = handle;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);

    cpu.gpr[9] = 0x0A;  // WaitEvent clears the delivered flag
    cpu.gpr[4] = handle;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0);

    cpu.gpr[9] = 0x0C;  // EnableEvent on a live handle
    cpu.gpr[4] = handle;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);

    cpu.gpr[9] = 0x09;  // CloseEvent
    cpu.gpr[4] = handle;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0);
}

// B(35h) is FileWrite(fd,src,length), the same function as A(03h); games
// reach it through either kernel table while printing boot-time messages.
void file_write_reaches_stdout() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    write_string(memory, 0x300, "boot log\n");
    cpu.gpr[9] = 0x35;  // B(35h) FileWrite(fd, src, length)
    cpu.gpr[4] = 1;     // stdout
    cpu.gpr[5] = 0x300;
    cpu.gpr[6] = 8;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 8);
    R6_CHECK(bios.tty_output() == "boot log");

    write_string(memory, 0x320, "A-table\n");
    cpu.gpr[9] = 0x03;  // A(03h) is the same function
    cpu.gpr[4] = 1;
    cpu.gpr[5] = 0x320;
    cpu.gpr[6] = 8;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 8);
    R6_CHECK(bios.tty_output() == "boot logA-table\n");

    cpu.gpr[9] = 0x35;  // no file system behind other descriptors
    cpu.gpr[4] = 2;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0xFFFFFFFFu);

    cpu.gpr[9] = 0x03;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0xFFFFFFFFu);
    R6_CHECK(bios.tty_output() == "boot logA-table\n");

    R6_CHECK(bios.call_count(BiosTable::b0, 0x35) == 2);
    R6_CHECK(bios.call_count(BiosTable::a0, 0x03) == 2);
    R6_CHECK(bios.unhandled_call_count() == 0);
}

// _card_info is the retail card probe.  Silent Hill waits for a ready
// result before it leaves its memory-card initialisation path.
void card_info_reports_ready() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    cpu.gpr[9] = 0xAB;
    cpu.gpr[4] = 0;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 1);
    R6_CHECK(bios.unhandled_call_count() == 0);
    R6_CHECK(bios.call_count(BiosTable::a0, 0xAB) == 1);
}

void kernel_tables_are_modelled_in_ram() {
    BiosServices bios;
    Memory memory(0x2000, true);
    CpuState cpu;

    cpu.gpr[9] = 0x57;  // GetB0Table
    bios.dispatch(BiosTable::b0, cpu, memory);
    const auto b0 = cpu.gpr[2];
    R6_CHECK(b0 == BiosServices::kB0TableAddress);

    cpu.gpr[9] = 0x56;  // GetC0Table
    bios.dispatch(BiosTable::b0, cpu, memory);
    const auto c0 = cpu.gpr[2];
    R6_CHECK(c0 == BiosServices::kC0TableAddress);

    // Every entry is a thunk that dispatches its own vector.
    const auto open_event = memory.read32(b0 + 0x08u * 4u);
    R6_CHECK(open_event == BiosServices::kB0ThunksAddress + 8u * 16u);
    R6_CHECK(memory.read32(open_event) == 0x24090008u);
    R6_CHECK(memory.read32(open_event + 4) == 0x240A00B0u);

    // B0[0x5B] names the kernel block libcd patches at +0x884 and +0x9C8.
    R6_CHECK(memory.read32(b0 + 0x16Cu) == BiosServices::kKernelBlockAddress);
    R6_CHECK(memory.read32(BiosServices::kKernelBlockAddress) == 0x2409005Bu);

    // C0[6] names the CD entry table, whose +0x70/+0x74 halves rebuild the
    // CdRead body address that libcd patches and then calls at +0x3C.
    const auto cd_table = memory.read32(c0 + 0x18u);
    R6_CHECK(cd_table == BiosServices::kCdEntryTableAddress);
    const auto rebuilt = ((memory.read32(cd_table + 0x70u) & 0xFFFFu) << 16) |
                         (memory.read32(cd_table + 0x74u) & 0xFFFFu);
    R6_CHECK(rebuilt == BiosServices::kCdRoutineAddress);
    R6_CHECK(memory.read32(rebuilt) == 0x24020000u);
    R6_CHECK(memory.read32(rebuilt + BiosServices::kCdResumeOffset) ==
             0x24020000u);

    // The body is writable guest memory, which is what the hook install
    // needs: five words at +0x28 and then the entry at +0x3C.
    memory.write32(rebuilt + BiosServices::kCdPatchOffset, 0x3C02A001u);
    R6_CHECK(memory.read32(rebuilt + BiosServices::kCdPatchOffset) ==
             0x3C02A001u);

    // A second request returns the same table without rewriting it.
    cpu.gpr[9] = 0x57;
    bios.dispatch(BiosTable::b0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == b0);
    R6_CHECK(bios.unhandled_call_count() == 0);
}

void unhandled_vectors_are_recorded() {
    BiosServices bios;
    Memory memory(0x2000);
    CpuState cpu;

    cpu.gpr[9] = 0x54;
    bios.dispatch(BiosTable::a0, cpu, memory);
    R6_CHECK(cpu.gpr[2] == 0);
    R6_CHECK(bios.unhandled_call_count() == 1);
    R6_CHECK(bios.unhandled_vector(0) == 0x54);
    R6_CHECK(bios.call_count(BiosTable::a0, 0x54) == 1);

    cpu.gpr[9] = 0x08;
    bios.dispatch(BiosTable::c0, cpu, memory);
    R6_CHECK(bios.unhandled_call_count() == 2);
    R6_CHECK(bios.call_count(BiosTable::c0, 0x08) == 1);
}

}  // namespace

void run_bios_services_tests() {
    trap_recognition();
    tty_output();
    printf_formatting();
    printf_reads_stack_arguments();
    exit_sets_state();
    event_slots();
    file_write_reaches_stdout();
    card_info_reports_ready();
    kernel_tables_are_modelled_in_ram();
    unhandled_vectors_are_recorded();
}
