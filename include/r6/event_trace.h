#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace r6 {

// Ordered, human-readable events recorded by both execution engines. The
// differential compares these in addition to CPU and device state, so the same
// final state reached by a different interrupt/handler sequence is a failure.
class EventTrace {
public:
    static constexpr std::size_t kMaxEvents = 16384;

    void clear() {
        events_.clear();
        overflowed_ = false;
        count_ = 0;
        hash_ = kHashOffset;
    }

    void record(std::string event) {
        ++count_;
        for (const auto byte : event) {
            hash_ ^= static_cast<std::uint8_t>(byte);
            hash_ *= kHashPrime;
        }
        hash_ ^= 0xFFu;
        hash_ *= kHashPrime;
        if (events_.size() < kMaxEvents) {
            events_.push_back(std::move(event));
            return;
        }
        if (!overflowed_) {
            overflowed_ = true;
            events_.push_back("trace_overflow");
        }
    }

    bool empty() const { return count_ == 0; }
    std::size_t size() const { return events_.size(); }
    // The number of recorded events, including those after kMaxEvents. The
    // retained prefix is bounded, but count and hash still cover the full
    // ordered stream so two capped traces cannot compare equal just because
    // both overflowed.
    std::uint64_t total_events() const { return count_; }
    const std::vector<std::string>& events() const { return events_; }

    bool same_as(const EventTrace& other) const {
        return overflowed_ == other.overflowed_ && count_ == other.count_ &&
               hash_ == other.hash_ && events_ == other.events_;
    }

    // A compact first-difference description, empty when the traces match.
    std::string difference(const EventTrace& other) const {
        const auto common = events_.size() < other.events_.size()
                                ? events_.size()
                                : other.events_.size();
        for (std::size_t index = 0; index < common; ++index) {
            if (events_[index] == other.events_[index]) continue;
            return "event " + std::to_string(index) + ": native \"" +
                   events_[index] + "\", oracle \"" + other.events_[index] +
                   "\"";
        }
        if (events_.size() != other.events_.size() ||
            overflowed_ != other.overflowed_ || count_ != other.count_) {
            return "native trace has " + std::to_string(count_) +
                   " event(s), oracle has " + std::to_string(other.count_);
        }
        if (hash_ != other.hash_) {
            return "event fingerprint differs after the retained prefix (" +
                   std::to_string(events_.size()) + " event(s)): native 0x" +
                   hex(hash_) + ", oracle 0x" + hex(other.hash_);
        }
        return {};
    }

private:
    static constexpr std::uint64_t kHashOffset = 1469598103934665603ull;
    static constexpr std::uint64_t kHashPrime = 1099511628211ull;

    static std::string hex(std::uint64_t value) {
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llX",
                      static_cast<unsigned long long>(value));
        return buffer;
    }

    std::vector<std::string> events_;
    bool overflowed_{false};
    std::uint64_t count_{0};
    std::uint64_t hash_{kHashOffset};
};

}  // namespace r6
