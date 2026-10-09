#pragma once
// Progress on the application's taskbar entry through the Unity LauncherEntry D-Bus API,
// which KDE Plasma's task manager and most docks show. The entry is found by the desktop
// file name (silma.desktop), so it needs to be installed.

#include <QHash>
#include <QObject>
#include <QPointer>

class QProgressDialog;

namespace silma {

class TaskbarProgress : public QObject {
public:
    static TaskbarProgress& instance();
    // Progress 0..1 of an operation (identified by `source`); with several, their mean is shown.
    void set(const QObject* source, double fraction);
    void clear(const QObject* source);
    // Mirrors a progress dialog's value until it is destroyed. A dialog without a maximum
    // (busy) keeps the last value.
    void follow(QProgressDialog* dialog);
    // A number on the entry (e.g. pending background jobs); 0 hides it.
    void setCount(int count);

    // What was last sent (for tests): progress (< 0: hidden) and count.
    double shownProgress() const { return m_shown; }
    int shownCount() const { return m_count; }

private:
    TaskbarProgress() = default;
    void send();

    QHash<const QObject*, double> m_sources;
    double m_shown = -1;
    int m_count = 0, m_sentCount = 0;
};

} // namespace silma
