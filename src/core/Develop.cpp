#include "Develop.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace silma {

bool CurvePoints::isIdentity() const
{
    for (const auto& p : points)
        if (std::abs(p.x() - p.y()) > 1e-6)
            return false;
    return true;
}

namespace {

using S = DevelopSettings;

const ParamDef kParams[] = {
    {"temperature", "Temp", "wb", 2000, 50000, 5500, 50, 0, &S::temperature},
    {"tint", "Tint", "wb", -150, 150, 0, 1, 0, &S::tint},
    {"exposure", "Exposure", "tone", -5, 5, 0, 0.05, 2, &S::exposure},
    {"contrast", "Contrast", "tone", -100, 100, 0, 1, 0, &S::contrast},
    {"highlights", "Highlights", "tone", -100, 100, 0, 1, 0, &S::highlights},
    {"shadows", "Shadows", "tone", -100, 100, 0, 1, 0, &S::shadows},
    {"whites", "Whites", "tone", -100, 100, 0, 1, 0, &S::whites},
    {"blacks", "Blacks", "tone", -100, 100, 0, 1, 0, &S::blacks},
    {"texture", "Texture", "presence", -100, 100, 0, 1, 0, &S::texture},
    {"clarity", "Clarity", "presence", -100, 100, 0, 1, 0, &S::clarity},
    {"dehaze", "Dehaze", "presence", -100, 100, 0, 1, 0, &S::dehaze},
    {"vibrance", "Vibrance", "presence", -100, 100, 0, 1, 0, &S::vibrance},
    {"saturation", "Saturation", "presence", -100, 100, 0, 1, 0, &S::saturation},
    {"lens_distortion", "Distortion", "lens", 0, 200, 100, 1, 0, &S::lensDistortion},
    {"lens_vignetting", "Vignetting", "lens", 0, 200, 100, 1, 0, &S::lensVignetting},
    {"vertical", "Vertical", "transform", -100, 100, 0, 1, 0, &S::perspVertical},
    {"horizontal", "Horizontal", "transform", -100, 100, 0, 1, 0, &S::perspHorizontal},
    {"rotate", "Rotate", "transform", -45, 45, 0, 0.1, 1, &S::rotate},
    {"scale", "Scale", "transform", 50, 150, 100, 1, 0, &S::scale},
    {"sharpen_amount", "Amount", "detail", 0, 150, 40, 1, 0, &S::sharpenAmount},
    {"sharpen_radius", "Radius", "detail", 0.5, 3, 1.0, 0.1, 1, &S::sharpenRadius},
    {"sharpen_detail", "Detail", "detail", 0, 100, 25, 1, 0, &S::sharpenDetail},
    {"sharpen_masking", "Masking", "detail", 0, 100, 0, 1, 0, &S::sharpenMasking},
    {"nr_luminance", "Luminance NR", "detail", 0, 100, 0, 1, 0, &S::nrLuminance},
    {"nr_luma_detail", "Luma detail", "detail", 0, 100, 50, 1, 0, &S::nrLumaDetail},
    {"nr_color", "Color NR", "detail", 0, 100, 25, 1, 0, &S::nrColor},
};

QJsonArray curveToJson(const CurvePoints& c)
{
    QJsonArray a;
    for (const auto& p : c.points)
        a.append(QJsonArray{p.x(), p.y()});
    return a;
}

CurvePoints curveFromJson(const QJsonValue& v)
{
    CurvePoints c;
    for (const auto& e : v.toArray()) {
        auto p = e.toArray();
        if (p.size() == 2)
            c.points.append(QPointF(std::clamp(p[0].toDouble(), 0.0, 1.0), std::clamp(p[1].toDouble(), 0.0, 1.0)));
    }
    std::sort(c.points.begin(), c.points.end(), [](auto& a, auto& b) { return a.x() < b.x(); });
    return c;
}

} // namespace

std::span<const ParamDef> paramDefs() { return kParams; }

namespace {
const SettingsPart kParts[] = {
    {"wb", "White Balance", "wb"},
    {"exposure", "Exposure", "tone"},
    {"contrast", "Contrast", "tone"},
    {"highlights", "Highlights", "tone"},
    {"shadows", "Shadows", "tone"},
    {"whites", "Whites", "tone"},
    {"blacks", "Blacks", "tone"},
    {"texture", "Texture", "presence"},
    {"clarity", "Clarity", "presence"},
    {"dehaze", "Dehaze", "presence"},
    {"vibrance", "Vibrance", "presence"},
    {"saturation", "Saturation", "presence"},
    {"curve", "Tone Curve", "curve"},
    {"lens_distortion", "Distortion", "lens"},
    {"lens_vignetting", "Vignetting", "lens"},
    {"vertical", "Vertical", "transform"},
    {"horizontal", "Horizontal", "transform"},
    {"rotate", "Rotate", "transform"},
    {"scale", "Scale", "transform"},
    {"crop", "Crop", "crop"},
    {"sharpen_amount", "Amount", "sharpening"},
    {"sharpen_radius", "Radius", "sharpening"},
    {"sharpen_detail", "Detail", "sharpening"},
    {"sharpen_masking", "Masking", "sharpening"},
    {"nr_luminance", "Luminance", "noise"},
    {"nr_luma_detail", "Luminance Detail", "noise"},
    {"nr_color", "Color", "noise"},
};
} // namespace

std::span<const SettingsPart> settingsParts() { return kParts; }

DevelopSettings copySettingsParts(const DevelopSettings& target, const DevelopSettings& source, const QSet<QString>& parts)
{
    DevelopSettings s = target;
    for (const QString& key : parts) {
        if (key == QLatin1String("wb")) {
            s.wbMode = source.wbMode;
            s.temperature = source.temperature;
            s.tint = source.tint;
        } else if (key == QLatin1String("curve")) {
            s.curveRgb = source.curveRgb;
            s.curveR = source.curveR;
            s.curveG = source.curveG;
            s.curveB = source.curveB;
        } else if (key == QLatin1String("crop")) {
            s.crop = source.crop;
            s.cropLockAspect = source.cropLockAspect;
            s.constrainCrop = source.constrainCrop;
        } else if (const ParamDef* p = findParam(key)) {
            s.*p->field = source.*p->field;
        }
    }
    return s;
}

const ParamDef* findParam(const QString& key)
{
    for (const auto& p : kParams)
        if (key == QLatin1String(p.key))
            return &p;
    return nullptr;
}

QString wbModeName(WbMode m)
{
    switch (m) {
    case WbMode::AsShot: return QStringLiteral("as-shot");
    case WbMode::Auto: return QStringLiteral("auto");
    case WbMode::Custom: return QStringLiteral("custom");
    }
    return {};
}

std::optional<WbMode> wbModeFromName(const QString& s)
{
    if (s == QLatin1String("as-shot")) return WbMode::AsShot;
    if (s == QLatin1String("auto")) return WbMode::Auto;
    if (s == QLatin1String("custom")) return WbMode::Custom;
    return std::nullopt;
}

DevelopSettings DevelopSettings::defaults(bool rendered)
{
    DevelopSettings s;
    if (rendered) {
        s.sharpenAmount = 0;
        s.nrColor = 0;
        s.lensDistortion = 0;
        s.lensVignetting = 0;
    }
    return s;
}

QJsonObject DevelopSettings::toJson() const
{
    static const DevelopSettings def;
    QJsonObject o;
    o["pv"] = processVersion;
    if (wbMode != def.wbMode)
        o["wb"] = wbModeName(wbMode);
    for (const auto& p : kParams) {
        if ((p.field == &S::temperature || p.field == &S::tint) && wbMode != WbMode::Custom)
            continue;
        double v = this->*p.field;
        if (std::abs(v - p.def) > 1e-9)
            o[p.key] = v;
    }
    auto curve = [&](const char* key, const CurvePoints& c) {
        if (!c.points.isEmpty() && !c.isIdentity())
            o[key] = curveToJson(c);
    };
    curve("curve", curveRgb);
    curve("curve_r", curveR);
    curve("curve_g", curveG);
    curve("curve_b", curveB);
    if (quarterTurns != 0)
        o["quarter_turns"] = quarterTurns;
    if (crop != def.crop)
        o["crop"] = QJsonArray{crop.x(), crop.y(), crop.width(), crop.height()};
    if (cropLockAspect != def.cropLockAspect)
        o["crop_lock_aspect"] = cropLockAspect;
    if (constrainCrop != def.constrainCrop)
        o["constrain_crop"] = constrainCrop;
    if (!heals.isEmpty()) {
        QJsonArray a;
        for (const auto& h : heals) {
            QJsonArray spot{h.x, h.y, h.radius, h.patchId};
            if (!h.path.isEmpty()) {
                QJsonArray pts;
                for (const QPointF& p : h.path)
                    pts << p.x() << p.y();
                spot.append(pts);
            }
            a.append(spot);
        }
        o["heal"] = a;
    }
    return o;
}

DevelopSettings DevelopSettings::fromJson(const QJsonObject& o)
{
    DevelopSettings s;
    s.processVersion = o.value("pv").toInt(kProcessVersion);
    if (auto m = wbModeFromName(o.value("wb").toString()))
        s.wbMode = *m;
    for (const auto& p : kParams)
        if (o.contains(p.key))
            s.*p.field = std::clamp(o.value(p.key).toDouble(p.def), p.min, p.max);
    s.curveRgb = curveFromJson(o.value("curve"));
    s.curveR = curveFromJson(o.value("curve_r"));
    s.curveG = curveFromJson(o.value("curve_g"));
    s.curveB = curveFromJson(o.value("curve_b"));
    s.quarterTurns = ((o.value("quarter_turns").toInt() % 4) + 4) % 4;
    auto c = o.value("crop").toArray();
    if (c.size() == 4)
        s.crop = QRectF(c[0].toDouble(), c[1].toDouble(), c[2].toDouble(), c[3].toDouble()).intersected(QRectF(0, 0, 1, 1));
    if (s.crop.width() < 0.01 || s.crop.height() < 0.01)
        s.crop = QRectF(0, 0, 1, 1);
    s.cropLockAspect = o.value("crop_lock_aspect").toBool(true);
    s.constrainCrop = o.value("constrain_crop").toBool(true);
    for (const auto& v : o.value("heal").toArray()) {
        auto a = v.toArray();
        if (a.size() < 4)
            continue;
        HealSpot h{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), qint64(a[3].toDouble()), {}};
        const QJsonArray pts = a.size() > 4 ? a[4].toArray() : QJsonArray();
        for (qsizetype i = 0; i + 1 < pts.size(); i += 2)
            h.path.append(QPointF(pts[i].toDouble(), pts[i + 1].toDouble()));
        s.heals.append(h);
    }
    return s;
}

QString DevelopSettings::toJsonString() const
{
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

DevelopSettings DevelopSettings::fromJsonString(const QString& s)
{
    return fromJson(QJsonDocument::fromJson(s.toUtf8()).object());
}

QString applyAssignment(DevelopSettings& s, const QString& assignment)
{
    int eq = assignment.indexOf('=');
    if (eq <= 0)
        return QStringLiteral("expected key=value, got '%1'").arg(assignment);
    QString key = assignment.left(eq).trimmed().replace('-', '_');
    QString val = assignment.mid(eq + 1).trimmed();

    if (key == QLatin1String("wb")) {
        auto m = wbModeFromName(val);
        if (!m)
            return QStringLiteral("wb must be as-shot, auto or custom");
        s.wbMode = *m;
        return {};
    }
    if (key == QLatin1String("crop")) {
        auto parts = val.split(',');
        if (val == QLatin1String("none") || val == QLatin1String("reset")) {
            s.crop = QRectF(0, 0, 1, 1);
            return {};
        }
        if (parts.size() != 4)
            return QStringLiteral("crop must be x,y,w,h (normalized) or none");
        QRectF r(parts[0].toDouble(), parts[1].toDouble(), parts[2].toDouble(), parts[3].toDouble());
        r = r.intersected(QRectF(0, 0, 1, 1));
        if (r.width() < 0.01 || r.height() < 0.01)
            return QStringLiteral("crop rectangle too small or outside the image");
        s.crop = r;
        return {};
    }
    if (key == QLatin1String("quarter_turns")) {
        bool ok = false;
        int n = val.toInt(&ok);
        if (!ok)
            return QStringLiteral("quarter_turns must be an integer (clockwise turns)");
        s.quarterTurns = ((n % 4) + 4) % 4;
        return {};
    }
    if (key == QLatin1String("crop_lock_aspect")) {
        s.cropLockAspect = val == QLatin1String("1") || val == QLatin1String("true") || val == QLatin1String("on");
        return {};
    }
    if (key == QLatin1String("constrain_crop")) {
        s.constrainCrop = val == QLatin1String("1") || val == QLatin1String("true") || val == QLatin1String("on");
        return {};
    }
    if (key.startsWith(QLatin1String("curve"))) {
        CurvePoints* c = key == QLatin1String("curve") ? &s.curveRgb
                         : key == QLatin1String("curve_r") ? &s.curveR
                         : key == QLatin1String("curve_g") ? &s.curveG
                         : key == QLatin1String("curve_b") ? &s.curveB
                                                           : nullptr;
        if (!c)
            return QStringLiteral("unknown curve '%1'").arg(key);
        c->points.clear();
        if (val == QLatin1String("none") || val == QLatin1String("linear"))
            return {};
        for (const QString& pt : val.split(',', Qt::SkipEmptyParts)) {
            auto xy = pt.split(':');
            if (xy.size() != 2)
                return QStringLiteral("curve points must be x:y pairs in 0..1");
            c->points.append(QPointF(std::clamp(xy[0].toDouble(), 0.0, 1.0), std::clamp(xy[1].toDouble(), 0.0, 1.0)));
        }
        std::sort(c->points.begin(), c->points.end(), [](auto& a, auto& b) { return a.x() < b.x(); });
        return {};
    }
    const ParamDef* p = findParam(key);
    if (!p)
        return QStringLiteral("unknown setting '%1'").arg(key);
    bool ok = false;
    double v = val.toDouble(&ok);
    if (!ok)
        return QStringLiteral("'%1' is not a number").arg(val);
    if (v < p->min || v > p->max)
        return QStringLiteral("%1 must be within %2..%3").arg(key).arg(p->min).arg(p->max);
    s.*p->field = v;
    if (p->field == &S::temperature || p->field == &S::tint)
        s.wbMode = WbMode::Custom;
    return {};
}

DevelopSettings turnedSettings(const DevelopSettings& in, int quarterTurnsCw)
{
    DevelopSettings s = in;
    const int n = ((quarterTurnsCw % 4) + 4) % 4;
    s.quarterTurns = (s.quarterTurns + n) % 4;
    for (int i = 0; i < n; ++i) {
        // The canvas turns clockwise: (u, v) -> (1 - v, u).
        const QRectF c = s.crop;
        s.crop = QRectF(1 - c.y() - c.height(), c.x(), c.height(), c.width());
        // Pitch and yaw swap axes; the small rotation and the scale stay.
        const double v = s.perspVertical, h = s.perspHorizontal;
        s.perspVertical = h;
        s.perspHorizontal = -v;
    }
    return s;
}

} // namespace silma
