#include "TaskbarProgress.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QProgressDialog>
#include <QTimer>
#include <QVariantMap>

namespace silma {

TaskbarProgress& TaskbarProgress::instance()
{
    static TaskbarProgress* p = new TaskbarProgress;
    return *p;
}

void TaskbarProgress::set(const QObject* source, double fraction)
{
    m_sources[source] = std::clamp(fraction, 0.0, 1.0);
    send();
}

void TaskbarProgress::clear(const QObject* source)
{
    if (m_sources.remove(source))
        send();
}

void TaskbarProgress::follow(QProgressDialog* dialog)
{
    set(dialog, 0);
    // QProgressDialog has no change signal: poll it while it exists.
    auto* poll = new QTimer(dialog);
    connect(poll, &QTimer::timeout, dialog, [this, dialog] {
        if (dialog->maximum() > dialog->minimum() && dialog->value() >= dialog->minimum())
            set(dialog, double(dialog->value() - dialog->minimum()) / (dialog->maximum() - dialog->minimum()));
    });
    poll->start(250);
    connect(dialog, &QObject::destroyed, this, [this, dialog] { clear(dialog); });
}

void TaskbarProgress::setCount(int count)
{
    if (count == m_count)
        return;
    m_count = count;
    send();
}

void TaskbarProgress::send()
{
    double p = -1;
    if (!m_sources.isEmpty()) {
        p = 0;
        for (double v : std::as_const(m_sources))
            p += v;
        p /= double(m_sources.size());
    }
    // Small steps are not worth a D-Bus signal.
    if (m_count == m_sentCount && (p < 0) == (m_shown < 0) && std::abs(p - m_shown) < 0.005)
        return;
    m_shown = p;
    m_sentCount = m_count;
    QVariantMap props{{QStringLiteral("progress"), std::max(0.0, p)},
                      {QStringLiteral("progress-visible"), p >= 0},
                      {QStringLiteral("count"), qint64(m_count)},
                      {QStringLiteral("count-visible"), m_count > 0}};
    QDBusMessage m = QDBusMessage::createSignal(QStringLiteral("/com/canonical/unity/launcherentry/silma"),
                                                QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
    const QString desktopFile = QGuiApplication::desktopFileName().isEmpty() ? QStringLiteral("silma") : QGuiApplication::desktopFileName();
    m << QStringLiteral("application://%1.desktop").arg(desktopFile) << props;
    QDBusConnection::sessionBus().send(m);
}

} // namespace silma
