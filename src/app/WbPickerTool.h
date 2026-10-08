#pragma once
// White balance picker: click a point that should be neutral.

#include "ImagePane.h"

#include <QObject>

namespace silma {

class WbPickerTool : public QObject, public PaneTool {
    Q_OBJECT
public:
    explicit WbPickerTool(QObject* parent = nullptr) : QObject(parent) {}

    void paint(QPainter& p, const ImagePane& pane) override;
    bool press(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void move(const ImagePane&, QPointF n, QMouseEvent*) override { m_hover = n; }
    void release(const ImagePane&, QPointF, QMouseEvent*) override {}
    Qt::CursorShape cursorAt(const ImagePane& pane, QPointF n) const override;

signals:
    void picked(QPointF cropPoint);

private:
    QPointF m_hover{-1, -1};
};

} // namespace silma
