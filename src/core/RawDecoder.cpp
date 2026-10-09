#include "RawDecoder.h"

#include "DngJxl.h"
#include "RenderedImage.h"
#include "TiffReader.h"

#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QStringList>
#include <libraw/libraw.h>
#include <tiffio.h>
#include <memory>
#include <stdexcept>

namespace silma {

namespace {

struct LibRawDeleter {
    void operator()(LibRaw* p) const
    {
        p->recycle();
        delete p;
    }
};
using LibRawPtr = std::unique_ptr<LibRaw, LibRawDeleter>;

void check(int rc, const char* what)
{
    if (rc != LIBRAW_SUCCESS)
        throw std::runtime_error(std::string(what) + ": " + libraw_strerror(rc));
}

LibRawPtr openRaw(const QString& path)
{
    LibRawPtr raw(new LibRaw(0));
    check(raw->open_file(QFile::encodeName(path).constData()), "Cannot open raw file");
    return raw;
}

std::array<double, 3> normalizeMul(const float* m)
{
    double g = m[1] > 0 ? m[1] : 1.0;
    std::array<double, 3> out{m[0] / g, 1.0, m[2] / g};
    for (auto& v : out)
        if (!(v > 0))
            v = 1.0;
    return out;
}

// DNG opcode lists of the main image; LibRaw does not expose them.
void collectOpcodes(const QByteArray& file, DngOpcodes& out)
{
    TiffReader t(file);
    const auto ifds = t.ifds();
    // The main image's IFD (NewSubfileType 0) first, then any other.
    QList<const TiffReader::Ifd*> order;
    for (const auto& ifd : ifds)
        if (t.uint(ifd, 0x00FE, 0) == 0)
            order << &ifd;
    for (const auto& ifd : ifds)
        if (!order.contains(&ifd))
            order << &ifd;
    for (const TiffReader::Ifd* ifd : order) {
        for (auto [tag, list] : {std::pair<quint16, QByteArray*>{0xC740, &out.list1}, {0xC741, &out.list2}, {0xC74E, &out.list3}})
            if (const TiffReader::Entry* e = ifd->find(tag); e && list->isEmpty())
                *list = t.blob(*e);
    }
}

// Coverage from a DNG transparency mask (NewSubfileType 4, e.g. our own panoramas) that
// libtiff can decompress; null when there is none.
std::shared_ptr<const ValidMask> readTransparencyMask(const QString& path, const QByteArray& file, int width, int height)
{
    TiffReader t(file);
    const auto ifds = t.ifds();
    for (const auto& ifd : ifds) {
        if (t.uint(ifd, 0x00FE) != 4 || t.uint(ifd, 0x0115, 1) != 1 || t.uint(ifd, 0x0102) != 8
            || int(t.uint(ifd, 0x0100)) != width || int(t.uint(ifd, 0x0101)) != height)
            continue;
        TIFFSetWarningHandler(nullptr);
        TIFF* tif = TIFFOpen(QFile::encodeName(path).constData(), "r");
        if (!tif)
            return nullptr;
        std::vector<uint8_t> mask(size_t(width) * height, 0);
        bool ok = TIFFSetSubDirectory(tif, ifd.offset);
        if (ok && TIFFIsTiled(tif)) {
            uint32_t tw = 0, th = 0;
            TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
            TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
            std::vector<uint8_t> tile(size_t(TIFFTileSize(tif)));
            for (uint32_t ty = 0; ok && ty < uint32_t(height); ty += th)
                for (uint32_t tx = 0; ok && tx < uint32_t(width); tx += tw) {
                    ok = TIFFReadTile(tif, tile.data(), tx, ty, 0, 0) >= 0 && tile.size() >= size_t(tw) * th;
                    for (uint32_t y = 0; ok && y < th && ty + y < uint32_t(height); ++y)
                        std::copy_n(&tile[size_t(y) * tw], std::min(tw, uint32_t(width) - tx), &mask[size_t(ty + y) * width + tx]);
                }
        } else if (ok) {
            for (int y = 0; ok && y < height; ++y)
                ok = TIFFReadScanline(tif, &mask[size_t(y) * width], uint32_t(y)) >= 0;
        }
        TIFFClose(tif);
        return ok ? makeValidMask(mask, width, height) : nullptr;
    }
    return nullptr;
}

// True for a single-plane 2x2 RGB Bayer raw that LibRaw has unpacked.
bool isUnpackedBayer(LibRaw& raw)
{
    const auto& I = raw.imgdata.idata;
    return I.colors == 3 && I.filters >= 1000 && raw.imgdata.rawdata.raw_image && raw.imgdata.sizes.pixel_aspect == 1.0 && !raw.is_fuji_rotated();
}

// As-shot white balance multipliers (green = 1).
std::array<double, 3> asShotMultipliers(const LibRaw& raw)
{
    const auto& C = raw.imgdata.color;
    if (raw.imgdata.idata.dng_version != 0 && C.dng_levels.asshotneutral[0] > 0 && C.dng_levels.asshotneutral[1] > 0) {
        const float* n = C.dng_levels.asshotneutral;
        return {n[1] / n[0], 1.0, n[1] / n[2]};
    }
    return normalizeMul(C.cam_mul[0] > 0 ? C.cam_mul : C.pre_mul);
}

} // namespace

int removeHotPixels(quint16* data, int width, int height, int pitch, const int color[2][2], const float black[2][2], float white,
                    const std::array<double, 3>& mul)
{
    // A hot (or stuck) pixel is one sample far above everything around it: the 8 adjacent
    // samples and the 8 nearest of its own colour (compared white-balanced, so colours are
    // comparable). Real detail, even a point light, also reaches the neighbours through the
    // lens. Such samples become the median of their own colour's neighbours.
    constexpr float kRatio = 2.5f, kFloor = 0.005f; // floor: in white-balanced units, 1 = white
    float scale[2][2];
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            scale[y][x] = float(mul[color[y][x]]) / std::max(1.0f, white - black[y][x]);
    auto value = [&](int x, int y) { return (float(data[size_t(y) * pitch + x]) - black[y & 1][x & 1]) * scale[y & 1][x & 1]; };
    std::vector<std::vector<std::pair<int, quint16>>> fixes(height);
#pragma omp parallel for schedule(dynamic, 64)
    for (int y = 2; y < height - 2; ++y)
        for (int x = 2; x < width - 2; ++x) {
            const float v = value(x, y);
            if (v < kFloor)
                continue;
            float mx = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if ((dx || dy) && (std::max(std::abs(dx), std::abs(dy)) == 1 || (dx % 2 == 0 && dy % 2 == 0)))
                        mx = std::max(mx, value(x + dx, y + dy));
            if (v > kRatio * mx && v - mx > kFloor) {
                std::array<quint16, 8> same;
                int n = 0;
                for (int dy = -2; dy <= 2; dy += 2)
                    for (int dx = -2; dx <= 2; dx += 2)
                        if (dx || dy)
                            same[n++] = data[size_t(y + dy) * pitch + x + dx];
                std::sort(same.begin(), same.end());
                fixes[y].push_back({x, quint16((same[3] + same[4] + 1) / 2)});
            }
        }
    int count = 0;
    for (int y = 0; y < height; ++y)
        for (auto [x, v] : fixes[y]) {
            data[size_t(y) * pitch + x] = v;
            ++count;
        }
    return count;
}

namespace {

// removeHotPixels on the visible area of an unpacked Bayer raw.
int removeHotPixels(LibRaw& raw, const std::array<double, 3>& mul)
{
    if (!isUnpackedBayer(raw) || qEnvironmentVariableIsSet("SILMA_KEEP_HOT_PIXELS")) // for comparing
        return 0;
    const auto& S = raw.imgdata.sizes;
    const auto& C = raw.imgdata.color;
    int color[2][2];
    float black[2][2];
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const int c = raw.COLOR(y, x);
            color[y][x] = c == 3 ? 1 : std::clamp(c, 0, 2);
            float b = float(C.black + C.cblack[std::clamp(c, 0, 3)]);
            if (C.cblack[4] && C.cblack[5]) // pattern black levels (the visible origin's phase)
                b += float(C.cblack[6 + ((y + S.top_margin) % C.cblack[4]) * C.cblack[5] + (x + S.left_margin) % C.cblack[5]]);
            black[y][x] = b;
        }
    const int pitch = int(S.raw_pitch / 2);
    quint16* origin = raw.imgdata.rawdata.raw_image + size_t(S.top_margin) * pitch + S.left_margin;
    return silma::removeHotPixels(origin, S.width, S.height, pitch, color, black, float(C.maximum), mul);
}

} // namespace

bool isRawFileName(const QString& fileName)
{
    static const QStringList exts = {
        "nef", "nrw", "dng", "cr2", "cr3", "crw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "srw", "3fr", "iiq", "x3f", "erf", "mef", "mos", "kdc", "dcr", "raw", "rwl",
    };
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

QStringList rawFileNameFilters()
{
    return {"*.NEF", "*.nef", "*.NRW", "*.nrw", "*.DNG", "*.dng", "*.CR2", "*.cr2", "*.CR3", "*.cr3", "*.ARW", "*.arw", "*.RAF", "*.raf", "*.ORF", "*.orf", "*.RW2", "*.rw2", "*.PEF", "*.pef", "*.SRW", "*.srw"};
}

RawImage decodeRaw(const QString& path, const RawDecodeOptions& opts)
{
    if (isRenderedFileName(path))
        return decodeRendered(path, opts.halfSize);
    LibRawPtr raw = openRaw(path);
    auto& P = raw->imgdata.params;
    auto& C = raw->imgdata.color;
    auto& S = raw->imgdata.sizes;

    RawImage out;
    out.make = QString::fromLatin1(raw->imgdata.idata.make);
    out.model = QString::fromLatin1(raw->imgdata.idata.model);
    out.lens = QString::fromLatin1(raw->imgdata.lens.Lens);
    out.isDng = raw->imgdata.idata.dng_version != 0;

    // LibRaw cannot unpack JPEG XL compressed DNGs (DNG 1.7): we decode those ourselves.
    QByteArray dngFile;
    if (out.isDng) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            dngFile = f.readAll();
    }
    const bool jxl = out.isDng && isJxlDng(dngFile);
    if (!jxl)
        check(raw->unpack(), "Cannot unpack raw data");
    // Full-resolution visible size (LibRaw shrinks sizes when decoding at half size).
    const int fullW = S.width, fullH = S.height;

    out.color.asShotMul = asShotMultipliers(*raw);
    out.color.daylightMul = normalizeMul(C.pre_mul);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.color.xyzToCam[r][c] = C.cam_xyz[r][c];
    for (int i = 0; i < 2; ++i) {
        const auto& d = C.dng_color[i];
        if (d.illuminant > 0 && (d.colormatrix[0][0] != 0 || d.colormatrix[1][1] != 0)) {
            out.color.dng[i].valid = true;
            out.color.dng[i].illuminant = d.illuminant;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    out.color.dng[i].colorMatrix[r][c] = d.colormatrix[r][c];
        }
    }
    out.color.baselineExposure = out.isDng ? C.dng_levels.baseline_exposure : 0.0;
    if (!std::isfinite(out.color.baselineExposure) || std::abs(out.color.baselineExposure) > 10)
        out.color.baselineExposure = 0;

    // LibRaw normalizes multipliers so the smallest is 1.
    double minMul = std::min({out.color.asShotMul[0], out.color.asShotMul[1], out.color.asShotMul[2]});
    for (int c = 0; c < 3; ++c)
        out.color.appliedMul[c] = out.color.asShotMul[c] / minMul;

    if (jxl) {
        DngJxlImage j = decodeDngJxl(dngFile);
        if (j.width != fullW || j.height != fullH)
            throw std::runtime_error("JPEG XL DNG: image size does not match the metadata");
        // Same as LibRaw's output: white-balanced with appliedMul and clipped at 1.
        const int f = opts.halfSize ? 2 : 1;
        out.image = ImageF(j.width / f, j.height / f);
        out.valid = j.valid;
        const float mul[3] = {float(out.color.appliedMul[0]), float(out.color.appliedMul[1]), float(out.color.appliedMul[2])};
        const float norm = 1.0f / float(f * f);
#pragma omp parallel for schedule(static)
        for (int y = 0; y < out.image.height; ++y)
            for (int x = 0; x < out.image.width; ++x)
                for (int c = 0; c < 3; ++c) {
                    float sum = 0;
                    for (int dy = 0; dy < f; ++dy)
                        for (int dx = 0; dx < f; ++dx)
                            sum += j.data[(size_t(y * f + dy) * j.width + x * f + dx) * 3 + c];
                    out.image.data[(size_t(y) * out.image.width + x) * 3 + c] = std::min(1.0f, sum * norm * mul[c]);
                }
    } else {
        out.hotPixels = removeHotPixels(*raw, out.color.asShotMul);
        // White-balance for demosaicing with as-shot multipliers; the pipeline re-balances later.
        for (int c = 0; c < 3; ++c)
            P.user_mul[c] = float(out.color.asShotMul[c]);
        P.user_mul[3] = float(out.color.asShotMul[1]);
        P.use_camera_wb = 0;
        P.use_auto_wb = 0;
        P.output_color = 0; // camera color space
        P.output_bps = 16;
        P.gamm[0] = 1.0;
        P.gamm[1] = 1.0;
        P.no_auto_bright = 1;
        P.highlight = 0; // clip
        P.user_flip = 0; // orientation handled by us
        P.half_size = opts.halfSize ? 1 : 0;
        P.user_qual = 3; // AHD

        check(raw->dcraw_process(), "Cannot process raw data");

        int err = 0;
        libraw_processed_image_t* img = raw->dcraw_make_mem_image(&err);
        if (!img)
            throw std::runtime_error(std::string("Cannot create image: ") + libraw_strerror(err));
        std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> guard(img, LibRaw::dcraw_clear_mem);
        if (img->colors != 3 || img->bits != 16)
            throw std::runtime_error("Unexpected raw output format");

        out.image = ImageF(img->width, img->height);
        const quint16* src = reinterpret_cast<const quint16*>(img->data);
        const size_t n = size_t(img->width) * img->height * 3;
        constexpr float scale = 1.0f / 65535.0f;
        float* dst = out.image.data.data();
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; ++i)
            dst[i] = src[i] * scale;
    }

    out.halfSize = opts.halfSize;
    out.fullWidth = fullW;
    out.fullHeight = fullH;
    // EXIF-style orientation from LibRaw flip (0 none, 3 180, 5 90 CCW, 6 90 CW).
    switch (S.flip) {
    case 3: out.orientation = 3; break;
    case 5: out.orientation = 8; break;
    case 6: out.orientation = 6; break;
    default: out.orientation = 1; break;
    }
    out.cropX = 0;
    out.cropY = 0;
    out.cropW = fullW;
    out.cropH = fullH;
    if (out.isDng) {
        const auto& ic = S.raw_inset_crops[0];
        // raw_inset_crops are relative to the raw frame; translate to the visible area.
        if (ic.cwidth > 0 && ic.cheight > 0 && ic.cwidth <= fullW && ic.cheight <= fullH) {
            out.cropX = std::max(0, int(ic.cleft) - int(S.left_margin));
            out.cropY = std::max(0, int(ic.ctop) - int(S.top_margin));
            out.cropW = std::min(int(ic.cwidth), fullW - out.cropX);
            out.cropH = std::min(int(ic.cheight), fullH - out.cropY);
        }
        collectOpcodes(dngFile, out.opcodes);
        if (!out.valid)
            out.valid = readTransparencyMask(path, dngFile, fullW, fullH);
    }
    return out;
}

BayerMosaic decodeBayerMosaic(const QString& path)
{
    if (isRenderedFileName(path))
        return {};
    LibRawPtr raw = openRaw(path);
    const auto& I = raw->imgdata.idata;
    BayerMosaic out;
    // filters: 0 = no CFA (linear), 1 = Leaf, 2 = Fuji SuperCCD-like, 9 = X-Trans; Bayer otherwise.
    if (I.colors != 3 || I.filters < 1000 || raw->imgdata.sizes.pixel_aspect != 1.0 || raw->is_fuji_rotated())
        return out;
    if (I.dng_version != 0) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && isJxlDng(f.readAll()))
            return out; // decoded ourselves into RGB
    }
    check(raw->unpack(), "Cannot unpack raw data");
    if (!isUnpackedBayer(*raw))
        return out; // not single-plane CFA data
    removeHotPixels(*raw, asShotMultipliers(*raw));
    // Visible area into image[][4], black level subtracted (per channel and pattern).
    check(raw->raw2image_ex(1), "Cannot extract raw data");
    const auto& S = raw->imgdata.sizes;
    const auto& C = raw->imgdata.color;
    out.width = S.iwidth;
    out.height = S.iheight;
    bool seen[3] = {};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            int c = raw->COLOR(y, x);
            out.color[y][x] = c == 3 ? 1 : c;
            if (c < 0 || c > 3)
                return {};
            seen[out.color[y][x]] = true;
        }
    if (!seen[0] || !seen[1] || !seen[2])
        return {};
    const float scale = 1.0f / float(std::max(1u, C.maximum - C.black));
    out.data.resize(size_t(out.width) * out.height);
    const auto* img = raw->imgdata.image;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const auto& p = img[size_t(y) * out.width + x];
            // One component holds the sample (which one depends on the colour index).
            out.data[size_t(y) * out.width + x] = float(std::max({p[0], p[1], p[2], p[3]})) * scale;
        }
    return out;
}

QByteArray extractEmbeddedPreview(const QString& path)
{
    LibRawPtr raw = openRaw(path);
    // Pick the largest JPEG thumbnail.
    auto& tl = raw->imgdata.thumbs_list;
    int best = -1;
    qint64 bestSize = 0;
    for (int i = 0; i < tl.thumbcount; ++i) {
        const auto& t = tl.thumblist[i];
        if (t.tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG && qint64(t.twidth) * t.theight > bestSize) {
            bestSize = qint64(t.twidth) * t.theight;
            best = i;
        }
    }
    if (raw->imgdata.idata.dng_version != 0) {
        // JPEG XL DNGs keep their bigger previews JPEG XL compressed too.
        QFile f(path);
        QImage jxl = f.open(QIODevice::ReadOnly) ? decodeDngJxlPreview(f.readAll()) : QImage();
        if (!jxl.isNull() && qint64(jxl.width()) * jxl.height() > bestSize) {
            QByteArray jpeg;
            QBuffer buf(&jpeg);
            buf.open(QIODevice::WriteOnly);
            jxl.save(&buf, "JPEG", 90);
            return jpeg;
        }
    }
    if (best >= 0) {
        if (raw->unpack_thumb_ex(best) != LIBRAW_SUCCESS)
            return {};
    } else if (raw->unpack_thumb() != LIBRAW_SUCCESS) {
        return {};
    }
    const auto& th = raw->imgdata.thumbnail;
    if (th.tformat != LIBRAW_THUMBNAIL_JPEG || !th.thumb)
        return {};
    return QByteArray(th.thumb, th.tlength);
}

} // namespace silma
