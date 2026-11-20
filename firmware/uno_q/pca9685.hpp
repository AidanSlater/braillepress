#pragma once

#include "pins.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace embosser {

inline constexpr int servo_channels = 16;
inline constexpr int servo_default_address = 0x40;
inline constexpr int servo_min_period_us = 656, servo_max_period_us = 41'943;

struct DotServo {
    int channel = -1, extend_us = 0, retract_us = 0;
};

struct ServoBank {
    int address = 0, period_us = 0, oe_pin = no_pin;
    unsigned extended = 0;
    std::array<DotServo, 6> dots{};

    int pulse_us(std::size_t dot) const {
        return extended & (1u << dot) ? dots[dot].extend_us : dots[dot].retract_us;
    }
};

namespace pca9685 {

// The built-in oscillator is nominally 25 MHz. Pulse widths and periods on
// the actual module can be checked with a scope during calibration.
inline constexpr int clock_mhz = 25, counts = 4096;
inline constexpr std::uint8_t mode1 = 0x00, mode2 = 0x01, first_channel = 0x06;
inline constexpr std::uint8_t all_off_high = 0xfd, prescale = 0xfe;
inline constexpr std::uint8_t auto_increment = 0x20, sleep = 0x10, restart = 0x80;
inline constexpr std::uint8_t totem_pole = 0x04, full_off = 0x10;

constexpr int prescaler(int period_us) {
    return (period_us * clock_mhz + counts / 2) / counts;
}

constexpr int ticks(int pulse_us, int period_us) {
    int divisor = prescaler(period_us);
    return (pulse_us * clock_mhz + divisor / 2) / divisor;
}

template<class Write>
bool disable(Write write) {
    return write(std::array<std::uint8_t, 2>{all_off_high, full_off});
}

template<class Write, class Wait>
bool start(const ServoBank& settings, Write write, Wait wait) {
    auto set = [&](std::uint8_t reg, std::uint8_t value) {
        return write(std::array<std::uint8_t, 2>{reg, value});
    };
    auto divider = static_cast<std::uint8_t>(prescaler(settings.period_us) - 1);
    if (!disable(write) || !set(mode1, sleep | auto_increment) || !set(mode2, totem_pole) ||
        !set(prescale, divider)) return false;

    for (std::size_t dot = 0; dot < settings.dots.size(); ++dot) {
        int channel = settings.dots[dot].channel;
        if (channel == -1) continue;
        int off = ticks(settings.pulse_us(dot), settings.period_us);
        if (!write(std::array<std::uint8_t, 5>{static_cast<std::uint8_t>(first_channel + 4 * channel),
            0, 0, static_cast<std::uint8_t>(off & 0xff), static_cast<std::uint8_t>(off >> 8)})) return false;
    }
    if (!set(mode1, auto_increment)) return false;
    wait(500);
    return set(mode1, restart | auto_increment);
}

} // namespace pca9685
} // namespace embosser
