#include "Metadata.h"

#include <QTimeZone>
#include <exiv2/exiv2.hpp>
#include <mutex>
#include <stdexcept>

namespace silma {

namespace {

void initExiv2()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Exiv2::XmpParser::initialize();
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
    });
}

Exiv2::Image::UniquePtr openImage(const QString& path)
{
    initExiv2();
    try {
        auto img = Exiv2::ImageFactory::open(path.toStdString());
        img->readMetadata();
        return img;
    } catch (const Exiv2::Error& e) {
        throw std::runtime_error(std::string("Cannot read metadata: ") + e.what());
    }
}

QString prettyValue(const Exiv2::Metadatum& md, const Exiv2::ExifData* exif)
{
    try {
        return QString::fromStdString(md.print(exif)).trimmed();
    } catch (...) {
        return QString::fromStdString(md.toString()).trimmed();
    }
}

double rationalValue(const Exiv2::ExifData& exif, const char* key)
{
    auto it = exif.findKey(Exiv2::ExifKey(key));
    if (it == exif.end() || it->count() == 0)
        return 0;
    auto r = it->toRational(0);
    return r.second ? double(r.first) / r.second : 0;
}

std::optional<double> gpsCoord(const Exiv2::ExifData& exif, const char* key, const char* refKey)
{
    auto it = exif.findKey(Exiv2::ExifKey(key));
    if (it == exif.end() || it->count() < 3)
        return std::nullopt;
    double v = 0, div = 1;
    for (int i = 0; i < 3; ++i) {
        auto r = it->toRational(i);
        if (r.second == 0)
            return std::nullopt;
        v += double(r.first) / r.second / div;
        div *= 60;
    }
    auto ref = exif.findKey(Exiv2::ExifKey(refKey));
    if (ref != exif.end()) {
        auto s = ref->toString();
        if (s == "S" || s == "W")
            v = -v;
    }
    return v;
}

} // namespace

MetadataSummary readMetadataSummary(const QString& path)
{
    auto img = openImage(path);
    const auto& exif = img->exifData();
    MetadataSummary s;

    auto str = [&](const char* key) -> QString {
        auto it = exif.findKey(Exiv2::ExifKey(key));
        return it == exif.end() ? QString() : QString::fromStdString(it->toString()).trimmed();
    };

    QString dt = str("Exif.Photo.DateTimeOriginal");
    if (dt.isEmpty())
        dt = str("Exif.Image.DateTimeOriginal");
    if (dt.isEmpty())
        dt = str("Exif.Image.DateTime");
    s.captureTime = QDateTime::fromString(dt, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    // Capture times are camera-local wall clock; keep them timezone-less.
    s.captureTime.setTimeZone(QTimeZone::LocalTime);
    QString sub = str("Exif.Photo.SubSecTimeOriginal");
    if (s.captureTime.isValid() && !sub.isEmpty())
        s.captureTime = s.captureTime.addMSecs(qRound(("0." + sub).toDouble() * 1000));

    s.make = str("Exif.Image.Make");
    s.model = str("Exif.Image.Model");
    s.software = str("Exif.Image.Software");

    if (auto it = Exiv2::lensName(exif); it != exif.end()) {
        s.lens = prettyValue(*it, &exif);
        // Nikon's lens ID resolver yields the full name; numeric junk means unknown.
        if (!s.lens.isEmpty() && s.lens[0].isDigit() && !s.lens.contains(QLatin1String("mm")))
            s.lens.clear();
    }
    if (s.lens.isEmpty())
        s.lens = str("Exif.Photo.LensModel");

    if (auto it = Exiv2::isoSpeed(exif); it != exif.end())
        s.iso = it->toFloat();
    s.exposureTime = rationalValue(exif, "Exif.Photo.ExposureTime");
    s.fNumber = rationalValue(exif, "Exif.Photo.FNumber");
    s.focalLength = rationalValue(exif, "Exif.Photo.FocalLength");
    if (auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.FocalLengthIn35mmFilm")); it != exif.end())
        s.focalLength35 = it->toFloat();
    if (auto it = Exiv2::orientation(exif); it != exif.end())
        s.orientation = int(it->toInt64());

    s.latitude = gpsCoord(exif, "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef");
    s.longitude = gpsCoord(exif, "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef");
    if (auto it = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitude")); it != exif.end()) {
        double a = rationalValue(exif, "Exif.GPSInfo.GPSAltitude");
        if (str("Exif.GPSInfo.GPSAltitudeRef") == QLatin1String("1"))
            a = -a;
        s.altitude = a;
    }
    s.width = img->pixelWidth();
    s.height = img->pixelHeight();
    return s;
}

QList<MetadataTag> readAllMetadata(const QString& path)
{
    auto img = openImage(path);
    QList<MetadataTag> out;
    const auto& exif = img->exifData();
    for (const auto& md : exif) {
        MetadataTag t;
        t.key = QString::fromStdString(md.key());
        t.group = QString::fromStdString(md.familyName()) + '.' + QString::fromStdString(md.groupName());
        t.label = QString::fromStdString(md.tagLabel());
        if (t.label.isEmpty())
            t.label = QString::fromStdString(md.tagName());
        if (md.size() > 256 && (md.typeId() == Exiv2::undefined || md.typeId() == Exiv2::unsignedByte))
            t.value = QStringLiteral("(%1 bytes of binary data)").arg(md.size());
        else
            t.value = prettyValue(md, &exif);
        if (t.value.size() > 512)
            t.value = t.value.left(509) + QStringLiteral("...");
        out.append(t);
    }
    for (const auto& md : img->iptcData())
        out.append({QStringLiteral("Iptc.") + QString::fromStdString(md.groupName()), QString::fromStdString(md.key()),
                    QString::fromStdString(md.tagLabel()), QString::fromStdString(md.toString())});
    for (const auto& md : img->xmpData())
        out.append({QStringLiteral("Xmp.") + QString::fromStdString(md.groupName()), QString::fromStdString(md.key()),
                    QString::fromStdString(md.tagLabel()), QString::fromStdString(md.toString())});
    return out;
}

} // namespace silma
