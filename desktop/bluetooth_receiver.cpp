#include "connection.hpp"

#include <QBluetoothAddress>
#include <QBluetoothLocalDevice>
#include <QBluetoothServer>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    bool valid = false;
    int port = args.size() == 2 ? args[1].toInt(&valid) : 8000;
    if ((args.size() == 2 && !valid) || args.size() > 2 || port < 1 || port > 65535) {
        QTextStream(stderr) << "Usage: braille-bluetooth [local HTTP port]\n";
        return 2;
    }
    QBluetoothLocalDevice adapter;
    if (!adapter.isValid() || adapter.hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
        QTextStream(stderr) << "A powered Bluetooth adapter is required. Pair the sender in system Bluetooth settings.\n";
        return 1;
    }
    QBluetoothServer server(QBluetoothServiceInfo::RfcommProtocol);
    server.setMaxPendingConnections(3);
    server.setSecurityFlags(QBluetooth::Security::Authentication | QBluetooth::Security::Encryption);
    auto uuid = QBluetoothUuid(QUuid(QString::fromLatin1(sender::service_uuid)));
    auto service = server.listen(uuid, "Open Braille Embosser");
    if (!service.isRegistered()) {
        QTextStream(stderr) << "Cannot register the Bluetooth RFCOMM printer service.\n";
        return 1;
    }
    int active = 0;
    QObject::connect(&server, &QBluetoothServer::newConnection, &app, [&] {
        auto socket = server.nextPendingConnection();
        if (!socket) return;
        if (active >= 3 || adapter.pairingStatus(socket->peerAddress()) == QBluetoothLocalDevice::Unpaired) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        ++active;
        QObject::connect(socket, &QObject::destroyed, &server, [&] { --active; });
        sender::relay(socket, port, &app, [socket] { socket->disconnectFromService(); });
    });
    QTextStream(stdout) << "Accepting paired Bluetooth submissions.\n";
    return app.exec();
}
