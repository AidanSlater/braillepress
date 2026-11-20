#pragma once

#include <QObject>
#include <QHash>
#include <QHostAddress>
#include <QSet>
#include <QUrl>
#include <functional>

class QUdpSocket;

namespace sender {

struct Printer {
    QString name;
    QUrl url;
};

class Discovery : public QObject {
public:
    Discovery(QObject* owner, std::function<void(const Printer&)> found);
    bool start();
    void accept_packet(const QByteArray& packet);
private:
    void query(const QString& name, int type);
    QUdpSocket* socket_;
    std::function<void(const Printer&)> found_;
    QSet<QString> pointers_, seen_;
    QHash<QString, QPair<QString, int>> services_;
    QHash<QString, QHostAddress> addresses_;
};

} // namespace sender
