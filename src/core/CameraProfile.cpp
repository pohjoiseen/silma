#include "CameraProfile.h"

#include "TiffReader.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace silma {

namespace color {
const Mat3 kProPhotoToXyzD50 = {{{0.7976749, 0.1351917, 0.0313534}, {0.2880402, 0.7118741, 0.0000857}, {0.0, 0.0, 0.8252100}}};
const Mat3 kXyzD50ToProPhoto = matInverse(kProPhotoToXyzD50);
const Mat3& proPhotoToRec2020()
{
    static const Mat3 m = matMul(kXyzToRec2020, matMul(bradford(kD50, kD65), kProPhotoToXyzD50));
    return m;
}
} // namespace color

// ---- Hue/saturation tables ------------------------------------------------------------

HueSatTable HueSatTable::blend(const HueSatTable& a, const HueSatTable& b, double t)
{
    HueSatTable r = a;
    if (!b.valid() || b.data.size() != a.data.size())
        return r;
    const float ft = float(t);
    for (size_t i = 0; i < r.data.size(); ++i)
        r.data[i] = a.data[i] * (1 - ft) + b.data[i] * ft;
    return r;
}

void HueSatTable::apply(float* rgb) const
{
    // As the DNG SDK's RefBaselineHueSatMap: HSV with hue in [0, 6).
    const float r = std::max(rgb[0], 0.0f), g = std::max(rgb[1], 0.0f), b = std::max(rgb[2], 0.0f);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), gap = mx - mn;
    if (mx <= 0)
        return;
    float h = 0;
    if (gap > 0) {
        if (r == mx) {
            h = (g - b) / gap;
            if (h < 0)
                h += 6;
        } else if (g == mx) {
            h = 2 + (b - r) / gap;
        } else {
            h = 4 + (r - g) / gap;
        }
    }
    float s = gap / mx;
    float v = srgbValues ? srgbEncode(std::min(mx, 1.0f)) : mx;

    const float hScaled = h * (hues / 6.0f), sScaled = s * (sats - 1);
    int h0 = std::min(int(hScaled), hues - 1), h1 = h0 + 1;
    if (h0 >= hues - 1) {
        h0 = hues - 1;
        h1 = 0;
    }
    if (hues == 1)
        h1 = 0;
    const int s0 = std::min(int(sScaled), sats - 2);
    const float hf = std::clamp(hScaled - h0, 0.0f, 1.0f), sf = std::clamp(sScaled - s0, 0.0f, 1.0f);
    int v0 = 0;
    float vf = 0;
    if (vals > 1) {
        const float vScaled = std::clamp(v, 0.0f, 1.0f) * (vals - 1);
        v0 = std::min(int(vScaled), vals - 2);
        vf = vScaled - v0;
    }
    auto entry = [&](int vi, int hi, int si) { return &data[((size_t(vi) * hues + hi) * sats + si) * 3]; };
    float d[3];
    auto bilinear = [&](int vi, float* out) {
        const float *e00 = entry(vi, h0, s0), *e01 = entry(vi, h0, s0 + 1), *e10 = entry(vi, h1, s0), *e11 = entry(vi, h1, s0 + 1);
        for (int c = 0; c < 3; ++c)
            out[c] = (e00[c] * (1 - sf) + e01[c] * sf) * (1 - hf) + (e10[c] * (1 - sf) + e11[c] * sf) * hf;
    };
    bilinear(v0, d);
    if (vals > 1) {
        float d1[3];
        bilinear(v0 + 1, d1);
        for (int c = 0; c < 3; ++c)
            d[c] = d[c] * (1 - vf) + d1[c] * vf;
    }
    h += d[0] * (6.0f / 360.0f);
    s = std::min(s * d[1], 1.0f);
    v *= d[2];
    if (srgbValues)
        v = srgbDecode(std::clamp(v, 0.0f, 1.0f)) + std::max(mx - 1.0f, 0.0f);

    // HSV -> RGB
    while (h < 0)
        h += 6;
    while (h >= 6)
        h -= 6;
    const int i = std::min(int(h), 5);
    const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
    case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
    case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
    case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
    case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
    case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
    default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
    }
}

// ---- DCP --------------------------------------------------------------------------------

namespace {

QString normalizedModel(QString s)
{
    return s.simplified().toLower();
}

Mat3 readMatrix(const TiffReader& t, const TiffReader::Ifd& ifd, quint16 tag)
{
    Mat3 m{};
    const auto* e = ifd.find(tag);
    if (!e || e->count != 9)
        return m;
    for (int i = 0; i < 9; ++i)
        m[i / 3][i % 3] = t.real(*e, i);
    return m;
}

QString readString(const TiffReader& t, const TiffReader::Ifd& ifd, quint16 tag)
{
    const auto* e = ifd.find(tag);
    if (!e)
        return {};
    QByteArray b = t.blob(*e);
    const int z = b.indexOf('\0');
    return QString::fromUtf8(z >= 0 ? b.left(z) : b).trimmed();
}

HueSatTable readTable(const TiffReader& t, const TiffReader::Ifd& ifd, quint16 dimsTag, quint16 dataTag, quint16 encodingTag)
{
    HueSatTable tab;
    const auto* dims = ifd.find(dimsTag);
    const auto* data = ifd.find(dataTag);
    if (!dims || !data || dims->count < 2)
        return tab;
    tab.hues = int(t.uint(*dims, 0));
    tab.sats = int(t.uint(*dims, 1));
    tab.vals = dims->count > 2 ? std::max(1, int(t.uint(*dims, 2))) : 1;
    if (tab.hues <= 0 || tab.sats <= 1 || tab.hues > 1000 || tab.sats > 1000 || tab.vals > 1000
        || data->count != quint32(tab.hues) * tab.sats * tab.vals * 3)
        return {};
    tab.data.resize(data->count);
    for (quint32 i = 0; i < data->count; ++i)
        tab.data[i] = float(t.real(*data, i));
    tab.srgbValues = t.uint(ifd, encodingTag, 0) == 1;
    return tab;
}

} // namespace

bool CameraProfile::matchesCamera(const QString& make, const QString& model) const
{
    if (kind != Kind::Dcp)
        return false;
    const QString m = normalizedModel(cameraModel);
    return !m.isEmpty() && (m == normalizedModel(make + ' ' + model) || m == normalizedModel(model));
}

CameraProfile loadDcp(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot open the file");
    const QByteArray d = f.readAll();
    if (d.size() < 8 || !(d.startsWith("IIRC") || d.startsWith("MMCR") || d.startsWith("II*") || d.startsWith("MM\0*")))
        throw std::runtime_error("not a DNG camera profile");
    TiffReader t(d);
    const auto ifds = t.ifds();
    if (ifds.isEmpty())
        throw std::runtime_error("not a DNG camera profile");
    const auto& ifd = ifds.first();
    CameraProfile p;
    p.kind = CameraProfile::Kind::Dcp;
    p.file = QFileInfo(path).fileName();
    p.cameraModel = readString(t, ifd, 50708);
    p.name = readString(t, ifd, 50936);
    if (p.name.isEmpty())
        p.name = QFileInfo(path).completeBaseName();
    p.illuminant1 = int(t.uint(ifd, 50778, 0));
    p.illuminant2 = int(t.uint(ifd, 50779, 0));
    p.colorMatrix1 = readMatrix(t, ifd, 50721);
    p.colorMatrix2 = readMatrix(t, ifd, 50722);
    p.forwardMatrix1 = readMatrix(t, ifd, 50964);
    p.forwardMatrix2 = readMatrix(t, ifd, 50965);
    p.hueSatMap1 = readTable(t, ifd, 50937, 50938, 51107);
    p.hueSatMap2 = readTable(t, ifd, 50937, 50939, 51107);
    p.lookTable = readTable(t, ifd, 50981, 50982, 51108);
    if (const auto* e = ifd.find(51109))
        p.baselineExposureOffset = t.real(*e);
    if (matIsZero(p.colorMatrix1))
        throw std::runtime_error("the profile has no colour matrix");
    if (p.cameraModel.isEmpty())
        throw std::runtime_error("the profile does not name its camera");
    return p;
}

// ---- Camera Raw look profiles (.xmp) ----------------------------------------------------

namespace {

QString xmlUnescape(QString s)
{
    static const QRegularExpression num(QStringLiteral("&#(x?)([0-9A-Fa-f]+);"));
    QString out;
    qsizetype last = 0;
    for (auto it = num.globalMatch(s); it.hasNext();) {
        auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        bool ok = false;
        const uint c = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
        if (ok)
            out += QString::fromUcs4(reinterpret_cast<const char32_t*>(&c), 1);
        last = m.capturedEnd();
    }
    out += s.mid(last);
    out.replace(QLatin1String("&lt;"), QLatin1String("<"));
    out.replace(QLatin1String("&gt;"), QLatin1String(">"));
    out.replace(QLatin1String("&quot;"), QLatin1String("\""));
    out.replace(QLatin1String("&apos;"), QLatin1String("'"));
    out.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return out;
}

QString xmpAttribute(const QString& text, const QString& name)
{
    const QRegularExpression re(QStringLiteral("\\bcrs:%1\\s*=\\s*\"([^\"]*)\"").arg(name));
    const auto m = re.match(text);
    return m.hasMatch() ? xmlUnescape(m.captured(1)) : QString();
}

// A property given either as an attribute or as an element holding an rdf:Alt / plain text.
QString xmpText(const QString& text, const QString& name)
{
    QString v = xmpAttribute(text, name);
    if (!v.isEmpty())
        return v;
    const QRegularExpression re(QStringLiteral("<crs:%1>(.*?)</crs:%1>").arg(name), QRegularExpression::DotMatchesEverythingOption);
    const auto m = re.match(text);
    if (!m.hasMatch())
        return {};
    static const QRegularExpression li(QStringLiteral("<rdf:li[^>]*>(.*?)</rdf:li>"), QRegularExpression::DotMatchesEverythingOption);
    const auto l = li.match(m.captured(1));
    return xmlUnescape((l.hasMatch() ? l.captured(1) : m.captured(1)).trimmed());
}

CurvePoints xmpCurve(const QString& text, const QString& name)
{
    CurvePoints c;
    const QRegularExpression re(QStringLiteral("<crs:%1>(.*?)</crs:%1>").arg(name), QRegularExpression::DotMatchesEverythingOption);
    const auto m = re.match(text);
    if (!m.hasMatch())
        return c;
    static const QRegularExpression li(QStringLiteral("<rdf:li>\\s*([0-9.]+)\\s*,\\s*([0-9.]+)\\s*</rdf:li>"));
    for (auto it = li.globalMatch(m.captured(1)); it.hasNext();) {
        const auto p = it.next();
        c.points << QPointF(p.captured(1).toDouble() / 255.0, p.captured(2).toDouble() / 255.0);
    }
    if (c.isIdentity())
        c.points.clear();
    return c;
}

// Little-endian uint32 at `o` (the caller checks the size). Byte by byte: some GCC versions
// flag reading from constData() as out of bounds, taking it for the empty array's storage.
quint32 le32(const QByteArray& b, qsizetype o)
{
    return quint32(uchar(b.at(o))) | quint32(uchar(b.at(o + 1))) << 8 | quint32(uchar(b.at(o + 2))) << 16 | quint32(uchar(b.at(o + 3))) << 24;
}

// Camera Raw's encoding of large tables in XMP (dng_big_table): base 85 with its own alphabet
// (4 bytes, little-endian, per 5 characters), holding a 32-bit little-endian uncompressed
// size and a zlib stream.
QByteArray decodeBigTable(const QString& s)
{
    static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?`'|()[]{}@%$#";
    static const auto decodeTable = [] {
        std::array<int, 128> t;
        t.fill(-1);
        for (int i = 0; i < 85; ++i)
            t[uchar(kAlphabet[i])] = i;
        return t;
    }();
    QByteArray bytes;
    bytes.reserve(s.size() * 4 / 5 + 4);
    quint32 value = 0, mul = 1;
    int n = 0;
    for (QChar ch : s) {
        const ushort c = ch.unicode();
        if (c <= ' ')
            continue;
        if (c >= 128 || decodeTable[c] < 0)
            throw std::runtime_error("bad character in an encoded table");
        value += quint32(decodeTable[c]) * mul;
        mul *= 85;
        if (++n == 5) {
            char b[4];
            qToLittleEndian(value, b);
            bytes.append(b, 4);
            value = 0;
            mul = 1;
            n = 0;
        }
    }
    if (n > 1) {
        char b[4];
        qToLittleEndian(value, b);
        bytes.append(b, n - 1);
    }
    if (bytes.size() < 8)
        throw std::runtime_error("encoded table too short");
    // qUncompress wants a big-endian size in front of the zlib stream.
    const quint32 size = le32(bytes, 0);
    for (int k = 0; k < 4; ++k)
        bytes[k] = char(size >> (24 - 8 * k));
    QByteArray out = qUncompress(bytes);
    if (out.size() != qsizetype(size))
        throw std::runtime_error("cannot decompress an encoded table");
    return out;
}

HueSatTable parseLookTableStream(const QByteArray& b)
{
    // uint32 kind (0), version (1), hue, saturation and value divisions; the entries as
    // float triples; uint32 value encoding (0 linear, 1 sRGB).
    if (b.size() < 20)
        throw std::runtime_error("look table too short");
    auto u32 = [&](qsizetype o) { return le32(b, o); };
    HueSatTable t;
    t.hues = int(u32(8));
    t.sats = int(u32(12));
    t.vals = int(u32(16));
    if (u32(0) != 0 || t.hues <= 0 || t.sats <= 1 || t.vals <= 0 || t.hues > 1000 || t.sats > 1000 || t.vals > 1000)
        throw std::runtime_error("not a look table");
    const qsizetype n = qsizetype(t.hues) * t.sats * t.vals * 3;
    if (b.size() < 20 + n * 4)
        throw std::runtime_error("look table too short");
    t.data.resize(n);
    for (qsizetype i = 0; i < n; ++i) {
        const quint32 bits = le32(b, 20 + 4 * i);
        std::memcpy(&t.data[i], &bits, 4);
    }
    if (b.size() >= 20 + n * 4 + 4)
        t.srgbValues = u32(20 + n * 4) == 1;
    return t;
}

} // namespace

CameraProfile loadLookXmp(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot open the file");
    const QString text = QString::fromUtf8(f.readAll());
    if (!text.contains(QLatin1String("camera-raw-settings")))
        throw std::runtime_error("not a Camera Raw profile");
    if (xmpAttribute(text, QStringLiteral("PresetType")) != QLatin1String("Look"))
        throw std::runtime_error("a Camera Raw preset, not a profile");
    if (!xmpAttribute(text, QStringLiteral("RGBTable")).isEmpty())
        throw std::runtime_error("a creative profile with an RGB table (not supported)");
    if (xmpAttribute(text, QStringLiteral("ConvertToGrayscale")) == QLatin1String("True"))
        throw std::runtime_error("a monochrome profile (not supported)");
    CameraProfile p;
    p.kind = CameraProfile::Kind::Look;
    p.file = QFileInfo(path).fileName();
    p.name = xmpText(text, QStringLiteral("Name"));
    if (p.name.isEmpty())
        p.name = QFileInfo(path).completeBaseName();
    p.baseProfile = xmpAttribute(text, QStringLiteral("CameraProfile"));
    if (p.baseProfile.isEmpty())
        throw std::runtime_error("the profile does not name its base camera profile");
    p.modelRestriction = xmpAttribute(text, QStringLiteral("CameraModelRestriction"));
    const QString digest = xmpAttribute(text, QStringLiteral("LookTable"));
    if (!digest.isEmpty()) {
        const QString table = xmpAttribute(text, QStringLiteral("Table_") + digest);
        if (table.isEmpty())
            throw std::runtime_error("the look table is missing");
        p.lookTable = parseLookTableStream(decodeBigTable(table));
    }
    p.toneCurve = xmpCurve(text, QStringLiteral("ToneCurvePV2012"));
    return p;
}

CameraProfile loadProfile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("dcp"))
        return loadDcp(path);
    if (suffix == QLatin1String("xmp"))
        return loadLookXmp(path);
    throw std::runtime_error("not a .dcp or .xmp file");
}

// ---- Installed profiles -----------------------------------------------------------------

QString profileDir()
{
    QString d = qEnvironmentVariable("SILMA_PROFILE_DIR");
    if (d.isEmpty())
        d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/silma/profiles";
    return d;
}

QList<std::shared_ptr<const CameraProfile>> installedProfiles()
{
    static std::mutex mutex;
    static QString cachedKey;
    static QList<std::shared_ptr<const CameraProfile>> cached;
    const QString dir = profileDir();
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.dcp"), QStringLiteral("*.DCP"), QStringLiteral("*.xmp"), QStringLiteral("*.XMP")},
                                                        QDir::Files, QDir::Name);
    QString key = dir;
    for (const QFileInfo& fi : files)
        key += QStringLiteral("|%1:%2:%3").arg(fi.fileName()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    std::lock_guard lock(mutex);
    if (key == cachedKey)
        return cached;
    cached.clear();
    for (const QFileInfo& fi : files) {
        try {
            cached << std::make_shared<const CameraProfile>(loadProfile(fi.filePath()));
        } catch (const std::exception&) {
        }
    }
    cachedKey = key;
    return cached;
}

QStringList installProfiles(const QStringList& paths, QStringList* errors)
{
    QStringList files;
    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(p, {QStringLiteral("*.dcp"), QStringLiteral("*.DCP"), QStringLiteral("*.xmp"), QStringLiteral("*.XMP")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                files << it.next();
        } else {
            files << p;
        }
    }
    const QString dir = profileDir();
    QDir().mkpath(dir);
    QStringList names;
    for (const QString& file : files) {
        try {
            const CameraProfile p = loadProfile(file);
            const QString dst = dir + '/' + QFileInfo(file).fileName();
            if (QFileInfo(dst).canonicalFilePath() != QFileInfo(file).canonicalFilePath()) {
                QFile::remove(dst);
                if (!QFile::copy(file, dst))
                    throw std::runtime_error("cannot copy it to " + dir.toStdString());
            }
            names << (p.kind == CameraProfile::Kind::Dcp ? QStringLiteral("%1 (%2)").arg(p.name, p.cameraModel) : p.name);
        } catch (const std::exception& e) {
            if (errors)
                *errors << QStringLiteral("%1: %2").arg(QFileInfo(file).fileName(), QString::fromStdString(e.what()));
        }
    }
    return names;
}

QString defaultProfile()
{
    return QSettings().value(QStringLiteral("develop/profile")).toString();
}

void setDefaultProfile(const QString& name)
{
    QSettings().setValue(QStringLiteral("develop/profile"), name);
}

DevelopSettings newPhotoSettings(bool rendered)
{
    DevelopSettings s = DevelopSettings::defaults(rendered);
    if (!rendered)
        s.profile = defaultProfile();
    return s;
}

ResolvedProfile resolveProfile(const QString& name, const QString& make, const QString& model)
{
    ResolvedProfile r;
    if (name.isEmpty())
        return r;
    const auto all = installedProfiles();
    auto dcpNamed = [&](const QString& n) -> std::shared_ptr<const CameraProfile> {
        for (const auto& p : all)
            if (p->kind == CameraProfile::Kind::Dcp && p->name == n && p->matchesCamera(make, model))
                return p;
        return {};
    };
    if (auto d = dcpNamed(name)) {
        r.dcp = d;
        r.name = name;
        return r;
    }
    for (const auto& p : all) {
        if (p->kind != CameraProfile::Kind::Look || p->name != name)
            continue;
        if (!p->modelRestriction.isEmpty() && normalizedModel(p->modelRestriction) != normalizedModel(make + ' ' + model))
            continue;
        if (auto d = dcpNamed(p->baseProfile)) {
            r.dcp = d;
            r.look = p;
            r.name = name;
            return r;
        }
    }
    return r;
}

QStringList profilesForCamera(const QString& make, const QString& model)
{
    QStringList names;
    for (const auto& p : installedProfiles()) {
        if (p->kind == CameraProfile::Kind::Dcp ? p->matchesCamera(make, model) : resolveProfile(p->name, make, model).valid())
            if (!names.contains(p->name))
                names << p->name;
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

// ---- Transform --------------------------------------------------------------------------

ProfileTransform::ProfileTransform(const ResolvedProfile& p, const RawColorInfo& raw, const Vec3& mul)
    : m_dcp(p.dcp), m_lookProfile(p.look)
{
    const CameraProfile& d = *p.dcp;
    // The DCP's colour matrices determine the white point and the interpolation weight.
    RawColorInfo info = raw;
    info.dng[0] = {};
    info.dng[1] = {};
    info.dng[0].valid = true;
    info.dng[0].illuminant = d.illuminant1;
    info.dng[0].colorMatrix = d.colorMatrix1;
    if (!matIsZero(d.colorMatrix2)) {
        info.dng[1].valid = true;
        info.dng[1].illuminant = d.illuminant2;
        info.dng[1].colorMatrix = d.colorMatrix2;
    }
    info.xyzToCam = matIsZero(d.colorMatrix2) ? d.colorMatrix1 : d.colorMatrix2;
    CameraColor cc(info);
    const XY white = cc.whiteXyFor(mul);
    double temp, tint;
    xyToTempTint(white, temp, tint);
    // matrixWeight is for the lower-temperature illuminant.
    const double t1 = illuminantTemperature(d.illuminant1), t2 = illuminantTemperature(d.illuminant2);
    const double wLow = cc.matrixWeight(temp);
    const double w1 = matIsZero(d.colorMatrix2) ? 1.0 : (t1 <= t2 ? wLow : 1.0 - wLow);

    Mat3 fm{};
    const bool fm1 = !matIsZero(d.forwardMatrix1), fm2 = !matIsZero(d.forwardMatrix2);
    if (fm1 || fm2) {
        const Mat3& a = fm1 ? d.forwardMatrix1 : d.forwardMatrix2;
        const Mat3& b = fm2 ? d.forwardMatrix2 : d.forwardMatrix1;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                fm[i][j] = w1 * a[i][j] + (1 - w1) * b[i][j];
        // Normalize so that camera neutral maps to D50 exactly.
        const Vec3 xyz = matMul(fm, Vec3{1, 1, 1});
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                fm[i][j] *= color::kD50[i] / xyz[i];
    } else {
        // No forward matrix: invert the colour matrix and adapt the white to D50.
        fm = matMul(bradford(xyToXyz(white), color::kD50),
                    matMul(matInverse(cc.xyzToCam(temp)), matDiag({1.0 / mul[0], 1.0 / mul[1], 1.0 / mul[2]})));
        const Vec3 xyz = matMul(fm, Vec3{1, 1, 1});
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                fm[i][j] *= color::kD50[i] / xyz[i];
    }
    m_camToPP = matMul(color::kXyzD50ToProPhoto, fm);

    if (d.hueSatMap1.valid() && d.hueSatMap2.valid())
        m_hsm = HueSatTable::blend(d.hueSatMap1, d.hueSatMap2, 1 - w1);
    else if (d.hueSatMap1.valid())
        m_hsm = d.hueSatMap1;
    else if (d.hueSatMap2.valid())
        m_hsm = d.hueSatMap2;
    // A look profile's table takes the place of the DCP's own (as Camera Raw does: Adobe Color
    // over Adobe Standard matches Lightroom only this way).
    if (p.look && p.look->lookTable.valid())
        m_look = &p.look->lookTable;
    else if (d.lookTable.valid())
        m_look = &d.lookTable;
    m_baselineOffset = d.baselineExposureOffset;
}

} // namespace silma
