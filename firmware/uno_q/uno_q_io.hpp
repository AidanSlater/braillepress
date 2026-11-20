#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <zephyrPinctrl.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#include "actuator.hpp"

namespace embosser {

class UnoQIo final : public SignalIo {
    static auto write_pca(int address) {
        return [address](const auto& bytes) {
            Wire.beginTransmission(static_cast<std::uint8_t>(address));
            return Wire.write(bytes.data(), bytes.size()) == bytes.size() && Wire.endTransmission() == 0;
        };
    }

public:
    std::uint32_t clock_us() override {
        auto now = k_cycle_get_32();
        // Accumulate cycle deltas before converting so the 32-bit cycle
        // counter can wrap within a long run without changing the timebase.
        cycles_ += static_cast<std::uint32_t>(now - last_cycle_);
        last_cycle_ = now;
        return static_cast<std::uint32_t>(k_cyc_to_us_floor64(cycles_));
    }

    bool output(int pin, bool high) override {
        return configure(pin, high ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
    }

    bool write(int pin, bool high) override {
        return gpio_pin_set_raw(digitalPinToPortDevice(pin), digitalPinToPinIndex(pin), high) == 0;
    }

    bool pwm_on(int pin, bool high, int period_us, int on_us) override {
        auto index = pwm_index(pin);
        if (index == pwm_count) return false;
        auto channel = pwm_channels[index];
        if (!channel.dev || (!pwm_is_ready_dt(&channel) && device_init(channel.dev) != 0)) return false;
        active_pwm_ = channel;
        if (!high) active_pwm_.flags |= PWM_POLARITY_INVERTED;
        pwm_period_ns_ = PWM_USEC(period_us);
        if (pwm_set_dt(&active_pwm_, pwm_period_ns_, PWM_USEC(on_us)) != 0) return false;

        return configure_pwm_pin(pin, index);
    }

    bool pwm_off(int pin, bool idle_high) override {
        bool idle = output(pin, idle_high);
        if (!active_pwm_.dev) return idle;
        int stopped = pwm_set_dt(&active_pwm_, pwm_period_ns_, 0);
        active_pwm_ = {};
        return idle && stopped == 0;
    }

    int digital_input(int pin, Pull pull) override {
        gpio_flags_t flags = GPIO_INPUT;
        if (pull == pull_up) flags |= GPIO_PULL_UP;
        if (pull == pull_down) flags |= GPIO_PULL_DOWN;
        if (!configure(pin, flags)) return io_error;
        return gpio_pin_get_raw(digitalPinToPortDevice(pin), digitalPinToPinIndex(pin));
    }

    int analog_input(int pin) override { return analogRead(pin); }

    bool servos_start(const ServoBank& settings) override {
        if (settings.oe_pin != no_pin && !output(settings.oe_pin, true)) return false;
        Wire.begin();
        wire_started_ = true;
        if (!pca9685::start(settings, write_pca(settings.address), [](int us) { k_busy_wait(us); })) return false;
        return settings.oe_pin == no_pin || write(settings.oe_pin, false);
    }

    bool servos_stop(const ServoBank& settings) override {
        bool ok = settings.oe_pin == no_pin || output(settings.oe_pin, true);
        if (wire_started_) {
            ok = pca9685::disable(write_pca(settings.address)) && ok;
            Wire.end();
            wire_started_ = false;
        }
        return ok;
    }

private:
    static bool configure(int pin, gpio_flags_t flags) {
        auto port = digitalPinToPortDevice(pin);
        return device_is_ready(port) && gpio_pin_configure(port, digitalPinToPinIndex(pin), flags) == 0;
    }

    static std::size_t pwm_index(int pin) {
        auto port = digitalPinToPortDevice(pin);
        auto pad = digitalPinToPinIndex(pin);
        for (std::size_t i = 0; i < pwm_count; ++i)
            if (pwm_pads[i].port == port && pwm_pads[i].pin == pad) return i;
        return pwm_count;
    }

    static bool configure_pwm_pin(int pin, std::size_t slot) {
        auto index = zephyr::arduino::state_pin_index_from_spec_index(pwm_channels, slot);
        // These pads appear in a different order in pinctrl-2 and pwms.
        if (pin == 12) index = 3;
        if (pin == 13) index = 2;
        if (pin == 20) index = 2;
        if (pin == 21) index = 1;
        return zephyr::arduino::init_dev_apply_channel_pinctrl(pwm_channels[slot].dev, index) == 0;
    }

#define OBE_PWM_CHANNEL(node, prop, index) PWM_DT_SPEC_GET_BY_IDX(node, index),
#define OBE_PWM_PAD(node, prop, index) GPIO_DT_SPEC_GET_BY_IDX(node, prop, index),
    static constexpr pwm_dt_spec pwm_channels[] = {
        DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), pwms, OBE_PWM_CHANNEL)
    };
    static constexpr gpio_dt_spec pwm_pads[] = {
        DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), pwm_pin_gpios, OBE_PWM_PAD)
    };
#undef OBE_PWM_CHANNEL
#undef OBE_PWM_PAD
    static constexpr std::size_t pwm_count = sizeof(pwm_channels) / sizeof(*pwm_channels);
    static_assert(pwm_count == sizeof(pwm_pads) / sizeof(*pwm_pads));

    std::uint32_t last_cycle_ = 0, pwm_period_ns_ = 0;
    std::uint64_t cycles_ = 0;
    pwm_dt_spec active_pwm_{};
    bool wire_started_ = false;
};

}
