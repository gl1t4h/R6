#include "r6/bios_services.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace r6 {

namespace {

constexpr std::size_t kMaxStringLength = 4096;
constexpr std::size_t kMaxTtyBytes = 1u << 16;
constexpr std::uint32_t kKernelAddressMask = 0x1FFFFFFFu;

std::string format_unsigned(std::uint32_t value,
                            std::uint32_t base,
                            bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) return "0";
    std::string result;
    while (value != 0) {
        result.push_back(digits[value % base]);
        value /= base;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

}  // namespace

bool BiosServices::trap_table(std::uint32_t pc, BiosTable& table) {
    switch (pc & kKernelAddressMask) {
        case kTrapA0:
            table = BiosTable::a0;
            return true;
        case kTrapB0:
            table = BiosTable::b0;
            return true;
        case kTrapC0:
            table = BiosTable::c0;
            return true;
        default:
            return false;
    }
}

std::uint32_t BiosServices::dispatch(BiosTable table, CpuState& cpu,
                                     Memory& memory) {
    const auto vector = cpu.gpr[9];
    record_call(table, vector);

    std::uint32_t result = 0;
    switch (table) {
        case BiosTable::a0:
            result = handle_a0(vector, cpu, memory);
            break;
        case BiosTable::b0:
            result = handle_b0(vector, cpu, memory);
            break;
        case BiosTable::c0:
            result = handle_c0(vector, cpu);
            break;
    }
    cpu.gpr[2] = result;
    return result;
}

std::uint32_t BiosServices::handle_a0(std::uint32_t vector, CpuState& cpu,
                                      Memory& memory) {
    switch (vector) {
        case 0x39:  // InitHeap: the native runtime owns the heap.
            return 0;
        case 0x3A:  // _exit
            exited_ = true;
            exit_code_ = cpu.gpr[4];
            return exit_code_;
        case 0x3C:  // putchar
            putchar(static_cast<char>(cpu.gpr[4] & 0xFFu));
            return cpu.gpr[4];
        case 0x3E:  // puts
            return puts(memory, cpu.gpr[4]);
        case 0x3F:  // printf
            return print_format(memory, cpu);
        default:
            record_unhandled(vector);
            return 0;
    }
}

std::uint32_t BiosServices::handle_b0(std::uint32_t vector, CpuState& cpu,
                                      Memory& memory) {
    switch (vector) {
        case 0x07:  // DeliverEvent(class, spec)
            return deliver_event(cpu.gpr[4], cpu.gpr[5]);
        case 0x08:  // OpenEvent(class, spec, mode, handler)
            return open_event(cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7]);
        case 0x09:  // CloseEvent(event)
            return close_event(cpu.gpr[4]);
        case 0x0A:  // WaitEvent(event)
            return wait_event(cpu.gpr[4]);
        case 0x0B:  // TestEvent(event)
            return test_event(cpu.gpr[4]);
        case 0x0C:  // EnableEvent(event)
        case 0x0D:  // DisableEvent(event)
            return set_event_enabled(cpu.gpr[4]);
        case 0x20:  // UnDeliverEvent(class, spec)
            return undeliver_event(cpu.gpr[4], cpu.gpr[5]);
        case 0x3D:  // putchar
            putchar(static_cast<char>(cpu.gpr[4] & 0xFFu));
            return cpu.gpr[4];
        case 0x3F:  // puts
            return puts(memory, cpu.gpr[4]);
        default:
            record_unhandled(vector);
            return 0;
    }
}

std::uint32_t BiosServices::handle_c0(std::uint32_t vector, CpuState&) {
    // Kernel table entries are recorded rather than guessed at.
    record_unhandled(vector);
    return 0;
}

BiosServices::EventSlot* BiosServices::find_event(std::uint32_t handle) {
    if (handle == 0 || handle > events_.size()) return nullptr;
    auto& slot = events_[handle - 1];
    return slot.open ? &slot : nullptr;
}

std::uint32_t BiosServices::open_event(std::uint32_t event_class,
                                       std::uint32_t spec,
                                       std::uint32_t mode,
                                       std::uint32_t handler) {
    for (std::size_t index = 0; index < events_.size(); ++index) {
        if (events_[index].open) continue;
        events_[index] = EventSlot{};
        events_[index].open = true;
        events_[index].event_class = event_class;
        events_[index].spec = spec;
        events_[index].mode = mode;
        events_[index].handler = handler;
        return static_cast<std::uint32_t>(index) + 1;
    }
    return 0;
}

std::uint32_t BiosServices::close_event(std::uint32_t handle) {
    auto* slot = find_event(handle);
    if (slot == nullptr) return 0;
    *slot = EventSlot{};
    return 1;
}

std::uint32_t BiosServices::deliver_event(std::uint32_t event_class,
                                          std::uint32_t spec) {
    std::uint32_t delivered = 0;
    for (auto& slot : events_) {
        if (!slot.open) continue;
        if (slot.event_class != event_class || slot.spec != spec) continue;
        slot.delivered = true;
        delivered = 1;
    }
    return delivered;
}

std::uint32_t BiosServices::undeliver_event(std::uint32_t event_class,
                                            std::uint32_t spec) {
    std::uint32_t cleared = 0;
    for (auto& slot : events_) {
        if (!slot.open) continue;
        if (slot.event_class != event_class || slot.spec != spec) continue;
        slot.delivered = false;
        cleared = 1;
    }
    return cleared;
}

// A single-threaded native runtime cannot block, so WaitEvent reports whether
// the event was already delivered instead of waiting for an interrupt.
std::uint32_t BiosServices::wait_event(std::uint32_t handle) {
    auto* slot = find_event(handle);
    if (slot == nullptr || !slot->delivered) return 0;
    slot->delivered = false;
    return 1;
}

std::uint32_t BiosServices::test_event(std::uint32_t handle) {
    const auto* slot = find_event(handle);
    if (slot == nullptr) return 0;
    return slot->delivered ? 1u : 0u;
}

std::uint32_t BiosServices::set_event_enabled(std::uint32_t handle) {
    return find_event(handle) == nullptr ? 0u : 1u;
}

void BiosServices::putchar(char character) {
    write_text(std::string(1, character));
}

void BiosServices::write_text(const std::string& text) {
    for (const char character : text) {
        if (tty_.size() >= kMaxTtyBytes) return;
        tty_.push_back(character);
    }
}

std::string BiosServices::read_string(Memory& memory,
                                      std::uint32_t address) const {
    std::string text;
    if (address == 0) return text;
    for (std::size_t offset = 0; offset < kMaxStringLength; ++offset) {
        std::uint8_t byte = 0;
        if (!memory.try_read8(address + static_cast<std::uint32_t>(offset),
                             byte)) {
            break;
        }
        if (byte == 0) break;
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

std::uint32_t BiosServices::puts(Memory& memory, std::uint32_t address) {
    const auto text = read_string(memory, address);
    write_text(text);
    return 1;
}

std::uint32_t BiosServices::read_argument(Memory& memory,
                                          const CpuState& cpu,
                                          std::size_t index) const {
    if (index < 3) return cpu.gpr[5 + index];
    const auto stack =
        cpu.gpr[29] + 0x10u + static_cast<std::uint32_t>(index - 3) * 4u;
    std::uint32_t value = 0;
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        std::uint8_t part = 0;
        if (!memory.try_read8(stack + byte, part)) return value;
        value |= static_cast<std::uint32_t>(part) << (byte * 8u);
    }
    return value;
}

std::uint32_t BiosServices::print_format(Memory& memory, const CpuState& cpu) {
    const auto format = read_string(memory, cpu.gpr[4]);
    std::string text;
    std::size_t argument = 0;

    for (std::size_t index = 0; index < format.size(); ++index) {
        const char character = format[index];
        if (character != '%') {
            text.push_back(character);
            continue;
        }

        std::size_t cursor = index + 1;
        bool zero_pad = false;
        while (cursor < format.size()) {
            const char flag = format[cursor];
            if (flag == '0') {
                zero_pad = true;
            } else if (flag != '-' && flag != '+' && flag != ' ' &&
                       flag != '#') {
                break;
            }
            ++cursor;
        }
        std::size_t width = 0;
        while (cursor < format.size() && format[cursor] >= '0' &&
               format[cursor] <= '9') {
            width = width * 10 + static_cast<std::size_t>(format[cursor] - '0');
            ++cursor;
        }
        while (cursor < format.size() &&
               (format[cursor] == 'l' || format[cursor] == 'h')) {
            ++cursor;
        }
        if (cursor >= format.size()) {
            text.push_back('%');
            break;
        }

        const char conversion = format[cursor];
        index = cursor;
        const auto value = read_argument(memory, cpu, argument);

        std::string rendered;
        bool negative = false;
        switch (conversion) {
            case 'c':
                rendered.assign(1, static_cast<char>(value & 0xFFu));
                break;
            case 's':
                rendered = read_string(memory, value);
                break;
            case 'd':
            case 'i': {
                const auto signed_value = static_cast<std::int32_t>(value);
                if (signed_value < 0) {
                    negative = true;
                    rendered = format_unsigned(
                        static_cast<std::uint32_t>(
                            -static_cast<std::int64_t>(signed_value)),
                        10, false);
                } else {
                    rendered = format_unsigned(value, 10, false);
                }
                break;
            }
            case 'u':
                rendered = format_unsigned(value, 10, false);
                break;
            case 'o':
                rendered = format_unsigned(value, 8, false);
                break;
            case 'x':
                rendered = format_unsigned(value, 16, false);
                break;
            case 'X':
                rendered = format_unsigned(value, 16, true);
                break;
            case 'p':
                rendered = format_unsigned(value, 16, false);
                if (width == 0) width = 8;
                break;
            case '%':
                text.push_back('%');
                continue;
            default:
                text.push_back('%');
                text.push_back(conversion);
                continue;
        }

        ++argument;
        if (negative) rendered.insert(rendered.begin(), '-');
        if (width > rendered.size()) {
            const std::string padding(width - rendered.size(),
                                      zero_pad ? '0' : ' ');
            if (zero_pad && negative) {
                rendered = "-" + padding + rendered.substr(1);
            } else {
                rendered = padding + rendered;
            }
        }
        text += rendered;
    }

    write_text(text);
    return 1;
}

void BiosServices::record_call(BiosTable table, std::uint32_t vector) {
    ++call_counts_[{static_cast<std::uint8_t>(table), vector}];
}

void BiosServices::record_unhandled(std::uint32_t vector) {
    unhandled_calls_.push_back(vector);
}

std::size_t BiosServices::call_count(BiosTable table,
                                     std::uint32_t vector) const {
    const auto found =
        call_counts_.find({static_cast<std::uint8_t>(table), vector});
    return found == call_counts_.end() ? 0 : found->second;
}

std::uint32_t BiosServices::unhandled_vector(std::size_t index) const {
    return index < unhandled_calls_.size() ? unhandled_calls_[index] : 0;
}

}  // namespace r6
