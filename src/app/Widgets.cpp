#include "Widgets.h"

#include "Filters.h"

#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace silma {

// ---- CollapsibleSection -------------------------------------------------------

SectionHeader::SectionHeader(const QString& title, QWidget* parent) : QWidget(parent), m_title(title)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

QFont SectionHeader::titleFont() const
{
    QFont f = font();
    f.setBold(true);
    return f;
}

QSize SectionHeader::sizeHint() const
{
    QFontMetrics fm(titleFont());
    return {fm.horizontalAdvance(m_title) + fm.height() + 8, fm.height() + 6};
}

void SectionHeader::setExpanded(bool e)
{
    if (e == m_expanded)
        return;
    m_expanded = e;
    update();
    emit toggled(e);
}

void SectionHeader::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        setExpanded(!m_expanded);
}

void SectionHeader::enterEvent(QEnterEvent*)
{
    m_hover = true;
    update();
}

void SectionHeader::leaveEvent(QEvent*)
{
    m_hover = false;
    update();
}

void SectionHeader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    QColor fg = pal.color(QPalette::WindowText);
    if (!m_hover)
        fg.setAlphaF(0.85f);
    // Disclosure triangle.
    const double h = height(), a = std::round(h * 0.18);
    QPointF c(a + 3, h / 2);
    QPolygonF tri;
    if (m_expanded)
        tri << c + QPointF(-a, -a * 0.5) << c + QPointF(a, -a * 0.5) << c + QPointF(0, a * 0.6);
    else
        tri << c + QPointF(-a * 0.5, -a) << c + QPointF(-a * 0.5, a) << c + QPointF(a * 0.6, 0);
    p.setPen(Qt::NoPen);
    p.setBrush(fg);
    p.drawPolygon(tri);
    p.setPen(fg);
    p.setFont(titleFont());
    p.drawText(rect().adjusted(int(2 * a + 8), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_title);
}

CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Separator line above each section.
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setForegroundRole(QPalette::Mid);
    outer->addWidget(line);
    m_headerRow = new QWidget(this);
    auto* hl = new QHBoxLayout(m_headerRow);
    hl->setContentsMargins(0, 1, 0, 1);
    hl->setSpacing(4);
    m_header = new SectionHeader(title, m_headerRow);
    hl->addWidget(m_header, 1);
    outer->addWidget(m_headerRow);
    m_content = new QWidget(this);
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(4, 0, 2, 6);
    m_layout->setSpacing(1);
    outer->addWidget(m_content);
    connect(m_header, &SectionHeader::toggled, this, [this](bool on) {
        m_content->setVisible(on);
        emit toggled(on);
    });
}

void CollapsibleSection::setExpanded(bool e) { m_header->setExpanded(e); }
bool CollapsibleSection::isExpanded() const { return m_header->isExpanded(); }
void CollapsibleSection::addHeaderWidget(QWidget* w) { m_headerRow->layout()->addWidget(w); }

// ---- ParamSlider ---------------------------------------------------------------

namespace {
constexpr int kSliderSteps = 1000;
}

ParamSlider::ParamSlider(const ParamDef& def, QWidget* parent)
    : QWidget(parent), m_def(def), m_value(def.def), m_mired(QLatin1String(def.key) == QLatin1String("temperature"))
{
    // One row: label, slider, value (like Lightroom).
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    m_label = new QLabel(QString::fromUtf8(def.label), this);
    m_label->setToolTip(tr("Double-click to reset"));
    m_label->installEventFilter(this);
    // Common label width so the sliders line up across sections.
    m_label->setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("Luminance NR")) + 4);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, kSliderSteps);
    m_slider->setPageStep(kSliderSteps / 20);
    m_slider->installEventFilter(this);
    m_spin = new QDoubleSpinBox(this);
    m_spin->setRange(def.min, def.max);
    m_spin->setDecimals(def.decimals);
    m_spin->setSingleStep(def.step);
    m_spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spin->setFrame(false);
    m_spin->setAlignment(Qt::AlignRight);
    m_spin->setKeyboardTracking(false);
    m_spin->setFixedWidth(m_spin->fontMetrics().horizontalAdvance(QStringLiteral("-0000.00")) + 6);
    m_spin->installEventFilter(this);
    l->addWidget(m_label);
    l->addWidget(m_slider, 1);
    l->addWidget(m_spin);
    setValue(def.def);

    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        if (m_updating)
            return;
        emitChange(fromSlider(v), !m_slider->isSliderDown());
    });
    connect(m_slider, &QSlider::sliderReleased, this, [this] { emitChange(fromSlider(m_slider->value()), true); });
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating)
            emitChange(v, true);
    });
}

int ParamSlider::toSlider(double v) const
{
    if (m_mired) {
        // Temperature slider is linear in mired (perceptually even).
        double lo = 1e6 / m_def.max, hi = 1e6 / m_def.min;
        return int(std::round((hi - 1e6 / v) / (hi - lo) * kSliderSteps));
    }
    return int(std::round((v - m_def.min) / (m_def.max - m_def.min) * kSliderSteps));
}

double ParamSlider::fromSlider(int s) const
{
    double t = double(s) / kSliderSteps;
    double v;
    if (m_mired) {
        double lo = 1e6 / m_def.max, hi = 1e6 / m_def.min;
        v = 1e6 / (hi - t * (hi - lo));
    } else {
        v = m_def.min + t * (m_def.max - m_def.min);
    }
    // Snap to the parameter's step.
    v = std::round(v / m_def.step) * m_def.step;
    return std::clamp(v, m_def.min, m_def.max);
}

void ParamSlider::setValue(double v)
{
    m_updating = true;
    m_value = v;
    m_slider->setValue(toSlider(v));
    m_spin->setValue(v);
    m_updating = false;
}

void ParamSlider::emitChange(double v, bool final)
{
    if (v == m_value && !final)
        return;
    m_value = v;
    m_updating = true;
    m_spin->setValue(v);
    if (!m_slider->isSliderDown())
        m_slider->setValue(toSlider(v));
    m_updating = false;
    emit edited(v, final);
}

bool ParamSlider::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_label && e->type() == QEvent::MouseButtonDblClick) {
        setValue(m_def.def);
        emit edited(m_def.def, true);
        return true;
    }
    if (o == m_slider && e->type() == QEvent::MouseButtonDblClick) {
        setValue(m_def.def);
        emit edited(m_def.def, true);
        return true;
    }
    // Don't let the wheel change sliders while scrolling the panel unless focused.
    if ((o == m_slider || o == m_spin) && e->type() == QEvent::Wheel && !static_cast<QWidget*>(o)->hasFocus()) {
        e->ignore();
        return true;
    }
    return QWidget::eventFilter(o, e);
}

// ---- HistogramWidget -----------------------------------------------------------

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(120);
}

void HistogramWidget::setHistogram(const Histogram& h)
{
    m_hist = h;
    m_valid = true;
    update();
}

void HistogramWidget::setInfo(const QString& text)
{
    m_info = text;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.fillRect(r, QColor(30, 30, 30));
    if (m_valid) {
        // Scale by a robust maximum (ignore the extreme bins which often spike).
        double mx = 1;
        for (const auto& c : m_hist)
            for (int i = 1; i < 255; ++i)
                mx = std::max(mx, double(c[i]));
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        // Channel overlaps add up to grey rather than glaring white.
        const QColor cols[3] = {QColor(115, 10, 10), QColor(10, 115, 10), QColor(10, 10, 115)};
        for (int c = 0; c < 3; ++c) {
            QPainterPath path;
            path.moveTo(r.left(), r.bottom());
            for (int i = 0; i < 256; ++i) {
                // Light smoothing over neighbouring bins.
                double v = m_hist[c][i];
                if (i > 0 && i < 255)
                    v = 0.25 * m_hist[c][i - 1] + 0.5 * v + 0.25 * m_hist[c][i + 1];
                double h = std::sqrt(std::min(1.0, v / mx)) * (r.height() - 4);
                path.lineTo(r.left() + r.width() * i / 255.0, r.bottom() - h);
            }
            path.lineTo(r.right(), r.bottom());
            path.closeSubpath();
            p.fillPath(path, cols[c]);
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // Clipping indicators.
        quint64 total = 0;
        for (int i = 0; i < 256; ++i)
            total += m_hist[1][i];
        auto clipped = [&](int bin) {
            quint64 n = 0;
            for (int c = 0; c < 3; ++c)
                n = std::max<quint64>(n, m_hist[c][bin]);
            return total > 0 && double(n) / total > 0.002;
        };
        if (clipped(0))
            p.fillRect(QRectF(r.left() + 3, r.top() + 3, 8, 8), QColor(80, 140, 255));
        if (clipped(255))
            p.fillRect(QRectF(r.right() - 11, r.top() + 3, 8, 8), QColor(255, 90, 90));
    }
    if (!m_info.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        // Dark outline keeps the text readable over the histogram.
        QRectF tr = r.adjusted(4, 2, -4, -2);
        p.setPen(QColor(0, 0, 0, 200));
        for (QPointF d : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)})
            p.drawText(tr.translated(d), Qt::AlignBottom | Qt::AlignHCenter, m_info);
        p.setPen(QColor(225, 225, 225));
        p.drawText(tr, Qt::AlignBottom | Qt::AlignHCenter, m_info);
    }
}

// ---- CurveWidget -----------------------------------------------------------------

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(180, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
    setToolTip(tr("Click to add a point, drag to move, double-click a point to remove it"));
}

void CurveWidget::setSettings(const DevelopSettings& s)
{
    if (m_drag >= 0)
        return;
    m_curves[0] = s.curveRgb;
    m_curves[1] = s.curveR;
    m_curves[2] = s.curveG;
    m_curves[3] = s.curveB;
    update();
}

void CurveWidget::setChannel(int c)
{
    m_channel = std::clamp(c, 0, 3);
    update();
}

QRectF CurveWidget::plotRect() const
{
    double s = std::min(width(), height()) - 8;
    return QRectF((width() - s) / 2, (height() - s) / 2, s, s);
}

QPointF CurveWidget::toPlot(QPointF n) const
{
    QRectF r = plotRect();
    return {r.left() + n.x() * r.width(), r.bottom() - n.y() * r.height()};
}

QPointF CurveWidget::toNorm(QPointF w) const
{
    QRectF r = plotRect();
    return {std::clamp((w.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - w.y()) / r.height(), 0.0, 1.0)};
}

QList<QPointF>& CurveWidget::points() { return m_curves[m_channel].points; }

void CurveWidget::ensureEndpoints()
{
    auto& p = points();
    if (p.isEmpty())
        p = {QPointF(0, 0), QPointF(1, 1)};
}

void CurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = plotRect();
    p.fillRect(r, QColor(35, 35, 35));
    p.setPen(QPen(QColor(70, 70, 70), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    p.drawLine(r.bottomLeft(), r.topRight());
    const QColor cols[4] = {QColor(230, 230, 230), QColor(235, 80, 80), QColor(80, 210, 80), QColor(90, 130, 255)};
    // Faint other channels.
    for (int c = 0; c < 4; ++c) {
        if (m_curves[c].points.isEmpty() && c != m_channel)
            continue;
        CurveLut lut(m_curves[c].points);
        QPainterPath path;
        for (int i = 0; i <= 200; ++i) {
            double x = i / 200.0;
            QPointF pt = toPlot(QPointF(x, lut(float(x))));
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        QColor col = cols[c];
        if (c != m_channel)
            col.setAlpha(90);
        p.setPen(QPen(col, c == m_channel ? 2 : 1));
        p.drawPath(path);
    }
    p.setPen(QPen(cols[m_channel], 1.5));
    p.setBrush(QColor(30, 30, 30));
    for (const auto& pt : m_curves[m_channel].points)
        p.drawEllipse(toPlot(pt), 4, 4);
}

void CurveWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    ensureEndpoints();
    auto& pts = points();
    QPointF n = toNorm(e->position());
    m_drag = -1;
    for (int i = 0; i < pts.size(); ++i)
        if (QLineF(toPlot(pts[i]), e->position()).length() < 8)
            m_drag = i;
    if (m_drag < 0) {
        // Insert a new point on the curve at this x.
        int idx = 0;
        while (idx < pts.size() && pts[idx].x() < n.x())
            ++idx;
        pts.insert(idx, n);
        m_drag = idx;
    }
    m_moved = false;
    update();
}

void CurveWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0)
        return;
    auto& pts = points();
    QPointF n = toNorm(e->position());
    // Keep x ordering; endpoints may move vertically and inwards.
    double lo = m_drag > 0 ? pts[m_drag - 1].x() + 0.01 : 0.0;
    double hi = m_drag < pts.size() - 1 ? pts[m_drag + 1].x() - 0.01 : 1.0;
    n.setX(std::clamp(n.x(), lo, hi));
    pts[m_drag] = n;
    m_moved = true;
    update();
    emit curveEdited(m_channel, m_curves[m_channel], false);
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*)
{
    if (m_drag < 0)
        return;
    m_drag = -1;
    emit curveEdited(m_channel, m_curves[m_channel], true);
}

void CurveWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    auto& pts = points();
    for (int i = 1; i + 1 < pts.size(); ++i) {
        if (QLineF(toPlot(pts[i]), e->position()).length() < 8) {
            pts.removeAt(i);
            update();
            emit curveEdited(m_channel, m_curves[m_channel], true);
            return;
        }
    }
}

} // namespace silma
