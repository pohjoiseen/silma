#include "DenoiseDialog.h"

#include "AiDenoise.h"
#include "Derived.h"
#include "Engine.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <mutex>

namespace silma {

namespace {

constexpr int kPreviewSize = 480;  // logical pixels, square
constexpr int kOverviewSize = 260; // long edge
constexpr int kMargin = 48;        // sensor pixels denoised around the region (tile edges)

// Runs `f` on the GUI thread if the dialog still exists.
template <typename F>
void post(QPointer<DenoiseDialog> dlg, F f)
{
    QMetaObject::invokeMethod(qApp, [dlg, f = std::move(f)]() mutable {
        if (dlg)
            f(dlg.data());
    }, Qt::QueuedConnection);
}

} // namespace

// The 1:1 preview: shows the denoised region, the original while the mouse is held, and
// follows drags (the region is rendered again when the drag ends).
class DenoisePreview : public QWidget {
    Q_OBJECT
public:
    explicit DenoisePreview(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(kPreviewSize, kPreviewSize);
        setCursor(Qt::OpenHandCursor);
        setToolTip(tr("Drag to move the preview; press and hold to see the original"));
    }
    void setImages(const QImage& before, const QImage& after)
    {
        m_before = before;
        m_after = after;
        m_drag = {};
        update();
    }
    void setAfter(const QImage& after)
    {
        m_after = after;
        update();
    }
    void setLabel(const QString& text)
    {
        m_label = text;
        update();
    }
    bool showingOriginal() const { return m_pressed; }

signals:
    void dragged(QPoint delta); // logical pixels, when the drag ends

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 40));
        const QImage& img = m_pressed || m_after.isNull() ? m_before : m_after;
        if (!img.isNull()) {
            const QSizeF s = img.deviceIndependentSize();
            p.drawImage(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2) + m_drag, img);
        }
        const QString text = m_pressed && !m_before.isNull() ? tr("Original") : m_label;
        if (!text.isEmpty()) {
            QFont f = font();
            f.setBold(true);
            p.setFont(f);
            const QRect box = p.fontMetrics().boundingRect(text).adjusted(-6, -3, 6, 3).translated(10 - p.fontMetrics().boundingRect(text).left() + 6, 10 + p.fontMetrics().ascent() + 3);
            p.fillRect(box, QColor(0, 0, 0, 150));
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, text);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_pressed = true;
        m_pressAt = e->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_pressed)
            return;
        m_drag = e->position().toPoint() - m_pressAt;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !m_pressed)
            return;
        m_pressed = false;
        setCursor(Qt::OpenHandCursor);
        const QPoint d = m_drag;
        if (d.manhattanLength() > 3)
            emit dragged(d); // the image stays shifted until the new region arrives
        else
            m_drag = {};
        update();
    }

private:
    QImage m_before, m_after;
    QString m_label;
    bool m_pressed = false;
    QPoint m_pressAt, m_drag;
};

// The whole photo with the previewed region marked; click or drag to move the region.
class DenoiseOverview : public QWidget {
    Q_OBJECT
public:
    explicit DenoiseOverview(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(kOverviewSize, kOverviewSize * 2 / 3);
        setCursor(Qt::PointingHandCursor);
    }
    void setImage(const QImage& img)
    {
        m_image = img;
        const QSizeF s = img.deviceIndependentSize();
        setFixedSize(s.toSize().expandedTo(QSize(1, 1)));
        update();
    }
    void setRegion(const QRectF& r)
    {
        m_region = r;
        update();
    }

signals:
    void moved(QPointF center);    // while dragging
    void released(QPointF center); // drag ended

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 40));
        if (!m_image.isNull())
            p.drawImage(QPointF(0, 0), m_image);
        if (m_region.isValid()) {
            const QRectF r(m_region.x() * width(), m_region.y() * height(), m_region.width() * width(), m_region.height() * height());
            const QRectF box = r.adjusted(-1, -1, 1, 1);
            p.setPen(QPen(QColor(0, 0, 0, 160), 3));
            p.drawRect(box);
            p.setPen(QPen(Qt::white, 1));
            p.drawRect(box);
        }
    }
    void mousePressEvent(QMouseEvent* e) override { emit moved(at(e)); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            emit moved(at(e));
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
            emit released(at(e));
    }

private:
    QPointF at(QMouseEvent* e) const { return {e->position().x() / std::max(1, width()), e->position().y() / std::max(1, height())}; }

    QImage m_image;
    QRectF m_region;
};

// Everything the background jobs share. Jobs run one at a time (`mutex`).
struct DenoiseDialog::State {
    std::mutex mutex;
    QString modelId, path;
    QMap<qint64, HealPatch> patches;
    std::shared_ptr<DevelopEngine> engine;
    std::unique_ptr<AiDenoiser> denoiser;
    std::atomic<int> strength{75};
    std::atomic<bool> closing{false};
    std::shared_ptr<std::atomic<bool>> regionCancel;
    // The current region: denoised at full strength, and how to render it.
    int denGen = -1;
    ImageF den;
    QRect denRect;
    RenderRequest req;
    DevelopSettings after;

    QImage renderAfter()
    {
        ImageF blended = den;
        blendDenoised(blended, engine->raw().image, denRect, strength / 100.0);
        engine->setSensorOverlay(std::make_shared<HealPatch>(HealPatch{denRect, std::move(blended)}));
        return engine->render(after, req).image;
    }
};

DenoiseDialog::DenoiseDialog(const QString& modelId, const QString& path, const DevelopSettings& settings, const QMap<qint64, HealPatch>& patches,
                             int photoCount, int renderedLeftOut, QWidget* parent)
    : QDialog(parent), m_state(std::make_shared<State>()), m_settings(settings)
{
    setWindowTitle(tr("AI Denoise"));
    m_state->modelId = modelId;
    m_state->path = path;
    m_state->patches = patches;

    auto* top = new QHBoxLayout(this);
    // Wrapped labels report their height for a width only: size the dialog from its contents.
    top->setSizeConstraint(QLayout::SetFixedSize);
    auto* left = new QVBoxLayout();
    m_preview = new DenoisePreview(this);
    m_preview->setObjectName("preview");
    left->addWidget(m_preview);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setMaximumWidth(kPreviewSize);
    left->addWidget(m_status);
    left->addStretch();
    top->addLayout(left);

    auto* right = new QVBoxLayout();
    m_overview = new DenoiseOverview(this);
    right->addWidget(m_overview, 0, Qt::AlignHCenter);
    auto* form = new QFormLayout();
    const AiModelInfo* m = findAiModel(modelId);
    form->addRow(tr("Model:"), new QLabel(m ? QStringLiteral("%1 — %2").arg(m->name, m->license) : modelId, this));
    auto* strengthRow = new QHBoxLayout();
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 100);
    m_spin = new QSpinBox(this);
    m_spin->setRange(0, 100);
    m_spin->setSuffix(QStringLiteral(" %"));
    strengthRow->addWidget(m_slider, 1);
    strengthRow->addWidget(m_spin);
    form->addRow(tr("Strength:"), strengthRow);
    right->addLayout(form);
    connect(m_slider, &QSlider::valueChanged, m_spin, &QSpinBox::setValue);
    connect(m_spin, &QSpinBox::valueChanged, m_slider, &QSlider::setValue);
    m_strengthTimer.setSingleShot(true);
    m_strengthTimer.setInterval(120);
    connect(&m_strengthTimer, &QTimer::timeout, this, &DenoiseDialog::requestStrength);
    connect(m_spin, &QSpinBox::valueChanged, this, [this](int v) {
        m_state->strength = v;
        m_strengthTimer.start();
        updateStatus();
    });
    // At full strength the finest detail starts to go.
    m_spin->setValue(QSettings().value("ai/strength", 75).toInt());

    const QStringList prov = aiExecutionProviders();
    const bool gpu = prov.contains("MIGraphX") || prov.contains("ROCM") || prov.contains("CUDA");
    auto* info = new QLabel(gpu ? tr("Runs on the GPU (%1).").arg(prov.join(", "))
                                : tr("Runs on the CPU: about 10 s per 24 MP photo. Install onnxruntime-rocm (AMD) or onnxruntime-cuda for GPU acceleration."),
                            this);
    info->setWordWrap(true);
    info->setFixedWidth(kOverviewSize + 60);
    right->addWidget(info);
    right->addWidget(new QLabel(tr("%n photo(s) will be denoised into new DNG files.", nullptr, photoCount), this));
    if (renderedLeftOut)
        right->addWidget(new QLabel(tr("%n JPEG/HEIC photo(s) left out (AI Denoise works on raw photos).", nullptr, renderedLeftOut), this));
    right->addStretch();
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Denoise"));
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        QSettings().setValue("ai/strength", strength());
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(bb);
    top->addLayout(right);

    connect(m_preview, &DenoisePreview::dragged, this, [this](QPoint d) {
        if (!m_loaded || m_outputSize.isEmpty())
            return;
        const qreal dpr = devicePixelRatioF();
        setRegionCenter(m_center - QPointF(d.x() * dpr / m_outputSize.width(), d.y() * dpr / m_outputSize.height()));
    });
    connect(m_overview, &DenoiseOverview::moved, this, [this](QPointF c) {
        // Mark the new place right away; render when the drag ends.
        const QSize px = previewPixels();
        const double rw = std::min(1.0, px.width() / std::max(1.0, m_outputSize.width()));
        const double rh = std::min(1.0, px.height() / std::max(1.0, m_outputSize.height()));
        c = QPointF(std::clamp(c.x(), rw / 2, 1 - rw / 2), std::clamp(c.y(), rh / 2, 1 - rh / 2));
        m_overview->setRegion(QRectF(c.x() - rw / 2, c.y() - rh / 2, rw, rh));
    });
    connect(m_overview, &DenoiseOverview::released, this, &DenoiseDialog::setRegionCenter);

    updateStatus();
    // Load the photo (full resolution, a private engine: the overlay changes its renders) and
    // the model in the background.
    auto st = m_state;
    QPointer<DenoiseDialog> self(this);
    const QSize overviewPx = QSize(kOverviewSize, kOverviewSize) * devicePixelRatioF();
    const qreal dpr = devicePixelRatioF();
    (void)QtConcurrent::run([st, self, settings, overviewPx, dpr] {
        std::lock_guard lock(st->mutex);
        QString error;
        QImage overview;
        QSizeF out;
        try {
            st->engine = std::make_shared<DevelopEngine>(st->path);
            addHealPatches(*st->engine, st->patches);
            out = st->engine->outputSize(settings);
            const double k = std::min(double(overviewPx.width()) / out.width(), double(overviewPx.height()) / out.height());
            overview = st->engine->render(settings, {.width = std::max(1, int(out.width() * k)), .height = std::max(1, int(out.height() * k))}).image;
            overview.setDevicePixelRatio(dpr);
            if (!st->closing)
                st->denoiser = std::make_unique<AiDenoiser>(st->modelId, st->path, st->engine->raw());
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        post(self, [error, overview, out](DenoiseDialog* d) { d->loaded(error, overview, out); });
    });
}

DenoiseDialog::~DenoiseDialog()
{
    m_state->closing = true;
    if (m_state->regionCancel)
        *m_state->regionCancel = true;
}

int DenoiseDialog::strength() const { return m_spin->value(); }

void DenoiseDialog::setStrength(int percent) { m_spin->setValue(percent); }

QSize DenoiseDialog::previewPixels() const { return QSize(kPreviewSize, kPreviewSize) * devicePixelRatioF(); }

void DenoiseDialog::loaded(const QString& error, const QImage& overview, QSizeF outputSize)
{
    if (!error.isEmpty()) {
        m_status->setText(tr("No preview: %1").arg(error));
        return;
    }
    m_loaded = true;
    m_outputSize = outputSize;
    m_overview->setImage(overview);
    requestRegion();
}

void DenoiseDialog::setRegionCenter(QPointF c)
{
    m_center = c;
    if (m_loaded)
        requestRegion();
}

void DenoiseDialog::requestRegion()
{
    // The region in normalized crop coordinates: preview-sized at 1:1, inside the photo.
    const QSize px = previewPixels();
    const double rw = std::min(1.0, px.width() / m_outputSize.width()), rh = std::min(1.0, px.height() / m_outputSize.height());
    m_center = QPointF(std::clamp(m_center.x(), rw / 2, 1 - rw / 2), std::clamp(m_center.y(), rh / 2, 1 - rh / 2));
    const QRectF roi(m_center.x() - rw / 2, m_center.y() - rh / 2, rw, rh);
    m_overview->setRegion(roi);
    const int gen = ++m_regionGen;
    m_denoising = true;
    updateStatus();
    if (m_state->regionCancel)
        *m_state->regionCancel = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_state->regionCancel = cancel;

    auto st = m_state;
    QPointer<DenoiseDialog> self(this);
    const DevelopSettings s = m_settings;
    const RenderRequest req{.width = std::max(1, int(std::lround(rw * m_outputSize.width()))), .height = std::max(1, int(std::lround(rh * m_outputSize.height()))), .roi = roi};
    const qreal dpr = devicePixelRatioF();
    (void)QtConcurrent::run([st, self, s, req, gen, cancel, dpr] {
        std::lock_guard lock(st->mutex);
        if (*cancel || st->closing || !st->engine)
            return;
        QImage before, after;
        QString error;
        try {
            st->engine->setSensorOverlay(nullptr);
            before = st->engine->render(s, req).image;
            before.setDevicePixelRatio(dpr);
            post(self, [gen, before](DenoiseDialog* d) { d->regionRendered(gen, before, QImage(), QString()); });
            if (!st->denoiser)
                return;
            // The sensor area behind the region (the geometry may rotate and bend it).
            const Geometry g = st->engine->geometry(s);
            const RawImage& raw = st->engine->raw();
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            for (int i = 0; i <= 16; ++i)
                for (int j = 0; j <= 16; ++j) {
                    if (i != 0 && i != 16 && j != 0 && j != 16)
                        continue;
                    float xy[6], gain[3];
                    g.mapCrop(req.roi.x() + req.roi.width() * i / 16, req.roi.y() + req.roi.height() * j / 16, xy, gain);
                    x0 = std::min(x0, xy[2] * double(raw.fullWidth));
                    x1 = std::max(x1, xy[2] * double(raw.fullWidth));
                    y0 = std::min(y0, xy[3] * double(raw.fullHeight));
                    y1 = std::max(y1, xy[3] * double(raw.fullHeight));
                }
            const QRect rect = QRect(QPoint(int(std::floor(x0)) - kMargin, int(std::floor(y0)) - kMargin), QPoint(int(std::ceil(x1)) + kMargin, int(std::ceil(y1)) + kMargin))
                                   .intersected(QRect(0, 0, raw.image.width, raw.image.height));
            st->den = st->denoiser->denoise(rect, {}, cancel.get());
            st->denRect = rect;
            st->denGen = gen;
            st->req = req;
            st->after = aiDenoisedSettings(s);
            const int strength = st->strength;
            after = st->renderAfter();
            after.setDevicePixelRatio(dpr);
            post(self, [gen, after, strength](DenoiseDialog* d) {
                d->regionRendered(gen, QImage(), after, QString());
                d->strengthRendered(gen, strength, after);
            });
        } catch (const std::exception& e) {
            if (*cancel || st->closing)
                return;
            error = QString::fromUtf8(e.what());
            post(self, [gen, error](DenoiseDialog* d) { d->regionRendered(gen, QImage(), QImage(), error); });
        }
    });
}

void DenoiseDialog::requestStrength()
{
    if (!m_loaded || m_denoising)
        return; // the region job renders at the latest strength
    const int gen = m_regionGen;
    const int request = ++m_strengthGen;
    auto st = m_state;
    QPointer<DenoiseDialog> self(this);
    const qreal dpr = devicePixelRatioF();
    (void)QtConcurrent::run([st, self, gen, request, dpr] {
        std::lock_guard lock(st->mutex);
        if (st->closing || st->denGen != gen || st->den.empty())
            return;
        const int strength = st->strength;
        QImage after = st->renderAfter();
        after.setDevicePixelRatio(dpr);
        post(self, [gen, request, strength, after](DenoiseDialog* d) {
            if (request == d->m_strengthGen)
                d->strengthRendered(gen, strength, after);
        });
    });
}

void DenoiseDialog::regionRendered(int gen, const QImage& before, const QImage& after, const QString& error)
{
    if (gen != m_regionGen)
        return;
    if (!before.isNull()) {
        m_before = before;
        m_after = {};
        m_preview->setImages(before, QImage());
        m_shownRegion = -1;
    }
    if (!error.isEmpty()) {
        m_denoising = false;
        m_status->setText(tr("No preview: %1").arg(error));
        return;
    }
    if (!after.isNull())
        m_denoising = false;
    updateStatus();
}

void DenoiseDialog::strengthRendered(int gen, int strength, const QImage& after)
{
    if (gen != m_regionGen)
        return;
    m_after = after;
    m_preview->setAfter(after);
    m_shownRegion = gen;
    m_shownStrength = strength;
    if (strength != m_spin->value())
        m_strengthTimer.start(); // moved on meanwhile
    updateStatus();
}

void DenoiseDialog::updateStatus()
{
    if (!m_loaded) {
        m_preview->setLabel(tr("Loading…"));
        m_status->setText(tr("Loading the photo and the model…"));
        return;
    }
    if (m_denoising) {
        m_preview->setLabel(tr("Denoising…"));
    } else {
        m_preview->setLabel(m_shownStrength >= 0 ? tr("Denoised %1 %").arg(m_shownStrength) : QString());
    }
    m_status->setText(tr("1:1 preview with the photo's settings. Drag it or click the overview to look elsewhere; press and hold to compare with the original."));
}

bool DenoiseDialog::previewReady() const { return !m_denoising && m_shownRegion == m_regionGen && m_shownStrength == m_spin->value(); }

QImage DenoiseDialog::previewImage() const { return m_after; }

QImage DenoiseDialog::originalImage() const { return m_before; }

} // namespace silma

#include "DenoiseDialog.moc"
