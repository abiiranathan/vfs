#include <QApplication>
#include "mainwindow.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    if (argc >= 2) { window.setVFSFile(argv[1]); }
    window.show();
    return app.exec();
}
