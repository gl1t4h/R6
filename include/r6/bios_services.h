#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "r6/cpu_state.h"
#include "r6/memory.h"

namespace r6 {

// The retail kernel is reached by jumping to 0x000000A0, 0x000000B0 or
// 0x000000C0 with the function number in $t1 (r9). Args are $a0-$a3 and the
// return value goes to $v0.
enum class BiosTable : std::uint8_t {
    a0 = 0xA0,
    b0 = 0xB0,
    c0 = 0xC0,
};

class BiosServices {
public:
    static constexpr std::uint32_t kTrapA0 = 0x000000A0u;
    static constexpr std::uint32_t kTrapB0 = 0x000000B0u;
    static constexpr std::uint32_t kTrapC0 = 0x000000C0u;

    // Recognises a kernel trap address in any of the kuseg/kseg aliases.
    static bool trap_table(std::uint32_t pc, BiosTable& table);

    // Dispatches the vector currently in $t1. Unknown vectors return 0 and are
    // recorded so the analysis can enumerate what the game still needs.
    std::uint32_t dispatch(BiosTable table, CpuState& cpu, Memory& memory);

    bool exited() const { return exited_; }
    std::uint32_t exit_code() const { return exit_code_; }

    // The kernel's GPU vectors write the display registers directly. The GPU
    // object lives in the platform layer, so the host installs a sink instead
    // of the BIOS owning a second GPU.
    using GpuWriter = std::function<void(std::uint32_t, std::uint32_t)>;
    using GpuStatusReader = std::function<std::uint32_t()>;
    void set_gpu_access(GpuWriter writer, GpuStatusReader reader) {
        gpu_writer_ = std::move(writer);
        gpu_status_ = std::move(reader);
    }

    const std::string& tty_output() const { return tty_; }
    std::size_t unhandled_call_count() const { return unhandled_calls_.size(); }
    std::size_t call_count(BiosTable table, std::uint32_t vector) const;
    std::size_t total_calls() const;
    // (table, vector) -> call count, ordered by table then vector.
    const std::map<std::pair<std::uint8_t, std::uint32_t>, std::size_t>& calls()
        const {
        return call_counts_;
    }
    std::uint32_t unhandled_vector(std::size_t index) const;

    // B(19h) HookEntryInt: the kernel runs the jmp_buf's pc as the last step
    // of exception handling, which is how libetc dispatches its IRQ callbacks.
    std::uint32_t entry_int() const { return entry_int_; }
    // B(17h) ReturnFromException was called by the current handler: its return
    // is the end of exception handling, not a normal return to the caller.
    bool take_return_from_exception() {
        const bool value = return_from_exception_;
        return_from_exception_ = false;
        return value;
    }

    // SysEnqIntRP/SysDeqIntRP bookkeeping. The interpreter calls the queued
    // routine when a hardware interrupt has to be delivered.
    std::uint32_t interrupt_entry(std::uint32_t priority) const {
        return priority < interrupt_entries_.size()
                   ? interrupt_entries_[priority]
                   : 0;
    }
    // Human-readable log of the first unhandled calls, arguments included, so
    // the next kernel vector to implement is obvious during bring-up.
    const std::vector<std::string>& unhandled_log() const {
        return unhandled_log_;
    }

private:
    struct EventSlot {
        bool open{false};
        std::uint32_t event_class{0};
        std::uint32_t spec{0};
        std::uint32_t mode{0};
        std::uint32_t handler{0};
        bool delivered{false};
    };

    std::uint32_t handle_a0(std::uint32_t vector, CpuState& cpu,
                            Memory& memory);
    std::uint32_t handle_b0(std::uint32_t vector, CpuState& cpu,
                            Memory& memory);
    std::uint32_t handle_c0(std::uint32_t vector, CpuState& cpu);

    EventSlot* find_event(std::uint32_t handle);
    std::uint32_t open_event(std::uint32_t event_class, std::uint32_t spec,
                             std::uint32_t mode, std::uint32_t handler);
    std::uint32_t close_event(std::uint32_t handle);
    std::uint32_t deliver_event(std::uint32_t event_class,
                                std::uint32_t spec);
    std::uint32_t undeliver_event(std::uint32_t event_class,
                                  std::uint32_t spec);
    std::uint32_t wait_event(std::uint32_t handle);
    std::uint32_t test_event(std::uint32_t handle);
    std::uint32_t set_event_enabled(std::uint32_t handle);

    void putchar(char character);
    // B(35h) FileWrite(fd, src, length), same as A(03h). Descriptor 1 is the
    // TTY console; the shim has no file system behind any other descriptor.
    std::uint32_t file_write(std::uint32_t fd, std::uint32_t source,
                             std::uint32_t length, Memory& memory);
    void write_text(const std::string& text);
    std::uint32_t puts(Memory& memory, std::uint32_t address);
    std::uint32_t print_format(Memory& memory, const CpuState& cpu);
    std::uint32_t read_argument(Memory& memory, const CpuState& cpu,
                                std::size_t index) const;
    std::string read_string(Memory& memory, std::uint32_t address) const;

    void gpu_write(std::uint32_t register_address, std::uint32_t value);
    std::uint32_t gpu_status() const;

    void record_call(BiosTable table, std::uint32_t vector);
    void record_unhandled(BiosTable table, std::uint32_t vector,
                          const CpuState& cpu);

    std::array<std::uint32_t, 3> interrupt_entries_{};
    std::uint32_t entry_int_{0};
    bool return_from_exception_{false};
    // B(5Bh) ChangeClearPAD: the kernel's pad/card auto-acknowledge flag.
    bool clear_pad_{false};
    GpuWriter gpu_writer_;
    GpuStatusReader gpu_status_;
    std::vector<EventSlot> events_{16};
    std::vector<std::uint32_t> unhandled_calls_;
    std::vector<std::string> unhandled_log_;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::size_t> call_counts_;
    std::string tty_;
    bool exited_{false};
    std::uint32_t exit_code_{0};
};

}  // namespace r6
