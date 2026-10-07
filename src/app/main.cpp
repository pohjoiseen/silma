#include "Cli.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>

int main(int argc, char** argv)
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    if (silma::isCliInvocation(args)) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("silma"));
        return silma::runCli(args);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("silma"));
    QApplication::setApplicationDisplayName(QStringLiteral("Silmä"));
    QMessageBox::information(nullptr, QStringLiteral("Silmä"), QStringLiteral("The GUI is not implemented yet."));
    return 0;
}
