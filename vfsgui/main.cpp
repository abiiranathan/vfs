#include <QApplication>
#include "mainwindow.hpp"

auto main(int argc, char* argv[]) -> int {
    QApplication app(argc, argv);
    app.setApplicationName("VFS Native Explorer");
    app.setOrganizationName("VFS Engine Project");

    MainWindow window;
    if (argc >= 2) {
        window.setVFSFile(argv[1]);
    }
    window.show();
    return QApplication::exec();
}
