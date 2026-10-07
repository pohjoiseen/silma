#include "RawDecoder.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <libraw/libraw.h>
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

// Reads a raw TIFF tag blob for DNG opcode lists. LibRaw does not expose them, so we
// locate the tags with a tiny TIFF walker over IFD0 and its SubIFDs.
class TiffWalker {
public:
    explicit TiffWalker(const QByteArray& d) : m_d(d) {}

    bool valid() const { return m_d.size() > 8 && (m_d.startsWith("II") || m_d.startsWith("MM")); }

    void collect(DngOpcodes& out)
    {
        if (!valid())
            return;
        m_le = m_d.startsWith("II");
        walk(u32(4), out, 0);
    }

private:
    quint16 u16(qint64 o) const
    {
        if (o < 0 || o + 2 > m_d.size())
            return 0;
        auto p = reinterpret_cast<const uchar*>(m_d.constData() + o);
        return m_le ? quint16(p[0] | p[1] << 8) : quint16(p[0] << 8 | p[1]);
    }
    quint32 u32(qint64 o) const
    {
        if (o < 0 || o + 4 > m_d.size())
            return 0;
        auto p = reinterpret_cast<const uchar*>(m_d.constData() + o);
        return m_le ? quint32(p[0] | p[1] << 8 | p[2] << 16 | quint32(p[3]) << 24)
                    : quint32(quint32(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
    }

    void walk(quint32 ifd, DngOpcodes& out, int depth)
    {
        if (depth > 4 || ifd == 0 || ifd >= quint32(m_d.size()))
            return;
        int n = u16(ifd);
        for (int i = 0; i < n; ++i) {
            qint64 e = ifd + 2 + i * 12;
            quint16 tag = u16(e), type = u16(e + 2);
            quint32 count = u32(e + 4);
            static const int typeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
            qint64 bytes = qint64(count) * (type < 14 ? typeSize[type] : 1);
            qint64 valOff = bytes <= 4 ? e + 8 : u32(e + 8);
            auto blob = [&] { return valOff + bytes <= m_d.size() ? m_d.mid(valOff, bytes) : QByteArray(); };
            switch (tag) {
            case 0x014A: // SubIFDs
                for (quint32 k = 0; k < count && k < 8; ++k)
                    walk(count == 1 ? u32(e + 8) : u32(valOff + 4 * k), out, depth + 1);
                break;
            case 0xC740: if (out.list1.isEmpty()) out.list1 = blob(); break;
            case 0xC741: if (out.list2.isEmpty()) out.list2 = blob(); break;
            case 0xC74E: if (out.list3.isEmpty()) out.list3 = blob(); break;
            default: break;
            }
        }
    }

    const QByteArray& m_d;
    bool m_le = true;
};

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
    LibRawPtr raw = openRaw(path);
    auto& P = raw->imgdata.params;
    auto& C = raw->imgdata.color;
    auto& S = raw->imgdata.sizes;

    RawImage out;
    out.make = QString::fromLatin1(raw->imgdata.idata.make);
    out.model = QString::fromLatin1(raw->imgdata.idata.model);
    out.lens = QString::fromLatin1(raw->imgdata.lens.Lens);
    out.isDng = raw->imgdata.idata.dng_version != 0;

    check(raw->unpack(), "Cannot unpack raw data");
    // Full-resolution visible size (LibRaw shrinks sizes when decoding at half size).
    const int fullW = S.width, fullH = S.height;

    out.color.asShotMul = normalizeMul(C.cam_mul);
    out.color.daylightMul = normalizeMul(C.pre_mul);
    if (out.isDng && C.dng_levels.asshotneutral[0] > 0 && C.dng_levels.asshotneutral[1] > 0) {
        const float* n = C.dng_levels.asshotneutral;
        out.color.asShotMul = {n[1] / n[0], 1.0, n[1] / n[2]};
    }
    if (C.cam_mul[0] <= 0)
        out.color.asShotMul = out.color.daylightMul;
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

    // LibRaw normalizes multipliers so the smallest is 1.
    double minMul = std::min({out.color.asShotMul[0], out.color.asShotMul[1], out.color.asShotMul[2]});
    for (int c = 0; c < 3; ++c)
        out.color.appliedMul[c] = out.color.asShotMul[c] / minMul;

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
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray d = f.readAll();
            TiffWalker(d).collect(out.opcodes);
        }
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
