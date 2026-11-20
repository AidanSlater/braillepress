#pragma once

#include "router.hpp"
#include "simulator.hpp"

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace bench {

class Window final : public QWidget {
public:
    explicit Window(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Timings {
        QSpinBox *on, *off, *repeats;
    };
    struct Input {
        QComboBox* pin;
        QSpinBox* sample;
        QWidget* sample_label;
    };
    struct SwitchFields {
        QComboBox *pin, *level, *pull;
        embosser::SwitchInput settings() const;
    };
    struct AxisSettings {
        std::vector<int> arguments;
        double steps_per_mm = 0;
        int simulated_level = 0;
    };
    struct ServoFields {
        QSpinBox *channel, *extend, *retract;
        QCheckBox* extended;
    };

    QWidget* signal_panel();
    QWidget* pin_reference();
    void cell_form(QFormLayout* form);
    SwitchFields switch_fields(QFormLayout* form, const QString& name);
    std::vector<int> step_arguments() const;
    void change_axis(int index);
    void describe_jog();
    std::vector<int> signal_arguments() const;
    bool live() const;
    bool measuring() const;
    void refresh_controls();
    void connect_board();
    void connection_failed(const QString& message);
    void start();
    void read_input();
    void poll();
    void stop();
    void update_run(int result);
    void report_timing(int elapsed);
    void record(const Transition& event);

    Router router_;
    Simulator simulator_;
    QElapsedTimer clock_;
    QTimer poll_timer_;
    QComboBox *target_, *mode_;
    QSpinBox* port_;
    QFormLayout* connection_form_;
    QPushButton *connect_, *start_, *stop_;
    QStackedWidget* forms_;
    QLabel* status_;
    QPlainTextEdit* trace_;

    QComboBox *pulse_pin_, *pulse_level_;
    Timings pulse_timing_;
    QComboBox *step_pin_, *step_level_, *dir_pin_, *dir_level_, *enable_pin_, *enable_level_;
    QSpinBox* setup_us_;
    Timings step_timing_;
    QDoubleSpinBox* steps_per_mm_;
    QLabel* jog_estimate_;
    SwitchFields jog_guard_;
    QComboBox* jog_sample_;
    QWidget* jog_sample_label_;
    std::array<AxisSettings, 3> axes_;
    int axis_ = 0;
    QComboBox *pwm_pin_, *pwm_level_;
    QSpinBox *period_us_, *pwm_on_us_, *duration_us_;
    std::array<QComboBox*, 6> dots_;
    std::array<QCheckBox*, 6> selected_dots_;
    QComboBox* dot_level_;
    Timings dot_timing_;
    QGroupBox *servo_bank_, *press_, *press_simulation_;
    QSpinBox *servo_address_, *servo_period_, *settle_, *press_on_, *press_return_, *press_debounce_;
    QSpinBox *simulated_leave_, *simulated_return_;
    QComboBox *servo_oe_, *press_pin_, *press_level_, *press_response_;
    SwitchFields head_up_;
    std::array<ServoFields, 6> servos_;
    Input digital_, analog_;
    QComboBox* pull_;
    QCheckBox* monitor_input_;

    int run_id_ = 0, events_ = 0;
    std::uint32_t started_us_ = 0;
    int last_sample_ = -1;
    bool connected_ = false, connecting_ = false, reading_ = false, stopping_ = false, closing_ = false;
    bool monitoring_ = false, timing_press_return_ = false;
};

}
