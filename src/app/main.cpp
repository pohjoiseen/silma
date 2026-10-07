#include "Cli.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QTimer>

int main(int argc, char** argv)
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    if (silma::isCliInvocation(args)) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("silma"));
        QCoreApplication::setApplicationName(QStringLiteral("silma"));
        return silma::runCli(args);
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("silma"));
    QApplication::setApplicationName(QStringLiteral("silma"));
    QApplication::setApplicationDisplayName(QStringLiteral("Silmä"));
    QApplication::setDesktopFileName(QStringLiteral("silma"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("camera-photo")));

    silma::MainWindow w;
    w.show();
    QString batch = args.size() == 2 ? args[1] : QSettings().value("batch/last").toString();
    if (!batch.isEmpty() && QFileInfo::exists(batch))
        w.openBatch(batch);
    // Debug/automation aid: SILMA_SCREENSHOT=out.png [SILMA_SCREENSHOT_DELAY=ms] grabs the
    // window and exits (works with QT_QPA_PLATFORM=offscreen).
    QString shot = qEnvironmentVariable("SILMA_SCREENSHOT");
    if (!shot.isEmpty()) {
        int delay = qEnvironmentVariableIntValue("SILMA_SCREENSHOT_DELAY");
        QTimer::singleShot(delay > 0 ? delay : 3000, &w, [&w, shot] {
            w.grab().save(shot);
            w.close();
        });
    }
    return app.exec();
}
