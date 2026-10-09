#pragma once

#include "Develop.h"
#include "Engine.h"

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QSlider;
class QToolButton;
class QVBoxLayout;
class QComboBox;

namespace silma {

// Clickable section title: disclosure arrow and bold text, left aligned.
class SectionHeader : public QWidget {
    Q_OBJECT
public:
    SectionHeader(const QString& title, QWidget* parent = nullptr);
    void setExpanded(bool e);
    bool isExpanded() const { return m_expanded; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void toggled(bool expanded);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QFont titleFont() const;
    QString m_title;
    bool m_expanded = true;
    bool m_hover = false;
};

// A titled, collapsible section for the develop panel.
class CollapsibleSection : public QWidget {
    Q_OBJECT
public:
    CollapsibleSection(const QString& title, QWidget* parent = nullptr);
    QVBoxLayout* contentLayout() const { return m_layout; }
    void setExpanded(bool e);
    bool isExpanded() const;
    // Extra widgets (e.g. an "Auto" button) shown at the right of the header.
    void addHeaderWidget(QWidget* w);

signals:
    void toggled(bool expanded);

private:
    SectionHeader* m_header;
    QWidget* m_content;
    QVBoxLayout* m_layout;
    QWidget* m_headerRow;
};

// Clicking the groove puts the handle at the click position.
void makeSliderAbsolute(QSlider* s);

// Label + slider + spin box for one numeric develop parameter.
class ParamSlider : public QWidget {
    Q_OBJECT
public:
    ParamSlider(const ParamDef& def, QWidget* parent = nullptr);
    const ParamDef& def() const { return m_def; }
    double value() const { return m_value; }
    // Programmatic update; does not emit.
    void setValue(double v);
    // The value a double-click resets to (default: the parameter's default).
    void setResetValue(double v) { m_resetValue = v; }

signals:
    // `final` is false while dragging and true when the change is complete.
    void edited(double value, bool final);
    // The user is working with this parameter: dragging the slider or editing the number.
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    int toSlider(double v) const;
    double fromSlider(int s) const;
    void emitChange(double v, bool final);
    void updateActive();

    ParamDef m_def;
    double m_value;
    double m_resetValue;
    bool m_mired;
    QLabel* m_label;
    QSlider* m_slider;
    QDoubleSpinBox* m_spin;
    bool m_updating = false;
    bool m_active = false;
    bool m_spinFocused = false;
};

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);
    void setHistogram(const Histogram& h);
    void setInfo(const QString& text);
    QSize sizeHint() const override { return {260, 120}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Histogram m_hist{};
    bool m_valid = false;
    QString m_info;
};

// Editable tone curve with channel selector.
class CurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(QWidget* parent = nullptr);
    void setSettings(const DevelopSettings& s);
    // Channel 0 = RGB, 1..3 = R, G, B
    void setChannel(int c);
    QSize sizeHint() const override { return {260, 260}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

signals:
    void curveEdited(int channel, const CurvePoints& c, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QRectF plotRect() const;
    QPointF toPlot(QPointF norm) const;
    QPointF toNorm(QPointF widget) const;
    QList<QPointF>& points();
    void ensureEndpoints();

    CurvePoints m_curves[4];
    int m_channel = 0;
    int m_drag = -1;
    bool m_moved = false;
};

} // namespace silma
