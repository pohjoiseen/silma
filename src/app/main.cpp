#include "Cli.h"
#include "ColorScheme.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QAction>
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
    // The installed icon (themes may restyle it), else the built-in one.
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("silma"), QIcon(QStringLiteral(":/silma.svg"))));

    silma::applyColorScheme();

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
        // SILMA_SCREENSHOT_ACTIONS="Crop Tool;Before / After" triggers menu actions first.
        QStringList actions = qEnvironmentVariable("SILMA_SCREENSHOT_ACTIONS").split(';', Qt::SkipEmptyParts);
        QTimer::singleShot(std::max(500, (delay > 0 ? delay : 3000) / 2), &w, [&w, actions] {
            for (const QString& name : actions)
                for (QAction* a : w.findChildren<QAction*>())
                    if (a->text().remove('&') == name.trimmed())
                        a->trigger();
        });
        QTimer::singleShot(delay > 0 ? delay : 3000, &w, [&w, shot] {
            w.grab().save(shot);
            w.close();
        });
    }
    return app.exec();
}
