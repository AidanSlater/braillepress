#pragma once

#include <cstddef>

namespace embosser {

inline constexpr int no_pin = -1;
inline constexpr int servo_sda = 20, servo_scl = 21;

struct Pin {
    const char* label;
    const char* pad;
    const char* function;
    bool gpio;
    bool adc;
    int pwm_max_period_us;
};

// Arduino pin numbers on JDIGITAL and JANALOG in ArduinoCore-zephyr 1.1.0.
// D0/D1 are reserved here because the core uses USART1 for boot diagnostics.
// PWM limits fit the STM32 timers at the board's 160 MHz maximum clock;
// TIM2 is 32-bit, TIM1/8 use /64 and TIM3/4 use /5.
inline constexpr Pin pins[] = {
    {"D0",       "PB7",  "USART1 RX (boot console)", false, false, 0},
    {"D1",       "PB6",  "USART1 TX (boot console)", false, false, 0},
    {"D2",       "PB3",  "TIM2 CH2",                 true,  false, 100'000},
    {"D3",       "PB0",  "TIM3 CH3",                 true,  false, 2'000},
    {"D4",       "PA12", "FDCAN1 TX",                true,  false, 0},
    {"D5",       "PA11", "TIM1 CH4 / FDCAN1 RX",     true,  false, 25'000},
    {"D6",       "PB1",  "TIM3 CH4",                 true,  false, 2'000},
    {"D7",       "PB2",  "TIM8 CH4N",                true,  false, 25'000},
    {"D8",       "PB4",  "TIM3 CH1",                 true,  false, 2'000},
    {"D9",       "PB8",  "TIM4 CH3",                 true,  false, 2'000},
    {"D10",      "PB9",  "TIM4 CH4 / SPI CS",        true,  false, 2'000},
    {"D11",      "PB15", "TIM1 CH3N / SPI COPI",     true,  false, 25'000},
    {"D12",      "PB14", "TIM1 CH2N / SPI CIPO",     true,  false, 25'000},
    {"D13",      "PB13", "TIM1 CH1N / SPI SCK",      true,  false, 25'000},
    {"A0 / D14", "PA4",  "ADC1 IN9 / DAC0",          true,  true,  0},
    {"A1 / D15", "PA5",  "ADC1 IN10 / DAC1",         true,  true,  0},
    {"A2 / D16", "PA6",  "ADC1 IN11",                true,  true,  0},
    {"A3 / D17", "PA7",  "ADC1 IN12",                true,  true,  0},
    {"A4 / D18", "PC1",  "ADC1 IN2 / I2C3 SDA",      true,  true,  0},
    {"A5 / D19", "PC0",  "ADC1 IN1 / I2C3 SCL",      true,  true,  0},
    {"D20",      "PB11", "TIM2 CH4 / I2C2 SDA",      true,  false, 100'000},
    {"D21",      "PB10", "TIM2 CH3 / I2C2 SCL",      true,  false, 100'000},
};

inline constexpr std::size_t pin_count = sizeof(pins) / sizeof(*pins);

constexpr const Pin* board_pin(int number) {
    return number >= 0 && static_cast<std::size_t>(number) < pin_count ? &pins[number] : nullptr;
}

constexpr bool gpio_pin(int number) {
    const auto* pin = board_pin(number);
    return pin && pin->gpio;
}

}
