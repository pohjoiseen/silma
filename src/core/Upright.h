#pragma once
// Automatic perspective correction ("Upright") and constrained crop.

#include "Develop.h"
#include "Geometry.h"
#include "Image.h"

#include <QPointF>
#include <QRectF>
#include <vector>

namespace silma {

class DevelopEngine;

struct LineSegment {
    QPointF a, b;
    double length() const;
};

// Detects straight line segments in a grey image (display-encoded, 0..1).
// minLength in pixels. Region-growing on gradient orientation (LSD-like).
std::vector<LineSegment> detectLineSegments(const PlaneF& gray, double minLength);

enum class UprightMode { Vertical, Horizontal, Both };

struct UprightResult {
    double vertical = 0, horizontal = 0, rotate = 0; // slider values
    int segmentsUsed = 0;
    bool ok = false;
};

// Estimates transform settings that make lines vertical/horizontal.
UprightResult autoUpright(const DevelopEngine& engine, const DevelopSettings& s, UprightMode mode);

// Largest crop (normalized canvas coords) with the given pixel aspect ratio (width / height)
// that contains only valid image data: centred in the canvas, or anywhere in it when the
// source has a validity mask (panoramas), where aspect <= 0 means any aspect ratio.
QRectF autoCropRect(const Geometry& canvasGeometry, double aspect);

} // namespace silma
