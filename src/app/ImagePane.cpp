#include "ImagePane.h"

#include "Export.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QThreadPool>
#include <QWheelEvent>
#include <QtConcurrent>
#include <cmath>

namespace silma {

namespace {
QThreadPool* renderPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(2); // each render is internally parallel
        return p;
    }();
    return pool;
}
} // namespace

ImagePane::ImagePane(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(200, 150);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_detailTimer.setSingleShot(true);
    m_detailTimer.setInterval(40);
    connect(&m_detailTimer, &QTimer::timeout, this, &ImagePane::runDetail);
}

namespace {

// Magnifier cursor with a + (zoom in) or - (back to fit), outlined for any background.
QCursor loupeCursor(bool zoomIn, qreal dpr)
{
    const int size = 24;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(9.5, 9.5);
    const double r = 6.5;
    // Frame and handle: white with a dark outline; the sign dark on a light lens.
    for (auto [color, width] : {std::pair{QColor(0, 0, 0, 200), 4.0}, {QColor(Qt::white), 1.8}}) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r);
        p.drawLine(c + QPointF(r * 0.72, r * 0.72), QPointF(21, 21)); // handle
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 170));
    p.drawEllipse(c, r - 0.9, r - 0.9);
    p.setPen(QPen(QColor(20, 20, 20), 1.6, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(c - QPointF(3.2, 0), c + QPointF(3.2, 0));
    if (zoomIn)
        p.drawLine(c - QPointF(0, 3.2), c + QPointF(0, 3.2));
    p.end();
    return QCursor(pm, int(c.x()), int(c.y()));
}

} // namespace

void ImagePane::updateCursor()
{
    if (m_tool)
        return;
    if (m_cropPx.isEmpty() || !m_engine)
        unsetCursor();
    else
        setCursor(loupeCursor(m_view.fit, devicePixelRatioF()));
}

void ImagePane::setEngine(std::shared_ptr<DevelopEngine> engine)
{
    if (engine == m_engine)
        return;
    bool samePhoto = engine && m_engine && engine->path() == m_engine->path();
    m_engine = std::move(engine);
    if (!samePhoto) {
        m_base = {};
        m_detail = {};
    }
    m_cropPx = cropPx();
    clampCenter();
    scheduleBase();
    scheduleDetail();
    updateCursor();
    update();
}

void ImagePane::setSettings(const DevelopSettings& s)
{
    if (s == m_settings && !m_base.image.isNull())
        return;
    m_settings = s;
    m_cropPx = cropPx();
    clampCenter();
    scheduleBase();
    scheduleDetail();
}

void ImagePane::refresh()
{
    scheduleBase();
    scheduleDetail();
}

void ImagePane::setPlaceholder(const QImage& img)
{
    m_placeholder = img;
    update();
}

void ImagePane::setLabel(const QString& label)
{
    m_label = label;
    update();
}

void ImagePane::setMessage(const QString& msg)
{
    m_message = msg;
    update();
}

void ImagePane::setIgnoreCrop(bool on)
{
    if (on == m_ignoreCrop)
        return;
    m_ignoreCrop = on;
    m_base = {};
    m_detail = {};
    m_cropPx = cropPx();
    scheduleBase();
    scheduleDetail();
}

void ImagePane::setTool(PaneTool* tool)
{
    m_tool = tool;
    m_toolActive = false;
    unsetCursor();
    updateCursor();
    update();
}

void ImagePane::setOverlay(OverlayPainter p)
{
    m_overlay = std::move(p);
    update();
}

QSizeF ImagePane::cropPx() const
{
    if (!m_engine)
        return {};
    return m_engine->geometry(m_settings, m_ignoreCrop).cropSize();
}

double ImagePane::fitZoom() const
{
    if (m_cropPx.isEmpty())
        return 1.0;
    return std::min((width() - 8) / m_cropPx.width(), (height() - 8) / m_cropPx.height());
}

QRectF ImagePane::imageRect() const
{
    if (m_cropPx.isEmpty()) {
        if (!m_placeholder.isNull()) {
            QSizeF s = QSizeF(m_placeholder.size()).scaled(QSizeF(size()) - QSizeF(8, 8), Qt::KeepAspectRatio);
            return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
        }
        return {};
    }
    double z = currentZoom();
    QSizeF s(m_cropPx.width() * z, m_cropPx.height() * z);
    const double dpr = devicePixelRatioF();
    if (m_view.fit && z * dpr <= 1.0) // the size of the base render, so that it is drawn unscaled
        s = QSizeF(std::round(s.width() * dpr) / dpr, std::round(s.height() * dpr) / dpr);
    QPointF c = m_view.fit ? QPointF(0.5, 0.5) : m_view.center;
    // On the device pixel grid: renders drawn at a fractional offset would be resampled (blurred).
    QPointF tl(std::round((width() / 2.0 - c.x() * s.width()) * dpr) / dpr, std::round((height() / 2.0 - c.y() * s.height()) * dpr) / dpr);
    return QRectF(tl, s);
}

QPointF ImagePane::widgetToImage(QPointF w) const
{
    QRectF r = imageRect();
    if (r.isEmpty())
        return {};
    return {(w.x() - r.left()) / r.width(), (w.y() - r.top()) / r.height()};
}

QPointF ImagePane::imageToWidget(QPointF n) const
{
    QRectF r = imageRect();
    return {r.left() + n.x() * r.width(), r.top() + n.y() * r.height()};
}

void ImagePane::clampCenter()
{
    if (m_view.fit || m_cropPx.isEmpty())
        return;
    double z = m_view.zoom;
    double hw = width() / 2.0 / (m_cropPx.width() * z), hh = height() / 2.0 / (m_cropPx.height() * z);
    auto clampAxis = [](double c, double half) { return half >= 0.5 ? 0.5 : std::clamp(c, half, 1.0 - half); };
    m_view.center = QPointF(clampAxis(m_view.center.x(), hw), clampAxis(m_view.center.y(), hh));
}

void ImagePane::setViewState(const ViewState& v)
{
    if (v == m_view)
        return;
    m_view = v;
    clampCenter();
    scheduleDetail();
    updateCursor();
    update();
}

void ImagePane::zoomTo(double zoom, QPointF anchor)
{
    if (m_cropPx.isEmpty())
        return;
    QPointF imgPt = widgetToImage(anchor);
    double fz = fitZoom();
    zoom = std::clamp(zoom, std::min(fz, 0.05), 8.0);
    if (zoom <= fz * 1.001) {
        m_view.fit = true;
    } else {
        m_view.fit = false;
        m_view.zoom = zoom;
        // Keep the image point under the anchor fixed.
        QSizeF s(m_cropPx.width() * zoom, m_cropPx.height() * zoom);
        m_view.center = QPointF(imgPt.x() - (anchor.x() - width() / 2.0) / s.width(), imgPt.y() - (anchor.y() - height() / 2.0) / s.height());
        clampCenter();
    }
    scheduleDetail();
    updateCursor();
    update();
    emit viewStateChanged(m_view);
}

void ImagePane::toggleZoom(QPointF anchor)
{
    if (m_view.fit) {
        // 1:1 means one image pixel per device pixel.
        zoomTo(1.0 / devicePixelRatioF(), anchor);
    } else {
        m_view.fit = true;
        updateCursor();
        update();
        emit viewStateChanged(m_view);
    }
}

void ImagePane::scheduleBase()
{
    if (!m_engine)
        return;
    if (m_baseBusy) {
        m_baseDirty = true;
        return;
    }
    runBase();
}

void ImagePane::scheduleDetail()
{
    m_detailTimer.start();
}

// Downscaled views look like an export at that size: bicubic resampling, and the output
// sharpening chosen for exports.
namespace {
void previewQuality(RenderRequest& req)
{
    req.highQuality = true;
    const int level = QSettings().value("export/sharpening", int(OutputSharpening::Standard)).toInt();
    req.outputSharpening = outputSharpeningAmount(OutputSharpening(std::clamp(level, 0, 3)));
}
} // namespace

void ImagePane::runBase()
{
    if (!m_engine || m_cropPx.isEmpty() || width() < 10)
        return;
    // Fit-size render of the whole crop, in device pixels.
    double dpr = devicePixelRatioF();
    double z = std::min(fitZoom(), 1.0 / dpr) * dpr;
    RenderRequest req;
    req.width = std::max(1, int(std::round(m_cropPx.width() * z)));
    req.height = std::max(1, int(std::round(m_cropPx.height() * z)));
    req.ignoreCrop = m_ignoreCrop;
    previewQuality(req);
    m_baseBusy = true;
    m_baseDirty = false;
    quint64 gen = ++m_baseGen;
    auto engine = m_engine;
    auto settings = m_settings;
    QPointer<ImagePane> self(this);
    QThreadPool* pool = renderPool();
    pool->start([self, engine, settings, req, gen] {
        RenderResult r;
        try {
            r = engine->render(settings, req);
        } catch (...) {
        }
        QMetaObject::invokeMethod(qApp, [self, r, gen, engine] {
            if (!self)
                return;
            self->m_baseBusy = false;
            if (engine == self->m_engine && gen == self->m_baseGen && !r.image.isNull()) {
                self->m_base = {r.image, QRectF(0, 0, 1, 1), gen};
                self->m_placeholder = QImage();
                self->update();
                emit self->rendered(r.histogram, r.wb);
            }
            if (self->m_baseDirty)
                self->runBase();
        });
    });
}

void ImagePane::runDetail()
{
    if (!m_engine || m_cropPx.isEmpty() || m_view.fit) {
        m_detail = {};
        update();
        return;
    }
    if (m_detailBusy) {
        m_detailDirty = true;
        return;
    }
    double dpr = devicePixelRatioF();
    // The base render already has enough resolution for small zooms.
    if (m_view.zoom * dpr <= double(m_base.image.width()) / m_cropPx.width() * 1.05 && !m_base.image.isNull()) {
        m_detail = {};
        update();
        return;
    }
    QRectF r = imageRect();
    QRectF visible = QRectF(rect()).intersected(r);
    if (visible.isEmpty())
        return;
    QRectF roi((visible.left() - r.left()) / r.width(), (visible.top() - r.top()) / r.height(), visible.width() / r.width(), visible.height() / r.height());
    RenderRequest req;
    req.width = std::max(1, int(std::round(visible.width() * dpr)));
    req.height = std::max(1, int(std::round(visible.height() * dpr)));
    req.roi = roi;
    req.ignoreCrop = m_ignoreCrop;
    previewQuality(req);
    m_detailBusy = true;
    m_detailDirty = false;
    quint64 gen = ++m_detailGen;
    auto engine = m_engine;
    auto settings = m_settings;
    QPointer<ImagePane> self(this);
    renderPool()->start([self, engine, settings, req, gen, roi] {
        RenderResult res;
        try {
            res = engine->render(settings, req);
        } catch (...) {
        }
        QMetaObject::invokeMethod(qApp, [self, res, gen, roi, engine] {
            if (!self)
                return;
            self->m_detailBusy = false;
            if (engine == self->m_engine && gen == self->m_detailGen && !res.image.isNull()) {
                self->m_detail = {res.image, roi, gen};
                self->update();
            }
            if (self->m_detailDirty)
                self->runDetail();
        });
    });
}

void ImagePane::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(46, 46, 46));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF r = imageRect();
    if (!m_base.image.isNull()) {
        p.drawImage(r, m_base.image);
    } else if (!m_placeholder.isNull()) {
        QSizeF s = QSizeF(m_placeholder.size()).scaled(r.isEmpty() ? QSizeF(size()) : r.size(), Qt::KeepAspectRatio);
        QRectF pr(r.center() - QPointF(s.width() / 2, s.height() / 2), s);
        if (r.isEmpty())
            pr.moveCenter(QRectF(rect()).center());
        p.drawImage(pr, m_placeholder);
    }
    if (!m_detail.image.isNull() && !m_view.fit) {
        QRectF dr(r.left() + m_detail.roi.left() * r.width(), r.top() + m_detail.roi.top() * r.height(), m_detail.roi.width() * r.width(), m_detail.roi.height() * r.height());
        p.drawImage(dr, m_detail.image);
    }
    if (m_overlay && !r.isEmpty())
        m_overlay(p, [this](QPointF n) { return imageToWidget(n); }, r);
    if (m_tool && !r.isEmpty()) {
        p.save();
        m_tool->paint(p, *this);
        p.restore();
    }

    auto badge = [&](const QString& text, Qt::Alignment align) {
        QFont f = font();
        p.setFont(f);
        QRectF tr = p.fontMetrics().boundingRect(text).adjusted(-6, -3, 6, 3);
        QRectF area = QRectF(rect()).adjusted(8, 8, -8, -8);
        if (align & Qt::AlignRight)
            tr.moveTopRight(area.topRight());
        else if (align & Qt::AlignHCenter)
            tr.moveCenter(area.center());
        else
            tr.moveTopLeft(area.topLeft());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(tr, 4, 4);
        p.setPen(QColor(235, 235, 235));
        p.drawText(tr, Qt::AlignCenter, text);
    };
    if (!m_label.isEmpty())
        badge(m_label, Qt::AlignLeft);
    if (!m_view.fit)
        badge(QStringLiteral("%1%").arg(qRound(m_view.zoom * devicePixelRatioF() * 100)), Qt::AlignRight);
    if (!m_message.isEmpty())
        badge(m_message, Qt::AlignHCenter);
}

void ImagePane::resizeEvent(QResizeEvent*)
{
    clampCenter();
    scheduleBase();
    scheduleDetail();
}

void ImagePane::mousePressEvent(QMouseEvent* e)
{
    if (PaneTool* tool = m_tool; tool && e->button() == Qt::LeftButton && tool->press(*this, widgetToImage(e->position()), e)) {
        // The press may have ended the tool (e.g. a one-shot picker): then no drag follows.
        m_toolActive = m_tool == tool;
        return;
    }
    if (!m_interaction) {
        emit imagePressed(widgetToImage(e->position()), e);
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_pressed = true;
        m_dragging = false;
        m_pressPos = m_lastPos = e->position();
    }
}

void ImagePane::mouseMoveEvent(QMouseEvent* e)
{
    if (m_toolActive && m_tool) {
        m_tool->move(*this, widgetToImage(e->position()), e);
        update();
        return;
    }
    if (m_tool && !m_pressed) {
        setCursor(m_tool->cursorAt(*this, widgetToImage(e->position())));
        update();
    }
    if (!m_pressed)
        return;
    if (!m_dragging && QLineF(e->position(), m_pressPos).length() > 4)
        m_dragging = true;
    if (m_dragging && !m_view.fit && !m_cropPx.isEmpty()) {
        QPointF d = e->position() - m_lastPos;
        QRectF r = imageRect();
        m_view.center -= QPointF(d.x() / r.width(), d.y() / r.height());
        clampCenter();
        setCursor(Qt::ClosedHandCursor);
        update();
        scheduleDetail();
        emit viewStateChanged(m_view);
    }
    m_lastPos = e->position();
}

void ImagePane::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_toolActive && m_tool) {
        m_toolActive = false;
        m_tool->release(*this, widgetToImage(e->position()), e);
        update();
        return;
    }
    if (!m_pressed)
        return;
    m_pressed = false;
    updateCursor();
    if (!m_dragging && e->button() == Qt::LeftButton)
        toggleZoom(e->position());
    m_dragging = false;
}

void ImagePane::wheelEvent(QWheelEvent* e)
{
    if (m_cropPx.isEmpty())
        return;
    double steps = e->angleDelta().y() / 120.0;
    if (steps == 0)
        return;
    zoomTo(currentZoom() * std::pow(1.25, steps), e->position());
    e->accept();
}

} // namespace silma
