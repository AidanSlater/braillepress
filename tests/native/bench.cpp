#include "window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHostAddress>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QtTest>

namespace {

using embosser::rpc::json;

class BoardStub : public QObject {
public:
    explicit BoardStub(int version = embosser::bench_version) : version_(version) {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this] { respond(); });
        });
    }

    QTcpServer server;
    QTcpSocket* socket = nullptr;
    std::vector<json> commands;
    int measured_us = -1;

private:
    void respond() {
        input_ += socket->readAll();
        auto call = json::from_msgpack(input_.cbegin(), input_.cend(), true, false);
        if (call.is_discarded()) return;
        input_.clear();
        commands.push_back(call);
        auto method = call.at(2).get<std::string>();
        int reply = embosser::accepted;
        if (method == embosser::version_method) reply = version_;
        else if (method == embosser::stop_method) reply = embosser::cancelled;
        else if (method == embosser::adc_method) reply = 2345;
        else if (method == embosser::status_method && measured_us >= 0) reply = embosser::complete;
        else if (method == embosser::timing_method) reply = measured_us;
        auto bytes = json::to_msgpack(json::array({1, call.at(1), nullptr, reply}));
        QByteArray block(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // The router's stream can split even short MessagePack replies.
        socket->write(block.left(2));
        QTimer::singleShot(15, socket, [peer = socket, block] { peer->write(block.mid(2)); });
    }

    int version_;
    QByteArray input_;
};

template<class Widget>
Widget* field(bench::Window& window, const char* name) {
    auto* child = window.findChild<Widget*>(name);
    if (!child) qFatal("Missing bench widget %s", name);
    return child;
}

class BenchTest : public QObject {
    Q_OBJECT

private slots:
    void pulse_simulation() {
        bench::Window window;
        window.show();
        auto* pin = field<QComboBox>(window, "pulsePin");
        QCOMPARE(pin->findData(0), -1);
        QCOMPARE(pin->findData(1), -1);
        pin->setCurrentIndex(pin->findData(13));
        field<QSpinBox>(window, "pulseOn")->setValue(4'000);
        field<QSpinBox>(window, "pulseOff")->setValue(3'000);
        field<QSpinBox>(window, "pulseRepeats")->setValue(2);
        QTest::mouseClick(field<QPushButton>(window, "runSignal"), Qt::LeftButton);
        auto* status = field<QLabel>(window, "status");
        QTRY_COMPARE(status->text(), QString("Test complete."));
        const auto log = field<QPlainTextEdit>(window, "trace")->toPlainText();
        QVERIFY(log.contains("0 us  D13  high"));
        QVERIFY(log.contains("4000 us  D13  low"));
        QVERIFY(log.contains("7000 us  D13  high"));
        QVERIFY(log.contains("11000 us  D13  low"));
    }

    void read_adc_in_simulation() {
        bench::Window window;
        window.show();
        auto* mode = field<QComboBox>(window, "signalMode");
        mode->setCurrentIndex(mode->findText("Analog input"));
        QTest::mouseClick(field<QPushButton>(window, "runSignal"), Qt::LeftButton);
        QCOMPARE(field<QLabel>(window, "status")->text(), QString("A0 / D14 read 2048 of 4095 ADC counts."));
    }

    void axis_settings_stay_separate() {
        bench::Window window;
        window.show();
        auto* mode = field<QComboBox>(window, "signalMode");
        mode->setCurrentIndex(mode->findText("Step and direction"));
        auto* axis = field<QComboBox>(window, "jogAxis");
        auto* step = field<QComboBox>(window, "stepPin");
        auto* direction = field<QComboBox>(window, "directionPin");
        auto* steps = field<QSpinBox>(window, "jogSteps");
        auto* calibration = field<QDoubleSpinBox>(window, "stepsPerMm");
        axis->setCurrentIndex(1);
        QCOMPARE(step->currentData().toInt(), embosser::no_pin);
        step->setCurrentIndex(step->findData(5));
        direction->setCurrentIndex(direction->findData(6));
        steps->setValue(4);
        calibration->setValue(80);
        axis->setCurrentIndex(2);
        QCOMPARE(step->currentData().toInt(), embosser::no_pin);
        step->setCurrentIndex(step->findData(9));
        axis->setCurrentIndex(1);
        QCOMPARE(step->currentData().toInt(), 5);
        QCOMPARE(steps->value(), 4);
        QCOMPARE(calibration->value(), 80.0);
        QTest::mouseClick(field<QPushButton>(window, "runSignal"), Qt::LeftButton);
        QTRY_COMPARE(field<QLabel>(window, "status")->text(), QString("Test complete."));
        QVERIFY(field<QPlainTextEdit>(window, "trace")->toPlainText().contains("5000 us  D5  high"));
    }

    void monitor_switch_changes() {
        bench::Window window;
        window.show();
        auto* mode = field<QComboBox>(window, "signalMode");
        mode->setCurrentIndex(mode->findText("Digital input"));
        field<QCheckBox>(window, "monitorInput")->setChecked(true);
        QTest::mouseClick(field<QPushButton>(window, "runSignal"), Qt::LeftButton);
        auto* log = field<QPlainTextEdit>(window, "trace");
        QVERIFY(log->toPlainText().contains("D2 read low."));
        auto* sample = field<QSpinBox>(window, "digitalSample");
        QVERIFY(sample->isEnabled());
        sample->setValue(1);
        QTRY_VERIFY(log->toPlainText().contains("D2 read high."));
        QTest::mouseClick(field<QPushButton>(window, "stopSignal"), Qt::LeftButton);
        QCOMPARE(field<QLabel>(window, "status")->text(), QString("Input monitor stopped."));
    }

    void cell_arguments_and_timing_through_router() {
        BoardStub board;
        board.measured_us = 124'800;
        QVERIFY(board.server.listen(QHostAddress::LocalHost));
        bench::Window window;
        window.show();
        field<QComboBox>(window, "target")->setCurrentIndex(1);
        field<QSpinBox>(window, "tunnelPort")->setValue(board.server.serverPort());
        QTest::mouseClick(field<QPushButton>(window, "connectBoard"), Qt::LeftButton);
        auto* start = field<QPushButton>(window, "runSignal");
        QTRY_VERIFY(start->isEnabled());
        auto* mode = field<QComboBox>(window, "signalMode");
        mode->setCurrentIndex(mode->findText("Dot servos and press"));
        field<QSpinBox>(window, "servoExtend1")->setValue(1'730);
        field<QSpinBox>(window, "servoChannel4")->setValue(5);
        field<QSpinBox>(window, "servoRetract4")->setValue(1'200);
        field<QSpinBox>(window, "cellSettle")->setValue(50'000);
        field<QGroupBox>(window, "usePress")->setChecked(true);
        auto* trigger = field<QComboBox>(window, "pressPin");
        trigger->setCurrentIndex(trigger->findData(8));
        auto* head_up = field<QComboBox>(window, "headUpPin");
        head_up->setCurrentIndex(head_up->findData(9));
        QTest::mouseClick(start, Qt::LeftButton);

        auto* log = field<QPlainTextEdit>(window, "trace");
        QTRY_VERIFY(log->toPlainText().contains("Controller measured 124800 us from trigger to confirmed head-up."));
        const auto run = board.commands.at(1).at(3);
        const auto expected = json::array({embosser::cell_cycle, 0x40, 20'000, -1, 1,
            0, 1'730, 1'500, -1, 1'500, 1'500, -1, 1'500, 1'500, 5, 1'500, 1'200,
            -1, 1'500, 1'500, -1, 1'500, 1'500, 50'000, 8, 1, 50'000, 9, 0, 1, 500'000, 5'000});
        QVERIFY(run.at(1) == expected);
        QVERIFY(board.commands.back().at(2) == embosser::timing_method);
        QVERIFY(board.commands.back().at(3) == json::array({run.at(0)}));
    }

    void live_tunnel() {
        BoardStub board;
        QVERIFY(board.server.listen(QHostAddress::LocalHost));
        bench::Window window;
        window.show();
        auto* start = field<QPushButton>(window, "runSignal");
        auto* stop = field<QPushButton>(window, "stopSignal");
        auto* status = field<QLabel>(window, "status");
        field<QComboBox>(window, "target")->setCurrentIndex(1);
        QVERIFY(!start->isEnabled());
        field<QSpinBox>(window, "tunnelPort")->setValue(board.server.serverPort());
        QTest::mouseClick(field<QPushButton>(window, "connectBoard"), Qt::LeftButton);
        QTRY_VERIFY(start->isEnabled());
        QCOMPARE(board.commands.size(), std::size_t{1});
        QVERIFY(board.commands[0].at(2) == embosser::version_method);

        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_COMPARE(status->text(), QString("Running on the UNO Q."));
        QVERIFY(board.commands.at(1).at(2) == embosser::run_method);
        const auto args = board.commands.at(1).at(3);
        const auto id = args.at(0);
        QVERIFY(id > 0);
        QVERIFY(args.at(1) == json::array({embosser::pulse, 2, 1, 50'000, 100'000, 3}));
        QTRY_VERIFY(board.commands.size() >= 3);
        QVERIFY(board.commands.at(2).at(2) == embosser::status_method);
        QVERIFY(board.commands.at(2).at(3) == json::array({id}));
        QTest::mouseClick(stop, Qt::LeftButton);
        QTRY_COMPARE(status->text(), QString("Test stopped."));
        QVERIFY(board.commands.back().at(2) == embosser::stop_method);
        QVERIFY(board.commands.back().at(3) == json::array({id}));

        auto* mode = field<QComboBox>(window, "signalMode");
        mode->setCurrentIndex(mode->findText("Analog input"));
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_COMPARE(status->text(), QString("A0 / D14 read 2345 of 4095 ADC counts."));
        QVERIFY(board.commands.back().at(2) == embosser::adc_method);
        QVERIFY(board.commands.back().at(3) == json::array({14}));

        mode->setCurrentIndex(0);
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_COMPARE(status->text(), QString("Running on the UNO Q."));
        board.socket->disconnectFromHost();
        QTRY_VERIFY(status->text().contains("two-second lease"));
        QVERIFY(!start->isEnabled());
        QVERIFY(!stop->isEnabled());
    }

    void rejects_old_firmware() {
        BoardStub board(embosser::bench_version + 1);
        QVERIFY(board.server.listen(QHostAddress::LocalHost));
        bench::Window window;
        window.show();
        field<QComboBox>(window, "target")->setCurrentIndex(1);
        field<QSpinBox>(window, "tunnelPort")->setValue(board.server.serverPort());
        QTest::mouseClick(field<QPushButton>(window, "connectBoard"), Qt::LeftButton);
        QTRY_VERIFY(field<QLabel>(window, "status")->text().contains("incompatible"));
        QVERIFY(!field<QPushButton>(window, "runSignal")->isEnabled());
    }
};

}

QTEST_MAIN(BenchTest)
#include "bench.moc"
