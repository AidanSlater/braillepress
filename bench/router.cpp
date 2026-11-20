#include "router.hpp"

#include <QHostAddress>
#include <QNetworkProxy>
#include <limits>
#include <utility>

namespace bench {

Router::Router(QObject* owner, std::function<void(QString)> failure)
    : QObject(owner), failure_(std::move(failure)) {
    socket_.setProxy(QNetworkProxy::NoProxy);
    socket_.setReadBufferSize(embosser::rpc::reply_limit + 1);
    timeout_.setInterval(1'500);
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail(tr("The Arduino Router timed out.")); });
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        timeout_.stop();
        auto callback = std::exchange(connected_, {});
        callback();
    });
    connect(&socket_, &QTcpSocket::readyRead, this, [this] { receive(); });
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](auto) { fail(socket_.errorString()); });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] { fail(tr("The Arduino Router disconnected.")); });
}

Router::~Router() { close(); }

void Router::open(int port, std::function<void()> connected) {
    close();
    active_ = true;
    connected_ = std::move(connected);
    timeout_.start();
    socket_.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
}

void Router::close() {
    active_ = false;
    timeout_.stop();
    reply_ = {};
    connected_ = {};
    input_.clear();
    socket_.abort();
    id_ = 0;
}

void Router::call(std::string_view method, const embosser::rpc::json& args, Reply reply) {
    if (socket_.state() != QAbstractSocket::ConnectedState || waiting()) {
        fail(tr("An Arduino Router request could not be sent."));
        return;
    }
    id_ = id_ == std::numeric_limits<int>::max() ? 1 : id_ + 1;
    method_ = method;
    input_.clear();
    reply_ = std::move(reply);
    auto packet = embosser::rpc::request(id_, method_, args);
    auto size = static_cast<qint64>(packet.size());
    timeout_.start();
    if (socket_.write(reinterpret_cast<const char*>(packet.data()), size) != size)
        fail(tr("Could not write to the Arduino Router."));
}

void Router::receive() {
    input_ += socket_.readAll();
    if (!waiting() || input_.size() > embosser::rpc::reply_limit) {
        fail(tr("Unexpected data from the Arduino Router."));
        return;
    }
    std::optional<int> result;
    try {
        result = embosser::rpc::response(input_.cbegin(), input_.cend(), id_, method_);
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }
    if (result) {
        timeout_.stop();
        auto callback = std::exchange(reply_, {});
        callback(*result);
    }
}

void Router::fail(const QString& message) {
    if (!active_) return;
    // socket_.errorString() and timeout callbacks can be re-entered by abort().
    auto reason = message;
    close();
    failure_(reason);
}

}
