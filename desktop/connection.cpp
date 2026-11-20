#include "connection.hpp"

#include <QBluetoothAddress>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace sender {
namespace {

constexpr int chunk_size = 65536;
constexpr int response_limit = 256 * 1024;

Response parse_response(int status, const QByteArray& data) {
    auto json = QJsonDocument::fromJson(data);
    if (!json.isObject())
        return {status, {}, QObject::tr("The printer returned an invalid response.")};
    auto object = json.object();
    QString problem;
    if (status < 200 || status >= 300)
        problem = object.value("error").toString(QObject::tr("The printer rejected the request."));
    return {status, object, problem};
}

QMap<QByteArray, QByteArray> header_fields(const QList<QByteArray>& lines) {
    QMap<QByteArray, QByteArray> values;
    for (int i = 1; i < lines.size(); ++i) {
        auto field = lines[i].trimmed();
        auto at = field.indexOf(':');
        if (at > 0)
            values.insert(field.left(at).toLower(), field.mid(at + 1).trimmed());
    }
    return values;
}

class StreamRequest : public QObject {
public:
    StreamRequest(QIODevice& socket, QByteArray outgoing, QObject* owner, Completion complete)
        : QObject(owner), socket_(socket), request_(std::move(outgoing)), complete_(std::move(complete)) {
        timer_.setSingleShot(true);
        timer_.setInterval(30000);
        connect(&timer_, &QTimer::timeout, this, [this] { fail(tr("The printer connection timed out.")); });
        connect(&socket_, &QIODevice::readyRead, this, [this] { incoming(); });
        connect(&socket_, &QIODevice::bytesWritten, this, [this] { pump(); });
        connect(&socket_, &QIODevice::readChannelFinished, this, [this] {
            incoming();
            if (complete_) fail(tr("The printer connection closed before returning a response."));
        });
        connect(&socket_, &QObject::destroyed, this, [this] {
            if (complete_) fail(tr("The printer connection closed."));
        });
        timer_.start();
        pump();
    }

    void fail(QString problem) { done({0, {}, std::move(problem)}); }

private:
    void pump() {
        if (!complete_ || !socket_.isOpen())
            return;
        timer_.start();
        while (at_ < request_.size() && socket_.bytesToWrite() < 2 * chunk_size) {
            qint64 length = std::min<qint64>(chunk_size, request_.size() - at_);
            auto sent = socket_.write(request_.constData() + at_, length);
            if (sent < 0) {
                fail(tr("Cannot send to the printer."));
                return;
            }
            if (!sent) return;
            at_ += sent;
        }
    }

    void incoming() {
        if (!complete_)
            return;
        timer_.start();
        response_ += socket_.readAll();
        if (response_.size() > response_limit) {
            fail(tr("The printer returned too much data."));
            return;
        }
        if (header_ < 0) {
            auto end = response_.indexOf("\r\n\r\n");
            if (end < 0) return;
            auto fields = response_.left(end).split('\n');
            auto status = fields[0].simplified().split(' ');
            bool ok = false;
            length_ = header_fields(fields).value("content-length").toLongLong(&ok);
            if (status.size() < 2 || !status[0].startsWith("HTTP/1.") || !ok || length_ < 0 || length_ > response_limit) {
                fail(tr("The printer returned an invalid response header."));
                return;
            }
            status_ = status[1].toInt();
            header_ = end + 4;
        }
        if (response_.size() - header_ >= length_)
            done(parse_response(status_, response_.mid(header_, length_)));
    }

    void done(Response result) {
        if (!complete_)
            return;
        timer_.stop();
        auto callback = std::move(complete_);
        deleteLater();
        callback(std::move(result));
    }

    QIODevice& socket_;
    QByteArray request_, response_;
    Completion complete_;
    QTimer timer_;
    qsizetype at_ = 0, header_ = -1;
    qint64 length_ = 0;
    int status_ = 0;
};

class Relay : public QObject {
public:
    Relay(QIODevice* radio, int port, QObject* owner, std::function<void()> close)
        : QObject(owner), radio_(radio), close_(std::move(close)) {
        radio_->setParent(this);
        timer_.setSingleShot(true);
        timer_.setInterval(30000);
        connect(&timer_, &QTimer::timeout, this, [this] { finish(); });
        connect(radio_, &QIODevice::readyRead, this, [this] { upload(); });
        connect(radio_, &QIODevice::bytesWritten, this, [this] { download(); });
        connect(radio_, &QIODevice::readChannelFinished, this, [this] { finish(); });
        connect(&local_, &QTcpSocket::connected, this, [this] { upload(); });
        connect(&local_, &QTcpSocket::bytesWritten, this, [this] { upload(); });
        connect(&local_, &QTcpSocket::readyRead, this, [this] { download(); });
        connect(&local_, &QTcpSocket::disconnected, this, [this] { eof_ = true; download(); });
        connect(&local_, &QTcpSocket::errorOccurred, this, [this](auto error) {
            if (error != QAbstractSocket::RemoteHostClosedError) finish();
        });
        local_.setReadBufferSize(4 * chunk_size);
        local_.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
        timer_.start();
    }

private:
    void upload() {
        if (finished_ || local_.state() != QAbstractSocket::ConnectedState)
            return;
        timer_.start();
        if (remaining_ < 0) {
            auto prefix = radio_->peek(8193);
            auto end = prefix.indexOf("\r\n\r\n");
            if (end < 0) {
                if (prefix.size() == 8193) finish();
                return;
            }
            qint64 body = 0;
            if (!relay_request_valid(prefix.left(end), body)) { finish(); return; }
            remaining_ = end + 4 + body;
        }
        while (remaining_ > 0 && radio_->bytesAvailable() && local_.bytesToWrite() < 2 * chunk_size) {
            auto bytes = radio_->read(std::min<qint64>(chunk_size, remaining_));
            if (bytes.isEmpty() || local_.write(bytes) != bytes.size()) { finish(); return; }
            remaining_ -= bytes.size();
        }
    }

    void download() {
        if (finished_)
            return;
        timer_.start();
        while (local_.bytesAvailable() && radio_->bytesToWrite() < 2 * chunk_size) {
            auto bytes = local_.read(chunk_size);
            if (bytes.isEmpty() || radio_->write(bytes) != bytes.size()) { finish(); return; }
        }
        if (eof_ && !local_.bytesAvailable() && !radio_->bytesToWrite()) finish();
    }

    void finish() {
        if (finished_)
            return;
        finished_ = true;
        timer_.stop();
        local_.abort();
        close_();
        deleteLater();
    }

    QIODevice* radio_;
    std::function<void()> close_;
    QTcpSocket local_;
    QTimer timer_;
    qint64 remaining_ = -1;
    bool eof_ = false, finished_ = false;
};

} // namespace

QUrl printer_url(const QString& raw) {
    auto text = raw.trimmed();
    if (!text.contains("://")) text = "http://" + text;
    auto url = QUrl(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "http" || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.path() != "/" && !url.path().isEmpty()) || url.hasQuery() || url.hasFragment() || url.port() == 0)
        return {};
    if (url.port() < 0) url.setPort(8000);
    url.setPath("/");
    return url;
}

QByteArray http_request(const QByteArray& path, const Headers& headers, const QByteArray& body) {
    bool post = path == "/api/jobs";
    QByteArray request = (post ? "POST " : "GET ") + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    for (auto i = headers.begin(); i != headers.end(); ++i)
        request += i.key() + ": " + i.value() + "\r\n";
    if (post)
        request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += "\r\n";
    request += body;
    return request;
}

void stream_request(QIODevice& socket, QByteArray request, QObject* owner, Completion finished) {
    new StreamRequest(socket, std::move(request), owner, std::move(finished));
}

bool relay_request_valid(const QByteArray& bytes, qint64& length) {
    auto lines = bytes.split('\n');
    auto start = lines.first().trimmed();
    const bool write = start == "POST /api/jobs HTTP/1.1";
    static const QRegularExpression read(R"(^GET /api/(?:capabilities|jobs/[1-9][0-9]{0,9}) HTTP/1\.1$)");
    if (!write && !read.match(QString::fromLatin1(start)).hasMatch()) return false;
    auto headers = header_fields(lines);
    if (headers.contains("transfer-encoding") || !headers.contains("connection") || headers["connection"].toLower() != "close")
        return false;
    int content_lengths = 0;
    for (const auto& line : lines)
        if (line.toLower().startsWith("content-length:")) ++content_lengths;
    if (write && content_lengths != 1) return false;
    if (!write && content_lengths != 0) return false;
    bool valid = true;
    length = write ? headers.value("content-length").toLongLong(&valid) : 0;
    return valid && length >= 0 && length <= document_limit;
}

Connection::Connection(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
    network_->setProxy(QNetworkProxy::NoProxy);
}

void Connection::request(const Destination& printer, const QByteArray& path, const Headers& fields,
                         const QByteArray& body, Completion finished) {
    if (printer.link == Link::bluetooth) {
        auto socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
        socket->setPreferredSecurityFlags(QBluetooth::Security::Authentication | QBluetooth::Security::Encryption);
        auto timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        auto callback = std::make_shared<Completion>(std::move(finished));
        auto complete = [socket, timeout, callback](Response response) {
            if (!*callback) return;
            timeout->stop();
            auto completed = std::move(*callback);
            socket->disconnectFromService();
            socket->deleteLater();
            completed(std::move(response));
        };
        connect(timeout, &QTimer::timeout, socket, [complete] {
            complete({0, {}, tr("The Bluetooth connection timed out.")});
        });
        connect(socket, &QBluetoothSocket::errorOccurred, socket, [socket, complete](auto) {
            complete({0, {}, tr("Bluetooth connection failed: %1").arg(socket->errorString())});
        });
        connect(socket, &QBluetoothSocket::connected, socket, [socket, timeout, complete, request = http_request(path, fields, body)] {
            timeout->stop();
            stream_request(*socket, request, socket, complete);
        });
        timeout->start(20000);
        socket->connectToService(QBluetoothAddress(printer.address), QBluetoothUuid(QUuid(QString::fromLatin1(service_uuid))));
        return;
    }
    QUrl url = printer.url;
    url.setPath(QString::fromLatin1(path));
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    for (auto i = fields.begin(); i != fields.end(); ++i)
        request.setRawHeader(i.key(), i.value());
    auto incoming = path == "/api/jobs" ? network_->post(request, body) : network_->get(request);
    connect(incoming, &QNetworkReply::downloadProgress, incoming, [incoming](qint64 received, qint64) {
        if (received > response_limit) incoming->abort();
    });
    connect(incoming, &QNetworkReply::finished, this, [incoming, finished = std::move(finished)] {
        int status = incoming->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        Response result = status ? parse_response(status, incoming->readAll()) : Response{0, {}, incoming->errorString()};
        incoming->deleteLater();
        finished(std::move(result));
    });
}

void relay(QIODevice* device, int port, QObject* owner, std::function<void()> disconnect) {
    new Relay(device, port, owner, std::move(disconnect));
}

} // namespace sender
