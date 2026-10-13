// Camera profiles (.dcp and Camera Raw look .xmp). Adobe's profiles cannot be part of the
// repository, so the tests write small synthetic ones.

#include "Batch.h"
#include "CameraProfile.h"
#include "Engine.h"
#include "Importer.h"
#include "TestRaws.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cstring>

using namespace silma;

namespace {

// A minimal little-endian DCP ("IIRC") writer.
class DcpWriter {
public:
    void ascii(quint16 tag, const QByteArray& s) { add(tag, 2, s.size() + 1, s + '\0'); }
    void shortValue(quint16 tag, quint16 v)
    {
        QByteArray b(2, 0);
        qToLittleEndian(v, b.data());
        add(tag, 3, 1, b);
    }
    void longs(quint16 tag, const QList<quint32>& v)
    {
        QByteArray b(v.size() * 4, 0);
        for (int i = 0; i < v.size(); ++i)
            qToLittleEndian(v[i], b.data() + 4 * i);
        add(tag, 4, v.size(), b);
    }
    void matrix(quint16 tag, const Mat3& m)
    {
        QByteArray b(72, 0);
        for (int i = 0; i < 9; ++i) {
            qToLittleEndian(qint32(std::lround(m[i / 3][i % 3] * 10000)), b.data() + 8 * i);
            qToLittleEndian(qint32(10000), b.data() + 8 * i + 4);
        }
        add(tag, 10, 9, b);
    }
    void floats(quint16 tag, const std::vector<float>& v)
    {
        QByteArray b(qsizetype(v.size()) * 4, 0);
        std::memcpy(b.data(), v.data(), b.size());
        add(tag, 11, quint32(v.size()), b);
    }
    QByteArray bytes() const
    {
        auto entries = m_entries;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.tag < b.tag; });
        QByteArray out("IIRC", 4);
        out.append(QByteArray(4, 0));
        qToLittleEndian(quint32(8), out.data() + 4);
        const qsizetype ifdSize = 2 + entries.size() * 12 + 4;
        QByteArray ifd(ifdSize, 0), data;
        qToLittleEndian(quint16(entries.size()), ifd.data());
        for (int i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            char* p = ifd.data() + 2 + 12 * i;
            qToLittleEndian(e.tag, p);
            qToLittleEndian(e.type, p + 2);
            qToLittleEndian(e.count, p + 4);
            if (e.data.size() <= 4) {
                std::memcpy(p + 8, e.data.constData(), e.data.size());
            } else {
                qToLittleEndian(quint32(8 + ifdSize + data.size()), p + 8);
                data += e.data;
                if (data.size() % 2)
                    data += '\0';
            }
        }
        return out + ifd + data;
    }

private:
    struct Entry {
        quint16 tag, type;
        quint32 count;
        QByteArray data;
    };
    void add(quint16 tag, quint16 type, quint32 count, const QByteArray& d) { m_entries.append({tag, type, count, d}); }
    QList<Entry> m_entries;
};

// hue x sat x 1 table with every entry (hueShift, satScale, valScale).
std::vector<float> uniformTable(int hues, int sats, int vals, float hueShift, float satScale, float valScale)
{
    std::vector<float> t;
    for (int i = 0; i < hues * sats * vals; ++i)
        t.insert(t.end(), {hueShift, satScale, valScale});
    return t;
}

const Mat3 kD65ToCamIdentityish = {{{0.9, -0.3, -0.05}, {-0.4, 1.2, 0.2}, {-0.08, 0.17, 0.7}}};

void writeDcp(const QString& path, const QString& camera, const QString& name, float satScale, const Mat3& cm = kD65ToCamIdentityish)
{
    DcpWriter w;
    w.ascii(50708, camera.toUtf8());
    w.matrix(50721, cm);
    w.shortValue(50778, 21); // D65
    w.ascii(50936, name.toUtf8());
    w.longs(50937, {6, 4, 1});
    w.floats(50938, uniformTable(6, 4, 1, 0, satScale, 1));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(w.bytes());
}

// Camera Raw's table encoding, the inverse of what CameraProfile.cpp decodes.
QString encodeBigTable(const QByteArray& raw)
{
    static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?`'|()[]{}@%$#";
    QByteArray z = qCompress(raw).mid(4); // drop Qt's big-endian size
    QByteArray b(4, 0);
    qToLittleEndian(quint32(raw.size()), b.data());
    b += z;
    QString s;
    for (qsizetype i = 0; i < b.size(); i += 4) {
        const qsizetype n = std::min<qsizetype>(4, b.size() - i);
        quint32 v = 0;
        for (qsizetype k = 0; k < n; ++k)
            v |= quint32(uchar(b[i + k])) << (8 * k);
        for (qsizetype k = 0; k < (n == 4 ? 5 : n + 1); ++k) {
            s += QLatin1Char(kAlphabet[v % 85]);
            v /= 85;
        }
    }
    return s;
}

QByteArray lookTableStream(int hues, int sats, int vals, const std::vector<float>& entries)
{
    QByteArray b(20, 0);
    const quint32 head[5] = {0, 1, quint32(hues), quint32(sats), quint32(vals)};
    for (int i = 0; i < 5; ++i)
        qToLittleEndian(head[i], b.data() + 4 * i);
    b.append(reinterpret_cast<const char*>(entries.data()), qsizetype(entries.size() * 4));
    b.append(QByteArray(4, 0)); // linear encoding
    return b;
}

void writeLook(const QString& path, const QString& name, const QString& base, float satScale, bool brighteningCurve)
{
    const QString table = encodeBigTable(lookTableStream(6, 4, 2, uniformTable(6, 4, 2, 0, satScale, 1)));
    QString xmp = QStringLiteral(
                      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                      " <rdf:Description rdf:about=\"\" xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
                      "   crs:PresetType=\"Look\" crs:CameraModelRestriction=\"\" crs:CameraProfile=\"%1\"\n"
                      "   crs:LookTable=\"0123ABCD\" crs:Table_0123ABCD=\"%2\" crs:HasSettings=\"True\">\n"
                      "  <crs:Name><rdf:Alt><rdf:li xml:lang=\"x-default\">%3</rdf:li></rdf:Alt></crs:Name>\n")
                      .arg(base, table.toHtmlEscaped(), name);
    if (brighteningCurve)
        xmp += QStringLiteral("  <crs:ToneCurvePV2012><rdf:Seq><rdf:li>0, 0</rdf:li><rdf:li>128, 180</rdf:li><rdf:li>255, 255</rdf:li></rdf:Seq></crs:ToneCurvePV2012>\n");
    xmp += QStringLiteral(" </rdf:Description></rdf:RDF></x:xmpmeta>\n");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(xmp.toUtf8());
}

// Mean (max - min channel) and mean value of an encoded image.
std::pair<double, double> chromaAndMean(const ImageF& img)
{
    double c = 0, m = 0;
    for (size_t i = 0; i < img.pixelCount(); ++i) {
        const float* p = img.data.data() + i * 3;
        c += std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
        m += (p[0] + p[1] + p[2]) / 3;
    }
    return {c / img.pixelCount(), m / img.pixelCount()};
}

} // namespace

class TestProfile : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SILMA_PROFILE_DIR", m_dir.filePath("profiles").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toUtf8()); // QSettings
    }

    void hueSatTableIdentityAndShift()
    {
        HueSatTable t;
        t.hues = 6;
        t.sats = 3;
        t.vals = 1;
        t.data = uniformTable(6, 3, 1, 0, 1, 1);
        for (auto c : {std::array<float, 3>{0.8f, 0.2f, 0.1f}, {0.1f, 0.5f, 0.9f}, {0.3f, 0.3f, 0.3f}, {1.4f, 0.9f, 0.2f}}) {
            float rgb[3] = {c[0], c[1], c[2]};
            t.apply(rgb);
            for (int k = 0; k < 3; ++k)
                QVERIFY2(std::abs(rgb[k] - c[k]) < 1e-5f, qPrintable(QString("%1 -> %2").arg(c[k]).arg(rgb[k])));
        }
        // A hue shift of 120 degrees turns red into green; saturation 0 makes it grey.
        t.data = uniformTable(6, 3, 1, 120, 1, 1);
        float red[3] = {0.8f, 0.1f, 0.1f};
        t.apply(red);
        QVERIFY(red[1] > 0.79f && red[0] < 0.11f && red[2] < 0.11f);
        t.data = uniformTable(6, 3, 1, 0, 0, 1);
        float c[3] = {0.8f, 0.4f, 0.1f};
        t.apply(c);
        QVERIFY(std::abs(c[0] - 0.8f) < 1e-5f && std::abs(c[1] - 0.8f) < 1e-5f && std::abs(c[2] - 0.8f) < 1e-5f);
    }

    void loadsDcpAndLook()
    {
        const QString dcp = m_dir.filePath("Test Cam Grey.dcp"), look = m_dir.filePath("Test Look.xmp");
        writeDcp(dcp, "Test Cam", "Grey", 0.0f);
        writeLook(look, "Test Look", "Grey", 0.5f, true);
        const CameraProfile d = loadDcp(dcp);
        QCOMPARE(d.name, QString("Grey"));
        QCOMPARE(d.cameraModel, QString("Test Cam"));
        QCOMPARE(d.illuminant1, 21);
        QVERIFY(std::abs(d.colorMatrix1[1][1] - 1.2) < 1e-9);
        QVERIFY(d.hueSatMap1.valid());
        QCOMPARE(d.hueSatMap1.data[1], 0.0f);
        QVERIFY(!d.lookTable.valid());
        QVERIFY(d.matchesCamera("Test", "Cam"));
        QVERIFY(d.matchesCamera("test", "cam"));
        QVERIFY(!d.matchesCamera("Test", "Cam II"));

        const CameraProfile l = loadLookXmp(look);
        QCOMPARE(l.kind, CameraProfile::Kind::Look);
        QCOMPARE(l.name, QString("Test Look"));
        QCOMPARE(l.baseProfile, QString("Grey"));
        QVERIFY(l.lookTable.valid());
        QCOMPARE(l.lookTable.hues, 6);
        QCOMPARE(l.lookTable.sats, 4);
        QCOMPARE(l.lookTable.vals, 2);
        QCOMPARE(l.lookTable.data[1], 0.5f);
        QCOMPARE(l.toneCurve.points.size(), 3);
        QVERIFY(std::abs(l.toneCurve.points[1].y() - 180 / 255.0) < 1e-9);

        // Not profiles.
        QFile preset(m_dir.filePath("preset.xmp"));
        QVERIFY(preset.open(QIODevice::WriteOnly));
        preset.write("<x:xmpmeta><rdf:Description xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" crs:PresetType=\"Normal\"/></x:xmpmeta>");
        preset.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadLookXmp(preset.fileName()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadDcp(look));
    }

    void installAndResolve()
    {
        const QString dcp = m_dir.filePath("src/Test Cam Grey.dcp");
        QDir().mkpath(m_dir.filePath("src"));
        writeDcp(dcp, "Test Cam", "Grey", 0.0f);
        writeLook(m_dir.filePath("src/Test Look.xmp"), "Test Look", "Grey", 0.5f, false);
        QFile junk(m_dir.filePath("src/notes.xmp"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("hello");
        junk.close();
        QStringList errors;
        const QStringList names = installProfiles({m_dir.filePath("src")}, &errors);
        QCOMPARE(names.size(), 2);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors[0].startsWith("notes.xmp"));
        QVERIFY(QFile::exists(profileDir() + "/Test Cam Grey.dcp"));
        QCOMPARE(profilesForCamera("Test", "Cam"), QStringList({"Grey", "Test Look"}));
        QVERIFY(profilesForCamera("Other", "Cam").isEmpty());
        const ResolvedProfile r = resolveProfile("Test Look", "Test", "Cam");
        QVERIFY(r.valid());
        QCOMPARE(r.dcp->name, QString("Grey"));
        QCOMPARE(r.look->name, QString("Test Look"));
        QVERIFY(!resolveProfile("Test Look", "Other", "Cam").valid());
        QVERIFY(!resolveProfile("Missing", "Test", "Cam").valid());
    }

    void settingsKeepTheProfile()
    {
        DevelopSettings s;
        QVERIFY(applyAssignment(s, "profile=Adobe Color").isEmpty());
        QCOMPARE(s.profile, QString("Adobe Color"));
        const DevelopSettings r = DevelopSettings::fromJsonString(s.toJsonString());
        QCOMPARE(r.profile, QString("Adobe Color"));
        QVERIFY(!DevelopSettings().toJson().contains("profile"));
        QVERIFY(applyAssignment(s, "profile=none").isEmpty());
        QVERIFY(s.profile.isEmpty());
        DevelopSettings src;
        src.profile = "X";
        QCOMPARE(copySettingsParts(DevelopSettings(), src, {"profile"}).profile, QString("X"));
    }

    void newPhotosGetTheLastProfile()
    {
        QCOMPARE(newPhotoSettings(false), DevelopSettings());
        setDefaultProfile("Adobe Color");
        QCOMPARE(defaultProfile(), QString("Adobe Color"));
        QCOMPARE(newPhotoSettings(false).profile, QString("Adobe Color"));
        QVERIFY(newPhotoSettings(true).profile.isEmpty()); // JPEG/HEIC: no camera profiles
        const QString raw = testRaw("batch/DSC_0001.NEF");
        if (!raw.isEmpty()) {
            Batch b;
            b.create(m_dir.filePath("import.silma"));
            const ImportResult r = importFiles(b, {raw}, {});
            QCOMPARE(r.added.size(), 1);
            QCOMPARE(DevelopSettings::fromJsonString(b.settings(r.added[0])).profile, QString("Adobe Color"));
        }
        setDefaultProfile({});
        QCOMPARE(newPhotoSettings(false), DevelopSettings());
    }

    void rendersWithProfile()
    {
        const QString raw = testRaw("batch/DSC_0001.NEF");
        if (raw.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(raw, {.halfSize = true});
        const QString make = e.raw().make, model = e.raw().model;
        const QString dir = m_dir.filePath("cam");
        QDir().mkpath(dir);
        // The camera's own matrix: only the tables differ from the built-in colour.
        writeDcp(dir + "/neutral.dcp", make + ' ' + model, "Neutral", 1.0f, e.raw().color.xyzToCam);
        writeDcp(dir + "/grey.dcp", make + ' ' + model, "Grey", 0.0f, e.raw().color.xyzToCam);
        writeLook(dir + "/look.xmp", "Grey Look", "Neutral", 0.0f, false);
        writeLook(dir + "/bright.xmp", "Bright Look", "Neutral", 1.0f, true);
        QStringList errors;
        installProfiles({dir}, &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        const QStringList avail = e.availableProfiles();
        for (const char* n : {"Neutral", "Grey", "Grey Look", "Bright Look"})
            QVERIFY2(avail.contains(n), n);

        RenderRequest req{.width = 300, .height = 200};
        DevelopSettings s;
        const ImageF builtin = e.renderEncoded(s, req);
        auto render = [&](const QString& name) {
            DevelopSettings p = s;
            p.profile = name;
            return chromaAndMean(e.renderEncoded(p, req));
        };
        // A profile that is not installed renders like the built-in colour.
        {
            DevelopSettings p = s;
            p.profile = "Not Installed";
            const ImageF img = e.renderEncoded(p, req);
            QCOMPARE(img.data, builtin.data);
        }
        const auto [cBuiltin, mBuiltin] = chromaAndMean(builtin);
        const auto [cNeutral, mNeutral] = render("Neutral");
        // Same matrix, no tables: about the built-in colour, without its extra chroma.
        QVERIFY2(cNeutral < cBuiltin && cNeutral > cBuiltin * 0.6, qPrintable(QString("%1 vs %2").arg(cNeutral).arg(cBuiltin)));
        QVERIFY(std::abs(mNeutral - mBuiltin) < 0.02);
        // Saturation 0 in the hue/saturation map, or in a look's table over a neutral DCP: grey.
        for (const char* n : {"Grey", "Grey Look"}) {
            const auto [c, m] = render(n);
            QVERIFY2(c < 0.01 && std::abs(m - mNeutral) < 0.05, qPrintable(QString("%1: chroma %2, mean %3").arg(n).arg(c).arg(m)));
        }
        // The look's tone curve brightens the midtones.
        QVERIFY(render("Bright Look").second > mNeutral + 0.05);
    }
};

QTEST_GUILESS_MAIN(TestProfile)
#include "test_profile.moc"
