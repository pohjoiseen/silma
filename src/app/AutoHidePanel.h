#pragma once
// Lightroom-style "auto hide & show" panel: an overlay at one edge of the window that
// slides in while the mouse is at that edge and hides again when it leaves.

#include <QFrame>
#include <QPropertyAnimation>
#include <QTimer>

namespace silma {

class AutoHidePanel : public QFrame {
    Q_OBJECT
public:
    // `edge` is Qt::LeftEdge, Qt::RightEdge or Qt::BottomEdge; the panel is a child of `window`.
    AutoHidePanel(Qt::Edge edge, QWidget* window);

    void setContent(QWidget* w);
    QWidget* takeContent();
    QWidget* content() const { return m_content; }

    // Width (left/right) or height (bottom) of the revealed panel.
    int extent() const { return m_extent; }
    void setExtent(int e);

    // Area (in window coordinates) the panel slides into; it sits at the `edge` side.
    void setArea(const QRect& r);

    // Enables the edge trigger; disabled panels stay hidden.
    void setActive(bool on);
    bool isActive() const { return m_active; }
    bool isRevealed() const { return m_revealed; }
    void reveal();
    void conceal(bool animate = true);

signals:
    void revealed();

private:
    QRect shownGeometry() const;
    QRect hiddenGeometry() const;
    void checkConceal();

    Qt::Edge m_edge;
    QWidget* m_content = nullptr;
    QWidget* m_trigger; // thin strip at the window edge
    QRect m_area;
    int m_extent = 300;
    bool m_active = false;
    bool m_revealed = false;
    int m_outsideTicks = 0;
    QTimer m_revealDelay;
    QTimer m_poll;
    QPropertyAnimation* m_anim;
    friend class EdgeTrigger;
};

} // namespace silma
