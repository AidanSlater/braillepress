#include <Arduino_RouterBridge.h>
#include "uno_q_io.hpp"

namespace {

embosser::DryRun job;
embosser::UnoQIo io;
embosser::Actuator actuator(io);
struct k_mutex actuator_mutex;

// Bridge.provide callbacks run on the Bridge thread. The sketch loop also
// updates the outputs so a dropped connection cannot leave a test running.
class ActuatorLock {
public:
    ActuatorLock() { k_mutex_lock(&actuator_mutex, K_FOREVER); }
    ~ActuatorLock() { k_mutex_unlock(&actuator_mutex); }
};

int braille_begin(int id, int count) { return job.begin(id, count); }
int braille_cell(int id, int sequence, int operation) { return job.cell(id, sequence, operation); }
int braille_finish(int id) { return job.finish(id); }
int braille_cancel(int id) { return job.cancel(id); }

int bench_version() { return embosser::bench_version; }

int bench_run(int id, MsgPack::arr_t<int> args) {
    ActuatorLock lock;
    return actuator.run(id, args.data(), args.size());
}

int bench_status(int id) {
    ActuatorLock lock;
    return actuator.status(id);
}

int bench_stop(int id) {
    ActuatorLock lock;
    return actuator.stop(id);
}

int bench_timing(int id) {
    ActuatorLock lock;
    return actuator.timing(id);
}

int bench_read_digital(int pin, int pull) {
    ActuatorLock lock;
    return actuator.read_digital(pin, pull);
}

int bench_read_adc(int pin) {
    ActuatorLock lock;
    return actuator.read_adc(pin);
}

}

void setup() {
    k_mutex_init(&actuator_mutex);
    analogReadResolution(12);
    Bridge.begin();
    Bridge.provide("braille_begin", braille_begin);
    Bridge.provide("braille_cell", braille_cell);
    Bridge.provide("braille_finish", braille_finish);
    Bridge.provide("braille_cancel", braille_cancel);
    Bridge.provide(embosser::version_method, bench_version);
    Bridge.provide(embosser::run_method, bench_run);
    Bridge.provide(embosser::status_method, bench_status);
    Bridge.provide(embosser::stop_method, bench_stop);
    Bridge.provide(embosser::timing_method, bench_timing);
    Bridge.provide(embosser::digital_method, bench_read_digital);
    Bridge.provide(embosser::adc_method, bench_read_adc);
}

void loop() {
    std::uint32_t wait;
    {
        ActuatorLock lock;
        actuator.service();
        wait = actuator.until_service(io.clock_us());
    }
    // Bound the busy wait to the end of a GPIO phase. Sleep whenever
    // there is time for a Zephyr tick so the Bridge can service requests.
    if (wait >= 1'000) k_msleep(1);
    else if (wait) k_busy_wait(wait);
}
