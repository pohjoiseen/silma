#include "AutoHidePanel.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace silma {

namespace {
constexpr int kTriggerWidth = 6;   // px of window edge that reveals the panel
constexpr int kRevealDelayMs = 120; // avoid revealing when the mouse just passes by
constexpr int kPollMs = 120;
constexpr int kConcealTicks = 3;    // ~0.35 s outside before hiding
constexpr int kSlack = 12;          // px around the panel that still counts as inside
} // namespace

namespace {

// Drawn pin for icon themes without window-pin / window-unpin.
QIcon fallbackPinIcon(bool pinned, const QPalette& pal)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(16, 16);
    p.rotate(pinned ? 0 : 45);
    p.setPen(QPen(pal.color(QPalette::WindowText), 2.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(pal.color(QPalette::WindowText));
    p.drawRoundedRect(QRectF(-6, -12, 12, 4), 1, 1); // head
    p.drawLine(QPointF(-3, -8), QPointF(-3, 2));
    p.drawLine(QPointF(3, -8), QPointF(3, 2));
    p.drawLine(QPointF(-9, 2), QPointF(9, 2)); // collar
    p.drawLine(QPointF(0, 2), QPointF(0, 13)); // needle
    return QIcon(pm);
}

QToolButton* titleButton(QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    int s = parent->style()->pixelMetric(QStyle::PM_SmallIconSize);
    b->setIconSize(QSize(s, s));
    return b;
}

} // namespace

QToolButton* makePinButton(bool pinned, QWidget* parent)
{
    auto* b = titleButton(parent);
    const char* name = pinned ? "window-pin" : "window-unpin";
    b->setIcon(QIcon::hasThemeIcon(QLatin1String(name)) ? QIcon::fromTheme(QLatin1String(name)) : fallbackPinIcon(pinned, parent->palette()));
    b->setToolTip(pinned ? QObject::tr("Auto hide: show this panel only while the mouse is at the window edge")
                         : QObject::tr("Pin this panel (keep it visible)"));
    return b;
}

DockTitleBar::DockTitleBar(QDockWidget* dock) : QWidget(dock)
{
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(6, 1, 2, 1);
    l->setSpacing(0);
    m_title = new QLabel(this);
    l->addWidget(m_title, 1);
    auto updateTitle = [this, dock] { m_title->setText(dock->windowTitle().replace(QLatin1String("&&"), QLatin1String("&"))); };
    updateTitle();
    connect(dock, &QDockWidget::windowTitleChanged, this, updateTitle);

    QToolButton* pin = makePinButton(true, this);
    connect(pin, &QToolButton::clicked, this, &DockTitleBar::autoHideRequested);
    l->addWidget(pin);
    QToolButton* fl = titleButton(this);
    fl->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton, nullptr, this));
    fl->setToolTip(tr("Detach / attach"));
    connect(fl, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
    l->addWidget(fl);
    QToolButton* close = titleButton(this);
    close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, this));
    close->setToolTip(tr("Close"));
    connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
    l->addWidget(close);
    // Pinning makes no sense for a detached panel.
    connect(dock, &QDockWidget::topLevelChanged, pin, [pin](bool floating) { pin->setVisible(!floating); });
}

// Thin strip along the window edge with a small arrow; hovering it reveals the panel.
class EdgeTrigger : public QWidget {
public:
    EdgeTrigger(AutoHidePanel* panel, Qt::Edge edge, QWidget* parent) : QWidget(parent), m_panel(panel), m_edge(edge)
    {
        setAttribute(Qt::WA_Hover);
        setToolTip(QObject::tr("Hover to show the panel"));
    }

protected:
    void enterEvent(QEnterEvent*) override { m_panel->m_revealDelay.start(); }
    void leaveEvent(QEvent*) override { m_panel->m_revealDelay.stop(); }
    void mousePressEvent(QMouseEvent*) override { m_panel->reveal(); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().color(QPalette::Window));
        QColor c = palette().color(QPalette::WindowText);
        c.setAlphaF(0.6f);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        // Arrow pointing into the window.
        QPointF m = QRectF(rect()).center();
        const double a = 3;
        QPolygonF tri;
        switch (m_edge) {
        case Qt::LeftEdge: tri << m + QPointF(-a / 2, -a) << m + QPointF(-a / 2, a) << m + QPointF(a / 2, 0); break;
        case Qt::RightEdge: tri << m + QPointF(a / 2, -a) << m + QPointF(a / 2, a) << m + QPointF(-a / 2, 0); break;
        default: tri << m + QPointF(-a, a / 2) << m + QPointF(a, a / 2) << m + QPointF(0, -a / 2); break;
        }
        p.drawPolygon(tri);
    }

private:
    AutoHidePanel* m_panel;
    Qt::Edge m_edge;
};

AutoHidePanel::AutoHidePanel(Qt::Edge edge, QWidget* window) : QFrame(window), m_edge(edge)
{
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(1, 1, 1, 1);
    l->setSpacing(0);
    m_header = new QWidget(this);
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(6, 1, 2, 1);
    m_headerTitle = new QLabel(m_header);
    hl->addWidget(m_headerTitle, 1);
    QToolButton* pin = makePinButton(false, m_header);
    connect(pin, &QToolButton::clicked, this, &AutoHidePanel::pinRequested);
    hl->addWidget(pin);
    l->addWidget(m_header);
    m_header->hide();
    m_trigger = new EdgeTrigger(this, edge, window);
    m_trigger->hide();
    hide();

    m_revealDelay.setSingleShot(true);
    m_revealDelay.setInterval(kRevealDelayMs);
    connect(&m_revealDelay, &QTimer::timeout, this, [this] {
        // Still at the edge?
        if (m_trigger->rect().adjusted(-2, -2, 2, 2).contains(m_trigger->mapFromGlobal(QCursor::pos())))
            reveal();
    });
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &AutoHidePanel::checkConceal);

    m_anim = new QPropertyAnimation(this, "geometry", this);
    m_anim->setDuration(140);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QPropertyAnimation::finished, this, [this] {
        if (!m_revealed)
            hide();
    });
}

void AutoHidePanel::setTitle(const QString& title)
{
    m_headerTitle->setText(title);
    m_header->setVisible(!title.isEmpty());
}

void AutoHidePanel::setContent(QWidget* w)
{
    m_content = w;
    w->setParent(this);
    layout()->addWidget(w);
    w->show();
}

QWidget* AutoHidePanel::takeContent()
{
    QWidget* w = m_content;
    if (w) {
        layout()->removeWidget(w);
        w->setParent(nullptr);
    }
    m_content = nullptr;
    return w;
}

void AutoHidePanel::setExtent(int e)
{
    m_extent = std::max(80, e);
    setArea(m_area);
}

QRect AutoHidePanel::shownGeometry() const
{
    const QRect& a = m_area;
    switch (m_edge) {
    case Qt::LeftEdge: return QRect(a.left(), a.top(), std::min(m_extent, a.width()), a.height());
    case Qt::RightEdge: return QRect(a.right() + 1 - std::min(m_extent, a.width()), a.top(), std::min(m_extent, a.width()), a.height());
    default: return QRect(a.left(), a.bottom() + 1 - std::min(m_extent, a.height()), a.width(), std::min(m_extent, a.height()));
    }
}

QRect AutoHidePanel::hiddenGeometry() const
{
    QRect r = shownGeometry();
    switch (m_edge) {
    case Qt::LeftEdge: return r.translated(-r.width(), 0);
    case Qt::RightEdge: return r.translated(r.width(), 0);
    default: return r.translated(0, r.height());
    }
}

void AutoHidePanel::setArea(const QRect& r)
{
    m_area = r;
    QRect t;
    switch (m_edge) {
    case Qt::LeftEdge: t = QRect(r.left(), r.top(), kTriggerWidth, r.height()); break;
    case Qt::RightEdge: t = QRect(r.right() + 1 - kTriggerWidth, r.top(), kTriggerWidth, r.height()); break;
    default: t = QRect(r.left(), r.bottom() + 1 - kTriggerWidth, r.width(), kTriggerWidth); break;
    }
    m_trigger->setGeometry(t);
    m_trigger->raise();
    if (m_anim->state() == QAbstractAnimation::Running)
        m_anim->stop();
    if (m_revealed) {
        setGeometry(shownGeometry());
        raise();
    } else {
        hide();
    }
}

void AutoHidePanel::setActive(bool on)
{
    m_active = on;
    m_trigger->setVisible(on);
    if (on)
        m_trigger->raise();
    else
        conceal(false);
}

void AutoHidePanel::reveal()
{
    if (!m_active || m_revealed)
        return;
    m_revealed = true;
    m_outsideTicks = 0;
    m_anim->stop();
    if (!isVisible())
        setGeometry(hiddenGeometry());
    show();
    raise();
    m_anim->setStartValue(geometry());
    m_anim->setEndValue(shownGeometry());
    m_anim->start();
    m_poll.start();
    emit revealed();
}

void AutoHidePanel::conceal(bool animate)
{
    m_poll.stop();
    m_revealDelay.stop();
    if (!m_revealed && !isVisible())
        return;
    m_revealed = false;
    m_anim->stop();
    if (!animate || !isVisible()) {
        hide();
        return;
    }
    m_anim->setStartValue(geometry());
    m_anim->setEndValue(hiddenGeometry());
    m_anim->start();
}

void AutoHidePanel::checkConceal()
{
    // Stay while the user interacts: open popups (combo boxes, menus), held buttons
    // (slider drags) and dialogs.
    if (QApplication::activePopupWidget() || QApplication::mouseButtons() != Qt::NoButton || QApplication::activeModalWidget()) {
        m_outsideTicks = 0;
        return;
    }
    QRect r = shownGeometry().adjusted(-kSlack, -kSlack, kSlack, kSlack);
    QPoint p = parentWidget()->mapFromGlobal(QCursor::pos());
    bool inside = r.contains(p) || m_trigger->geometry().contains(p);
    m_outsideTicks = inside ? 0 : m_outsideTicks + 1;
    if (m_outsideTicks >= kConcealTicks)
        conceal();
}

} // namespace silma
