#include "cells.hpp"
#include "connection.hpp"
#include "discovery.hpp"
#include "window.hpp"

#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <QPointer>
#include <stdexcept>
#include <utility>

namespace {
using namespace sender;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void wait_for(const std::function<bool()>& ready, const char* message) {
    for (int i = 0; i < 400; ++i) {
        if (ready()) return;
        QTest::qWait(25);
    }
    throw std::runtime_error(message);
}

int port() {
    QTcpServer service;
    check(service.listen(QHostAddress::LocalHost), "Cannot reserve a loopback test port");
    return service.serverPort();
}

class Service {
public:
    Service(QString executable, QString state) : file_(std::move(executable)), state_(std::move(state)), http(port()), ipp(port()) {
        while (ipp == http) ipp = port();
        start();
    }

    void start() {
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start(file_, {"--state", state_, "--port", QString::number(http), "--ipp-port", QString::number(ipp)});
        check(process_.waitForStarted(5000), "Cannot start the test service");
        wait_for([&] {
            if (process_.state() == QProcess::NotRunning) {
                auto log = process_.readAll();
                QFile ipp_log(state_ + "/ipp.log");
                if (ipp_log.open(QIODevice::ReadOnly)) log += ipp_log.readAll().right(5000);
                throw std::runtime_error(log.constData());
            }
            return process_.canReadLine();
        }, "Test service did not become ready");
    }

    void stop() {
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();
            if (!process_.waitForFinished(12000)) {
                process_.kill();
                process_.waitForFinished(8000);
            }
        }
        process_.readAll();
    }

    ~Service() { stop(); }

    QString address() const { return "127.0.0.1:" + QString::number(http); }

    const QString file_, state_;
    int http, ipp;
private:
    QProcess process_;
};

Response call(Connection& connection, const Destination& printer,
             const QByteArray& path, const Headers& headers = {}, const QByteArray& body = {}) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto result = std::make_shared<Response>();
    QPointer<QEventLoop> waiting(&loop);
    connection.request(printer, path, headers, body, [waiting, result](Response response) {
        *result = std::move(response);
        if (waiting) waiting->quit();
    });
    deadline.start(8000);
    loop.exec();
    if (result->status == 0 && result->problem.isEmpty())
        throw std::runtime_error("No reply to queue request");
    return *result;
}

Response radio_call(quint16 port, const QByteArray& path, const Headers& headers = {}, const QByteArray& body = {}) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    check(socket.waitForConnected(4000), "Cannot connect to the simulated radio link");
    Response result;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    stream_request(socket, http_request(path, headers, body), &loop, [&](Response reply) {
        result = std::move(reply);
        loop.quit();
    });
    timer.start(8000);
    loop.exec();
    return result;
}

QByteArray dns_answer() {
    // Synthetic DNS-SD response with compressed PTR and SRV names and an A record.
    QByteArray packet = QByteArray::fromHex("000084000000000300000000");
    auto number = [&](unsigned x) {
        packet += char(x >> 8); packet += char(x);
    };
    auto name = [&](const QByteArray& dns) {
        for (auto part : dns.split('.')) { packet += char(part.size()); packet += part; }
        packet += '\0';
    };
    auto header = [&](int type, int length) {
        number(type); number(1); number(0); number(120); number(length);
    };
    int service = packet.size();
    name("_openbraille._tcp.local");
    header(12, 10);
    int instance = packet.size();
    packet.append("\x07Printer", 8);
    number(0xc000 | service);
    number(0xc000 | instance);
    header(33, 19);
    number(0); number(0); number(8000);
    int host = packet.size();
    name("board.local");
    number(0xc000 | host);
    header(1, 4);
    packet += QByteArray::fromHex("c0a80142");
    return packet;
}

void discovery_test() {
    auto packet = dns_answer();
    int found = 0;
    Discovery discovery(nullptr, [&](const Printer& printer) {
        check(printer.name == "Printer", "Discovery did not retain the printer name");
        check(printer.url == QUrl("http://192.168.1.66:8000/"), "Discovery chose the wrong address");
        ++found;
    });
    discovery.accept_packet(packet.left(packet.size() - 2));
    check(!found, "Incomplete DNS reply was treated as complete");
    discovery.accept_packet(packet);
    discovery.accept_packet(packet);
    check(found == 1, "Expected exactly one DNS-SD result");
    check(!printer_url("http://someone@printer:8000/").isValid() &&
        printer_url("printer.local").port() == 8000, "Printer hostname validation");
}

void ui(Service& service, const QString& root) {
    Window window;
    window.show();
    window.activateWindow();
    QTest::qWait(80);
    auto address = window.findChild<QLineEdit*>("printerAddress");
    auto status = window.findChild<QPlainTextEdit*>("jobStatus");
    auto send = window.findChild<QPushButton*>("sendJob");
    auto find = window.findChild<QPushButton*>("findPrinter");
    auto mode = window.findChild<QComboBox*>("processingMode");
    check(address && status && send && find && mode, "Required sender controls are missing");
    auto screen_reader = QAccessible::queryAccessibleInterface(status);
    check(screen_reader && screen_reader->text(QAccessible::Name) == "Job status" && screen_reader->state().readOnly,
        "Status needs a readable accessible name and read-only state");
    check(QAccessible::queryAccessibleInterface(mode)->text(QAccessible::Name) == "Treat as: Auto", "Mode must have a label");
    address->setFocus();
    check(address->hasFocus(), "Hostname field could not receive focus");
    QTest::keyClicks(address, service.address());
    QTest::keyClick(address, Qt::Key_Tab);
    if (!find->hasFocus()) {
        auto selected = QApplication::focusWidget();
        throw std::runtime_error("Tab left hostname at " + (selected ? selected->accessibleName().toStdString() +
            "/" + selected->objectName().toStdString() : "no focused control"));
    }
    QTest::keyClick(find, Qt::Key_Space);
    wait_for([&] { return status->toPlainText().contains("Connected to Open Braille Embosser"); }, "Find printer over IP");
    check(window.use_document(root + "/tests/fixtures/simple.txt"), "Choose the original TXT fixture");
    send->setFocus();
    QTest::keyClick(send, Qt::Key_Space);
    wait_for([&] { return status->toPlainText().contains("Held for hardware"); }, "Wi-Fi upload did not reach the held queue");
    check(QSettings().value("wifiPrinter").toString().contains(service.address()), "Last selected printer was not saved");
}

QByteArray file_bytes(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot open test document");
    return file.readAll();
}

void firmware_rpc(const Service& service, QStringList args, const QString& result, bool wrong_id = false) {
    using json = nlohmann::json;
    embosser::DryRun firmware;
    QByteArray pending;
    bool first = true;
    QTemporaryDir temporary;
    QLocalServer router;
    check(temporary.isValid() && router.listen(temporary.filePath("router.sock")), "Cannot start the test router");
    QObject::connect(&router, &QLocalServer::newConnection, &router, [&] {
        auto* socket = router.nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
            pending += socket->readAll();
            auto request = json::from_msgpack(pending.begin(), pending.end(), true, false);
            if (request.is_discarded()) return;
            pending.clear();
            check(request.is_array() && request.size() == 4 && request[0] == 0, "Invalid MessagePack-RPC request");
            auto method = request.at(2).get<std::string>();
            const auto& arguments = request.at(3);
            auto arg = [&](int i) { return arguments.at(i).get<int>(); };
            int reply = 0;
            if (method == "braille_begin") reply = firmware.begin(arg(0), arg(1));
            else if (method == "braille_cell") reply = firmware.cell(arg(0), arg(1), arg(2));
            else if (method == "braille_finish") reply = firmware.finish(arg(0));
            else if (method == "braille_cancel") reply = firmware.cancel(arg(0));
            else throw std::runtime_error("Unexpected firmware RPC name");
            int id = request.at(1).get<int>();
            auto bytes = json::to_msgpack(json::array({1, wrong_id ? id + 1 : id, nullptr, reply}));
            QByteArray response(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (first) {
                first = false;
                socket->write(response.first(2));
                socket->flush();
                QTimer::singleShot(4, socket, [socket, tail = response.sliced(2)] { socket->write(tail); });
            } else {
                socket->write(response);
            }
        });
    });
    QProcess dryrun;
    args << "--rpc-socket" << router.fullServerName();
    dryrun.start(service.file_, args);
    check(dryrun.waitForStarted(3000), "Cannot start remote firmware dry run");
    wait_for([&] { return dryrun.state() == QProcess::NotRunning; }, "Firmware RPC simulation did not finish");
    if (wrong_id) {
        check(dryrun.exitCode() == 2 && dryrun.readAllStandardError().contains("Unexpected firmware RPC reply"),
            "Simulation accepted a mismatched MessagePack-RPC ID");
    } else {
        auto summary = dryrun.readAllStandardOutput();
        if (dryrun.exitCode() != 0) throw std::runtime_error(dryrun.readAllStandardError().constData());
        check(summary.contains("\"simulation\":\"" + result.toUtf8() + '"'), "Remote firmware simulation returned the wrong status");
        if (result == "cancelled")
            check(summary.contains("\"acknowledged\":2"), "Cancellation happened at the wrong cell");
    }
}

void queue_roundtrip(Service& service, const QString& root, bool radio) {
    auto url = printer_url(service.address());
    Destination wifi{Link::wifi, service.address(), url}, usb{Link::usb, service.address(), url};
    QObject owner;
    Connection connection(&owner);
    auto caps = call(connection, wifi, "/api/capabilities");
    auto identity = caps.json.value("printer_id").toString();
    check(caps.status == 200 && identity.size() == 32, "Service did not identify its queue");
    auto image = file_bytes(root + "/demo/examples/campus-map.png");
    Headers headers{{"Content-Type", "application/octet-stream"}, {"X-Braille-Mode", "map"},
        {"X-Braille-Filename", "campus-map.png"}, {"X-Braille-Legend", "Pond%20and%20paths"},
        {"X-Braille-Request", "0123456789abcdef0000000000000001"}, {"X-Braille-Printer", identity.toLatin1()}};
    auto sent = call(connection, wifi, "/api/jobs", headers, image);
    int id = sent.json.value("id").toInt();
    if (sent.status != 202 || id < 1) throw std::runtime_error(sent.problem.toStdString());
    QByteArray path = "/api/jobs/" + QByteArray::number(id);
    wait_for([&] {
        return call(connection, wifi, path).json.value("status").toString() == "Held for hardware";
    }, "Map job was not held for hardware");
    auto directory = service.state_ + "/jobs/" + QString::number(id);
    check(file_bytes(directory + "/input.png") == image, "Original image bytes changed during upload");
    check(file_bytes(directory + "/imported.txt").contains("Pond and paths"), "Map legend was lost");
    check(call(connection, usb, "/api/jobs", headers, image).json.value("id").toInt() == id,
        "Same job must be returned when a retry uses another address for this queue");
    check(call(connection, wifi, "/api/jobs", headers, image + "different").status == 409,
        "A reused submission ID with different bytes must fail");
    service.stop();
    service.start();
    check(call(connection, usb, "/api/capabilities").json.value("printer_id").toString() == identity,
        "Printer identity changed on restart");
    check(call(connection, wifi, "/api/jobs", headers, image).json.value("id").toInt() == id,
        "Retry created a duplicate after queue restart");
    check(call(connection, wifi, path).json.value("name").toString() == "campus-map.png",
        "Job name must hide the sender request marker");
    // A crash after queueing but before the journal records the job number must
    // reconcile the retry with the queued job instead of reporting an unknown outcome.
    auto interrupted = headers;
    interrupted["X-Braille-Request"] = "0123456789abcdef0000000000000002";
    auto queued = call(connection, wifi, "/api/jobs", interrupted, image);
    int second = queued.json.value("id").toInt();
    check(queued.status == 202 && second > id, "Second submission was not queued");
    QFile journal(service.state_ + "/requests/0123456789abcdef0000000000000002");
    check(journal.open(QIODevice::ReadOnly), "Missing submission journal");
    auto entry = nlohmann::json::parse(journal.readAll().toStdString());
    journal.close();
    entry.erase("job");
    check(journal.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        journal.write(QByteArray::fromStdString(entry.dump())) > 0, "Cannot rewrite the submission journal");
    journal.close();
    auto reconciled = call(connection, wifi, "/api/jobs", interrupted, image);
    check(reconciled.status == 202 && reconciled.json.value("id").toInt() == second,
        "Retry after an interrupted journal update did not find the queued job");
    QProcess dryrun;
    QStringList args{"--state", service.state_, "--ipp-port", QString::number(service.ipp), "--dry-run", QString::number(id)};
    dryrun.start(service.file_, args);
    check(dryrun.waitForFinished(8000) && dryrun.exitCode() == 0 && dryrun.readAllStandardOutput().contains("\"simulation\":\"complete\""),
        "Queue-to-firmware simulation did not acknowledge the job");
    args << "--cancel-after" << "2";
    dryrun.start(service.file_, args);
    check(dryrun.waitForFinished(8000) && dryrun.exitCode() == 0 &&
        dryrun.readAllStandardOutput().contains("\"simulation\":\"cancelled\""), "Simulation did not acknowledge cancellation");
    if (!radio) {
        firmware_rpc(service, args, "cancelled");
        args.removeLast();
        args.removeLast();
        firmware_rpc(service, args, "complete");
        firmware_rpc(service, args, {}, true);
    }
    check(call(connection, wifi, path).json.value("status") == "Held for hardware", "Simulation changed the real queue state");
    if (!radio) return;

    QTcpServer bridge;
    check(bridge.listen(QHostAddress::LocalHost), "Cannot start simulated Bluetooth receiver");
    QObject::connect(&bridge, &QTcpServer::newConnection, &bridge, [&] {
        auto socket = bridge.nextPendingConnection();
        relay(socket, service.http, &bridge, [socket] { socket->disconnectFromHost(); });
    });
    check(radio_call(bridge.serverPort(), "/api/capabilities").json.value("printer_id").toString() == identity,
        "RFCOMM relay simulation did not find the same printer");
    auto resumed = radio_call(bridge.serverPort(), "/api/jobs", headers, image);
    check(resumed.status == 202 && resumed.json.value("id").toInt() == id, "Reconnection created a duplicate job");
    auto status = radio_call(bridge.serverPort(), path);
    check(status.json.value("status") == "Held for hardware", "Cannot read the job status through the relay");
    qint64 length;
    check(!relay_request_valid("GET /jobs/1 HTTP/1.1\r\nConnection: close", length), "Browser route entered the Bluetooth relay");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        check(argc == 4, "Sender test expects stage, service path, source directory");
        QTemporaryDir temporary;
        check(temporary.isValid(), "Cannot create a private test directory");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QCoreApplication::setApplicationName("Sender test");
        QCoreApplication::setOrganizationName("Open Braille Embosser test");
        QString stage = argv[1];
        if (stage == "discovery") {
            discovery_test();
        } else {
            Service service(QString::fromLocal8Bit(argv[2]), temporary.path() + "/queue");
            if (stage == "wifi") ui(service, QString::fromLocal8Bit(argv[3]));
            else queue_roundtrip(service, QString::fromLocal8Bit(argv[3]), stage == "radio");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
