#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QUrl>

#include <functional>

class QIODevice;
class QNetworkAccessManager;
class QTcpSocket;

namespace sender {

inline constexpr int document_limit = 64 * 1024 * 1024;
inline constexpr char service_uuid[] = "{472add60-35af-4fe3-bb9c-4c89fa878b61}";

enum class Link { wifi, usb, bluetooth };

struct Destination {
    Link link;
    QString address;
    QUrl url;
};

struct Response {
    int status = 0;
    QJsonObject json;
    QString problem;
};

using Headers = QMap<QByteArray, QByteArray>;
using Completion = std::function<void(Response)>;

QUrl printer_url(const QString& text);
QByteArray http_request(const QByteArray& path, const Headers& headers, const QByteArray& body);
void stream_request(QIODevice& socket, QByteArray request, QObject* owner, Completion finished);
bool relay_request_valid(const QByteArray& headers, qint64& length);

class Connection : public QObject {
public:
    explicit Connection(QObject* parent);
    void request(const Destination& printer, const QByteArray& path, const Headers& headers,
                 const QByteArray& body, Completion finished);

private:
    QNetworkAccessManager* network_;
};

// Relay one bounded HTTP queue request from one reliable byte stream to the IPP
// service's HTTP endpoint. Used by the paired radio receiver and loopback tests.
void relay(QIODevice* device, int port, QObject* owner, std::function<void()> disconnect);

} // namespace sender
