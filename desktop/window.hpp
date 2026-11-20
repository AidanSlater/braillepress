#pragma once

#include "connection.hpp"

#include <QSettings>
#include <QWidget>

class QBluetoothServiceDiscoveryAgent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace sender {
class Discovery;

class Window : public QWidget {
public:
    explicit Window(QWidget* parent = nullptr);
    bool use_document(const QString& path);

private:
    Link link() const;
    QString setting_for(Link link) const;
    Destination selected_printer() const;
    void connection_changed();
    void find_printer();
    void check_printer(const Destination& printer);
    void choose_file();
    void send();
    void submit(const Destination& printer, const QString& identity, const QByteArray& document);
    void refresh();
    void show_status(const QString& message, bool announce = true);
    void busy(bool working);
    void remember(const Destination& printer);

    QSettings settings_;
    Connection connection_;
    Discovery* discovery_;
    QBluetoothServiceDiscoveryAgent* radio_;
    QComboBox *transport_, *discovered_, *mode_;
    QLineEdit *address_, *file_, *legend_;
    QLabel *address_label_, *format_;
    QPushButton *find_, *choose_, *send_, *refresh_, *review_;
    QPlainTextEdit* status_;
    QTimer* poll_;
    QString path_, filename_, previous_status_;
    Destination job_printer_{Link::wifi, {}, {}};
    int job_ = 0;
    bool refreshing_ = false;
};

} // namespace sender
