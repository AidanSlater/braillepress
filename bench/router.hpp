#pragma once

#include "firmware_rpc.hpp"

#include <QTcpSocket>
#include <QTimer>

#include <functional>

namespace bench {

class Router : public QObject {
public:
    using Reply = std::function<void(int)>;

    Router(QObject* owner, std::function<void(QString)> failure);
    ~Router() override;
    void open(int port, std::function<void()> connected);
    void close();
    void call(std::string_view method, const embosser::rpc::json& args, Reply reply);
    bool waiting() const { return static_cast<bool>(reply_); }

private:
    void receive();
    void fail(const QString& message);

    QTcpSocket socket_;
    QTimer timeout_;
    bool active_ = false;
    int id_ = 0;
    QByteArray input_;
    std::string method_;
    Reply reply_;
    std::function<void()> connected_;
    std::function<void(QString)> failure_;
};

}
