#include "EngineCache.h"

#include <QPointer>
#include <QThreadPool>
#include <QtConcurrent>

namespace silma {

namespace {
QThreadPool* loaderPool()
{
    // Decoding is itself multi-threaded; two loaders overlap I/O and processing.
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}
} // namespace

EngineCache::EngineCache(QObject* parent) : QObject(parent) {}

EngineCache::~EngineCache() = default;

std::shared_ptr<DevelopEngine> EngineCache::get(const QString& path, bool wantFull)
{
    Entry& e = m_entries[path];
    e.lastUse = ++m_clock;
    if (!e.error.isEmpty())
        return nullptr;
    if (e.full)
        return e.full;
    if (!e.half && !e.loadingHalf)
        startLoad(path, false);
    if (wantFull && !e.loadingFull)
        startLoad(path, true);
    return e.half;
}

void EngineCache::prefetch(const QString& path)
{
    Entry& e = m_entries[path];
    if (e.lastUse == 0)
        e.lastUse = m_clock; // older than the current photo
    if (!e.half && !e.full && !e.loadingHalf && e.error.isEmpty())
        startLoad(path, false);
}

void EngineCache::clear() { m_entries.clear(); }

void EngineCache::startLoad(const QString& path, bool full)
{
    Entry& e = m_entries[path];
    (full ? e.loadingFull : e.loadingHalf) = true;
    QPointer<EngineCache> self(this);
    QThreadPool* pool = loaderPool();
    // Full decodes go to the back of the queue so half-size previews come first.
    auto task = [self, path, full] {
        std::shared_ptr<DevelopEngine> engine;
        QString error;
        try {
            engine = std::make_shared<DevelopEngine>(path, EngineLoadOptions{.halfSize = !full});
        } catch (const std::exception& ex) {
            error = QString::fromUtf8(ex.what());
        }
        QMetaObject::invokeMethod(qApp, [self, path, full, engine, error] {
            if (!self)
                return;
            auto it = self->m_entries.find(path);
            if (it == self->m_entries.end())
                return;
            (full ? it->loadingFull : it->loadingHalf) = false;
            if (!error.isEmpty()) {
                it->error = error;
                emit self->loadFailed(path, error);
                return;
            }
            if (full) {
                it->full = engine;
                it->half.reset();
            } else if (!it->full) {
                it->half = engine;
            }
            self->evict();
            emit self->engineReady(path);
        });
    };
    pool->start(QRunnable::create(task), full ? 0 : 1);
}

void EngineCache::evict()
{
    // Keep at most m_capacity entries holding engines, dropping least recently used.
    for (;;) {
        int loaded = 0;
        QString oldest;
        quint64 oldestUse = ~0ull;
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (!it->half && !it->full)
                continue;
            ++loaded;
            if (it->lastUse < oldestUse) {
                oldestUse = it->lastUse;
                oldest = it.key();
            }
        }
        if (loaded <= m_capacity)
            return;
        m_entries.remove(oldest);
    }
}

} // namespace silma
