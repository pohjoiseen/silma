#pragma once
// Develop settings: everything that describes an edit. Stored in the batch as JSON
// (only non-default values), versioned by a process version so old edits keep rendering
// the same way after algorithm changes.

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <span>

namespace silma {

// Bump when rendering of existing settings changes; keep old behaviour selectable by pv.
constexpr int kProcessVersion = 1;

enum class WbMode { AsShot, Auto, Custom };

struct CurvePoints {
    QList<QPointF> points; // in [0,1]^2, sorted by x; empty = identity
    bool isIdentity() const;
    bool operator==(const CurvePoints&) const = default;
};

// A healed spot: centre and radius in sensor coordinates (normalized; radius relative to
// the sensor's long edge) with the generated fill stored in the batch.
struct HealSpot {
    double x = 0, y = 0, radius = 0.02;
    qint64 patchId = 0;
    bool operator==(const HealSpot&) const = default;
};

struct DevelopSettings {
    int processVersion = kProcessVersion;

    // White balance
    WbMode wbMode = WbMode::AsShot;
    double temperature = 5500; // K, used when wbMode == Custom
    double tint = 0;

    // Tone
    double exposure = 0;   // EV, -5..5
    double contrast = 0;   // -100..100
    double highlights = 0; // -100..100
    double shadows = 0;
    double whites = 0;
    double blacks = 0;

    // Presence
    double texture = 0; // -100..100, medium-scale detail
    double clarity = 0; // -100..100
    double dehaze = 0;  // -100..100
    double vibrance = 0;
    double saturation = 0;

    // Tone curve (applied to display-encoded values)
    CurvePoints curveRgb, curveR, curveG, curveB;

    // Lens corrections (profile always on; amounts in percent of the profile)
    double lensDistortion = 100; // 0..200
    double lensVignetting = 100; // 0..200

    // Transform
    double perspVertical = 0;   // -100..100
    double perspHorizontal = 0; // -100..100
    double rotate = 0;          // degrees, -45..45
    double scale = 100;         // percent, 50..150

    // Crop, in normalized coordinates of the transformed canvas
    QRectF crop{0, 0, 1, 1};
    bool cropLockAspect = true;

    // Heal
    QList<HealSpot> heals;

    // Detail
    double sharpenAmount = 40; // 0..150
    double sharpenRadius = 1.0; // 0.5..3
    double sharpenDetail = 25;  // 0..100
    double sharpenMasking = 0;  // 0..100
    double nrLuminance = 0;     // 0..100
    double nrLumaDetail = 50;   // 0..100
    double nrColor = 25;        // 0..100

    bool operator==(const DevelopSettings&) const = default;

    QJsonObject toJson() const; // only non-default values (plus "pv")
    static DevelopSettings fromJson(const QJsonObject& o);
    QString toJsonString() const;
    static DevelopSettings fromJsonString(const QString& s);
};

// Generic description of every scalar slider, used by the CLI and to build GUI panels.
struct ParamDef {
    const char* key;   // JSON / CLI key
    const char* label; // UI label
    const char* group; // panel id: "wb", "tone", "presence", "lens", "transform", "detail"
    double min, max, def, step;
    int decimals;
    double DevelopSettings::*field;
};

std::span<const ParamDef> paramDefs();
const ParamDef* findParam(const QString& key);

// Parts of the settings that can be pasted separately, in panel order. Keys are "wb"
// (mode, temperature and tint), "curve", "crop" and the scalar param keys otherwise.
// Heal spots are not among them: their fills are made from the source photo's pixels.
struct SettingsPart {
    const char* key;
    const char* label;
    const char* group; // "wb", "tone", "presence", "curve", "lens", "transform", "crop", "sharpening", "noise"
};
std::span<const SettingsPart> settingsParts();

// `target` with the given parts taken from `source`.
DevelopSettings copySettingsParts(const DevelopSettings& target, const DevelopSettings& source, const QSet<QString>& parts);

QString wbModeName(WbMode m);
std::optional<WbMode> wbModeFromName(const QString& s);

// Parses "a=b" assignments (scalar params, wb=as-shot|auto|custom, crop=x,y,w,h,
// curve=x:y,x:y,...) into settings; returns an error message or empty on success.
QString applyAssignment(DevelopSettings& s, const QString& assignment);

} // namespace silma
