#include "window.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <limits>
#include <utility>

namespace bench {
namespace {

using namespace embosser;
using rpc::json;

constexpr int digital_mode = cell_cycle + 1;
constexpr int adc_mode = digital_mode + 1;
constexpr int default_port = 8878;
constexpr int trace_limit = 160;

QComboBox* pin_selector(bool allow_none = false, bool pwm_only = false, bool adc_only = false) {
    auto* choice = new QComboBox;
    if (allow_none) choice->addItem(QObject::tr("Unassigned"), no_pin);
    for (std::size_t i = 0; i < pin_count; ++i) {
        const auto& pin = pins[i];
        if (!pin.gpio || (pwm_only && !pin.pwm_max_period_us) || (adc_only && !pin.adc)) continue;
        choice->addItem(QString("%1 (%2)").arg(pin.label, pin.pad), static_cast<int>(i));
        choice->setItemData(choice->count() - 1, QString::fromLatin1(pin.function), Qt::ToolTipRole);
    }
    return choice;
}

QComboBox* levels(bool high = true) {
    auto* choice = new QComboBox;
    choice->addItems({QObject::tr("Low"), QObject::tr("High")});
    choice->setCurrentIndex(high ? 1 : 0);
    return choice;
}

QComboBox* pulls() {
    auto* choice = new QComboBox;
    choice->addItems({QObject::tr("Floating"), QObject::tr("Pull-up"), QObject::tr("Pull-down")});
    return choice;
}

QSpinBox* number(int minimum, int maximum, int value, const QString& suffix = {}, int increment = 1) {
    auto* box = new QSpinBox;
    box->setRange(minimum, maximum);
    box->setValue(value);
    box->setSuffix(suffix);
    box->setSingleStep(increment);
    box->setGroupSeparatorShown(true);
    box->setAccelerated(true);
    return box;
}

QSpinBox* microseconds(int minimum, int maximum, int value, int increment = 1'000) {
    return number(minimum, maximum, value, QObject::tr(" us"), increment);
}

QString pin_name(int number) { return QString::fromLatin1(pins[number].label); }

QString seconds(std::uint32_t time_us) {
    return QString::number(time_us / 1'000'000.0, 'f', 3) + QObject::tr(" s");
}

QString bench_error(int value) {
    switch (value) {
        case busy: return QObject::tr("The controller is busy with another test.");
        case wrong_job: return QObject::tr("The controller could not find this test.");
        case bad_pin: return QObject::tr("Choose supported, distinct pins. A PCA9685 test uses D20 and D21 for I²C.");
        case bad_signal: return QObject::tr("Check the channel assignments, timing fields and 30-second duration limit.");
        case io_error: return QObject::tr("The controller reported a GPIO, timer or ADC error.");
        case lease_expired: return QObject::tr("The controller stopped the test when the connection lease expired.");
        case time_limit: return QObject::tr("The controller stopped the test at the 30-second limit.");
        case guard_open: return QObject::tr("The required switch level was missing. Test stopped.");
        case press_timeout: return QObject::tr("The press did not leave and return to the head-up switch before the timeout.");
        case servo_io_error: return QObject::tr("PCA9685 communication failed. Check its address, power and I²C wiring.");
        default: return QObject::tr("Unexpected controller reply (%1).").arg(value);
    }
}

}

Window::Window(QWidget* parent)
    : QWidget(parent), router_(this, [this](const QString& message) { connection_failed(message); }),
      simulator_([this](const Transition& event) { record(event); }) {
    setWindowTitle(tr("Open Braille Bench"));
    resize(980, 660);
    clock_.start();

    target_ = new QComboBox;
    target_->setObjectName("target");
    target_->addItems({tr("Simulation"), tr("UNO Q via SSH tunnel")});
    port_ = number(1, 65535, default_port);
    port_->setGroupSeparatorShown(false);
    port_->setObjectName("tunnelPort");
    connect_ = new QPushButton(tr("Connect"));
    connect_->setObjectName("connectBoard");
    auto* endpoint = new QWidget;
    auto* endpoint_layout = new QHBoxLayout(endpoint);
    endpoint_layout->setContentsMargins(0, 0, 0, 0);
    endpoint_layout->addWidget(port_);
    endpoint_layout->addWidget(connect_);
    endpoint_layout->addStretch();
    connection_form_ = new QFormLayout;
    connection_form_->addRow(tr("&Target"), target_);
    connection_form_->addRow(tr("Tunnel &port"), endpoint);

    auto* tabs = new QTabWidget;
    tabs->addTab(signal_panel(), tr("Signals"));
    tabs->addTab(pin_reference(), tr("UNO Q pins"));
    status_ = new QLabel(tr("Simulation ready."));
    status_->setObjectName("status");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);

    auto* root = new QVBoxLayout(this);
    root->addLayout(connection_form_);
    root->addWidget(tabs, 1);
    root->addWidget(status_);

    poll_timer_.setTimerType(Qt::PreciseTimer);
    connect(&poll_timer_, &QTimer::timeout, this, [this] { poll(); });
    connect(target_, &QComboBox::currentIndexChanged, this, [this] {
        router_.close();
        connected_ = false;
        trace_->clear();
        status_->setText(live() ? tr("Open an SSH tunnel to the board, then connect.") : tr("Simulation ready."));
        refresh_controls();
    });
    connect(connect_, &QPushButton::clicked, this, [this] { connect_board(); });
    connect(start_, &QPushButton::clicked, this, [this] { start(); });
    connect(stop_, &QPushButton::clicked, this, [this] { stop(); });
    refresh_controls();
}

QWidget* Window::signal_panel() {
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    auto* splitter = new QSplitter;
    layout->addWidget(splitter, 1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* configuration = new QWidget;
    auto* form_layout = new QVBoxLayout(configuration);
    scroll->setWidget(configuration);
    splitter->addWidget(scroll);

    mode_ = new QComboBox;
    mode_->setObjectName("signalMode");
    mode_->addItems({tr("GPIO pulse"), tr("Step and direction"), tr("Hardware PWM"),
        tr("GPIO dot-bank pulse"), tr("Dot servos and press"), tr("Digital input"), tr("Analog input")});
    auto* selector = new QFormLayout;
    selector->addRow(tr("Si&gnal"), mode_);
    form_layout->addLayout(selector);
    forms_ = new QStackedWidget;
    form_layout->addWidget(forms_);
    form_layout->addStretch();

    auto next_form = [this] {
        auto* page = new QWidget;
        forms_->addWidget(page);
        return new QFormLayout(page);
    };
    auto timings = [this](QFormLayout* form, int on, int off, int repeats, const QString& repeat_label = {}) {
        Timings fields{microseconds(min_phase_us, max_run_us, on), microseconds(0, max_run_us, off),
            number(1, max_repeats, repeats)};
        form->addRow(tr("&Active time"), fields.on);
        form->addRow(tr("&Idle time"), fields.off);
        form->addRow(repeat_label.isEmpty() ? tr("&Repeats") : repeat_label, fields.repeats);
        connect(fields.repeats, &QSpinBox::valueChanged, fields.off, [off = fields.off](int count) {
            off->setMinimum(count > 1 ? min_phase_us : 0);
        });
        fields.off->setMinimum(repeats > 1 ? min_phase_us : 0);
        return fields;
    };

    auto* pulse_form = next_form();
    pulse_pin_ = pin_selector();
    pulse_pin_->setObjectName("pulsePin");
    pulse_level_ = levels();
    pulse_form->addRow(tr("&Output pin"), pulse_pin_);
    pulse_form->addRow(tr("Active &level"), pulse_level_);
    pulse_timing_ = timings(pulse_form, 50'000, 100'000, 3);
    pulse_timing_.on->setObjectName("pulseOn");
    pulse_timing_.off->setObjectName("pulseOff");
    pulse_timing_.repeats->setObjectName("pulseRepeats");

    auto* step_form = next_form();
    auto* axis = new QComboBox;
    axis->setObjectName("jogAxis");
    axis->addItems({tr("Generic STEP/DIR"), tr("X · Paper feed"), tr("Y · Printhead carriage")});
    step_form->addRow(tr("&Axis"), axis);
    step_pin_ = pin_selector(true);
    step_pin_->setObjectName("stepPin");
    step_pin_->setCurrentIndex(step_pin_->findData(2));
    step_level_ = levels();
    dir_pin_ = pin_selector(true);
    dir_pin_->setObjectName("directionPin");
    dir_pin_->setCurrentIndex(dir_pin_->findData(3));
    dir_level_ = levels();
    enable_pin_ = pin_selector(true);
    enable_level_ = levels(false);
    setup_us_ = microseconds(0, max_run_us, 5'000);
    step_form->addRow(tr("&STEP pin"), step_pin_);
    step_form->addRow(tr("STEP active level"), step_level_);
    step_form->addRow(tr("&DIR pin"), dir_pin_);
    step_form->addRow(tr("Direction &level"), dir_level_);
    step_form->addRow(tr("&Enable pin"), enable_pin_);
    step_form->addRow(tr("Enable &active level"), enable_level_);
    step_form->addRow(tr("Setup before STEP"), setup_us_);
    step_timing_ = timings(step_form, 5'000, 5'000, 20, tr("Ste&ps"));
    step_timing_.repeats->setObjectName("jogSteps");
    connect(enable_pin_, &QComboBox::currentIndexChanged, this, [this] {
        enable_level_->setEnabled(enable_pin_->currentData().toInt() != no_pin);
    });
    enable_level_->setEnabled(false);
    steps_per_mm_ = new QDoubleSpinBox;
    steps_per_mm_->setObjectName("stepsPerMm");
    steps_per_mm_->setRange(0, 1'000'000);
    steps_per_mm_->setDecimals(3);
    steps_per_mm_->setSpecialValueText(tr("Uncalibrated"));
    step_form->addRow(tr("Steps per &mm"), steps_per_mm_);
    jog_estimate_ = new QLabel;
    jog_estimate_->setWordWrap(true);
    step_form->addRow(jog_estimate_);
    connect(steps_per_mm_, &QDoubleSpinBox::valueChanged, this, [this] { describe_jog(); });
    for (auto* control : {step_timing_.on, step_timing_.off, step_timing_.repeats})
        connect(control, &QSpinBox::valueChanged, this, [this] { describe_jog(); });
    jog_guard_ = switch_fields(step_form, tr("Guard switch"));
    jog_guard_.pin->setObjectName("jogGuardPin");
    jog_sample_ = levels(false);
    step_form->addRow(tr("Simulated guard level"), jog_sample_);
    jog_sample_label_ = step_form->labelForField(jog_sample_);
    for (auto& settings : axes_) settings.arguments = step_arguments();
    for (int i : {1, 2}) {
        axes_[i].arguments[1] = no_pin;
        axes_[i].arguments[2] = no_pin;
    }
    connect(axis, &QComboBox::currentIndexChanged, this, [this](int index) { change_axis(index); });
    describe_jog();

    auto* pwm_form = next_form();
    pwm_pin_ = pin_selector(false, true);
    pwm_pin_->setObjectName("pwmPin");
    pwm_level_ = levels();
    period_us_ = microseconds(min_pwm_period_us, pins[2].pwm_max_period_us, 20'000, 100);
    pwm_on_us_ = microseconds(min_pwm_phase_us, period_us_->value() - min_pwm_phase_us, 1'500, 100);
    duration_us_ = microseconds(period_us_->value(), max_run_us, 500'000);
    pwm_form->addRow(tr("&PWM pin"), pwm_pin_);
    pwm_form->addRow(tr("Active &level"), pwm_level_);
    pwm_form->addRow(tr("&Period"), period_us_);
    pwm_form->addRow(tr("&Active time per period"), pwm_on_us_);
    pwm_form->addRow(tr("Run &duration"), duration_us_);
    connect(pwm_pin_, &QComboBox::currentIndexChanged, this, [this] {
        period_us_->setMaximum(pins[pwm_pin_->currentData().toInt()].pwm_max_period_us);
    });
    connect(period_us_, &QSpinBox::valueChanged, this, [this](int period) {
        pwm_on_us_->setMaximum(period - min_pwm_phase_us);
        duration_us_->setMinimum(period);
    });

    auto* dot_form = next_form();
    for (std::size_t i = 0; i < dots_.size(); ++i) {
        auto* row = new QWidget;
        auto* dot_layout = new QHBoxLayout(row);
        dot_layout->setContentsMargins(0, 0, 0, 0);
        dots_[i] = pin_selector(true);
        dots_[i]->setAccessibleName(tr("Dot %1 output pin").arg(i + 1));
        selected_dots_[i] = new QCheckBox(tr("Pulse"));
        selected_dots_[i]->setAccessibleName(tr("Pulse dot %1").arg(i + 1));
        selected_dots_[i]->setChecked(i == 0);
        dot_layout->addWidget(dots_[i], 1);
        dot_layout->addWidget(selected_dots_[i]);
        dot_form->addRow(tr("Dot %1").arg(i + 1), row);
    }
    dot_level_ = levels();
    dot_form->addRow(tr("Active &level"), dot_level_);
    dot_timing_ = timings(dot_form, 50'000, 100'000, 1);

    cell_form(next_form());

    auto* digital_form = next_form();
    digital_.pin = pin_selector();
    pull_ = pulls();
    digital_.sample = number(0, 1, 0);
    digital_.sample->setObjectName("digitalSample");
    digital_form->addRow(tr("&Input pin"), digital_.pin);
    digital_form->addRow(tr("Internal &pull"), pull_);
    digital_form->addRow(tr("&Simulated level"), digital_.sample);
    digital_.sample_label = digital_form->labelForField(digital_.sample);
    monitor_input_ = new QCheckBox(tr("Monitor changes"));
    monitor_input_->setObjectName("monitorInput");
    digital_form->addRow(monitor_input_);
    connect(monitor_input_, &QCheckBox::toggled, this, [this] { refresh_controls(); });

    auto* analog_form = next_form();
    analog_.pin = pin_selector(false, false, true);
    analog_.sample = number(0, adc_max, 2048);
    analog_form->addRow(tr("&ADC pin"), analog_.pin);
    analog_form->addRow(tr("&Simulated counts"), analog_.sample);
    analog_.sample_label = analog_form->labelForField(analog_.sample);

    auto* observations = new QWidget;
    auto* observation_layout = new QVBoxLayout(observations);
    observation_layout->setContentsMargins(8, 0, 0, 0);
    auto* trace_label = new QLabel(tr("Test &log"));
    trace_ = new QPlainTextEdit;
    trace_->setObjectName("trace");
    trace_->setReadOnly(true);
    trace_->setPlaceholderText(tr("Run a signal to see the simulation trace or controller status."));
    trace_label->setBuddy(trace_);
    observation_layout->addWidget(trace_label);
    observation_layout->addWidget(trace_);
    splitter->addWidget(observations);
    splitter->setSizes({470, 480});

    auto* actions = new QHBoxLayout;
    start_ = new QPushButton;
    start_->setObjectName("runSignal");
    stop_ = new QPushButton(tr("Stop"));
    stop_->setObjectName("stopSignal");
    actions->addWidget(start_);
    actions->addWidget(stop_);
    actions->addStretch();
    layout->addLayout(actions);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        forms_->setCurrentIndex(index);
        refresh_controls();
    });
    return panel;
}

SwitchInput Window::SwitchFields::settings() const {
    return {pin->currentData().toInt(), level->currentIndex(), pull->currentIndex()};
}

Window::SwitchFields Window::switch_fields(QFormLayout* form, const QString& name) {
    SwitchFields fields{pin_selector(true), levels(false), pulls()};
    fields.pull->setCurrentIndex(pull_up);
    form->addRow(name, fields.pin);
    form->addRow(tr("Required switch level"), fields.level);
    form->addRow(tr("Switch pull"), fields.pull);
    fields.level->setEnabled(false);
    fields.pull->setEnabled(false);
    connect(fields.pin, &QComboBox::currentIndexChanged, this, [fields] {
        bool active = fields.settings().pin != no_pin;
        fields.level->setEnabled(active);
        fields.pull->setEnabled(active);
    });
    return fields;
}

void Window::cell_form(QFormLayout* form) {
    servo_bank_ = new QGroupBox(tr("PCA9685 dot servos"));
    servo_bank_->setObjectName("useServoBank");
    servo_bank_->setCheckable(true);
    auto* servo_form = new QFormLayout(servo_bank_);
    servo_address_ = number(servo_default_address, 0x77, servo_default_address);
    servo_address_->setDisplayIntegerBase(16);
    servo_address_->setPrefix("0x");
    servo_period_ = microseconds(servo_min_period_us, servo_max_period_us, 20'000, 100);
    servo_oe_ = pin_selector(true);
    servo_form->addRow(tr("I²C &address"), servo_address_);
    servo_form->addRow(tr("PWM &period"), servo_period_);
    servo_form->addRow(tr("OE &pin"), servo_oe_);
    servo_oe_->setToolTip(tr("Optional active-low output enable. D20 and D21 carry I²C."));

    auto* dots = new QWidget;
    auto* grid = new QGridLayout(dots);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(new QLabel(tr("Dot")), 0, 0);
    grid->addWidget(new QLabel(tr("Channel")), 0, 1);
    grid->addWidget(new QLabel(tr("Extend (us)")), 0, 2);
    grid->addWidget(new QLabel(tr("Retract (us)")), 0, 3);
    grid->addWidget(new QLabel(tr("Raised")), 0, 4);
    for (std::size_t i = 0; i < servos_.size(); ++i) {
        auto& fields = servos_[i];
        int row = static_cast<int>(i) + 1;
        fields.channel = number(-1, servo_channels - 1, i == 0 ? 0 : -1);
        fields.channel->setSpecialValueText(tr("Off"));
        fields.extend = number(1, servo_period_->value() - 1, 1'500, {}, 10);
        fields.retract = number(1, servo_period_->value() - 1, 1'500, {}, 10);
        fields.extended = new QCheckBox;
        fields.channel->setObjectName(QStringLiteral("servoChannel%1").arg(row));
        fields.extend->setObjectName(QStringLiteral("servoExtend%1").arg(row));
        fields.retract->setObjectName(QStringLiteral("servoRetract%1").arg(row));
        fields.extended->setObjectName(QStringLiteral("servoRaised%1").arg(row));
        fields.channel->setAccessibleName(tr("Dot %1 PCA9685 channel").arg(row));
        fields.extend->setAccessibleName(tr("Dot %1 extended pulse in microseconds").arg(row));
        fields.retract->setAccessibleName(tr("Dot %1 retracted pulse in microseconds").arg(row));
        fields.extended->setAccessibleName(tr("Raise dot %1 in the test cell").arg(row));
        fields.extended->setChecked(i == 0);
        grid->addWidget(new QLabel(QString::number(row)), row, 0);
        grid->addWidget(fields.channel, row, 1);
        grid->addWidget(fields.extend, row, 2);
        grid->addWidget(fields.retract, row, 3);
        grid->addWidget(fields.extended, row, 4, Qt::AlignCenter);
        auto assigned = [fields](int channel) {
            fields.extend->setEnabled(channel >= 0);
            fields.retract->setEnabled(channel >= 0);
            fields.extended->setEnabled(channel >= 0);
        };
        connect(fields.channel, &QSpinBox::valueChanged, this, assigned);
        assigned(fields.channel->value());
    }
    connect(servo_period_, &QSpinBox::valueChanged, this, [this](int period) {
        for (const auto& fields : servos_) {
            fields.extend->setMaximum(period - 1);
            fields.retract->setMaximum(period - 1);
        }
    });
    servo_form->addRow(dots);
    form->addRow(servo_bank_);

    settle_ = microseconds(0, max_run_us, 500'000);
    settle_->setObjectName("cellSettle");
    form->addRow(tr("Settle / hold &time"), settle_);
    settle_->setToolTip(tr("Delay before the press trigger, or how long the servos hold a dot pattern by themselves."));

    press_ = new QGroupBox(tr("GPIO press trigger"));
    press_->setObjectName("usePress");
    press_->setCheckable(true);
    press_->setChecked(false);
    auto* press_form = new QFormLayout(press_);
    press_pin_ = pin_selector(true);
    press_pin_->setObjectName("pressPin");
    press_level_ = levels();
    press_on_ = microseconds(min_phase_us, max_run_us, 50'000);
    press_on_->setObjectName("pressPulse");
    press_form->addRow(tr("Trigger &pin"), press_pin_);
    press_form->addRow(tr("Trigger active &level"), press_level_);
    press_form->addRow(tr("Trigger &duration"), press_on_);
    head_up_ = switch_fields(press_form, tr("Head-up switch"));
    head_up_.pin->setObjectName("headUpPin");
    press_return_ = microseconds(0, max_run_us, 500'000);
    press_return_->setObjectName("pressReturn");
    press_debounce_ = microseconds(0, max_run_us, 5'000);
    press_form->addRow(tr("Return &timeout / wait"), press_return_);
    press_form->addRow(tr("Switch &debounce"), press_debounce_);
    press_return_->setToolTip(tr("Time allowed for the switch to return after the pulse. With the switch unassigned this is a timed wait."));
    press_debounce_->setEnabled(false);
    connect(head_up_.pin, &QComboBox::currentIndexChanged, this, [this] {
        press_debounce_->setEnabled(head_up_.settings().pin != no_pin);
    });

    press_simulation_ = new QGroupBox(tr("Simulated switch response"));
    auto* simulation = new QFormLayout(press_simulation_);
    simulated_leave_ = microseconds(0, max_run_us, 10'000);
    simulated_return_ = microseconds(0, max_run_us, 100'000);
    press_response_ = new QComboBox;
    press_response_->setObjectName("pressResponse");
    press_response_->addItems({tr("Leaves and returns"), tr("Starts away from home"),
        tr("Stays at home"), tr("Leaves and stays away")});
    simulation->addRow(tr("Switch behavior"), press_response_);
    simulation->addRow(tr("Leave delay from trigger"), simulated_leave_);
    simulation->addRow(tr("Return delay from release"), simulated_return_);
    press_form->addRow(press_simulation_);
    form->addRow(press_);
}

std::vector<int> Window::step_arguments() const {
    auto input = jog_guard_.settings();
    return {step_direction, step_pin_->currentData().toInt(), dir_pin_->currentData().toInt(),
        dir_level_->currentIndex(), enable_pin_->currentData().toInt(), enable_level_->currentIndex(),
        setup_us_->value(), step_level_->currentIndex(), step_timing_.on->value(), step_timing_.off->value(),
        step_timing_.repeats->value(), input.pin, input.level, input.pull};
}

void Window::change_axis(int index) {
    axes_[axis_] = {step_arguments(), steps_per_mm_->value(), jog_sample_->currentIndex()};
    axis_ = index;
    const auto& settings = axes_[axis_];
    const auto& args = settings.arguments;
    step_pin_->setCurrentIndex(step_pin_->findData(args[1]));
    dir_pin_->setCurrentIndex(dir_pin_->findData(args[2]));
    dir_level_->setCurrentIndex(args[3]);
    enable_pin_->setCurrentIndex(enable_pin_->findData(args[4]));
    enable_level_->setCurrentIndex(args[5]);
    setup_us_->setValue(args[6]);
    step_level_->setCurrentIndex(args[7]);
    step_timing_.on->setValue(args[8]);
    step_timing_.repeats->setValue(args[10]);
    step_timing_.off->setValue(args[9]);
    jog_guard_.pin->setCurrentIndex(jog_guard_.pin->findData(args[11]));
    jog_guard_.level->setCurrentIndex(args[12]);
    jog_guard_.pull->setCurrentIndex(args[13]);
    steps_per_mm_->setValue(settings.steps_per_mm);
    jog_sample_->setCurrentIndex(settings.simulated_level);
}

void Window::describe_jog() {
    int steps = step_timing_.repeats->value();
    auto message = steps > 1 ? tr("Requested rate %1 steps/s.").arg(QString::number(
        1'000'000.0 / (step_timing_.on->value() + step_timing_.off->value()), 'f', 1)) : tr("One STEP pulse.");
    if (double scale = steps_per_mm_->value(); scale > 0)
        message += tr(" Commanded travel %1 mm.").arg(QString::number(steps / scale, 'f', 3));
    jog_estimate_->setText(message);
}

QWidget* Window::pin_reference() {
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    auto* heading = new QLabel(tr("STM32U585 shield headers on the UNO Q 4 GB. Signal levels are 3.3 V."));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto* table = new QTableWidget(static_cast<int>(pin_count), 4);
    table->setObjectName("pinReference");
    table->setAccessibleName(tr("UNO Q shield header pins"));
    table->setHorizontalHeaderLabels({tr("Header pin"), tr("MCU pad"), tr("Bench functions"), tr("Peripheral functions")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (std::size_t i = 0; i < pin_count; ++i) {
        const auto& pin = pins[i];
        QStringList functions;
        if (pin.gpio) functions << tr("GPIO");
        if (pin.adc) functions << tr("ADC");
        if (pin.pwm_max_period_us) functions << tr("PWM");
        table->setItem(i, 0, new QTableWidgetItem(pin_name(static_cast<int>(i))));
        table->setItem(i, 1, new QTableWidgetItem(QString::fromLatin1(pin.pad)));
        table->setItem(i, 2, new QTableWidgetItem(functions.empty() ? tr("Reserved") : functions.join(", ")));
        table->setItem(i, 3, new QTableWidgetItem(QString::fromLatin1(pin.function)));
    }
    layout->addWidget(table);
    return panel;
}

bool Window::live() const { return target_->currentIndex() == 1; }
bool Window::measuring() const { return mode_->currentIndex() >= digital_mode; }

void Window::refresh_controls() {
    bool editable = !run_id_ && !reading_ && !connecting_ && !monitoring_;
    bool can_send = editable && (!live() || connected_);
    target_->setEnabled(editable);
    port_->setEnabled(editable && !connected_);
    connect_->setEnabled(editable);
    connect_->setText(connected_ ? tr("Disconnect") : tr("Connect"));
    connection_form_->setRowVisible(1, live());
    mode_->setEnabled(editable);
    forms_->setEnabled(editable || monitoring_);
    digital_.pin->setEnabled(!monitoring_);
    pull_->setEnabled(!monitoring_);
    monitor_input_->setEnabled(!monitoring_);
    digital_.sample->setVisible(!live());
    digital_.sample_label->setVisible(!live());
    analog_.sample->setVisible(!live());
    analog_.sample_label->setVisible(!live());
    jog_sample_->setVisible(!live());
    jog_sample_label_->setVisible(!live());
    press_simulation_->setVisible(!live());
    start_->setEnabled(can_send);
    if (measuring()) {
        bool watch = mode_->currentIndex() == digital_mode && monitor_input_->isChecked();
        start_->setText(watch ? tr("Monitor input") : tr("Read input"));
    } else start_->setText(live() ? tr("Run on UNO Q") : tr("Run simulation"));
    stop_->setEnabled(monitoring_ || (run_id_ && !stopping_));
}

void Window::connect_board() {
    if (connected_) {
        router_.close();
        connected_ = false;
        status_->setText(tr("Disconnected."));
        refresh_controls();
        return;
    }
    connecting_ = true;
    status_->setText(tr("Checking the UNO Q firmware through the SSH tunnel."));
    refresh_controls();
    router_.open(port_->value(), [this] {
        router_.call(version_method, json::array(), [this](int version) {
            if (version != bench_version) {
                router_.close();
                connection_failed(tr("The UNO Q reported incompatible bench firmware (%1).").arg(version));
                return;
            }
            connected_ = true;
            connecting_ = false;
            status_->setText(tr("UNO Q connected. Run sends the selected signal to the board."));
            refresh_controls();
        });
    });
}

void Window::connection_failed(const QString& message) {
    auto notice = tr("UNO Q connection failed. %1").arg(message);
    if (run_id_) {
        notice += tr(" The firmware turns outputs idle when its two-second lease expires.");
        trace_->appendPlainText(notice);
    }
    status_->setText(notice);
    run_id_ = 0;
    connected_ = connecting_ = reading_ = stopping_ = monitoring_ = false;
    poll_timer_.stop();
    refresh_controls();
    if (closing_) QTimer::singleShot(0, this, [this] { close(); });
}

std::vector<int> Window::signal_arguments() const {
    auto pin = [](QComboBox* field) { return field->currentData().toInt(); };
    switch (mode_->currentIndex()) {
        case pulse:
            return {pulse, pin(pulse_pin_), pulse_level_->currentIndex(),
                pulse_timing_.on->value(), pulse_timing_.off->value(), pulse_timing_.repeats->value()};
        case step_direction:
            return step_arguments();
        case pwm:
            return {pwm, pin(pwm_pin_), pwm_level_->currentIndex(),
                period_us_->value(), pwm_on_us_->value(), duration_us_->value()};
        case dot_bank: {
            std::vector<int> args{dot_bank};
            int mask = 0;
            for (std::size_t i = 0; i < dots_.size(); ++i) {
                args.push_back(pin(dots_[i]));
                if (selected_dots_[i]->isChecked()) mask |= 1 << i;
            }
            args.insert(args.end(), {mask, dot_level_->currentIndex(),
                dot_timing_.on->value(), dot_timing_.off->value(), dot_timing_.repeats->value()});
            return args;
        }
        case cell_cycle: {
            bool servos = servo_bank_->isChecked();
            bool press = press_->isChecked();
            auto input = press ? head_up_.settings() : SwitchInput{};
            std::vector<int> args{cell_cycle, servos ? servo_address_->value() : 0,
                servos ? servo_period_->value() : 0, servos ? pin(servo_oe_) : no_pin, 0};
            for (std::size_t i = 0; i < servos_.size(); ++i) {
                const auto& fields = servos_[i];
                int channel = servos ? fields.channel->value() : -1;
                args.insert(args.end(), {channel, fields.extend->value(), fields.retract->value()});
                if (channel >= 0 && fields.extended->isChecked()) args[4] |= 1 << i;
            }
            args.insert(args.end(), {settle_->value(), press ? pin(press_pin_) : no_pin,
                press_level_->currentIndex(), press_on_->value(), input.pin, input.level, input.pull,
                press_return_->value(), press_debounce_->value()});
            return args;
        }
        default:
            return {};
    }
}

void Window::start() {
    if (measuring()) {
        monitoring_ = mode_->currentIndex() == digital_mode && monitor_input_->isChecked();
        if (monitoring_) {
            trace_->clear();
            last_sample_ = -1;
            started_us_ = static_cast<std::uint32_t>(clock_.nsecsElapsed() / 1'000);
            poll_timer_.start(100);
        }
        read_input();
        return;
    }
    if (mode_->currentIndex() == cell_cycle && press_->isChecked() && press_pin_->currentData().toInt() == no_pin) {
        status_->setText(tr("Choose a GPIO press trigger pin."));
        return;
    }
    Signal signal;
    auto args = signal_arguments();
    auto result = decode_signal(args.data(), args.size(), signal);
    if (result != accepted) {
        status_->setText(bench_error(result));
        return;
    }
    trace_->clear();
    timing_press_return_ = signal.cell && signal.required.pin != no_pin;
    trace_->appendPlainText((timing_press_return_ ? tr("Configured maximum duration %1") : tr("Requested duration %1"))
        .arg(seconds(signal.duration_us)));
    events_ = 0;
    started_us_ = static_cast<std::uint32_t>(clock_.nsecsElapsed() / 1'000);
    run_id_ = QRandomGenerator::global()->bounded(1, std::numeric_limits<int>::max());
    refresh_controls();
    poll_timer_.start(live() ? 250 : 50);
    if (live()) {
        status_->setText(tr("Sending the signal settings to the UNO Q."));
        router_.call(run_method, json::array({run_id_, args}), [this](int reply) { update_run(reply); });
    } else {
        simulator_.clear_press();
        simulator_.input_value = jog_sample_->currentIndex();
        if (signal.cell && signal.required.pin != no_pin) {
            const auto& cycle = signal.cycle;
            simulator_.configure_press(cycle.trigger, cycle.level, signal.required, simulated_leave_->value(),
                simulated_return_->value(), static_cast<PressResponse>(press_response_->currentIndex()));
        }
        simulator_.advance(started_us_);
        update_run(simulator_.actuator.run(run_id_, args.data(), args.size()));
    }
}

void Window::read_input() {
    bool digital = mode_->currentIndex() == digital_mode;
    const auto& input = digital ? digital_ : analog_;
    int pin = input.pin->currentData().toInt();
    int pull = pull_->currentIndex();
    bool watch = monitoring_;
    reading_ = true;
    if (!watch) status_->setText(tr("Reading %1.").arg(pin_name(pin)));
    refresh_controls();
    auto report = [this, pin, digital, watch](int value) {
        reading_ = false;
        if (watch && !monitoring_) { refresh_controls(); return; }
        QString message;
        bool valid = value >= 0 && value <= (digital ? 1 : adc_max);
        if (!valid) message = value < 0 ? bench_error(value) : tr("The controller returned an invalid sample.");
        else if (digital) message = tr("%1 read %2.").arg(pin_name(pin), value ? tr("high") : tr("low"));
        else message = tr("%1 read %2 of %3 ADC counts.").arg(pin_name(pin)).arg(value).arg(adc_max);
        if (watch && valid) {
            auto elapsed = static_cast<std::uint32_t>(clock_.nsecsElapsed() / 1'000) - started_us_;
            if (value != last_sample_) trace_->appendPlainText(tr("%1  %2").arg(seconds(elapsed), message));
            last_sample_ = value;
        } else {
            trace_->appendPlainText(message);
            if (watch) {
                monitoring_ = false;
                poll_timer_.stop();
            }
        }
        status_->setText(message);
        refresh_controls();
    };
    if (live()) {
        if (digital) router_.call(digital_method, json::array({pin, pull}), report);
        else router_.call(adc_method, json::array({pin}), report);
    } else {
        simulator_.clear_press();
        simulator_.input_value = input.sample->value();
        report(digital ? simulator_.actuator.read_digital(pin, pull) : simulator_.actuator.read_adc(pin));
    }
}

void Window::poll() {
    if (monitoring_) {
        if (!reading_) read_input();
        return;
    }
    if (!run_id_) return;
    if (live()) {
        if (router_.waiting()) return;
        router_.call(stopping_ ? stop_method : status_method, json::array({run_id_}),
            [this](int result) { update_run(result); });
    } else {
        simulator_.advance(static_cast<std::uint32_t>(clock_.nsecsElapsed() / 1'000));
        update_run(stopping_ ? simulator_.actuator.stop(run_id_) : simulator_.actuator.status(run_id_));
    }
}

void Window::stop() {
    if (monitoring_) {
        monitoring_ = false;
        poll_timer_.stop();
        status_->setText(tr("Input monitor stopped."));
        refresh_controls();
        return;
    }
    if (!run_id_ || stopping_) return;
    stopping_ = true;
    status_->setText(tr("Stopping the test."));
    refresh_controls();
    poll();
}

void Window::update_run(int result) {
    if (result == accepted) {
        if (stopping_) poll();
        else status_->setText(live() ? tr("Running on the UNO Q.") : tr("Running simulation."));
        return;
    }
    auto message = result == complete ? tr("Test complete.") :
        result == cancelled ? tr("Test stopped.") : bench_error(result);
    trace_->appendPlainText(message);
    status_->setText(message);
    int id = std::exchange(run_id_, 0);
    stopping_ = false;
    poll_timer_.stop();
    if (result == complete && !closing_) {
        if (live()) {
            reading_ = true;
            router_.call(timing_method, json::array({id}), [this](int elapsed) {
                report_timing(elapsed);
                reading_ = false;
                refresh_controls();
            });
        } else report_timing(simulator_.actuator.timing(id));
    }
    refresh_controls();
    if (closing_) QTimer::singleShot(0, this, [this] { close(); });
}

void Window::report_timing(int elapsed) {
    if (elapsed < 0) {
        trace_->appendPlainText(tr("Controller timing unavailable. %1").arg(bench_error(elapsed)));
        return;
    }
    auto context = live() ? tr("Controller") : tr("Simulation");
    auto message = timing_press_return_ ? tr("%1 measured %2 us from trigger to confirmed head-up.") :
        tr("%1 elapsed time %2 us.");
    trace_->appendPlainText(message.arg(context).arg(elapsed));
}

void Window::record(const Transition& event) {
    if (events_++ == trace_limit) trace_->appendPlainText(tr("Trace display limit reached."));
    if (events_ > trace_limit) return;
    auto level = event.high ? tr("high") : tr("low");
    QString source, message;
    if (event.kind == Transition::servo_pulse || event.kind == Transition::servo_stop) {
        source = tr("PCA9685 0x%1").arg(event.address, 2, 16, QLatin1Char('0'));
        message = event.kind == Transition::servo_stop ? tr("outputs disabled") :
            tr("channel %1, period %2 us, pulse %3 us").arg(event.pin).arg(event.period_us).arg(event.on_us);
    } else {
        source = pin_name(event.pin);
        message = event.kind == Transition::switch_read ? tr("switch %1").arg(level) : level;
    }
    if (event.kind == Transition::pwm_start)
        message = tr("PWM active %1, period %2 us, active time %3 us").arg(level).arg(event.period_us).arg(event.on_us);
    else if (event.kind == Transition::pwm_stop)
        message = tr("PWM stopped, idle %1").arg(level);
    trace_->appendPlainText(tr("%1 us  %2  %3").arg(event.time_us - started_us_).arg(source, message));
}

void Window::closeEvent(QCloseEvent* event) {
    if (live() && run_id_) {
        event->ignore();
        closing_ = true;
        stop();
    } else {
        router_.close();
        event->accept();
    }
}

}
