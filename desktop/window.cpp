#include "window.hpp"
#include "discovery.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QAbstractItemView>
#include <QBluetoothAddress>
#include <QBluetoothServiceDiscoveryAgent>
#include <QBluetoothServiceInfo>
#include <QBluetoothUuid>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QUuid>

namespace sender {
namespace {
const QMap<QString, QString> extensions{
    {"txt", "Plain text"}, {"docx", "Word document"}, {"pdf", "PDF document"}, {"brl", "Unicode Braille"},
    {"png", "PNG picture"}, {"jpg", "JPEG picture"}, {"jpeg", "JPEG picture"}, {"bmp", "BMP picture"},
    {"gif", "GIF picture"}, {"tif", "TIFF picture"}, {"tiff", "TIFF picture"}, {"jp2", "JPEG 2000 picture"},
    {"pwg", "PWG Raster"}, {"urf", "Apple Raster"}, {"musicxml", "MusicXML score"}, {"mxl", "Compressed MusicXML score"}};

// Qt 6.4 presents a combo box's current value as its accessible name.
// Keep the field name in the announcement when a reader moves focus or selection.
class NamedComboBox : public QComboBox {
public:
    NamedComboBox() {
        connect(this, &QComboBox::currentTextChanged, this, [this] {
            QAccessibleEvent event(this, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        });
    }
};

class ComboAccessibility : public QAccessibleWidget {
public:
    explicit ComboAccessibility(NamedComboBox* combo) : QAccessibleWidget(combo, QAccessible::ComboBox), combo_(combo) {}
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Name) {
            auto label = combo_->accessibleName();
            auto selection = combo_->currentText();
            return selection.isEmpty() ? label : label + ": " + selection;
        }
        if (type == QAccessible::Value)
            return combo_->currentText();
        return QAccessibleWidget::text(type);
    }
    QAccessible::State state() const override {
        auto state = QAccessibleWidget::state();
        state.expandable = true;
        state.expanded = combo_->view()->isVisible();
        state.collapsed = !state.expanded;
        return state;
    }
    QStringList actionNames() const override {
        auto actions = QAccessibleWidget::actionNames();
        if (combo_->isEnabled()) actions.append(showMenuAction());
        return actions;
    }
    void doAction(const QString& name) override {
        if (name == showMenuAction() && combo_->isEnabled()) combo_->showPopup();
        else QAccessibleWidget::doAction(name);
    }
private:
    NamedComboBox* combo_;
};

QAccessibleInterface* build_accessibility(const QString&, QObject* object) {
    if (auto combo = dynamic_cast<NamedComboBox*>(object))
        return new ComboAccessibility(combo);
    return nullptr;
}

QLabel* label(const QString& text, QWidget* control) {
    auto widget = new QLabel(text);
    widget->setBuddy(control);
    control->setAccessibleName(QString(text).remove('&'));
    return widget;
}

QHBoxLayout* alongside(QWidget* left, QWidget* right) {
    auto row = new QHBoxLayout;
    row->addWidget(left, 1);
    row->addWidget(right);
    return row;
}

} // namespace

Window::Window(QWidget* parent) : QWidget(parent), connection_(this) {
    static const bool factory = [] { QAccessible::installFactory(build_accessibility); return true; }();
    (void)factory;
    setWindowTitle(tr("Open Braille Embosser Sender"));
    resize(590, 410);
    auto form = new QFormLayout(this);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    transport_ = new NamedComboBox;
    transport_->addItem(tr("Wi-Fi"), int(Link::wifi));
    transport_->addItem(tr("USB-C Ethernet"), int(Link::usb));
    transport_->addItem(tr("Paired Bluetooth"), int(Link::bluetooth));
    form->addRow(label(tr("&Connection"), transport_), transport_);
    address_ = new QLineEdit;
    address_->setObjectName("printerAddress");
    find_ = new QPushButton(tr("&Find printer"));
    find_->setObjectName("findPrinter");
    address_label_ = label(tr("Printer &address"), address_);
    form->addRow(address_label_, alongside(address_, find_));
    discovered_ = new NamedComboBox;
    discovered_->setEnabled(false);
    form->addRow(label(tr("Discovered &printers"), discovered_), discovered_);
    file_ = new QLineEdit;
    file_->setObjectName("fileName");
    file_->setReadOnly(true);
    choose_ = new QPushButton(tr("Ch&oose file…"));
    form->addRow(label(tr("Selected fi&le"), file_), alongside(file_, choose_));
    format_ = new QLabel(tr("Choose a document"));
    form->addRow(tr("Detected format"), format_);
    mode_ = new NamedComboBox;
    mode_->setObjectName("processingMode");
    mode_->addItem(tr("Auto"), "auto");
    mode_->addItem(tr("Text or scan"), "text");
    mode_->addItem(tr("Picture or diagram"), "picture");
    mode_->addItem(tr("Map"), "map");
    form->addRow(label(tr("&Treat as"), mode_), mode_);
    legend_ = new QLineEdit;
    legend_->setMaxLength(1000);
    legend_->setEnabled(false);
    legend_->setAccessibleDescription(tr("Optional text to include alongside the map graphic."));
    form->addRow(label(tr("Map le&gend"), legend_), legend_);
    auto buttons = new QHBoxLayout;
    send_ = new QPushButton(tr("&Send"));
    send_->setObjectName("sendJob");
    send_->setEnabled(false);
    refresh_ = new QPushButton(tr("Refresh stat&us"));
    refresh_->setEnabled(false);
    review_ = new QPushButton(tr("Open &review in browser"));
    review_->setEnabled(false);
    review_->setAccessibleDescription(tr("Available when the printer can be reached over Wi-Fi or USB Ethernet."));
    for (auto button : {send_, refresh_, review_}) buttons->addWidget(button);
    buttons->addStretch();
    form->addRow(buttons);
    status_ = new QPlainTextEdit;
    status_->setObjectName("jobStatus");
    status_->setReadOnly(true);
    status_->setTabChangesFocus(true);
    status_->setMinimumHeight(90);
    form->addRow(label(tr("&Job status"), status_), status_);
    const QList<QWidget*> tab_order{transport_, address_, find_, discovered_, choose_, file_, mode_, legend_, send_, refresh_, review_, status_};
    for (auto control : tab_order) control->setFocusPolicy(Qt::StrongFocus);
    for (int i = 1; i < tab_order.size(); ++i) setTabOrder(tab_order[i - 1], tab_order[i]);

    discovery_ = new Discovery(this, [this](const Printer& printer) {
        if (link() != Link::wifi) return;
        auto url = printer.url.toString();
        auto at = discovered_->findData(url);
        if (at < 0) {
            discovered_->addItem(printer.name + " (" + printer.url.host() + ")", url);
            discovered_->setEnabled(true);
        }
        if (address_->text().trimmed().isEmpty()) address_->setText(url);
        show_status(tr("Found %1. Select the printer and choose a file.").arg(printer.name));
    });
    radio_ = new QBluetoothServiceDiscoveryAgent(this);
    radio_->setUuidFilter(QBluetoothUuid(QUuid(QString::fromLatin1(service_uuid))));
    connect(radio_, &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this, [this](const auto& service) {
        if (link() != Link::bluetooth) return;
        auto found = service.device().address().toString();
        if (discovered_->findData(found) < 0) {
            discovered_->addItem(service.device().name() + " (" + found + ")", found);
            discovered_->setEnabled(true);
        }
        if (address_->text().isEmpty()) address_->setText(found);
        show_status(tr("Found a Bluetooth printer. Select a device already paired in system settings."));
    });
    connect(radio_, &QBluetoothServiceDiscoveryAgent::errorOccurred, this, [this](auto) {
        show_status(tr("Bluetooth discovery failed. Enter the paired printer address. %1").arg(radio_->errorString()));
    });
    poll_ = new QTimer(this);
    poll_->setInterval(3000);
    connect(poll_, &QTimer::timeout, this, [this] { refresh(); });
    connect(transport_, &QComboBox::currentIndexChanged, this, [this] { connection_changed(); });
    connect(discovered_, &QComboBox::activated, this, [this](int row) {
        address_->setText(discovered_->itemData(row).toString());
        auto printer = selected_printer();
        remember(printer);
        check_printer(printer);
    });
    connect(address_, &QLineEdit::returnPressed, this, [this] { find_printer(); });
    connect(find_, &QPushButton::clicked, this, [this] { find_printer(); });
    connect(choose_, &QPushButton::clicked, this, [this] { choose_file(); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { legend_->setEnabled(mode_->currentData() == "map"); });
    connect(send_, &QPushButton::clicked, this, [this] { send(); });
    connect(refresh_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(review_, &QPushButton::clicked, this, [this] {
        if (job_ > 0 && job_printer_.link != Link::bluetooth) {
            auto url = job_printer_.url;
            url.setPath("/jobs/" + QString::number(job_));
            if (!QDesktopServices::openUrl(url)) show_status(tr("Cannot open the browser. Review at %1").arg(url.toString()));
        }
    });
    transport_->setCurrentIndex(qBound(0, settings_.value("connection", 0).toInt(), transport_->count() - 1));
    connection_changed();
    show_status(tr("Find a printer or enter its address."));
    transport_->setFocus(Qt::OtherFocusReason);
}

Link Window::link() const { return static_cast<Link>(transport_->currentData().toInt()); }

QString Window::setting_for(Link value) const {
    return value == Link::wifi ? "wifiPrinter" : value == Link::usb ? "usbPrinter" : "bluetoothPrinter";
}

Destination Window::selected_printer() const {
    auto value = address_->text().trimmed();
    auto use = link();
    return {use, value, use == Link::bluetooth ? QUrl{} : printer_url(value)};
}

void Window::connection_changed() {
    if (radio_->isActive()) radio_->stop();
    discovered_->clear();
    discovered_->setEnabled(false);
    auto selected = link();
    address_->setText(settings_.value(setting_for(selected)).toString());
    QString name = selected == Link::bluetooth ? tr("Bluetooth &address") : tr("Printer &address");
    address_label_->setText(name);
    address_->setAccessibleName(name.remove('&'));
    address_->setPlaceholderText(selected == Link::bluetooth ? tr("00:11:22:33:44:55") :
        selected == Link::usb ? tr("USB network IP address") : tr("Hostname or IP address"));
    show_status(selected == Link::usb ? tr("Connect USB Ethernet and enter the printer's USB network address.") :
        selected == Link::bluetooth ? tr("Pair the printer in system settings, then find it or enter its address.") :
        tr("Find a Wi-Fi printer or enter its hostname or IP address."));
}

void Window::find_printer() {
    auto printer = selected_printer();
    if (!printer.address.isEmpty()) {
        if (printer.link == Link::bluetooth && QBluetoothAddress(printer.address).isNull()) {
            show_status(tr("Enter a Bluetooth address such as 00:11:22:33:44:55."));
        } else if (printer.link != Link::bluetooth && !printer.url.isValid()) {
            show_status(tr("Enter a printer hostname or IP address, with a port if needed."));
        } else {
            check_printer(printer);
        }
        return;
    }
    if (printer.link == Link::wifi) {
        discovered_->clear();
        discovered_->setEnabled(false);
        show_status(discovery_->start() ? tr("Looking for printers on the local network…") :
            tr("Discovery is unavailable. Enter the printer hostname or IP address."));
        QTimer::singleShot(2200, this, [this] {
            if (link() == Link::wifi && discovered_->count() == 0 && address_->text().isEmpty())
                show_status(tr("No printer was discovered. Enter its hostname or IP address."));
        });
    } else if (printer.link == Link::bluetooth) {
        show_status(tr("Looking for a Bluetooth printer. You can also enter its paired address."));
        if (!radio_->isActive()) radio_->start(QBluetoothServiceDiscoveryAgent::FullDiscovery);
    } else {
        show_status(tr("Enter the printer's IP address on the USB Ethernet connection."));
    }
}

void Window::check_printer(const Destination& printer) {
    busy(true);
    show_status(tr("Checking the printer connection…"));
    connection_.request(printer, "/api/capabilities", {}, {}, [this, printer](Response response) {
        busy(false);
        if (!response.problem.isEmpty() || response.json.value("protocol").toInt() != 1) {
            show_status(response.problem.isEmpty() ? tr("This address did not identify an Open Braille printer.") : response.problem);
            return;
        }
        remember(printer);
        show_status(tr("Connected to %1. Choose a file to send.").arg(response.json.value("name").toString()));
    });
}

void Window::choose_file() {
    auto name = QFileDialog::getOpenFileName(this, tr("Choose document"), QFileInfo(path_).absolutePath(),
        tr("Supported documents (*.txt *.docx *.pdf *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.jp2 *.brl *.mxl *.musicxml *.pwg *.urf);;All files (*)"));
    if (!name.isEmpty()) use_document(name);
}

bool Window::use_document(const QString& path) {
    QFileInfo input(path);
    auto extension = input.suffix().toLower();
    auto upload_name = input.fileName();
    if (!input.isFile() || !input.isReadable()) {
        show_status(tr("Cannot read the selected file."));
        return false;
    }
    if (!extensions.contains(extension)) {
        auto type = QMimeDatabase().mimeTypeForFile(input.absoluteFilePath(), QMimeDatabase::MatchContent);
        extension = type.preferredSuffix().toLower();
        if (!extensions.contains(extension)) {
            show_status(tr("This file format is unavailable. Choose a supported document."));
            return false;
        }
        upload_name += "." + extension;
    }
    path_ = input.absoluteFilePath();
    filename_ = upload_name;
    file_->setText(input.fileName());
    format_->setText(extensions.value(extension));
    send_->setEnabled(true);
    show_status(tr("%1 selected. Choose a processing mode and send.").arg(extensions.value(extension)));
    return true;
}

void Window::remember(const Destination& printer) {
    settings_.setValue("connection", int(printer.link));
    settings_.setValue(setting_for(printer.link), printer.link == Link::bluetooth ? printer.address : printer.url.toString());
    settings_.sync();
}

void Window::busy(bool working) {
    for (auto widget : QList<QWidget*>{transport_, address_, find_, choose_, mode_}) widget->setEnabled(!working);
    discovered_->setEnabled(!working && discovered_->count() > 0);
    legend_->setEnabled(!working && mode_->currentData() == "map");
    send_->setEnabled(!working && !path_.isEmpty());
}

void Window::send() {
    auto printer = selected_printer();
    bool valid = printer.link == Link::bluetooth ? !QBluetoothAddress(printer.address).isNull() : printer.url.isValid();
    if (!valid) { show_status(tr("Choose a printer or enter its address.")); address_->setFocus(); return; }
    QFile input(path_);
    if (!input.open(QIODevice::ReadOnly)) { show_status(tr("Cannot read the selected file.")); return; }
    if (input.size() > document_limit) { show_status(tr("The file is larger than 64 MiB.")); return; }
    auto bytes = input.read(document_limit + 1);
    if (input.error() != QFile::NoError || bytes.size() > document_limit || !input.atEnd()) {
        show_status(tr("The file could not be read within the upload limit."));
        return;
    }
    busy(true);
    poll_->stop();
    show_status(tr("Checking the selected printer…"));
    connection_.request(printer, "/api/capabilities", {}, {}, [this, printer, bytes = std::move(bytes)](Response response) {
        auto identity = response.json.value("printer_id").toString();
        if (!response.problem.isEmpty() || identity.size() != 32 || response.json.value("protocol").toInt() != 1) {
            busy(false);
            show_status(response.problem.isEmpty() ? tr("This address did not identify an Open Braille printer.") : response.problem);
            return;
        }
        submit(printer, identity, bytes);
    });
}

void Window::submit(const Destination& printer, const QString& identity, const QByteArray& bytes) {
    auto mode = mode_->currentData().toByteArray();
    auto legend = mode == "map" ? legend_->text().trimmed() : QString();
    if (legend.toUtf8().size() > 1200) { busy(false); show_status(tr("Shorten the map legend to 1200 UTF-8 bytes.")); return; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (auto component : {identity.toUtf8(), filename_.toUtf8(), mode, legend.toUtf8()}) {
        hash.addData(component);
        hash.addData(QByteArray(1, '\0'));
    }
    hash.addData(bytes);
    auto key = "pending/" + QString::fromLatin1(hash.result().toHex());
    auto id = settings_.value(key).toByteArray();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toRfc4122().toHex();
        settings_.setValue(key, id);
        settings_.sync();
    }
    if (settings_.status() != QSettings::NoError) {
        busy(false);
        show_status(tr("Cannot save the retry ID. Check access to the sender's settings."));
        return;
    }
    Headers fields{{"Content-Type", "application/octet-stream"}, {"X-Braille-Request", id},
        {"X-Braille-Printer", identity.toLatin1()}, {"X-Braille-Mode", mode}, {"X-Braille-Filename", QUrl::toPercentEncoding(filename_)}};
    if (!legend.isEmpty()) fields.insert("X-Braille-Legend", QUrl::toPercentEncoding(legend));
    show_status(tr("Sending the original document…"));
    connection_.request(printer, "/api/jobs", fields, bytes, [this, key, printer](Response response) {
        busy(false);
        int job = response.json.value("id").toInt();
        if (!response.problem.isEmpty() || job < 1) {
            QString problem = response.problem.isEmpty() ? tr("The printer did not return a job number.") : response.problem;
            show_status(problem + (response.status == 0 ? tr(" Use Send to retry the same submission.") : QString{}));
            return;
        }
        settings_.remove(key);
        remember(printer);
        job_ = job;
        job_printer_ = printer;
        refresh_->setEnabled(true);
        review_->setEnabled(printer.link != Link::bluetooth);
        show_status(tr("Job %1 accepted. %2").arg(job).arg(response.json.value("status").toString()));
        poll_->start();
        refresh();
    });
}

void Window::refresh() {
    if (job_ < 1 || refreshing_) return;
    refreshing_ = true;
    auto job = job_;
    connection_.request(job_printer_, "/api/jobs/" + QByteArray::number(job), {}, {}, [this, job](Response response) {
        refreshing_ = false;
        if (job != job_) return;
        if (!response.problem.isEmpty()) {
            poll_->stop();
            show_status(tr("Cannot refresh job %1. %2").arg(job).arg(response.problem));
            return;
        }
        auto state = response.json.value("status").toString();
        auto error = response.json.value("error").toString();
        if (state == "Held for hardware" || state == "Held for OCR approval" || state == "Cancelled" ||
            state == "Aborted" || state == "Completed" || state == "Conversion failed") poll_->stop();
        auto message = tr("Job %1. %2").arg(job).arg(state);
        if (!error.isEmpty()) message += "\n" + error;
        show_status(message);
    });
}

void Window::show_status(const QString& text, bool announce) {
    if (text == previous_status_) return;
    previous_status_ = text;
    status_->setPlainText(text);
    if (announce) {
        QAccessibleEvent changed(status_, QAccessible::Alert);
        QAccessible::updateAccessibility(&changed);
    }
}

} // namespace sender
