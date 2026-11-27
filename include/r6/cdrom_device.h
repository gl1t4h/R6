#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "r6/platform_services.h"

namespace r6 {

// Minimal CD-ROM controller: register set, command responses and sector data.
//
// The disc image reader in PlatformServices already returns 2048-byte user
// sectors; this models the part a game talks to before it can ask for them.
// A command is held for a short time before it answers, as on hardware; then
// the acknowledgement interrupt is reported first, and the completion
// interrupt follows once the guest acknowledges it, which is the order
// library "sync" helpers wait for.
class CdromDevice {
public:
    // A real drive takes a short but non-zero time to accept a command and
    // raise its first interrupt. Answering on the store lets the interrupt
    // handler run before the driver reaches its wait loop, which then clears
    // the state byte the handler had just set; the command is lost and the
    // driver waits forever. Holding it briefly lets the guest get there.
    static constexpr std::uint64_t kCommandDelayCycles = 8000;
    // The drive delivers one sector every so often, not the instant the FIFO
    // empties. Presenting it synchronously made the guest's interrupt loop
    // drain a whole file inside one exception, which no real handler does and
    // which the interpreter's handler budget eventually has to break. A short
    // asynchronous delay lets the handler return after each sector. Timing the
    // drive at its real 2x rate (225792 cycles) was tried and is not what the
    // stalled stream waits on: the same refusal happens, just 30 sectors in
    // instead of 79, so the delay stays at the value the rest of the port was
    // measured with.
    static constexpr std::uint64_t kSectorDelayCycles = 4000;
    // A drive streams while a read is active; it does not stop and wait for
    // the guest to empty the data fifo. The next sector is armed when one is
    // presented, and a sector the guest acknowledged but did not collect
    // within its own sector time is given up and overwritten. That is what
    // lets a player which is behind on collecting still fill its read-ahead.
    //
    // A streamed movie additionally needs frame-granular service, not a uniform
    // cadence.
    // The guest's player claims a frame on its first published slice, converts
    // it, and only frees that frame's ring slots when it is done - 550,000 to
    // 800,000 cycles - while a 2x drive stages ten sectors in about 60,000. A
    // uniform fast cadence therefore laps the four-frame payload pool (the
    // reader bails at the frame boundary with its slot still claimed) and a
    // uniform slow one starves the converter mid-frame. Serving a record's
    // slices back to back and then waiting about one conversion satisfies both.
    // The gap only applies to STR records (type 0x0160/0x0161 with more than
    // one slice), so ordinary file reads are unaffected.
    // Measured on the movie: 700,000 cycles carries it 80 frames before the
    // release falls behind again, and 1,500,000 plays the whole stream; 2, 2.5
    // and 3 million give the identical end state, so the gap is not itself the
    // limiting factor above this value.
    static constexpr std::uint64_t kFrameGapCycles = 1500000;

    explicit CdromDevice(PlatformServices& services) : services_(services) {}

    // Tests that check the register handshake can pin the old immediate
    // behaviour; the default is the hardware-like delay.
    void set_command_delay(std::uint64_t cycles) {
        command_delay_cycles_ = cycles;
    }
    void set_sector_delay(std::uint64_t cycles) {
        sector_delay_cycles_ = cycles;
    }

    // Advances the command-acceptance and sector-delivery timers. Returns true
    // when something became visible to the guest, so the caller can refresh the
    // interrupt line.
    bool advance_cycles(std::uint64_t count) {
        bool changed = false;
        clock_ += count;
        if (command_pending_) {
            if (count < pending_delay_) {
                pending_delay_ -= count;
            } else {
                const auto command = pending_command_;
                command_pending_ = false;
                pending_delay_ = 0;
                parameters_ = pending_parameters_;
                pending_parameters_.clear();
                execute_command(command);
                changed = true;
            }
        }
        if (sector_pending_) {
            if (count < sector_delay_) {
                sector_delay_ -= count;
            } else if (interrupt_ != 0 && interrupt_ != kIntDataReady) {
                // A command's answer is still unacknowledged. Presenting the
                // sector now would overwrite the flag register with INT1 and
                // lose the answer, so it waits for the flag to clear.
                sector_delay_ = 0;
            } else if (presentation_allowed()) {
                sector_pending_ = false;
                sector_delay_ = 0;
                present_sector();
                changed = true;
            } else {
                // The guest is still working on the sector in the fifo. Keep
                // the sector armed and look again on the next tick, so the
                // stream continues as soon as it lets go of it.
                sector_delay_ = 0;
            }
        }
        return changed;
    }

    // Byte access to 0x1F801800..0x1F801803.
    //
    // The result (response) FIFO at +1 and the data FIFO at +2 are visible in
    // every bank; the bank only redirects +3 (interrupt flags in banks 1/3,
    // interrupt mask in banks 0/2). Sony's CD driver reads the flags with
    // bank 1 selected and then collects the response without switching back,
    // so treating +1 as bank-dependent hid every command answer.
    std::uint8_t read(std::uint32_t offset) {
        std::uint8_t value = 0;
        switch (offset) {
            case 0:
                ++status_reads_;
                if (response_index_ < response_.size()) ++response_seen_;
                value = status();
                break;
            case 1:
                ++response_reads_;
                value = pop_response();
                break;
            case 2:
                value = pop_data();
                break;
            default:
                value = (index_ & 1u) != 0
                            ? static_cast<std::uint8_t>(interrupt_)
                            : interrupt_mask_;
                break;
        }
        note_register(true, offset, value);
        return value;
    }

    void write(std::uint32_t offset, std::uint8_t value) {
        note_register(false, offset, value);
        switch (offset) {
            case 0:
                index_ = value & 3u;
                return;
            case 1:
                if (index_ == 0) queue_command(value);
                return;
            case 2:
                // Only bank 1 +2 is the interrupt enable register. Banks 2
                // and 3 +2 are the CD audio volumes; decoding one of those
                // as the mask disarms the interrupt line mid-transfer.
                if (index_ == 1) {
                    interrupt_mask_ = value & 0x1Fu;
                    return;
                }
                if (index_ == 0 && parameters_.size() < 8) {
                    parameters_.push_back(value);
                }
                return;
            default:
                if (index_ == 1) {
                    acknowledge(static_cast<std::uint32_t>(value));
                } else if (index_ == 0) {
                    // Bank 0 +3 is the request register (BFRD and friends),
                    // not the interrupt mask. Keeping a request-register
                    // write out of the mask stops a sector DMA from
                    // disabling the controller's interrupt line.
                    request_register_ = value;
                }
                // Banks 2/3 +3 are the left/right CD volumes; ignore them.
                return;
        }
    }

    // Data pushed to DMA channel 3, in 32-bit words.
    //
    // A short transfer collects a command's answer, which the drive reports as
    // status, track, index and then the position in BCD; callers hand the
    // buffer straight to a position converter, so the packet starts at the
    // position. A long transfer is sector data.
    //
    // Both are read from the data FIFO: DMA3 on the disc channel always drains
    // that FIFO, never the result FIFO. A sector starts with the 12-byte
    // header (BCD position, mode, subheader), which Sony's CD library reads
    // first to verify that the drive delivered the sector it asked for.
    std::uint32_t read_word() {
        std::uint32_t result = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const auto byte = pop_data();
            result |= static_cast<std::uint32_t>(byte) << shift;
        }
        return result;
    }

    // Bounded event log for bring-up: commands, interrupt transitions and
    // sector loads, most recent last.
    const std::vector<std::string>& log() const { return log_; }
    // Register-level trace (first events only), for checking the driver's
    // handshake against the modelled controller.
    const std::vector<std::string>& register_log() const { return reg_log_; }

    std::uint32_t status_reads() const { return status_reads_; }
    std::uint32_t response_seen() const { return response_seen_; }
    std::uint32_t response_reads() const { return response_reads_; }

    std::uint32_t sector() const { return sector_; }
    std::uint32_t sectors_read() const { return sectors_read_; }
    std::uint8_t status_byte() const { return status_; }
    std::uint32_t interrupt() const { return interrupt_; }
    // Bring-up state: tells "the drive is waiting for the guest" apart from
    // "the guest never asked again" when a stream stops.
    std::uint32_t presented_sector() const { return presented_sector_; }
    std::uint8_t last_command() const { return last_command_; }

    // The word the next read_word() would return, for diagnostics only.
    std::uint32_t peek_word() const {
        if (data_index_ + 4 > data_.size()) return 0;
        return static_cast<std::uint32_t>(data_[data_index_]) |
               (static_cast<std::uint32_t>(data_[data_index_ + 1]) << 8) |
               (static_cast<std::uint32_t>(data_[data_index_ + 2]) << 16) |
               (static_cast<std::uint32_t>(data_[data_index_ + 3]) << 24);
    }
    std::size_t data_size() const { return data_.size(); }
    std::size_t data_offset() const { return data_index_; }
    std::size_t data_remaining() const {
        return data_index_ < data_.size() ? data_.size() - data_index_ : 0;
    }
    bool data_acknowledged() const { return data_acknowledged_; }
    bool next_sector_pending() const { return next_sector_pending_; }
    bool sector_pending() const { return sector_pending_; }
    std::uint32_t deferred_interrupt() const { return deferred_interrupt_; }
    bool reading() const { return reading_; }
    // The controller drives its interrupt line only while a pending flag is
    // also enabled in its own mask register, as on hardware.
    bool interrupt_asserted() const {
        return (interrupt_ & interrupt_mask_) != 0;
    }

    // Full functional state, excluding diagnostic logs and the platform
    // service reference. The differential uses this to prove that the
    // persistent controller has not diverged from the oracle.
    bool same_state(const CdromDevice& other) const {
        return index_ == other.index_ && status_ == other.status_ &&
               event_counter_ == other.event_counter_ &&
               interrupt_ == other.interrupt_ &&
               interrupt_mask_ == other.interrupt_mask_ &&
               request_register_ == other.request_register_ &&
               complete_pending_ == other.complete_pending_ &&
               deferred_interrupt_ == other.deferred_interrupt_ &&
               parameters_ == other.parameters_ &&
               command_delay_cycles_ == other.command_delay_cycles_ &&
               pending_delay_ == other.pending_delay_ &&
               pending_command_ == other.pending_command_ &&
               pending_parameters_ == other.pending_parameters_ &&
               command_pending_ == other.command_pending_ &&
               response_ == other.response_ &&
               response_index_ == other.response_index_ &&
               data_ == other.data_ && data_index_ == other.data_index_ &&
               sector_ == other.sector_ &&
               sectors_read_ == other.sectors_read_ &&
               status_reads_ == other.status_reads_ &&
               response_seen_ == other.response_seen_ &&
               response_reads_ == other.response_reads_ &&
               reading_ == other.reading_ &&
               data_end_raised_ == other.data_end_raised_ &&
               start_read_after_ack_ == other.start_read_after_ack_ &&
               next_sector_pending_ == other.next_sector_pending_ &&
               sector_pending_ == other.sector_pending_ &&
               sector_delay_cycles_ == other.sector_delay_cycles_ &&
               sector_delay_ == other.sector_delay_ &&
               record_type_ == other.record_type_ &&
               record_slice_ == other.record_slice_ &&
               record_slices_ == other.record_slices_ &&
               frame_gap_pending_ == other.frame_gap_pending_ &&
               data_acknowledged_ == other.data_acknowledged_ &&
               xa_enabled_ == other.xa_enabled_;
    }

private:
    static constexpr std::uint8_t kMotorOn = 0x02;
    // A command always answers in two steps: INT3 acknowledges it, then the
    // completion interrupt carries the final status, or INT1 starts the sector
    // stream. Bits 0-2 of the flag register hold the interrupt type itself
    // (INT1=1 through INT5=5), not one flag per bit, so INT3 reads as 3.
    // Silent Hill's handler indexes a table with (flags & 7) - 1; returning
    // 0x04 made it see INT4 (DataEnd) and take the wrong completion path.
    static constexpr std::uint32_t kIntAcknowledge = 0x03;  // INT3
    static constexpr std::uint32_t kIntComplete = 0x02;     // INT2
    static constexpr std::uint32_t kIntDataReady = 0x01;    // INT1
    static constexpr std::uint32_t kIntDataEnd = 0x04;      // INT4

    std::uint8_t status() const {
        std::uint32_t value = index_ & 3u;
        value |= 0x08u;  // parameter FIFO empty
        value |= 0x10u;  // parameter FIFO not full
        if (response_index_ < response_.size()) value |= 0x20u;
        if (data_index_ < data_.size()) value |= 0x40u;
        return static_cast<std::uint8_t>(value);
    }

    std::uint8_t pop_response() {
        if (response_index_ >= response_.size()) return 0;
        return response_[response_index_++];
    }

    std::uint8_t pop_data() {
        if (data_index_ >= data_.size()) return 0;
        const auto value = data_[data_index_++];
        if (data_index_ >= data_.size()) {
            note("drained " + std::to_string(presented_sector_));
            // The sector has been collected, so a data-ready interrupt held
            // back behind a command answer is no longer owed.
            deferred_interrupt_ = 0;
            // Collecting the sector is not what arms the next one: the drive
            // did that when it presented this one. Draining only clears the
            // unread window that a presentation is allowed to overwrite.
            unread_since_ = clock_;
        }
        return value;
    }

    // Arms the next sector for delivery after the drive's sector time. The
    // presentation itself happens from advance_cycles(), outside the guest's
    // interrupt handler.
    void schedule_sector() {
        next_sector_pending_ = false;
        sector_pending_ = true;
        // A streamed record is served back to back and then held back for
        // about one frame conversion; see kFrameGapCycles. The flag is set by
        // load_sector() for the record's last slice.
        sector_delay_ = frame_gap_pending_ ? kFrameGapCycles
                                           : sector_delay_cycles_;
        frame_gap_pending_ = false;
    }

    // Bring-up: prints where the FIFO stands around the two places that can
    // swap the sector under the guest's feet.
    void trace_fifo_event(const char* what, std::uint8_t command) {
        static const bool enabled =
            std::getenv("R6_TRACE_CD_FIFO") != nullptr;
        if (!enabled) return;
        std::printf("EC %u %s cmd=%02X sec=%u off=%zu size=%zu reading=%u\n",
                    ++fifo_events_, what, command, sector_, data_index_,
                    data_.size(), reading_ ? 1u : 0u);
    }

    // Whether the drive may put another sector in the fifo now.
    //
    // The first sector of a read is always allowed. After that the guest must
    // have acknowledged the data-ready interrupt, and must either have
    // collected the sector or left it untouched for a whole sector time. A
    // drive does not wait for the fifo to drain: a player that has read only
    // a sector's header still sees the stream move on, which is what lets its
    // own read-ahead fill while it is behind.
    bool presentation_allowed() const {
        if (data_.empty()) return true;
        if (!data_acknowledged_) return false;
        if (data_index_ >= data_.size()) return true;
        return clock_ - unread_since_ >= sector_delay_cycles_;
    }

    void present_sector() {
        trace_fifo_event("present-in", last_command_);
        next_sector_pending_ = false;
        data_acknowledged_ = false;
        unread_since_ = clock_;
        load_sector();
        interrupt_ = kIntDataReady;
        trace_fifo_event("present-out", last_command_);
        // Arm the following sector now. Waiting for the guest to drain the
        // fifo before starting the drive's sector time would leave an
        // uncollected sector holding the whole stream up.
        if (reading_) {
            next_sector_pending_ = true;
            schedule_sector();
        }
    }

    void load_sector() {
        // The drive presents the 12-byte header in front of the 2048 user
        // bytes, so the guest's first DMA reads the sector position and the
        // second reads the payload. Without the header the position check in
        // Sony's CD library fails and it retries the sector forever.
        constexpr std::size_t kHeaderBytes = 12;
        constexpr std::size_t kUserBytes = 2048;
        // With XA-ADPCM enabled the drive decodes interleaved audio sectors
        // itself and does not put them in the data FIFO. A cutscene stream is
        // stored as data sectors with XA audio sectors between them; presenting
        // the audio bytes as if they were stream data makes the guest reject
        // the sector and stop reading one block short of the record.
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            data_.assign(kHeaderBytes + kUserBytes, 0);
            data_index_ = 0;
            if (!services_.read_disc_header(sector_, data_.data(),
                                            kHeaderBytes)) {
                // Synthesise address + mode when the backend has no raw access.
                const auto msf = sector_ + 150;
                data_[0] = to_bcd(msf / (60 * 75));
                data_[1] = to_bcd((msf / 75) % 60);
                data_[2] = to_bcd(msf % 75);
                data_[3] = 0x02;  // Mode 2
            }
            const bool loaded = services_.read_disc_sector(
                sector_, data_.data() + kHeaderBytes, kUserBytes);
            if (!loaded) {
                std::fill(data_.begin() + kHeaderBytes, data_.end(), std::uint8_t{0});
            }
            const auto presented = sector_;
            presented_sector_ = presented;
            ++sectors_read_;
            if (reading_) ++sector_;
            // A streamed record's header is the first eight bytes of user
            // data: type, then slice index, slice count and frame number. Only
            // a record's last slice arms the frame gap; ordinary file sectors
            // keep the plain cadence.
            const auto* record = data_.data() + kHeaderBytes;
            record_type_ = static_cast<std::uint16_t>(record[0] |
                                                      (record[1] << 8));
            record_slice_ = static_cast<std::uint16_t>(record[4] |
                                                       (record[5] << 8));
            record_slices_ = static_cast<std::uint16_t>(record[6] |
                                                        (record[7] << 8));
            frame_gap_pending_ =
                loaded &&
                (record_type_ == 0x0160u || record_type_ == 0x0161u) &&
                record_slices_ > 1 &&
                static_cast<std::uint32_t>(record_slice_) + 1u >= record_slices_;
            // Subheader byte 2 is the submode; bit 2 marks XA-ADPCM audio.
            if (loaded && xa_enabled_ && (data_[6] & 0x04u) != 0) {
                note("sector " + std::to_string(presented) + " xa skip");
                continue;
            }
            note("sector " + std::to_string(presented) +
                 (loaded ? " loaded" : " FAILED"));
            if (loaded && presented >= 128300 && presented < 128320) {
                // Bring-up: show the user-data head so a mismatch against the
                // disc image can be seen without a debugger.
                std::string head;
                constexpr char kHex[] = "0123456789abcdef";
                for (std::size_t index = 0; index < 32; ++index) {
                    const auto byte = data_[kHeaderBytes + index];
                    head += kHex[byte >> 4];
                    head += kHex[byte & 0x0Fu];
                }
                note("head " + head);
            }
            return;
        }
    }

    void acknowledge(std::uint32_t value) {
        const auto acked = value & 0x1Fu;
        note("ack " + std::to_string(acked) + " int=" +
             std::to_string(interrupt_));
        if (acked == 0) return;
        const auto previous = interrupt_;
        if (previous == 0) return;
        // Acknowledging clears only the flagged bits: the response FIFO keeps
        // its bytes until the guest reads them out. Libraries acknowledge
        // first and then collect the response, so clearing it here would
        // leave them with an empty status byte.
        interrupt_ &= ~acked;
        // The response bits are one interrupt type, so the queued completion
        // becomes visible only once the acknowledge fully clears them.
        if ((previous & 0x07u) == kIntAcknowledge && interrupt_ == 0 &&
            complete_pending_ != 0) {
            interrupt_ = complete_pending_;
            complete_pending_ = 0;
        }

        // The drive only presents the next sector after the guest acknowledges
        // the previous data-ready interrupt.
        if (start_read_after_ack_) {
            start_read_after_ack_ = false;
            schedule_sector();
            return;
        }
        if (previous == kIntDataReady && reading_) {
            data_acknowledged_ = true;
            // The sector's unread window starts here: the drive gives the
            // guest a full sector time from this acknowledgement to collect
            // the bytes before it overwrites them.
            unread_since_ = clock_;
        }
        if (interrupt_ == 0 && deferred_interrupt_ != 0 &&
            data_index_ < data_.size()) {
            // The sector could not take the flag register while the command's
            // answer was pending. Its bytes are still here, so raise the
            // data-ready interrupt now or the guest never collects them and
            // the read never arms another sector.
            interrupt_ = deferred_interrupt_;
            deferred_interrupt_ = 0;
            data_acknowledged_ = false;
        }
    }

    void begin_command(std::uint32_t interrupt, std::uint32_t complete) {
        // While a read is active the drive reports "reading" as well as motor
        // on; the loader waits for the reading bit before it collects sectors.
        status_ = static_cast<std::uint8_t>(kMotorOn | (reading_ ? 0x20 : 0x00));
        if (data_index_ < data_.size()) {
            // A sector is waiting in the fifo. The guest issued this command
            // and is waiting for its answer, so the answer takes the flag
            // register; the sector's data-ready interrupt follows once that
            // answer is acknowledged. Reporting the sector first makes a
            // library that acks blindly clear the data interrupt while it is
            // handling the answer, and it then never collects the bytes.
            deferred_interrupt_ = kIntDataReady;
        }
        interrupt_ = interrupt;
        complete_pending_ = complete;
        push_response(status_);
        // The drive answers every command with eight bytes: status, track,
        // index and the position in BCD. Libraries copy all eight into their
        // own location buffer and convert it with CdPosToInt, so stopping after
        // the status byte leaves them with position (0,0,0).
        push_response(0x00);  // track
        push_response(0x01);  // index
        push_msf(sector_);
        push_response(0x00);
        push_response(0x00);
    }

    void push_response(std::uint8_t value) {
        response_.push_back(value);
    }

    // The drive reports positions as absolute MSF (00:02:00 is LBA 0) while
    // sector transfers use the zero-based LBA. Treating a Setloc argument as
    // an LBA made a seek to the volume descriptor at LBA 16 fetch sector 166
    // instead, so the guest parsed executable bytes as a filesystem.
    static std::uint32_t msf_to_lba(std::uint32_t msf) {
        return msf > 150 ? msf - 150 : 0;
    }

    void push_msf(std::uint32_t lba) {
        const auto msf = lba + 150;
        push_response(to_bcd(msf / (60 * 75)));
        push_response(to_bcd((msf / 75) % 60));
        push_response(to_bcd(msf % 75));
    }

    void note(const std::string& text) {
        ++event_counter_;
        if (log_.size() >= 4096) log_.erase(log_.begin());
        log_.push_back(std::to_string(event_counter_) + " " + text);
    }

    void note_register(bool is_read, std::uint32_t offset,
                       std::uint8_t value) {
        constexpr std::size_t kMaxRegisterEvents = 160;
        if (reg_log_.size() >= kMaxRegisterEvents) return;
        std::string text = is_read ? "r" : "w";
        text += std::to_string(offset);
        text += " idx=" + std::to_string(index_);
        text += is_read ? " -> " : " <- ";
        text += hex_byte(value);
        if (!is_read && offset == 3) {
            text += " int=" + std::to_string(interrupt_);
        }
        reg_log_.push_back(text);
    }

    static std::string hex_byte(std::uint8_t value) {
        static const char* digits = "0123456789ABCDEF";
        std::string text(2, '0');
        text[0] = digits[(value >> 4) & 0xF];
        text[1] = digits[value & 0xF];
        return text;
    }

    void queue_command(std::uint8_t command) {
        if (command_delay_cycles_ == 0) {
            execute_command(command);
            return;
        }
        pending_command_ = command;
        pending_parameters_ = parameters_;
        parameters_.clear();
        pending_delay_ = command_delay_cycles_;
        command_pending_ = true;
    }

    void execute_command(std::uint8_t command) {
        last_command_ = command;
        trace_fifo_event("cmd-in", command);
        // A driver polling the drive's status while a sector is waiting must
        // not cost it that sector. Commands that abort or restart a transfer
        // do discard the FIFO; a status or parameter command leaves it alone.
        // Whether a sector's bytes are still in the fifo is a property of the
        // fifo, not of the flag register. The driver acknowledges the
        // data-ready interrupt before it collects the sector, so a command that
        // arrives between those two steps used to find the flags clear, treat
        // the sector as stale and drop it - and with nothing left to drain, the
        // drive never armed the next sector and the stream stopped for good.
        const bool data_present = data_index_ < data_.size();
        const bool data_interrupt = interrupt_ == kIntDataReady;
        const bool keeps_sector = command == 0x01 || command == 0x0B ||
                                  command == 0x0C || command == 0x0D ||
                                  command == 0x0E || command == 0x0F ||
                                  command == 0x10 || command == 0x11 ||
                                  command == 0x13 || command == 0x14 ||
                                  command == 0x19 || command == 0x1A;
        // Only an unanswered data-ready interrupt has to be reported after the
        // command's own answer; a sector that has been acknowledged but not yet
        // collected does not block the answer.
        sector_ready_for_command_ = data_present && data_interrupt &&
                                    keeps_sector;
        if (data_present && data_interrupt) {
            note("sector " +
                 std::to_string(sector_ready_for_command_ ? presented_sector_
                                                          : sector_) +
                 (sector_ready_for_command_ ? " held across cmd "
                                            : " discarded by cmd ") +
                 hex_byte(command));
        }
        if (data_present && !keeps_sector) {
            data_index_ = 0;
            data_.clear();
            deferred_interrupt_ = 0;
        }
        response_.clear();
        response_index_ = 0;

        std::string summary = "cmd " + hex_byte(command) + " p=";
        for (auto parameter : parameters_) {
            summary += hex_byte(parameter);
        }
        summary += " lba=" + std::to_string(sector_);
        note(summary);

        switch (command) {
            case 0x02:  // Setloc(min, sec, frame) -> absolute MSF, not LBA.
                if (parameters_.size() >= 3) {
                    sector_ = msf_to_lba(bcd(parameters_[0]) * 60 * 75 +
                                         bcd(parameters_[1]) * 75 +
                                         bcd(parameters_[2]));
                }
                begin_command(kIntAcknowledge, 0);
                break;
            case 0x06:  // ReadN
            case 0x1B:  // ReadS
                reading_ = true;
                data_end_raised_ = false;
                // The acknowledgement comes first; the first sector is
                // presented, with its own INT1, once the guest acknowledges it.
                data_index_ = 0;
                data_.clear();
                start_read_after_ack_ = true;
                next_sector_pending_ = false;
                frame_gap_pending_ = false;
                data_acknowledged_ = false;
                begin_command(kIntAcknowledge, 0);
                break;
            case 0x09:  // Pause
            case 0x08:  // Stop
                reading_ = false;
                data_end_raised_ = false;
                next_sector_pending_ = false;
                sector_pending_ = false;
                sector_delay_ = 0;
                data_acknowledged_ = false;
                begin_command(kIntAcknowledge, kIntComplete);
                break;
            case 0x01:  // GetStat
            case 0x0B:  // Mute
            case 0x0C:  // Demute
            case 0x0D:  // Setfilter
            case 0x1C:  // Reset
                begin_command(kIntAcknowledge, 0);
                break;
            case 0x0A:  // Init: acknowledge, then report completion.
                begin_command(kIntAcknowledge, kIntComplete);
                break;
            case 0x07:  // MotorOn
            case 0x15:  // SeekL
            case 0x16:  // SeekP
                begin_command(kIntAcknowledge, kIntComplete);
                break;
            case 0x0E:  // Setmode: answer is status twice.
                if (!parameters_.empty()) {
                    // Bit 6 enables XA-ADPCM decoding; while it is set the
                    // drive consumes audio sectors instead of presenting them.
                    xa_enabled_ = (parameters_[0] & 0x40u) != 0;
                }
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_response(status_);
                push_response(status_);
                break;
            case 0x10:  // GetlocL: position, then mode/file/channel.
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_msf(sector_);
                push_response(0x02);  // Mode 2, form 1.
                for (int index = 0; index < 4; ++index) push_response(0);
                break;
            case 0x11:  // GetlocP: track, index and both positions.
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_response(0x01);  // track
                push_response(0x01);  // index
                push_msf(sector_);
                push_msf(sector_);
                break;
            case 0x0F:  // Getparam: mode, then file/channel.
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_response(status_);
                push_response(0xA0);  // mode set by Init
                for (int index = 0; index < 6; ++index) push_response(0);
                break;
            case 0x19:  // Test: sub-function 0x20 reports the BIOS date
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                if (!parameters_.empty() && parameters_[0] == 0x20) {
                    push_response(0x94);
                    push_response(0x09);
                    push_response(0x19);
                    push_response(0xC0);
                }
                break;
            case 0x1A:  // GetID: first status, then the disc signature.
                begin_command(kIntAcknowledge, kIntComplete);
                response_.clear();
                response_index_ = 0;
                push_response(0x00);
                push_response(0x20);
                push_response(0x00);
                push_response('S');
                push_response('C');
                push_response('E');
                push_response('A');
                push_response(0x00);
                break;
            case 0x13:  // GetTN: status, first track, last track.
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_response(status_);
                push_response(0x01);
                push_response(0x01);
                break;
            case 0x14: {  // GetTD: status, then the track's minute/second.
                begin_command(kIntAcknowledge, 0);
                response_.clear();
                response_index_ = 0;
                push_response(status_);
                const auto msf = 150u;  // track 1 starts at 00:02:00
                push_response(to_bcd(msf / (60 * 75)));
                push_response(to_bcd((msf / 75) % 60));
                break;
            }
            default:  // GetStat and anything else: acknowledge and complete.
                begin_command(kIntAcknowledge, kIntComplete);
                break;
        }
        parameters_.clear();
        trace_fifo_event("cmd-out", command);
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
    std::uint64_t event_counter_{0};
    std::uint32_t interrupt_{0};
    std::uint8_t interrupt_mask_{0x1F};
    std::uint8_t request_register_{0};
    std::uint32_t complete_pending_{0};
    // Interrupt a command would have reported while a sector was still in the
    // FIFO; raised once that sector is acknowledged.
    std::uint32_t deferred_interrupt_{0};
    bool sector_ready_for_command_{false};
    std::vector<std::uint8_t> parameters_;
    std::uint64_t command_delay_cycles_{kCommandDelayCycles};
    std::uint64_t pending_delay_{0};
    std::uint8_t pending_command_{0};
    std::vector<std::uint8_t> pending_parameters_;
    bool command_pending_{false};
    std::vector<std::uint8_t> response_;
    std::size_t response_index_{0};
    std::vector<std::uint8_t> data_;
    std::size_t data_index_{0};
    std::uint32_t sector_{0};
    std::uint32_t sectors_read_{0};
    std::uint32_t status_reads_{0};
    std::uint32_t response_seen_{0};
    std::uint32_t response_reads_{0};
    bool reading_{false};
    bool data_end_raised_{false};
    // Sector most recently placed in the FIFO, for the drain trace.
    std::uint32_t presented_sector_{0};
    // Bring-up: the command whose data the FIFO currently holds.
    std::uint8_t last_command_{0};
    std::uint32_t fifo_events_{0};
    bool start_read_after_ack_{false};
    bool next_sector_pending_{false};
    bool sector_pending_{false};
    std::uint64_t sector_delay_cycles_{kSectorDelayCycles};
    std::uint64_t sector_delay_{0};
    // Record header of the sector just loaded, and whether it ended a record.
    std::uint16_t record_type_{0};
    std::uint16_t record_slice_{0};
    std::uint16_t record_slices_{0};
    bool frame_gap_pending_{false};
    bool data_acknowledged_{false};
    // Cycle count and the time the sector in the fifo was last acknowledged
    // or collected; together they bound how long unread bytes stay valid.
    std::uint64_t clock_{0};
    std::uint64_t unread_since_{0};
    // SetMode bit 6: the drive decodes XA-ADPCM audio sectors and keeps them
    // out of the data FIFO.
    bool xa_enabled_{false};
    std::vector<std::string> log_;
    std::vector<std::string> reg_log_;
};

}  // namespace r6
