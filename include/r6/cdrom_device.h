#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "r6/platform_services.h"

namespace r6 {

// Minimal CD-ROM controller: register set, command responses and sector data.
//
// The disc image reader in PlatformServices already returns 2048-byte user
// sectors; this models the part a game talks to before it can ask for them.
// Commands complete immediately: the acknowledgement interrupt is reported
// first, and the completion interrupt follows once the guest acknowledges it,
// which is the order library "sync" helpers wait for.
class CdromDevice {
public:
    explicit CdromDevice(PlatformServices& services) : services_(services) {}

    // Byte access to 0x1F801800..0x1F801803.
    std::uint8_t read(std::uint32_t offset) {
        switch (offset) {
            case 0:
                return status();
            case 1:
                return index_ == 0 ? pop_response() : 0;
            case 2:
                // Index 1 is the data FIFO; index 0 takes parameters on write.
                return index_ == 1 ? pop_data() : 0;
            default:
                return static_cast<std::uint8_t>(interrupt_);
        }
    }

    void write(std::uint32_t offset, std::uint8_t value) {
        switch (offset) {
            case 0:
                index_ = value & 3u;
                return;
            case 1:
                if (index_ == 0) execute_command(value);
                return;
            case 2:
                if (index_ == 0 && parameters_.size() < 8) {
                    parameters_.push_back(value);
                }
                return;
            default:
                acknowledge(static_cast<std::uint32_t>(value));
                return;
        }
    }

    // Data pushed to DMA channel 3, in 32-bit words.
    std::uint32_t read_word() {
        std::uint32_t result = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(pop_data()) << shift;
        }
        return result;
    }

    // Bounded event log for bring-up: commands, interrupt transitions and
    // sector loads, most recent last.
    const std::vector<std::string>& log() const { return log_; }

    std::uint32_t sector() const { return sector_; }
    std::uint32_t sectors_read() const { return sectors_read_; }
    std::uint8_t status_byte() const { return status_; }
    std::uint32_t interrupt() const { return interrupt_; }

private:
    static constexpr std::uint8_t kMotorOn = 0x02;
    static constexpr std::uint32_t kIntAcknowledge = 1;
    static constexpr std::uint32_t kIntComplete = 2;
    // ReadN reports the first sector with INT1 (the same interrupt as the
    // command acknowledgement), not INT3: libraries treat INT3 as data end.
    static constexpr std::uint32_t kIntDataReady = 1;
    static constexpr std::uint32_t kIntDataEnd = 3;

    std::uint8_t status() const {
        std::uint32_t value = index_ & 3u;
        value |= 0x08u;  // parameter FIFO empty
        value |= 0x10u;  // parameter FIFO not full
        if (response_index_ < response_.size()) value |= 0x20u;
        if (index_ == 1 && data_index_ < data_.size()) value |= 0x40u;
        return static_cast<std::uint8_t>(value);
    }

    std::uint8_t pop_response() {
        if (response_index_ >= response_.size()) return 0;
        return response_[response_index_++];
    }

    std::uint8_t pop_data() {
        if (reading_ && data_index_ >= data_.size() && !data_end_raised_) {
            // The guest has taken the whole sector, so the read reports data
            // end (INT3): libraries complete a transfer on it, and leaving the
            // read open makes their sync helper time out instead.
            data_end_raised_ = true;
            interrupt_ = kIntDataEnd;
            complete_pending_ = 0;
        }
        if (data_index_ >= data_.size()) return 0;
        return data_[data_index_++];
    }

    void load_sector() {
        data_.assign(2048, 0);
        data_index_ = 0;
        const bool loaded =
            services_.read_disc_sector(sector_, data_.data(), data_.size());
        note("sector " + std::to_string(sector_) +
             (loaded ? " loaded" : " FAILED"));
        if (!loaded) {
            data_.assign(2048, 0);
        }
        if (reading_) ++sector_;
        ++sectors_read_;
    }

    void acknowledge(std::uint32_t value) {
        note("ack " + std::to_string(value & 0x07u) + " int=" +
             std::to_string(interrupt_));
        if ((value & 0x07u) == 0) return;
        if (interrupt_ == 0) return;
        if (interrupt_ == kIntAcknowledge && complete_pending_) {
            interrupt_ = complete_pending_;
            complete_pending_ = 0;
            return;
        }
        if (interrupt_ == kIntDataReady && reading_ && !data_.empty()) {
            // Acknowledge of the data-ready interrupt ends this sector's
            // transfer, so the guest can also see data end even when its
            // library waits for it without draining the FIFO first.
            data_end_raised_ = true;
            interrupt_ = kIntDataEnd;
            return;
        }

        interrupt_ = 0;
        response_.clear();
        response_index_ = 0;
    }

    void begin_command(std::uint32_t interrupt, std::uint32_t complete) {
        status_ = kMotorOn;
        interrupt_ = interrupt;
        complete_pending_ = complete;
        push_response(status_);
    }

    void push_response(std::uint8_t value) {
        response_.push_back(value);
    }

    void note(const std::string& text) {
        if (log_.size() >= 96) log_.erase(log_.begin());
        log_.push_back(text);
    }

    static std::string hex_byte(std::uint8_t value) {
        static const char* digits = "0123456789ABCDEF";
        std::string text(2, '0');
        text[0] = digits[(value >> 4) & 0xF];
        text[1] = digits[value & 0xF];
        return text;
    }

    void execute_command(std::uint8_t command) {
        data_index_ = 0;
        data_.clear();
        response_.clear();
        response_index_ = 0;

        std::string summary = "cmd " + hex_byte(command) + " p=";
        for (auto parameter : parameters_) {
            summary += hex_byte(parameter);
        }
        summary += " sec=" + std::to_string(sector_);
        note(summary);

        switch (command) {
            case 0x02:  // Setloc(min, sec, frame)
                if (parameters_.size() >= 3) {
                    sector_ = bcd(parameters_[0]) * 60 * 75 +
                              bcd(parameters_[1]) * 75 + bcd(parameters_[2]);
                }
                begin_command(kIntAcknowledge, kIntComplete);
                break;
            case 0x06:  // ReadN
                reading_ = true;
                data_end_raised_ = false;
                // The sector is presented together with the data-ready
                // interrupt, so the FIFO already holds it when the guest starts
                // its DMA transfer.
                begin_command(kIntAcknowledge, kIntDataReady);
                load_sector();
                break;
            case 0x09:  // Pause
            case 0x08:  // Stop
                reading_ = false;
                data_end_raised_ = false;
                begin_command(kIntAcknowledge, kIntComplete);
                break;
            case 0x0E:  // GetlocL: minute, second, frame in BCD, then mode.
                begin_command(kIntAcknowledge, kIntComplete);
                push_response(to_bcd(sector_ / (60 * 75)));
                push_response(to_bcd((sector_ / 75) % 60));
                push_response(to_bcd(sector_ % 75));
                push_response(0x02);  // Mode 2, form 1.
                for (int index = 0; index < 4; ++index) push_response(0);
                break;
            case 0x19:  // Test: sub-function 0x20 reports the BIOS date
                begin_command(kIntAcknowledge, kIntComplete);
                if (!parameters_.empty() && parameters_[0] == 0x20) {
                    push_response(0x94);
                    push_response(0x09);
                    push_response(0x19);
                    push_response(0xC0);
                }
                break;
            case 0x1A:  // GetID
                begin_command(kIntAcknowledge, kIntComplete);
                push_response(0x00);
                push_response(0x20);
                push_response(0x00);
                push_response('S');
                push_response('C');
                push_response('E');
                push_response('A');
                break;
            case 0x13:  // GetTN
                begin_command(kIntAcknowledge, kIntComplete);
                push_response(1);
                push_response(1);
                break;
            case 0x14:  // GetTD
                begin_command(kIntAcknowledge, kIntComplete);
                push_response(0);
                push_response(2);
                break;
            default:  // GetStat and anything else: acknowledge and complete.
                begin_command(kIntAcknowledge, kIntComplete);
                break;
        }
        parameters_.clear();
    }

    static std::uint8_t to_bcd(std::uint32_t value) {
        return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
    }

    static std::uint32_t bcd(std::uint8_t value) {
        return static_cast<std::uint32_t>((value >> 4) * 10 + (value & 0x0Fu));
    }

    PlatformServices& services_;
    std::uint32_t index_{0};
    std::uint8_t status_{kMotorOn};
    std::uint32_t interrupt_{0};
    std::uint32_t complete_pending_{0};
    std::vector<std::uint8_t> parameters_;
    std::vector<std::uint8_t> response_;
    std::size_t response_index_{0};
    std::vector<std::uint8_t> data_;
    std::size_t data_index_{0};
    std::uint32_t sector_{0};
    std::uint32_t sectors_read_{0};
    bool reading_{false};
    bool data_end_raised_{false};
    std::vector<std::string> log_;
};

}  // namespace r6
