#pragma once

#include "cells.hpp"
#include "pca9685.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace embosser {

inline constexpr int bench_version = 2;
inline constexpr char run_method[] = "bench_run";
inline constexpr char status_method[] = "bench_status";
inline constexpr char stop_method[] = "bench_stop";
inline constexpr char version_method[] = "bench_version";
inline constexpr char digital_method[] = "bench_read_digital";
inline constexpr char adc_method[] = "bench_read_adc";
inline constexpr char timing_method[] = "bench_timing";

enum SignalMode : int { pulse, step_direction, pwm, dot_bank, cell_cycle };
enum Pull : int { floating, pull_up, pull_down };
enum BenchReply : int {
    bad_pin = -8, bad_signal = -9, io_error = -10, lease_expired = -11, time_limit = -12,
    guard_open = -13, press_timeout = -14, servo_io_error = -15
};

inline constexpr int min_phase_us = 2'000;
inline constexpr int min_pwm_period_us = 20;
inline constexpr int min_pwm_phase_us = 2;
inline constexpr int max_run_us = 30'000'000;
inline constexpr int max_repeats = 7'500;
inline constexpr std::uint32_t lease_us = 2'000'000;
inline constexpr int adc_max = 4095;
inline constexpr int switch_sample_us = 1'000;
inline constexpr std::size_t cell_cycle_args = 32;

struct SwitchInput {
    int pin = no_pin, level = 1, pull = pull_up;
};

struct CellCycle {
    ServoBank servos;
    int settle_us = 0, trigger = no_pin, level = 1, press_us = 0;
    int return_us = 0, debounce_us = 0;
};

struct Signal {
    std::array<int, 6> output{no_pin, no_pin, no_pin, no_pin, no_pin, no_pin};
    unsigned mask = 1;
    int level = 1, on_us = 0, off_us = 0, repeats = 1;
    int direction = no_pin, direction_level = 0, setup_us = 0;
    int enable = no_pin, enable_level = 0;
    int period_us = 0, duration_us = 0;
    SwitchInput required;
    bool cell = false;
    CellCycle cycle;
};

constexpr bool binary(int value) { return value == 0 || value == 1; }

inline int decode_cycle(const int* args, std::size_t size, Signal& signal) {
    if (size != cell_cycle_args) return bad_signal;
    auto& cycle = signal.cycle;
    auto& bank = cycle.servos;
    bank.address = args[1];
    bank.period_us = args[2];
    bank.oe_pin = args[3];
    if (args[4] < 0 || args[4] >= end_line) return bad_signal;
    bank.extended = args[4];
    for (std::size_t i = 0; i < bank.dots.size(); ++i) {
        auto field = 5 + 3 * i;
        bank.dots[i] = {args[field], args[field + 1], args[field + 2]};
    }
    cycle.settle_us = args[23];
    cycle.trigger = args[24];
    cycle.level = args[25];
    cycle.press_us = args[26];
    signal.required = {args[27], args[28], args[29]};
    cycle.return_us = args[30];
    cycle.debounce_us = args[31];

    std::uint32_t reserved = bank.address ? (1u << servo_sda) | (1u << servo_scl) : 0;
    for (int pin : {bank.oe_pin, cycle.trigger, signal.required.pin}) {
        if (pin == no_pin) continue;
        if (!gpio_pin(pin) || (reserved & (1u << pin))) return bad_pin;
        reserved |= 1u << pin;
    }
    if (bank.address) {
        if (bank.address < servo_default_address || bank.address > 0x77 || bank.address == 0x70 ||
            bank.period_us < servo_min_period_us || bank.period_us > servo_max_period_us) return bad_signal;
        unsigned assigned = 0;
        for (const auto& dot : bank.dots) {
            if (dot.channel == -1) continue;
            if (dot.channel < 0 || dot.channel >= servo_channels || (assigned & (1u << dot.channel)))
                return bad_signal;
            for (int width : {dot.extend_us, dot.retract_us}) {
                if (width <= 0 || width >= bank.period_us) return bad_signal;
                auto ticks = pca9685::ticks(width, bank.period_us);
                if (ticks < 1 || ticks >= pca9685::counts) return bad_signal;
            }
            assigned |= 1u << dot.channel;
        }
        if (!assigned) return bad_signal;
    } else if (bank.oe_pin != no_pin || cycle.trigger == no_pin) {
        return bad_signal;
    }

    if (cycle.settle_us < 0 || cycle.return_us < 0 || cycle.debounce_us < 0 ||
        !binary(signal.required.level) || signal.required.pull < floating || signal.required.pull > pull_down)
        return bad_signal;
    if (cycle.trigger == no_pin) {
        if (cycle.settle_us < bank.period_us || signal.required.pin != no_pin) return bad_signal;
        signal.duration_us = cycle.settle_us;
    } else {
        if (cycle.settle_us < (bank.address ? bank.period_us : 0) || !binary(cycle.level) ||
            cycle.press_us < min_phase_us ||
            (signal.required.pin != no_pin && cycle.return_us < std::int64_t{cycle.debounce_us} + switch_sample_us))
            return bad_signal;
        auto duration = std::int64_t{cycle.settle_us} + cycle.press_us + cycle.return_us;
        if (duration > max_run_us) return bad_signal;
        signal.duration_us = static_cast<int>(duration);
    }
    if (signal.duration_us > max_run_us) return bad_signal;
    signal.cell = true;
    return accepted;
}

// The mode is followed by integers in the order documented in README.md.
// Timings are microseconds. A negative pin is only valid for an optional output.
inline int decode_signal(const int* args, std::size_t size, Signal& result) {
    if (size == 0) return bad_signal;
    Signal signal;
    switch (args[0]) {
        case pulse:
            if (size != 6) return bad_signal;
            signal.output[0] = args[1];
            signal.level = args[2];
            signal.on_us = args[3];
            signal.off_us = args[4];
            signal.repeats = args[5];
            break;
        case step_direction:
            if (size != 11 && size != 14) return bad_signal;
            signal.output[0] = args[1];
            signal.direction = args[2];
            signal.direction_level = args[3];
            signal.enable = args[4];
            signal.enable_level = args[5];
            signal.setup_us = args[6];
            signal.level = args[7];
            signal.on_us = args[8];
            signal.off_us = args[9];
            signal.repeats = args[10];
            if (!gpio_pin(signal.direction)) return bad_pin;
            if (!binary(signal.direction_level) || !binary(signal.enable_level)) return bad_signal;
            if (size == 14) signal.required = {args[11], args[12], args[13]};
            break;
        case pwm: {
            if (size != 6) return bad_signal;
            const auto* pin = board_pin(args[1]);
            if (!pin || !pin->pwm_max_period_us) return bad_pin;
            signal.output[0] = args[1];
            signal.level = args[2];
            signal.period_us = args[3];
            signal.on_us = args[4];
            signal.duration_us = args[5];
            if (signal.period_us < min_pwm_period_us || signal.period_us > pin->pwm_max_period_us ||
                signal.on_us < min_pwm_phase_us || signal.on_us > signal.period_us - min_pwm_phase_us ||
                signal.duration_us < signal.period_us || signal.duration_us > max_run_us)
                return bad_signal;
            break;
        }
        case dot_bank:
            if (size != 12 || args[7] < 0 || args[7] >= end_line) return bad_signal;
            for (std::size_t dot = 0; dot < signal.output.size(); ++dot)
                signal.output[dot] = args[dot + 1];
            signal.mask = static_cast<unsigned>(args[7]);
            signal.level = args[8];
            signal.on_us = args[9];
            signal.off_us = args[10];
            signal.repeats = args[11];
            break;
        case cell_cycle: {
            int reply = decode_cycle(args, size, signal);
            if (reply == accepted) result = signal;
            return reply;
        }
        default:
            return bad_signal;
    }

    std::uint32_t used = 0;
    auto reserve = [&used](int pin) {
        if (pin == no_pin) return true;
        if (!gpio_pin(pin)) return false;
        auto bit = std::uint32_t{1} << pin;
        if (used & bit) return false;
        used |= bit;
        return true;
    };
    for (std::size_t dot = 0; dot < signal.output.size(); ++dot) {
        if (!reserve(signal.output[dot]) || (signal.output[dot] == no_pin && (signal.mask & (1u << dot))))
            return bad_pin;
    }
    if (!reserve(signal.direction) || !reserve(signal.enable) || !reserve(signal.required.pin)) return bad_pin;
    if (!binary(signal.level) || !binary(signal.required.level) ||
        signal.required.pull < floating || signal.required.pull > pull_down) return bad_signal;
    if (!signal.period_us) {
        if (signal.on_us < min_phase_us || signal.off_us < 0 ||
            (signal.repeats > 1 && signal.off_us < min_phase_us) ||
            signal.repeats < 1 || signal.repeats > max_repeats || signal.setup_us < 0)
            return bad_signal;
        auto total = signal.setup_us + (std::int64_t{signal.on_us} + signal.off_us) * signal.repeats;
        if (total > max_run_us) return bad_signal;
        signal.duration_us = static_cast<int>(total);
    }
    result = signal;
    return accepted;
}

class SignalIo {
public:
    virtual ~SignalIo() = default;
    virtual std::uint32_t clock_us() = 0;
    virtual bool output(int pin, bool high) = 0;      // Set mode and initial level together.
    virtual bool write(int pin, bool high) = 0;
    virtual bool pwm_on(int pin, bool high, int period_us, int on_us) = 0;
    virtual bool pwm_off(int pin, bool idle_high) = 0;
    virtual int digital_input(int pin, Pull pull) = 0;
    virtual int analog_input(int pin) = 0;
    virtual bool servos_start(const ServoBank& settings) = 0;
    virtual bool servos_stop(const ServoBank& settings) = 0;
};

// Call on one thread or under the sketch's mutex. Each service() call performs
// at most one edge. Late edges keep their minimum spacing.
class Actuator {
public:
    explicit Actuator(SignalIo& io) : io_(io) {}

    int run(int id, const int* args, std::size_t size) {
        service();
        if (id <= 0) return wrong_job;
        if (id == id_) return state_;
        if (running()) return busy;
        Signal signal;
        int result = decode_signal(args, size, signal);
        if (result != accepted) return result;
        signal_ = signal;
        id_ = id;
        state_ = check_switch();
        if (!running()) return state_;
        left_ = signal_.repeats;
        phase_ = setup;
        press_prepared_ = servos_attempted_ = false;

        result = signal_.cell ? prepare_cycle() : (prepare() ? static_cast<int>(accepted) : io_error);
        if (result != accepted) {
            finish(result);
            return state_;
        }
        auto now = io_.clock_us();
        measured_from_ = now;
        lease_ = now + lease_us;
        cutoff_ = now + max_run_us;
        if (signal_.cell) {
            phase_ = settling;
            edge_ = now + signal_.cycle.settle_us;
            service();
        } else if (signal_.period_us) {
            phase_ = timer;
            edge_ = now + signal_.duration_us;
        } else {
            edge_ = now + signal_.setup_us;
            service();
        }
        return state_;
    }

    int status(int id) {
        service();
        if (id <= 0 || id != id_) return wrong_job;
        if (running()) lease_ = io_.clock_us() + lease_us;
        return state_;
    }

    int stop(int id) {
        if (id <= 0 || id != id_) return wrong_job;
        if (running()) finish(cancelled);
        return state_;
    }

    int timing(int id) const {
        return id > 0 && id == id_ && state_ == complete ? measured_us_ : wrong_job;
    }

    int read_digital(int pin, int pull) {
        service();
        if (running()) return busy;
        if (!gpio_pin(pin)) return bad_pin;
        if (pull < floating || pull > pull_down) return bad_signal;
        int level = io_.digital_input(pin, static_cast<Pull>(pull));
        return binary(level) ? level : io_error;
    }

    int read_adc(int pin) {
        service();
        if (running()) return busy;
        const auto* spec = board_pin(pin);
        if (!spec || !spec->adc) return bad_pin;
        int value = io_.analog_input(pin);
        return value >= 0 && value <= adc_max ? value : io_error;
    }

    bool running() const { return state_ == accepted; }

    // Signed distance on the wrapping clock. Each deadline is under 2^31 us away.
    static std::uint32_t remaining(std::uint32_t now, std::uint32_t deadline) {
        auto distance = deadline - now;
        return distance < 0x8000'0000u ? distance : 0;
    }

    std::uint32_t until_service(std::uint32_t now) const {
        if (!running()) return 1'000;
        auto next = remaining(now, edge_);
        auto lease = remaining(now, lease_);
        auto cutoff = remaining(now, cutoff_);
        if (lease < next) next = lease;
        if (cutoff < next) next = cutoff;
        if (sampling_return()) {
            auto sample = remaining(now, sample_);
            if (sample < next) next = sample;
        }
        return next;
    }

    void service() {
        if (!running()) return;
        auto now = io_.clock_us();
        auto at_edge = remaining(now, edge_) == 0;
        if (at_edge && (phase_ == timer || (phase_ == inactive && left_ == 0))) {
            finish(complete);
        } else if (remaining(now, lease_) == 0) {
            finish(lease_expired);
        } else if (remaining(now, cutoff_) == 0) {
            finish(time_limit);
        } else if (signal_.cell) {
            service_cycle(now, at_edge);
        } else if (at_edge && phase_ == active) {
            if (!drive(false)) finish(io_error);
            else if (--left_ == 0 && !signal_.off_us) finish(complete);
            else {
                phase_ = inactive;
                edge_ = io_.clock_us() + signal_.off_us;
            }
        } else if (at_edge) {
            int guard = check_switch();
            if (guard != accepted) finish(guard);
            else if (!drive(true)) finish(io_error);
            else {
                phase_ = active;
                edge_ = io_.clock_us() + signal_.on_us;
            }
        }
    }

private:
    int switch_level() {
        return io_.digital_input(signal_.required.pin, static_cast<Pull>(signal_.required.pull));
    }

    int check_switch() {
        if (signal_.required.pin == no_pin) return accepted;
        int value = switch_level();
        if (!binary(value)) return io_error;
        return value == signal_.required.level ? static_cast<int>(accepted) : guard_open;
    }

    int prepare_cycle() {
        const auto& cycle = signal_.cycle;
        if (cycle.trigger != no_pin) {
            press_prepared_ = true;
            if (!io_.output(cycle.trigger, !cycle.level)) return io_error;
        }
        if (!cycle.servos.address) return accepted;
        servos_attempted_ = true;
        return io_.servos_start(cycle.servos) ? static_cast<int>(accepted) : servo_io_error;
    }

    bool sampling_return() const {
        return signal_.required.pin != no_pin && (phase_ == pressing || phase_ == returning);
    }

    void service_cycle(std::uint32_t now, bool at_edge) {
        const auto& cycle = signal_.cycle;
        if (phase_ == settling) {
            if (!at_edge) return;
            if (cycle.trigger == no_pin) { finish(complete); return; }
            int guard = check_switch();
            if (guard != accepted) { finish(guard); return; }
            if (!io_.write(cycle.trigger, cycle.level)) { finish(io_error); return; }

            auto triggered = io_.clock_us();
            if (signal_.required.pin != no_pin) measured_from_ = triggered;
            phase_ = pressing;
            edge_ = triggered + cycle.press_us;
            sample_ = switch_since_ = triggered;
            sampled_up_ = stable_up_ = true;
            left_up_ = false;
            return;
        }

        if (sampling_return() && remaining(now, sample_) == 0) {
            int value = switch_level();
            if (!binary(value)) { finish(io_error); return; }
            bool up = value == signal_.required.level;
            if (up != sampled_up_) {
                sampled_up_ = up;
                switch_since_ = now;
            }
            if (now - switch_since_ >= static_cast<std::uint32_t>(cycle.debounce_us)) {
                stable_up_ = sampled_up_;
                left_up_ = left_up_ || !stable_up_;
            }
            sample_ = now + switch_sample_us;
        }
        if (phase_ == pressing && at_edge) {
            if (!io_.write(cycle.trigger, !cycle.level)) { finish(io_error); return; }
            phase_ = returning;
            edge_ = io_.clock_us() + cycle.return_us;
            return;
        }
        if (phase_ == returning) {
            if (signal_.required.pin != no_pin) {
                if (left_up_ && stable_up_ && sampled_up_) finish(complete);
                else if (at_edge) finish(press_timeout);
            } else if (at_edge) finish(complete);
        }
    }

    bool prepare() {
        if (signal_.period_us)
            return io_.pwm_on(signal_.output[0], signal_.level, signal_.period_us, signal_.on_us);
        if (signal_.enable != no_pin && !io_.output(signal_.enable, !signal_.enable_level)) return false;
        for (int pin : signal_.output)
            if (pin != no_pin && !io_.output(pin, !signal_.level)) return false;
        if (signal_.direction != no_pin && !io_.output(signal_.direction, signal_.direction_level)) return false;
        return signal_.enable == no_pin || io_.write(signal_.enable, signal_.enable_level);
    }

    bool drive(bool asserted) {
        bool ok = true;
        for (std::size_t dot = 0; dot < signal_.output.size(); ++dot)
            if (signal_.mask & (1u << dot))
                ok = io_.write(signal_.output[dot], asserted == (signal_.level != 0)) && ok;
        return ok;
    }

    void finish(int result) {
        if (result == complete) measured_us_ = static_cast<int>(io_.clock_us() - measured_from_);
        bool ok = true;
        if (signal_.cell) {
            if (press_prepared_) ok = io_.output(signal_.cycle.trigger, !signal_.cycle.level);
            if (servos_attempted_ && !io_.servos_stop(signal_.cycle.servos)) result = servo_io_error;
        } else if (signal_.period_us) {
            ok = io_.pwm_off(signal_.output[0], !signal_.level);
        } else {
            if (signal_.enable != no_pin) ok = io_.output(signal_.enable, !signal_.enable_level);
            for (int pin : signal_.output)
                if (pin != no_pin) ok = io_.output(pin, !signal_.level) && ok;
            if (signal_.direction != no_pin) ok = io_.output(signal_.direction, false) && ok;
        }
        state_ = ok ? result : io_error;
    }

    enum Phase { setup, active, inactive, timer, settling, pressing, returning } phase_ = setup;
    SignalIo& io_;
    Signal signal_;
    int id_ = 0, state_ = wrong_job, left_ = 0;
    int measured_us_ = 0;
    std::uint32_t measured_from_ = 0, edge_ = 0, cutoff_ = 0, lease_ = 0;
    std::uint32_t sample_ = 0, switch_since_ = 0;
    bool sampled_up_ = false, stable_up_ = false, left_up_ = false;
    bool press_prepared_ = false, servos_attempted_ = false;
};

}
