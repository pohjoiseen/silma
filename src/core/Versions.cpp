#include "Versions.h"

#include <cstdio> // jpeglib.h needs FILE
#include <exiv2/exiv2.hpp>
#include <jpeglib.h>
#include <jxl/decode.h>
#include <lcms2.h>
#include <lensfun/lensfun.h>
#include <libraw/libraw.h>
#include <onnxruntime/onnxruntime_cxx_api.h>
#include <opencv2/core/version.hpp>
#include <sqlite3.h>
#include <tiffio.h>

namespace silma {

QList<std::pair<QString, QString>> libraryVersions()
{
    auto dotted = [](long major, long minor, long patch) { return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch); };
    const uint32_t jxl = JxlDecoderVersion();
    const int lcms = cmsGetEncodedCMMversion();
    const int jpeg = LIBJPEG_TURBO_VERSION_NUMBER;
    // "LIBTIFF, Version 4.7.0\n...": keep the number.
    QString tiff = QString::fromLatin1(TIFFGetVersion()).section('\n', 0, 0).section(QLatin1String("Version "), 1).trimmed();
    return {
        {"LibRaw", QString::fromLatin1(LibRaw::version())},
        {"Exiv2", QString::fromStdString(Exiv2::versionString())},
        {"Lensfun", dotted(LF_VERSION_MAJOR, LF_VERSION_MINOR, LF_VERSION_MICRO)},
        {"OpenCV", QStringLiteral(CV_VERSION)},
        {"ONNX Runtime", QString::fromStdString(Ort::GetVersionString())},
        {"libjxl", dotted(jxl / 1000000, jxl / 1000 % 1000, jxl % 1000)},
        {"libjpeg-turbo", dotted(jpeg / 1000000, jpeg / 1000 % 1000, jpeg % 1000)},
        {"libtiff", tiff},
        {"Little CMS", QStringLiteral("%1.%2").arg(lcms / 1000).arg(lcms / 10 % 100)},
        {"SQLite", QString::fromLatin1(sqlite3_libversion())},
    };
}

} // namespace silma
