#include "window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Open Braille Embosser");
    QCoreApplication::setApplicationName("Sender");
    sender::Window window;
    window.show();
    return app.exec();
}
