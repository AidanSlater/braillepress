#include "discovery.hpp"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

#include <stdexcept>

namespace sender {
namespace {

const QString type = "_openbraille._tcp.local";
const QHostAddress mdns("224.0.0.251");

class Packet {
public:
    explicit Packet(const QByteArray& data) : data_(data) {}

    friend class Discovery;

    quint16 number(qsizetype& offset) const {
        check(offset, 2);
        quint16 result = qFromBigEndian<quint16>(data_.constData() + offset);
        offset += 2;
        return result;
    }

    QString name(qsizetype& offset) const {
        qsizetype at = offset;
        QByteArray result;
        bool jumped = false;
        for (int labels = 0; labels < 128; ++labels) {
            check(at, 1);
            auto length = static_cast<unsigned char>(data_[at++]);
            if (!length) {
                if (!jumped) offset = at;
                return QString::fromUtf8(result);
            }
            if ((length & 0xc0) == 0xc0) {
                check(at, 1);
                auto target = ((length & 0x3f) << 8) | static_cast<unsigned char>(data_[at++]);
                if (!jumped) offset = at;
                if (target >= at - 2) throw std::runtime_error("Invalid DNS name pointer");
                jumped = true;
                at = target;
                continue;
            }
            if (length > 63 || result.size() + length > 254) throw std::runtime_error("Invalid DNS name");
            check(at, length);
            if (!result.isEmpty()) result += '.';
            result += data_.mid(at, length);
            at += length;
        }
        throw std::runtime_error("DNS name is too long");
    }

    void check(qsizetype offset, qsizetype length) const {
        if (offset < 0 || length > data_.size() - offset) throw std::runtime_error("Incomplete DNS packet");
    }

private:
    const QByteArray& data_;
};

void append_number(QByteArray& message, int value) {
    message += static_cast<char>(value >> 8);
    message += static_cast<char>(value & 255);
}

} // namespace

Discovery::Discovery(QObject* owner, std::function<void(const Printer&)> found)
    : QObject(owner), socket_(new QUdpSocket(this)), found_(std::move(found)) {
    connect(socket_, &QUdpSocket::readyRead, this, [this] {
        while (socket_->hasPendingDatagrams()) accept_packet(socket_->receiveDatagram(9000).data());
    });
}

bool Discovery::start() {
    pointers_.clear();
    seen_.clear();
    services_.clear();
    addresses_.clear();
    if (socket_->state() == QAbstractSocket::UnconnectedState && !socket_->bind(QHostAddress::AnyIPv4, 0))
        return false;
    query(type, 12);
    QTimer::singleShot(600, this, [this] { query(type, 12); });
    return true;
}

void Discovery::query(const QString& name, int kind) {
    if (!socket_->isOpen()) return;
    QByteArray message(12, '\0');
    message[5] = 1;
    for (auto label : name.toUtf8().split('.')) {
        if (label.isEmpty() || label.size() > 63) return;
        message += static_cast<char>(label.size());
        message += label;
    }
    message += '\0';
    append_number(message, kind);
    append_number(message, 1); // Legacy unicast response to this ephemeral port.
    bool sent = false;
    for (auto& adapter : QNetworkInterface::allInterfaces()) {
        auto flags = adapter.flags();
        bool ipv4 = false;
        for (auto& address : adapter.addressEntries()) ipv4 |= address.ip().protocol() == QAbstractSocket::IPv4Protocol;
        if (ipv4 && flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::CanMulticast)) {
            socket_->setMulticastInterface(adapter);
            sent |= socket_->writeDatagram(message, mdns, 5353) >= 0;
        }
    }
    if (!sent) socket_->writeDatagram(message, mdns, 5353);
}

void Discovery::accept_packet(const QByteArray& data) {
    try {
        Packet packet(data);
        qsizetype offset = 2;
        if ((packet.number(offset) & 0x800f) != 0x8000) return;
        int questions = packet.number(offset);
        int count = packet.number(offset);
        count += packet.number(offset);
        count += packet.number(offset);
        if (questions > 32 || count > 256) return;
        for (int i = 0; i < questions; ++i) {
            packet.name(offset);
            packet.number(offset);
            packet.number(offset);
        }
        auto pointers = pointers_;
        auto services = services_;
        auto addresses = addresses_;
        for (int i = 0; i < count; ++i) {
            auto owner = packet.name(offset);
            int kind = packet.number(offset), klass = packet.number(offset);
            int ttl = packet.number(offset);
            ttl |= packet.number(offset);
            int length = packet.number(offset);
            auto end = offset + length;
            packet.check(offset, length);
            if ((klass & 0x7fff) == 1 && ttl) {
                if (kind == 12 && owner.compare(type, Qt::CaseInsensitive) == 0) {
                    auto instance = packet.name(offset);
                    if (instance.endsWith("." + type, Qt::CaseInsensitive)) pointers.insert(instance);
                } else if (kind == 33 && length >= 7 && owner.endsWith("." + type, Qt::CaseInsensitive)) {
                    packet.number(offset);
                    packet.number(offset);
                    int port = packet.number(offset);
                    auto host = packet.name(offset);
                    if (port && host.endsWith(".local", Qt::CaseInsensitive)) services.insert(owner, {host, port});
                } else if (kind == 1 && length == 4) {
                    addresses.insert(owner.toLower(), QHostAddress(qFromBigEndian<quint32>(data.constData() + offset)));
                }
            }
            if (offset > end) return;
            offset = end;
        }
        const auto previous_pointers = pointers_;
        const auto previous_services = services_;
        pointers_ = std::move(pointers);
        services_ = std::move(services);
        addresses_ = std::move(addresses);
        for (auto& name : pointers_) {
            if (seen_.contains(name)) continue;
            if (!services_.contains(name)) {
                if (!previous_pointers.contains(name)) query(name, 33);
                continue;
            }
            auto [host, port] = services_.value(name);
            auto ip = addresses_.value(host.toLower());
            if (ip.isNull()) {
                if (!previous_services.contains(name)) query(host, 1);
                continue;
            }
            QUrl url;
            url.setScheme("http");
            url.setHost(ip.toString());
            url.setPort(port);
            url.setPath("/");
            seen_.insert(name);
            found_({name.left(name.size() - type.size() - 1), url});
        }
    } catch (const std::runtime_error&) { /* Ignore malformed or incomplete multicast packets. */ }
}

} // namespace sender
