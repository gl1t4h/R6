#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
    void write_text(const std::string& text);
    std::uint32_t puts(Memory& memory, std::uint32_t address);
    std::uint32_t print_format(Memory& memory, const CpuState& cpu);
    std::uint32_t read_argument(Memory& memory, const CpuState& cpu,
                                std::size_t index) const;
    std::string read_string(Memory& memory, std::uint32_t address) const;

    void record_call(BiosTable table, std::uint32_t vector);
    void record_unhandled(BiosTable table, std::uint32_t vector,
                          const CpuState& cpu);

    std::array<std::uint32_t, 3> interrupt_entries_{};
    // B(5Bh) ChangeClearPAD: the kernel's pad/card auto-acknowledge flag.
    bool clear_pad_{false};
    std::vector<EventSlot> events_{16};
    std::vector<std::uint32_t> unhandled_calls_;
    std::vector<std::string> unhandled_log_;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::size_t> call_counts_;
    std::string tty_;
    bool exited_{false};
    std::uint32_t exit_code_{0};
};

}  // namespace r6
