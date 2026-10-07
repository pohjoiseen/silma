#pragma once

#include "Engine.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <memory>

namespace silma {

// Loads DevelopEngines in the background: a fast half-size decode first, then full
// resolution. Keeps a few recently used engines in memory.
class EngineCache : public QObject {
    Q_OBJECT
public:
    explicit EngineCache(QObject* parent = nullptr);
    ~EngineCache() override;

    // Best engine available now (full preferred), or null. Starts loading if needed.
    std::shared_ptr<DevelopEngine> get(const QString& path, bool wantFull = true);
    // Starts loading without returning anything (e.g. the next photo in the filmstrip).
    void prefetch(const QString& path);
    QString errorFor(const QString& path) const { return m_entries.value(path).error; }
    void clear();

signals:
    void engineReady(const QString& path);
    void loadFailed(const QString& path, const QString& error);

private:
    struct Entry {
        std::shared_ptr<DevelopEngine> half, full;
        bool loadingHalf = false, loadingFull = false;
        QString error;
        quint64 lastUse = 0;
    };
    void startLoad(const QString& path, bool full);
    void evict();

    QHash<QString, Entry> m_entries;
    quint64 m_clock = 0;
    int m_capacity = 4;
};

} // namespace silma
