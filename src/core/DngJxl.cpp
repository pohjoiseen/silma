#include "DngJxl.h"

#include "TiffReader.h"

#include <jxl/cms.h>
#include <jxl/decode_cxx.h>
#include <algorithm>
#include <stdexcept>

namespace silma {

namespace {

constexpr quint16 kCompressionJxl = 52546;

enum Tag : quint16 {
    NewSubfileType = 0x00FE,
    ImageWidth = 0x0100,
    ImageLength = 0x0101,
    BitsPerSample = 0x0102,
    Compression = 0x0103,
    Photometric = 0x0106,
    StripOffsets = 0x0111,
    SamplesPerPixel = 0x0115,
    RowsPerStrip = 0x0116,
    StripByteCounts = 0x0117,
    TileWidth = 0x0142,
    TileLength = 0x0143,
    TileOffsets = 0x0144,
    TileByteCounts = 0x0145,
    BlackLevel = 0xC61A,
    WhiteLevel = 0xC61D,
};

constexpr quint32 kPhotometricRgb = 2, kPhotometricYCbCr = 6, kPhotometricLinearRaw = 34892;

const TiffReader::Ifd* mainIfd(const QList<TiffReader::Ifd>& ifds, const TiffReader& t)
{
    for (const auto& ifd : ifds)
        if (t.uint(ifd, NewSubfileType, 0) == 0 && ifd.find(ImageWidth))
            return &ifd;
    return nullptr;
}

// One JPEG XL codestream (a tile or strip) decoded to interleaved floats, nominal range 0..1.
struct Tile {
    int width = 0, height = 0;
    std::vector<float> data;
};

// `linear`: undo the codestream's transfer curve. Adobe stores linear raw data with an sRGB
// curve (so lossy compression is perceptually even); the DNG samples are the linear values.
bool decodeJxl(const char* p, size_t n, int channels, bool linear, Tile& out)
{
    auto dec = JxlDecoderMake(nullptr);
    if (!dec || JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
        return false;
    JxlDecoderSetCms(dec.get(), *JxlGetDefaultCms());
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(p), n);
    JxlDecoderCloseInput(dec.get());
    const JxlPixelFormat fmt{uint32_t(channels), JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        switch (JxlDecoderProcessInput(dec.get())) {
        case JXL_DEC_BASIC_INFO: {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS)
                return false;
            out.width = int(info.xsize);
            out.height = int(info.ysize);
            break;
        }
        case JXL_DEC_COLOR_ENCODING: {
            JxlColorEncoding enc;
            if (linear) {
                if (JxlDecoderGetColorAsEncodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &enc) != JXL_DEC_SUCCESS)
                    return false; // ICC profile: no way to know the linear values
                if (enc.transfer_function != JXL_TRANSFER_FUNCTION_LINEAR) {
                    enc.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                    if (JxlDecoderSetOutputColorProfile(dec.get(), &enc, nullptr, 0) != JXL_DEC_SUCCESS)
                        return false;
                }
            }
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            size_t bytes = 0;
            if (JxlDecoderImageOutBufferSize(dec.get(), &fmt, &bytes) != JXL_DEC_SUCCESS)
                return false;
            out.data.resize(bytes / sizeof(float));
            if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, out.data.data(), bytes) != JXL_DEC_SUCCESS)
                return false;
            break;
        }
        case JXL_DEC_FULL_IMAGE: break;
        case JXL_DEC_SUCCESS: return !out.data.empty();
        default: return false; // error, or truncated data
        }
    }
}

// Decodes a tiled or stripped JPEG XL image into interleaved floats (nominal range 0..1).
std::vector<float> decodeIfd(const TiffReader& t, const TiffReader::Ifd& ifd, int width, int height, int channels, bool linear)
{
    const TiffReader::Entry* offs = ifd.find(TileOffsets);
    const TiffReader::Entry* counts = ifd.find(TileByteCounts);
    int tw = int(t.uint(ifd, TileWidth)), th = int(t.uint(ifd, TileLength));
    if (!offs || !counts) { // strips: full-width tiles
        offs = ifd.find(StripOffsets);
        counts = ifd.find(StripByteCounts);
        tw = width;
        th = int(std::min<quint32>(t.uint(ifd, RowsPerStrip, quint32(height)), quint32(height)));
    }
    if (!offs || !counts || tw <= 0 || th <= 0)
        throw std::runtime_error("JPEG XL DNG: missing tile layout");
    const int across = (width + tw - 1) / tw, down = (height + th - 1) / th;
    if (offs->count < quint32(across * down) || counts->count < offs->count)
        throw std::runtime_error("JPEG XL DNG: incomplete tile table");

    std::vector<float> out(size_t(width) * height * channels, 0.0f);
    const QByteArray& d = t.data();
    bool failed = false;
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < across * down; ++i) {
        qint64 off = t.uint(*offs, quint32(i)), len = t.uint(*counts, quint32(i));
        Tile tile;
        if (off <= 0 || off + len > d.size() || !decodeJxl(d.constData() + off, size_t(len), channels, linear, tile)) {
#pragma omp atomic write
            failed = true;
            continue;
        }
        const int x0 = (i % across) * tw, y0 = (i / across) * th;
        const int w = std::min({tile.width, tw, width - x0}), h = std::min({tile.height, th, height - y0});
        for (int y = 0; y < h; ++y)
            std::copy_n(&tile.data[size_t(y) * tile.width * channels], size_t(w) * channels, &out[(size_t(y0 + y) * width + x0) * channels]);
    }
    if (failed)
        throw std::runtime_error("JPEG XL DNG: cannot decode image data");
    return out;
}

} // namespace

bool isJxlDng(const QByteArray& file)
{
    TiffReader t(file);
    auto ifds = t.ifds();
    const TiffReader::Ifd* m = mainIfd(ifds, t);
    return m && t.uint(*m, Compression) == kCompressionJxl;
}

DngJxlImage decodeDngJxl(const QByteArray& file)
{
    TiffReader t(file);
    auto ifds = t.ifds();
    const TiffReader::Ifd* m = mainIfd(ifds, t);
    if (!m || t.uint(*m, Compression) != kCompressionJxl)
        throw std::runtime_error("Not a JPEG XL DNG");
    if (t.uint(*m, Photometric) != kPhotometricLinearRaw || t.uint(*m, SamplesPerPixel) != 3)
        throw std::runtime_error("JPEG XL DNGs with mosaic (CFA) data are not supported yet");

    DngJxlImage img;
    img.width = int(t.uint(*m, ImageWidth));
    img.height = int(t.uint(*m, ImageLength));
    if (img.width <= 0 || img.height <= 0 || qint64(img.width) * img.height > 1'000'000'000)
        throw std::runtime_error("JPEG XL DNG: bad image size");
    img.data = decodeIfd(t, *m, img.width, img.height, 3, true);

    // Back to the stored sample range, then black/white levels (per sample or one for all).
    const double range = double((1u << std::clamp<quint32>(t.uint(*m, BitsPerSample, 16), 8, 16)) - 1);
    double black[3] = {0, 0, 0}, scale[3];
    const TiffReader::Entry* bl = m->find(BlackLevel);
    const TiffReader::Entry* wl = m->find(WhiteLevel);
    for (int c = 0; c < 3; ++c) {
        if (bl)
            black[c] = t.real(*bl, bl->count >= 3 ? quint32(c) : 0);
        double white = wl ? t.real(*wl, wl->count >= 3 ? quint32(c) : 0) : range;
        scale[c] = white > black[c] ? range / (white - black[c]) : 1.0;
        black[c] /= range;
    }
    const size_t n = img.data.size();
    float* p = img.data.data();
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; ++i) {
        const int c = int(i % 3);
        p[i] = float(std::max(0.0, (p[i] - black[c]) * scale[c]));
    }

    // Panoramas: areas outside the stitched photos hold smeared edge pixels and are marked
    // by a transparency mask; make them black like our own panoramas.
    for (const auto& ifd : ifds) {
        if (t.uint(ifd, NewSubfileType) != 4 || t.uint(ifd, Compression) != kCompressionJxl || t.uint(ifd, SamplesPerPixel) != 1
            || int(t.uint(ifd, ImageWidth)) != img.width || int(t.uint(ifd, ImageLength)) != img.height)
            continue;
        std::vector<float> mask = decodeIfd(t, ifd, img.width, img.height, 1, false);
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < mask.size(); ++i)
            for (int c = 0; c < 3; ++c)
                p[i * 3 + c] *= std::clamp(mask[i], 0.0f, 1.0f);
        // Coverage for auto crop.
        std::vector<uint8_t> m8(mask.size());
        for (size_t i = 0; i < mask.size(); ++i)
            m8[i] = uint8_t(std::clamp(mask[i] * 255.0f + 0.5f, 0.0f, 255.0f));
        img.valid = makeValidMask(m8, img.width, img.height);
        break;
    }
    return img;
}

QImage decodeDngJxlPreview(const QByteArray& file)
{
    TiffReader t(file);
    const TiffReader::Ifd* best = nullptr;
    qint64 bestPixels = 0;
    const auto ifds = t.ifds();
    for (const auto& ifd : ifds) {
        const quint32 photometric = t.uint(ifd, Photometric);
        if (t.uint(ifd, Compression) != kCompressionJxl || t.uint(ifd, NewSubfileType) != 1 || t.uint(ifd, SamplesPerPixel) != 3
            || (photometric != kPhotometricRgb && photometric != kPhotometricYCbCr))
            continue;
        qint64 px = qint64(t.uint(ifd, ImageWidth)) * t.uint(ifd, ImageLength);
        if (px > bestPixels && px <= 50'000'000) {
            bestPixels = px;
            best = &ifd;
        }
    }
    if (!best)
        return {};
    const int w = int(t.uint(*best, ImageWidth)), h = int(t.uint(*best, ImageLength));
    std::vector<float> rgb;
    try {
        rgb = decodeIfd(t, *best, w, h, 3, false);
    } catch (const std::exception&) {
        return {};
    }
    // libjxl returns the preview in its own (sRGB) color space.
    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const float* src = &rgb[size_t(y) * w * 3];
        for (int i = 0; i < w * 3; ++i)
            line[i] = uchar(std::clamp(src[i] * 255.0f + 0.5f, 0.0f, 255.0f));
    }
    return img;
}

} // namespace silma
