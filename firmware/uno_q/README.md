# UNO Q hardware and actuator bench

The Arduino UNO Q 4 GB (ABX00173) pairs a 64-bit Qualcomm QRB2210 running Debian
with a separate STM32U585 Cortex-M33 running Arduino on Zephyr. The 4 GB of RAM
and 32 GB eMMC serve Linux. The STM32 runs at up to 160 MHz and has 2 MB of flash
and 786 KB of SRAM. The MCU owns the shield GPIO and generates the timing.
The Arduino Router on Linux forwards MessagePack RPC from its Unix socket to
the MCU's internal LPUART1. [Arduino 4 GB specifications](https://store.arduino.cc/products/uno-q-4gb) ·
[Board datasheet](https://docs.arduino.cc/resources/datasheets/ABX00162-ABX00173-datasheet.pdf) ·
[STM32U585 datasheet](https://www.st.com/en/microcontrollers-microprocessors/stm32u585ai.html) ·
[Arduino Router](https://github.com/arduino/arduino-router)

## Pin selection

The app's **UNO Q pins** tab lists the STM32 pad and alternate functions for
each shield pin. The GUI and firmware share the [pin table](pins.hpp).

| Header pins | MCU resources | Bench use |
| --- | --- | --- |
| D0 / PB7 and D1 / PB6 | USART1 RX/TX | Reserved for the core's boot console |
| D2 / PB3, D20 / PB11, D21 / PB10 | TIM2 channels 2, 4, 3 | GPIO or hardware PWM, 20–100,000 us periods |
| D3 / PB0, D6 / PB1, D8 / PB4 | TIM3 channels 3, 4, 1 | GPIO or hardware PWM, 20–2,000 us periods |
| D9 / PB8, D10 / PB9 | TIM4 channels 3, 4 | GPIO or hardware PWM, 20–2,000 us periods |
| D5 / PA11, D11 / PB15, D12 / PB14, D13 / PB13 | TIM1 channels 4, 3N, 2N, 1N | GPIO or hardware PWM, 20–25,000 us periods |
| D7 / PB2 | TIM8 channel 4N | GPIO or hardware PWM, 20–25,000 us periods |
| D4 / PA12 | FDCAN1 TX | GPIO. D5 also carries FDCAN1 RX |
| A0–A5 / D14–D19 | ADC1. A0/A1 also route to DAC0/DAC1 | GPIO or 12-bit ADC |

PWM availability follows ArduinoCore-zephyr **1.1.0**, including additional
channels beyond the `~` marks on the header. The firmware uses the timer
driver to set the period and active time. The period ranges fit the core's
prescalers and the STM32's 16- or 32-bit counter widths. Each active and idle
portion of a PWM cycle must be at least 2 us. D2 is a starting point for a
20,000 us period and 1,500 us active time. [Core pin and timer map](https://github.com/arduino/ArduinoCore-zephyr/blob/1.1.0/variants/arduino_uno_q_stm32u585xx/arduino_uno_q_stm32u585xx.overlay) ·
[STM32 PWM driver](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/pwm/pwm_stm32.c)

D20/D21 also carry the default `Wire` bus. A4/A5 carry a separate I2C bus.
D10–D13 are SPI signals on the shield header. Use those pins for bench outputs
when the connected circuit leaves those bus lines free. The Qwiic connector
uses I2C4 on PD13/PD12. JSPI and the underside JMISC/JMEDIA headers have their
own mappings and are outside the bench pin selector. JMISC mixes 3.3 V MCU
signals, 1.8 V Qualcomm signals and analog audio. [Full board pinout](https://docs.arduino.cc/resources/pinouts/ABX00162-full-pinout.pdf) ·
[Arduino user manual](https://docs.arduino.cc/tutorials/uno-q/user-manual/)

Use 3.3 V logic on JDIGITAL and JANALOG and keep ADC signals in the 0–3.3 V
range. Drive motors and solenoids through a compatible external driver and
supply. Give driver inputs defined idle levels through reset using the
driver's wiring requirements. BOOT, RESET, AREF, power and ground are dedicated
pins. [Arduino power and I/O specifications](https://docs.arduino.cc/tutorials/uno-q/power-specification/)

## Build and simulate

From the repository root, build the STM32 sketch and run the Linux desktop
and simulation tests in Docker.

```sh
docker build --network host -f firmware/Dockerfile \
  --output type=local,dest=build/uno-q-firmware .
docker build --network host -f Dockerfile.verify \
  --build-arg "VERIFY_USER=$(id -u):$(id -g)" -t open-braille-check .
```

The firmware build pins Arduino CLI 1.6.0 and core `arduino:zephyr` 1.1.0
with board target `arduino:zephyr:unoq`. It exports the sketch artifacts locally. The verification image
runs the existing print protocol tests, signal state-machine tests, and the
offscreen Qt simulator and mock Router tests.

For a native desktop build, install CMake 3.23+, a C++20 compiler, Qt 6.4+
(Widgets and Network), and nlohmann-json 3.10+. Configure the bench independently
of the converter and printer service.

```sh
cmake -S . -B build/bench -DBRAILLE_CONVERTER=OFF -DBRAILLE_BENCH=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench --config Release
cmake --install build/bench --config Release --component Bench --prefix build/bench-app
```

| Platform | Setup and launch |
| --- | --- |
| Linux | Debian-family build packages include `qt6-base-dev` and `nlohmann-json3-dev`. Launch `build/bench-app/bin/open-braille-bench` with the distro's Qt runtime. |
| macOS | Use Xcode Command Line Tools and Qt for the laptop's architecture. With Homebrew you can pass `-DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix nlohmann-json)"` to CMake. Open `build/bench-app/open-braille-bench.app`. |
| Windows | Use MSVC and a matching Qt kit. Pass the kit's directory as `CMAKE_PREFIX_PATH`. With vcpkg, install `nlohmann-json:x64-windows` and pass vcpkg's `scripts/buildsystems/vcpkg.cmake` as `CMAKE_TOOLCHAIN_FILE`. Launch `build\bench-app\bin\open-braille-bench.exe`. |

On macOS and Windows the install step stages the Qt runtime using Qt's
deployment support. [Qt application deployment](https://doc.qt.io/qt-6/qt-generate-deploy-app-script.html)

The app starts in **Simulation**. Select a signal, choose the pins and timing,
then run it. The log shows virtual transitions with microsecond timestamps.
**Stop** cancels the current test.

### Adjust the mechanism

- **Step and direction** keeps separate X paper-feed and Y carriage settings
  during the session. Assign the driver pins, select the direction level, and
  adjust the setup delay, STEP high/low times, and step count. Enter measured
  steps per millimeter to display commanded travel. You can assign a guard
  input, such as a verified press-up switch. Firmware checks its level at
  startup and before each new pulse. The bench's maximum repeated rate is
  250 STEP pulses/s.
- **Dot servos and press** can test a PCA9685 pattern, a GPIO press trigger,
  or both. With servos selected, assign each dot a channel and separate
  extended and retracted pulse widths. Check **Raised** for each dot to
  extend. **Settle / hold time** is the delay from PWM setup to the trigger,
  or the hold time for a servo-only test. The PCA9685 uses D20/D21 and the
  selected 7-bit address, initially `0x40`. Use a dedicated board for the
  test. The firmware sets unused channels fully off and disables the
  board's outputs when the test ends. If OE is wired, the MCU drives it
  high to inhibit output before setup and at cleanup. The PWM period and
  pulse widths use the nominal PCA9685 oscillator. Measure them on the
  module for calibration. [PCA9685 data sheet](https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf)
- **GPIO press trigger** sends one active pulse to an external driver or
  trigger circuit. With a head-up switch assigned, the controller requires
  the configured up level before the test and again before the trigger.
  It looks for a debounced departure and return, and holds the servo
  pattern until return or timeout. The return timeout starts when the
  trigger goes idle. With the switch unassigned, the field sets the wait
  after the trigger. Set the pulse and wait for the actual stroke mechanism.
  Simulation can model a normal return, a head initially away, a switch
  that stays at home, or a head that leaves and stays away.
- **Digital input** can take one reading or monitor changes every 100 ms.
  It logs the time the app observed each change. The press test samples
  its assigned switch on the MCU at about 1 ms intervals and reports
  trigger-to-confirmed-return time after a successful stroke. Use the
  simulated level field to exercise the monitor before wiring a switch.

The original GPIO pulse, single-pin PWM, GPIO dot-bank pulse, and analog
input modes are available for individual component tests. Reserve D20/D21
while the PCA9685 bus is wired. Scope the motor signals and check the actual
switch polarity before testing the connected mechanism.

## Connect the board when available

Upload the `uno_q` sketch with the Arduino UNO Q platform selected. Use the
desktop app from this same version to connect to bench protocol 2. On your
laptop, open an OpenSSH session to forward a
loopback port to the board's Arduino Router socket. Replace the login and
hostname with the board's actual credentials and address.

```sh
ssh -o ExitOnForwardFailure=yes -N -T -L 127.0.0.1:8878:/var/run/arduino-router.sock arduino@uno-q.local
```

Leave the tunnel open. Select **UNO Q via SSH tunnel** in the app, leave the
port at 8878, and select **Connect**. The connection checks the firmware
version before enabling tests. The tunnel works with the Router's existing
Unix socket. [UNO Q SSH setup](https://docs.arduino.cc/tutorials/uno-q/ssh/) ·
[OpenSSH local socket forwarding](https://man.openbsd.org/ssh#L)

On first hardware use, capture a pulse and PWM on a logic analyzer before
attaching the drivers. The log in board mode reports MCU replies. The scope
checks the physical timing and polarity, including PWM pin multiplexing.

## Firmware protocol

The existing `braille_begin`, `braille_cell`, `braille_finish`, and
`braille_cancel` methods still count six-dot cells, lines and pages without
driving GPIO. The print queue continues to hold jobs for review. The bench
adds integer-valued calls on the same Arduino Bridge.

| Method | Arguments and result |
| --- | --- |
| `bench_version` | No arguments. Bench protocol version, currently 2. |
| `bench_run` | Positive run ID and an array of integers for the signal. Returns a status. Repeating the most recent ID returns its current status. |
| `bench_status` | Run ID. Returns the status and renews a running test's two-second lease. |
| `bench_stop` | Run ID. Returns the final status and drives the run's pins idle. A STEP/DIR test disables the driver first when an enable pin is assigned. |
| `bench_timing` | Run ID. Elapsed microseconds from setup to completion, or trigger to debounced head-up for a successful guarded press. Available for the most recently completed run. |
| `bench_read_digital` | Pin and pull mode (0 floating, 1 up, 2 down). Returns 0 or 1. |
| `bench_read_adc` | Analog pin number 14–19. Returns 0–4095 counts. |

The first field of the signal array is its mode. Each timing field is in
microseconds. Active and direction levels are 0 or 1.

| Mode | Remaining signal fields in wire order |
| --- | --- |
| 0 — GPIO pulse | Pin, active level, active time, idle time, repeats |
| 1 — STEP/DIR | STEP pin, DIR pin, DIR level, enable pin (−1 if unused), enable active level, setup time, STEP active level, active time, idle time, repeats. Optionally append guard pin (−1 if unused), required level, and pull mode. |
| 2 — PWM | Pin, active level, period, active time per period, run duration |
| 3 — dot bank | Six pin numbers in dot order (−1 if unassigned), six-bit mask, active level, active time, idle time, repeats |
| 4 — cell cycle | PCA9685 address (0 to skip), PWM period, OE pin (−1 if unused), six-bit extended-dot mask, six groups of channel (−1 if unassigned), extended width and retracted width, settle/hold time, GPIO trigger pin (−1 to skip), trigger level, trigger time, head-up pin (−1 if unused), head-up level, pull mode, return timeout or wait, debounce time. |

The controller permits one active signal and tests up to 30 seconds. Software
active phases and gaps between pulses are at least 2,000 us. STEP/DIR uses a
fixed rate with configurable setup delay. Software edge scheduling can run
late under Zephyr load. Each late edge starts the next interval from its
actual transition time. The simulator uses the same validation and scheduling
with an ideal clock. The GUI polls every 250 ms in board mode. If the link
drops, the STM32 loop stops the test when the lease expires.

Statuses are 0 running, 1 complete, 2 stopped, −1 busy, −2 unknown or invalid
run ID, −8 invalid or conflicting pins, −9 invalid signal, −10 GPIO/timer/ADC
error, −11 lease expired, −12 run time limit, −13 required switch level missing,
−14 press return timeout, and −15 PCA9685 communication error.
