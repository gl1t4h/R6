#pragma once

#include <cstddef>
#include <cstdint>

namespace r6 {

class PlatformServices {
public:
    virtual ~PlatformServices() = default;

    virtual std::uint16_t read_controller(std::size_t port) = 0;
    virtual void submit_audio_samples(const std::int16_t* samples, std::size_t count) = 0;
    virtual void present_frame(const std::uint32_t* pixels, std::size_t count) = 0;
};

class NullPlatformServices final : public PlatformServices {
public:
    std::uint16_t read_controller(std::size_t) override { return 0; }
    void submit_audio_samples(const std::int16_t*, std::size_t) override {}
    void present_frame(const std::uint32_t*, std::size_t) override {}
};

}  // namespace r6
