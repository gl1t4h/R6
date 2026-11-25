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


// The kernel exposes its C library through the A0 table; a guest that calls
// bzero/memcpy expects memory to actually change, so these must be real.
constexpr std::uint32_t kMaxBlockBytes = 1u << 21;

void block_fill(Memory& memory, std::uint32_t destination, std::uint32_t length,
                std::uint8_t value) {
    if (length > kMaxBlockBytes) length = kMaxBlockBytes;
    std::uint8_t probe = 0;
    for (std::uint32_t offset = 0; offset < length; ++offset) {
        const auto address = destination + offset;
        if (!memory.try_read8(address, probe)) return;
        memory.write8(address, value);
    }
}

void block_copy(Memory& memory, std::uint32_t source, std::uint32_t destination,
                std::uint32_t length) {
    if (length > kMaxBlockBytes) length = kMaxBlockBytes;
    std::uint8_t probe = 0;
    const bool overlaps_forward =
        destination > source && destination - source < length;
    if (overlaps_forward) {
        for (std::uint32_t offset = length; offset > 0; --offset) {
            std::uint8_t value = 0;
            if (!memory.try_read8(source + offset - 1, value)) return;
            if (!memory.try_read8(destination + offset - 1, probe)) return;
            memory.write8(destination + offset - 1, value);
        }
        return;
    }
    for (std::uint32_t offset = 0; offset < length; ++offset) {
        std::uint8_t value = 0;
        if (!memory.try_read8(source + offset, value)) return;
        if (!memory.try_read8(destination + offset, probe)) return;
        memory.write8(destination + offset, value);
    }
}

std::uint32_t block_compare(Memory& memory, std::uint32_t left,
                            std::uint32_t right, std::uint32_t length) {
    if (length > kMaxBlockBytes) length = kMaxBlockBytes;
    for (std::uint32_t offset = 0; offset < length; ++offset) {
        std::uint8_t first = 0;
        std::uint8_t second = 0;
        if (!memory.try_read8(left + offset, first)) return 0;
        if (!memory.try_read8(right + offset, second)) return 0;
        if (first != second) {
            return first < second ? static_cast<std::uint32_t>(-1)
                                  : static_cast<std::uint32_t>(1);
        }
    }
    return 0;
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
        case 0x25: {  // toupper
            const auto character = cpu.gpr[4] & 0xFFu;
            return character >= 'a' && character <= 'z' ? character - 0x20u
                                                        : character;
        }
        case 0x26: {  // tolower
            const auto character = cpu.gpr[4] & 0xFFu;
            return character >= 'A' && character <= 'Z' ? character + 0x20u
                                                        : character;
        }
        case 0x27:  // bcopy(src, dst, len)
            block_copy(memory, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
            return 0;
        case 0x28:  // bzero(dst, len)
            block_fill(memory, cpu.gpr[4], cpu.gpr[5], 0);
            return 0;
        case 0x29:  // bcmp(left, right, len): 0 when equal
            return block_compare(memory, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
        case 0x2A:  // memcpy(dst, src, len)
            block_copy(memory, cpu.gpr[5], cpu.gpr[4], cpu.gpr[6]);
            return cpu.gpr[4];
        case 0x2B:  // memset(dst, byte, len)
            block_fill(memory, cpu.gpr[4], cpu.gpr[6],
                       static_cast<std::uint8_t>(cpu.gpr[5]));
            return cpu.gpr[4];
        case 0x2C:  // memmove(dst, src, len)
            block_copy(memory, cpu.gpr[5], cpu.gpr[4], cpu.gpr[6]);
            return cpu.gpr[4];
        case 0x2D:  // memcmp(left, right, len)
            return block_compare(memory, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
        case 0x2E: {  // memchr(src, byte, len)
            const auto wanted = static_cast<std::uint8_t>(cpu.gpr[5]);
            std::uint32_t length = cpu.gpr[6];
            if (length > kMaxBlockBytes) length = kMaxBlockBytes;
            for (std::uint32_t offset = 0; offset < length; ++offset) {
                std::uint8_t value = 0;
                if (!memory.try_read8(cpu.gpr[4] + offset, value)) break;
                if (value == wanted) return cpu.gpr[4] + offset;
            }
            return 0;
        }
        case 0x13:  // setjmp(buf): RA, SP, FP, S0-S7 and GP.
            if (cpu.gpr[4] != 0) {
                const std::uint32_t base = cpu.gpr[4] & 0x1FFFFFFFu;
                memory.write32(base, cpu.gpr[31]);
                memory.write32(base + 4, cpu.gpr[29]);
                memory.write32(base + 8, cpu.gpr[30]);
                for (std::uint32_t index = 0; index < 8; ++index) {
                    memory.write32(base + 12 + index * 4, cpu.gpr[16 + index]);
                }
                memory.write32(base + 0x2C, cpu.gpr[28]);
            }
            return 0;
        case 0x14: {  // longjmp(buf, param): never returns to its caller.
            const auto base = cpu.gpr[4] & 0x1FFFFFFFu;
            if (base != 0) {
                cpu.gpr[31] = memory.read32(base);
                cpu.gpr[29] = memory.read32(base + 4);
                cpu.gpr[30] = memory.read32(base + 8);
                for (std::uint32_t index = 0; index < 8; ++index) {
                    cpu.gpr[16 + index] = memory.read32(base + 12 + index * 4);
                }
                cpu.gpr[28] = memory.read32(base + 0x2C);
                cpu.gpr[2] = cpu.gpr[5];
            }
            return cpu.gpr[2];
        }
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
        case 0x44:  // FlushCache: the host has no instruction cache to write
            return 0;
        case 0x70:  // _bu_init(): memory-card BIOS init, no host side to set up.
            return 0;
        case 0x71:  // _96_init(): the disc controller is modelled natively.
            return 0;
        case 0x72:  // _96_remove(): dequeues the CD interrupt; none is queued.
            return 0;
        default:
            record_unhandled(BiosTable::a0, vector, cpu);
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
        case 0x4A:  // InitCARD2(pad_enable): the card BIOS is not emulated.
            return 0;
        case 0x4B:  // StartCARD2()
        case 0x4C:  // StopCARD2()
            return 0;
        case 0x56:  // GetC0Table(): there is no retail jump table to hand out.
        case 0x57:  // GetB0Table()
            return 0;
        case 0x17:  // ReturnFromException(): abandon the rest of the chains.
            return_from_exception_ = true;
            return 0;
        case 0x18:  // ResetEntryInt(): back to the default exit structure.
            entry_int_ = 0;
            return 0;
        case 0x19:  // HookEntryInt(jmp_buf): the kernel runs this on every exit.
            entry_int_ = cpu.gpr[4];
            return entry_int_;
        case 0x5B: {  // ChangeClearPAD(mode): returns the previous flag.
            const std::uint32_t previous = clear_pad_ ? 1u : 0u;
            clear_pad_ = cpu.gpr[4] != 0;
            return previous;
        }
        case 0x3D:  // putchar
            putchar(static_cast<char>(cpu.gpr[4] & 0xFFu));
            return cpu.gpr[4];
        case 0x3F:  // puts
            return puts(memory, cpu.gpr[4]);
        default:
            record_unhandled(BiosTable::b0, vector, cpu);
            return 0;
    }
}

std::uint32_t BiosServices::handle_c0(std::uint32_t vector, CpuState& cpu) {
    switch (vector) {
        case 0x02:  // SysEnqIntRP(priority, entry)
            if (cpu.gpr[4] < interrupt_entries_.size()) {
                interrupt_entries_[cpu.gpr[4]] = cpu.gpr[5];
                return 1;
            }
            return 0;
        case 0x03:  // SysDeqIntRP(priority, entry)
            if (cpu.gpr[4] < interrupt_entries_.size()) {
                interrupt_entries_[cpu.gpr[4]] = 0;
                return 1;
            }
            return 0;
        case 0x0A:  // ChangeClearRCnt(timer, mode): the caller owns the flag.
            return 1;
        default:
            record_unhandled(BiosTable::c0, vector, cpu);
            return 0;
    }
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

void BiosServices::record_unhandled(BiosTable table, std::uint32_t vector,
                                   const CpuState& cpu) {
    unhandled_calls_.push_back(vector);
    constexpr std::size_t kMaxLoggedCalls = 64;
    if (unhandled_log_.size() >= kMaxLoggedCalls) return;
    const char prefix =
        table == BiosTable::a0 ? 'A' : (table == BiosTable::b0 ? 'B' : 'C');
    std::string entry;
    entry.push_back(prefix);
    entry += "(";
    entry += format_unsigned(vector, 16, false);
    entry += ")";
    for (std::size_t index = 4; index < 8; ++index) {
        entry += index == 4 ? " a=" : ",";
        entry += format_unsigned(cpu.gpr[index], 16, false);
    }
    entry += " ra=";
    entry += format_unsigned(cpu.gpr[31], 16, false);
    unhandled_log_.push_back(entry);
}

std::size_t BiosServices::call_count(BiosTable table,
                                     std::uint32_t vector) const {
    const auto found =
        call_counts_.find({static_cast<std::uint8_t>(table), vector});
    return found == call_counts_.end() ? 0 : found->second;
}

std::size_t BiosServices::total_calls() const {
    std::size_t total = 0;
    for (const auto& entry : call_counts_) total += entry.second;
    return total;
}

std::uint32_t BiosServices::unhandled_vector(std::size_t index) const {
    return index < unhandled_calls_.size() ? unhandled_calls_[index] : 0;
}

}  // namespace r6
