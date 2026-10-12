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

// `after` with its crop kept within the image when it changes the geometry of `before`
// (transform, lens distortion) or turns constrainCrop on, and constrainCrop is set: an
// automatic crop (the whole canvas, or autoCropRect's) is redone for the new geometry, so it
// also grows back; a crop set by hand stays as long as it shows no empty borders, else it
// shrinks about its centre. A change that sets the crop itself is left alone.
DevelopSettings constrainedCrop(const DevelopEngine& engine, const DevelopSettings& before, DevelopSettings after);
// Share of the crop (normalized canvas coordinates) without image data, which renders
// black: 0 when there is none, else at least 1e-4 (an empty sliver along an edge counts).
double emptyCropFraction(const Geometry& canvasGeometry, const QRectF& crop);
// Whether constrainedCrop may change anything (cheap; no engine needed).
bool cropConstraintApplies(const DevelopSettings& before, const DevelopSettings& after);

} // namespace silma
