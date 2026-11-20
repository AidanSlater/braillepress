#include "check.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <limits>

using namespace embosser;
using namespace native_test;
using bench::Simulator;
using bench::Transition;

namespace {

struct Rig {
    std::vector<Transition> trace;
    Simulator simulator{[this](const Transition& edge) { trace.push_back(edge); }};
    Actuator& actuator = simulator.actuator;

    int run(int id, std::initializer_list<int> args) {
        return actuator.run(id, args.begin(), args.size());
    }

    void level(std::size_t i, std::uint32_t when, int pin, bool high) const {
        const auto& edge = trace.at(i);
        check(edge.kind == Transition::level && edge.time_us == when && edge.pin == pin && edge.high == high,
            "unexpected GPIO transition " + std::to_string(i));
    }
};

struct ManualIo : SignalIo {
    std::uint32_t now = 0;
    bool level = false, fail_write = false;
    int initializations = 0, servo_stops = 0;

    std::uint32_t clock_us() override { return now; }
    bool output(int, bool high) override { ++initializations; level = high; return true; }
    bool write(int, bool high) override { level = high; return !fail_write; }
    bool pwm_on(int, bool, int, int) override { return false; }
    bool pwm_off(int, bool) override { return true; }
    int digital_input(int, Pull) override { return 1; }
    int analog_input(int) override { return adc_max; }
    bool servos_start(const ServoBank&) override { return false; }
    bool servos_stop(const ServoBank&) override { ++servo_stops; return true; }
};

std::array<int, cell_cycle_args> servo_press_test() {
    return {cell_cycle, 0x40, 20'000, 6, 1,
        2, 1'400, 700, -1, 0, 0, -1, 0, 0, 13, 1'600, 2'100, -1, 0, 0, -1, 0, 0,
        50'000, 5, 1, 30'000, 7, 1, pull_up, 100'000, 5'000};
}

}

TEST_CASE("active-low pulse timing, busy handling and replay") {
    Rig rig;
    check(rig.run(1, {pulse, 2, 0, 2'000, 3'000, 2}) == accepted, "start pulse");
    rig.level(0, 0, 2, true);
    rig.level(1, 0, 2, false);
    check(rig.run(1, {pulse, 2, 0, 2'000, 3'000, 2}) == accepted, "replay start");
    check_equal(rig.trace.size(), std::size_t{2}, "replay has no GPIO effect");
    check(rig.run(2, {pulse, 3, 1, 2'000, 0, 1}) == busy, "concurrent start");
    check(rig.actuator.stop(2) == wrong_job, "another run's stop");
    rig.simulator.advance(9'999);
    check(rig.actuator.status(1) == accepted, "still in last idle phase");
    rig.simulator.advance(10'000);
    check(rig.actuator.status(1) == complete, "completed pulse train");
    rig.level(2, 2'000, 2, true);
    rig.level(3, 5'000, 2, false);
    rig.level(4, 7'000, 2, true);
    check_equal(rig.trace.size(), std::size_t{5}, "only two pulses");
    check(rig.run(1, {pulse, 2, 0, 2'000, 3'000, 2}) == complete, "a completed ID cannot start again");
}

TEST_CASE("step driver setup and cancellation") {
    Rig rig;
    check(rig.run(31, {step_direction, 2, 5, 1, 6, 0, 5'000, 0, 3'000, 2'000, 10}) == accepted,
        "start step train");
    rig.level(0, 0, 6, true);
    rig.level(1, 0, 2, true);
    rig.level(2, 0, 5, true);
    rig.level(3, 0, 6, false);
    rig.simulator.advance(4'999);
    check_equal(rig.trace.size(), std::size_t{4}, "wait for direction setup");
    rig.simulator.advance(5'000);
    rig.level(4, 5'000, 2, false);
    check(rig.actuator.stop(31) == cancelled, "stop between rising and falling edges");
    rig.level(5, 5'000, 6, true);
    rig.level(6, 5'000, 2, true);
    rig.level(7, 5'000, 5, false);
    rig.simulator.advance(30'000);
    check_equal(rig.trace.size(), std::size_t{8}, "cancellation removes future edges");
    check(rig.actuator.stop(31) == cancelled, "repeated stop");
}

TEST_CASE("dot mask maps only the selected Braille dots") {
    Rig rig;
    check(rig.run(4, {dot_bank, 2, 3, 4, 5, 6, 7, 33, 1, 10'000, 0, 1}) == accepted, "start dot bank");
    for (int i = 0; i < 6; ++i) rig.level(i, 0, 2 + i, false);
    rig.level(6, 0, 2, true);
    rig.level(7, 0, 7, true);
    rig.simulator.advance(10'000);
    rig.level(8, 10'000, 2, false);
    rig.level(9, 10'000, 7, false);
    check_equal(rig.trace.size(), std::size_t{10}, "only dots 1 and 6 pulsed");
    check(rig.actuator.status(4) == complete, "complete dot test");
}

TEST_CASE("timer PWM envelope and capability limits") {
    Rig rig;
    check(rig.run(5, {pwm, 3, 1, 20'000, 1'500, 100'000}) == bad_signal, "TIM3 period too long");
    check(rig.run(5, {pwm, 14, 1, 20'000, 1'500, 100'000}) == bad_pin, "A0 is a DAC, no PWM in this core");
    check(rig.run(5, {pwm, 2, 0, 20'000, 1'500, 100'000}) == accepted, "20 ms PWM on TIM2");
    const auto& start = rig.trace.at(0);
    check(start.kind == Transition::pwm_start && start.pin == 2 && !start.high &&
        start.period_us == 20'000 && start.on_us == 1'500, "PWM settings passed to hardware");
    rig.simulator.advance(99'999);
    check(rig.actuator.status(5) == accepted, "PWM holds until duration");
    rig.simulator.advance(100'000);
    const auto& finish = rig.trace.at(1);
    check(finish.kind == Transition::pwm_stop && finish.high && finish.time_us == 100'000, "PWM returns to idle");
    check(rig.actuator.status(5) == complete, "PWM complete");
}

TEST_CASE("lease expires through clock rollover and cannot be revived") {
    Rig rig;
    auto start = std::numeric_limits<std::uint32_t>::max() - 10'000u;
    rig.simulator.advance(start);
    check(rig.run(5, {pulse, 3, 1, 6'000'000, 0, 1}) == accepted, "start near overflow");
    rig.simulator.advance(start + 1'000'000);
    check(rig.actuator.status(5) == accepted, "heartbeat after wrap");
    rig.simulator.advance(start + 1'000'000 + lease_us - 1);
    check_equal(rig.trace.size(), std::size_t{2}, "output still asserted");
    rig.simulator.advance(start + 1'000'000 + lease_us);
    rig.level(2, start + 1'000'000 + lease_us, 3, false);
    check(rig.actuator.status(5) == lease_expired, "expired output cannot be revived");
}

TEST_CASE("late software edges keep their spacing and a hard limit still applies") {
    ManualIo io;
    Actuator actuator(io);
    const int test[]{pulse, 4, 1, 10'000'000, 5'000'000, 2};
    check(actuator.run(6, test, std::size(test)) == accepted && io.level, "start large pulse");
    for (std::uint32_t now = 1'000'000; now <= max_run_us; now += 1'000'000) {
        io.now = now == 10'000'000 ? now - 1 : now;
        auto state = actuator.status(6);
        if (io.now < 10'000'000) check(io.level, "no early falling edge");
        else if (io.now < 16'000'000) check(!io.level, "late falling edge keeps the gap");
        if (io.now == 16'000'000) check(io.level, "next pulse after the full idle time");
        if (io.now == max_run_us)
            check(state == time_limit && !io.level, "hard limit stops a late run despite heartbeats");
    }
}

TEST_CASE("invalid pins, timings and reads are rejected before output changes") {
    Rig rig;
    check(rig.run(0, {pulse, 2, 1, 5'000, 0, 1}) == wrong_job, "non-positive ID");
    check(rig.run(1, {}) == bad_signal, "missing mode");
    check(rig.run(1, {pulse, 0, 1, 5'000, 0, 1}) == bad_pin, "USART1 reserved");
    check(rig.run(1, {pulse, 22, 1, 5'000, 0, 1}) == bad_pin, "pin beyond the bench header");
    check(rig.run(1, {pulse, 2, 2, 5'000, 0, 1}) == bad_signal, "non-binary polarity");
    check(rig.run(1, {pulse, 2, 1, 1, 0, 1}) == bad_signal, "pulse too short");
    check(rig.run(1, {pulse, 2, 1, 5'000, 0, 2}) == bad_signal, "missing idle time");
    check(rig.run(1, {pulse, 2, 1, max_run_us, 5'000, 10}) == bad_signal, "run too long");
    check(rig.run(1, {step_direction, 2, 3, 1, 3, 1, 0, 1, 5'000, 0, 1}) == bad_pin, "shared DIR and enable");
    check(rig.run(1, {dot_bank, 2, -1, -1, -1, -1, -1, 2, 1, 5'000, 0, 1}) == bad_pin, "selected unassigned dot");
    check(rig.run(1, {dot_bank, 2, -1, -1, -1, -1, -1, end_line, 1, 5'000, 0, 1}) == bad_signal, "line is not a dot mask");
    check(rig.actuator.read_adc(2) == bad_pin, "unsupported ADC pin");
    check(rig.actuator.read_digital(3, 5) == bad_signal, "unsupported pull setting");
    check(rig.trace.empty(), "validation leaves GPIO untouched");
    rig.simulator.input_value = 1775;
    check_equal(rig.actuator.read_adc(19), 1775, "ADC A5");
    rig.simulator.input_value = 1;
    check_equal(rig.actuator.read_digital(21, pull_up), 1, "GPIO D21");
    check(rig.run(1, {pulse, 2, 1, 5'000, 0, 1}) == accepted, "valid run after rejected commands");
    check(rig.actuator.read_digital(2, floating) == busy, "reads cannot remux active outputs");
}

TEST_CASE("failed GPIO writes still attempt to restore the idle level") {
    ManualIo io;
    io.fail_write = true;
    Actuator actuator(io);
    const int test[]{pulse, 5, 1, 5'000, 0, 1};
    check(actuator.run(1, test, std::size(test)) == io_error, "output reports the error");
    check(!io.level && io.initializations == 2, "attempted idle cleanup");
    check(actuator.stop(1) == io_error, "error is terminal");
}

TEST_CASE("PCA9685 writes selected channels using the quantized period and disables all outputs") {
    ServoBank bank;
    bank.period_us = 20'000;
    bank.extended = 1;
    bank.dots[0] = {3, 1'000, 1'500};
    bank.dots[4] = {15, 1'600, 2'100};
    std::vector<std::vector<int>> writes;
    auto write = [&](const auto& packet) {
        writes.emplace_back(packet.begin(), packet.end());
        return true;
    };
    int wait_us = 0;
    check(pca9685::start(bank, write, [&](int us) { wait_us = us; }), "configure servo board");
    const std::vector<std::vector<int>> expected{
        {0xfd, 0x10}, {0, 0x30}, {1, 0x04}, {0xfe, 121}, {0x12, 0, 0, 205, 0},
        {0x42, 0, 0, 174, 1}, {0, 0x20}, {0, 0xa0}};
    check_equal(writes, expected, "PCA9685 writes with prescale programmed during sleep");
    check_equal(wait_us, 500, "oscillator startup delay");
    check(pca9685::disable(write) && writes.back() == expected.front(), "stop sets full-off");
}

TEST_CASE("a jog checks the switch before energizing and before each new step") {
    Rig rig;
    const std::initializer_list<int> args{step_direction, 2, 3, 1, 4, 0, 5'000, 1, 2'000, 2'000, 4, 7, 1, pull_up};
    check(rig.run(1, args) == guard_open && rig.trace.empty(), "open guard blocks output setup");
    rig.simulator.input_value = 1;
    check(rig.run(2, args) == accepted, "closed guard permits jog");
    rig.simulator.advance(5'000);
    rig.level(4, 5'000, 2, true);
    rig.simulator.input_value = 0;
    rig.simulator.advance(9'000);
    rig.level(5, 7'000, 2, false);
    rig.level(6, 9'000, 4, true);
    check(rig.actuator.status(2) == guard_open, "opening the guard prevents the second step");
}

TEST_CASE("cell test holds servo positions through the stroke and measures debounced return") {
    Rig rig;
    const auto args = servo_press_test();
    rig.simulator.configure_press(5, 1, {7, 1, pull_up}, 8'000, 35'000, bench::PressResponse::normal);
    check(rig.actuator.run(1, args.data(), args.size()) == accepted, "start timed cell test");
    const auto& extended = rig.trace.at(3);
    const auto& retracted = rig.trace.at(4);
    check(extended.kind == Transition::servo_pulse && extended.pin == 2 && extended.on_us == 1'400,
        "select extended endpoint for the raised dot");
    check(retracted.kind == Transition::servo_pulse && retracted.pin == 13 && retracted.on_us == 2'100,
        "select retracted endpoint for the other dot");
    rig.simulator.advance(49'999);
    check_equal(rig.trace.size(), std::size_t{6}, "no press pulse before settling delay");
    rig.simulator.advance(119'999);
    rig.level(6, 50'000, 5, true);
    check(rig.trace.at(7).kind == Transition::switch_read && !rig.trace.at(7).high, "head leaves upper switch");
    rig.level(8, 80'000, 5, false);
    check(rig.actuator.status(1) == accepted, "servo outputs stay on until debounce completes");
    rig.simulator.advance(120'000);
    check(rig.actuator.status(1) == complete, "returned to head-up");
    check_equal(rig.actuator.timing(1), 70'000, "measured microseconds from trigger to confirmed return");
    rig.level(10, 120'000, 6, true);
    check(rig.trace.back().kind == Transition::servo_stop, "PCA9685 outputs disabled at finish");
}

TEST_CASE("cell test rejects conflicting assignments and stops on failed feedback or lost link") {
    auto args = servo_press_test();
    Signal signal;
    args[3] = servo_sda;
    check(decode_signal(args.data(), args.size(), signal) == bad_pin, "I2C pin conflict");
    args = servo_press_test();
    args[14] = 2;
    check(decode_signal(args.data(), args.size(), signal) == bad_signal, "duplicate PCA9685 channel");
    args = servo_press_test();
    args[1] = 0x70;
    check(decode_signal(args.data(), args.size(), signal) == bad_signal, "no I2C All Call address");

    args = servo_press_test();
    for (auto response : {bench::PressResponse::starts_away, bench::PressResponse::stays_up,
                         bench::PressResponse::stays_down}) {
        Rig rig;
        rig.simulator.configure_press(5, 1, {7, 1, pull_up}, 8'000, 35'000, response);
        int result = rig.actuator.run(1, args.data(), args.size());
        if (response == bench::PressResponse::starts_away) {
            check(result == guard_open && rig.trace.size() == 1, "initial guard blocks all outputs");
        } else {
            check(result == accepted, "start with head up");
            rig.simulator.advance(180'000);
            check(rig.actuator.status(1) == press_timeout, "accept only a departure and return");
            check(rig.trace.back().kind == Transition::servo_stop, "timeout disables PCA9685 outputs");
        }
    }

    Rig rig;
    args[23] = 3'000'000;
    rig.simulator.configure_press(5, 1, {7, 1, pull_up}, 8'000, 35'000, bench::PressResponse::normal);
    check(rig.actuator.run(1, args.data(), args.size()) == accepted, "start long settle");
    rig.simulator.advance(lease_us);
    check(rig.actuator.status(1) == lease_expired, "lost connection stops during settling");
    check(std::none_of(rig.trace.begin(), rig.trace.end(), [](const Transition& event) {
        return event.kind == Transition::level && event.pin == 5 && event.high;
    }), "press trigger stays idle after loss of link");
    check(rig.trace.back().kind == Transition::servo_stop, "lost connection disables servos");
}

TEST_CASE("isolated servo and press tests work and an I2C failure keeps the trigger idle") {
    auto args = servo_press_test();
    args[24] = args[27] = no_pin;
    Rig servo;
    check(servo.actuator.run(1, args.data(), args.size()) == accepted, "start servo-only hold");
    servo.simulator.advance(50'000);
    check(servo.actuator.status(1) == complete && servo.trace.back().kind == Transition::servo_stop,
        "end servo-only hold at requested time");

    args = servo_press_test();
    args[1] = args[23] = 0;
    args[3] = args[27] = no_pin;
    Rig press;
    check(press.actuator.run(1, args.data(), args.size()) == accepted, "start unguarded press pulse");
    press.level(1, 0, 5, true);
    press.simulator.advance(130'000);
    press.level(2, 30'000, 5, false);
    check(press.actuator.status(1) == complete && press.actuator.timing(1) == 130'000, "wait after pulse");

    args = servo_press_test();
    ManualIo io;
    Actuator failed(io);
    check(failed.run(1, args.data(), args.size()) == servo_io_error, "report I2C initialization failure");
    check(!io.level && io.servo_stops == 1, "keep trigger idle and attempt PCA cleanup");
}

int main(int argc, char** argv) { return native_test::run(argc, argv); }
