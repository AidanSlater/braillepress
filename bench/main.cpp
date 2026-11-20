#include "window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationDisplayName("Open Braille Bench");
    bench::Window window;
    window.show();
    return app.exec();
}
