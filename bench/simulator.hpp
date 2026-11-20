#pragma once

#include "actuator.hpp"

#include <functional>
#include <utility>

namespace bench {

struct Transition {
    enum Kind { level, pwm_start, pwm_stop, servo_pulse, servo_stop, switch_read } kind;
    std::uint32_t time_us;
    int pin;
    bool high;
    int period_us = 0, on_us = 0;
    int address = 0;
};

enum class PressResponse { normal, starts_away, stays_up, stays_down };

class Simulator final : public embosser::SignalIo {
public:
    using Record = std::function<void(const Transition&)>;

    explicit Simulator(Record record) : actuator(*this), record_(std::move(record)) {}

    embosser::Actuator actuator;
    int input_value = 0;

    void configure_press(int trigger, int level, embosser::SwitchInput head_up,
                        int leave_us, int return_us, PressResponse response) {
        trigger_ = trigger;
        trigger_level_ = level;
        head_up_ = head_up;
        leave_us_ = leave_us;
        return_us_ = return_us;
        response_ = response;
        fired_ = released_ = false;
        reported_switch_ = -1;
    }

    void clear_press() { trigger_ = embosser::no_pin; }

    void advance(std::uint32_t target) {
        while (actuator.running()) {
            auto wait = actuator.until_service(now_);
            if (target - now_ < wait) break;
            now_ += wait;
            actuator.service();
        }
        now_ = target;
    }

    std::uint32_t clock_us() override { return now_; }

    bool output(int pin, bool high) override { return write(pin, high); }

    bool write(int pin, bool high) override {
        if (levels_[pin] != static_cast<int>(high)) {
            levels_[pin] = high;
            record_({Transition::level, now_, pin, high});
            if (pin == trigger_ && high == static_cast<bool>(trigger_level_)) {
                fired_ = true;
                pressed_at_ = now_;
            } else if (pin == trigger_ && fired_) {
                released_ = true;
                released_at_ = now_;
            }
        }
        return true;
    }

    bool pwm_on(int pin, bool high, int period_us, int on_us) override {
        record_({Transition::pwm_start, now_, pin, high, period_us, on_us});
        return true;
    }

    bool pwm_off(int pin, bool idle_high) override {
        levels_[pin] = idle_high;
        record_({Transition::pwm_stop, now_, pin, idle_high});
        return true;
    }

    int digital_input(int pin, embosser::Pull) override {
        levels_[pin] = -1;
        if (trigger_ != embosser::no_pin && pin == head_up_.pin) {
            bool up = response_ != PressResponse::starts_away;
            if (fired_ && response_ != PressResponse::stays_up && now_ - pressed_at_ >= leave_us_) {
                up = response_ == PressResponse::normal && released_ && now_ - released_at_ >= return_us_;
            }
            int value = up ? head_up_.level : !head_up_.level;
            if (reported_switch_ != value) {
                reported_switch_ = value;
                record_({Transition::switch_read, now_, pin, static_cast<bool>(value)});
            }
            return value;
        }
        return input_value;
    }

    int analog_input(int pin) override {
        levels_[pin] = -1;
        return input_value;
    }

    bool servos_start(const embosser::ServoBank& settings) override {
        if (settings.oe_pin != embosser::no_pin) output(settings.oe_pin, true);
        for (std::size_t i = 0; i < settings.dots.size(); ++i) {
            const auto& dot = settings.dots[i];
            if (dot.channel != -1)
                record_({Transition::servo_pulse, now_, dot.channel, true,
                    settings.period_us, settings.pulse_us(i), settings.address});
        }
        if (settings.oe_pin != embosser::no_pin) write(settings.oe_pin, false);
        return true;
    }

    bool servos_stop(const embosser::ServoBank& settings) override {
        if (settings.oe_pin != embosser::no_pin) output(settings.oe_pin, true);
        record_({Transition::servo_stop, now_, -1, false, 0, 0, settings.address});
        return true;
    }

private:
    Record record_;
    std::uint32_t now_ = 0;
    int trigger_ = embosser::no_pin, trigger_level_ = 1, reported_switch_ = -1;
    embosser::SwitchInput head_up_;
    std::uint32_t leave_us_ = 0, return_us_ = 0, pressed_at_ = 0, released_at_ = 0;
    PressResponse response_ = PressResponse::normal;
    bool fired_ = false, released_ = false;
    std::array<int, embosser::pin_count> levels_ = [] {
        std::array<int, embosser::pin_count> levels;
        levels.fill(-1);
        return levels;
    }();
};

}
