#include "TiffReader.h"

#include <cstring>

namespace silma {

namespace {
const int kTypeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
int typeSize(quint16 type) { return type < 14 ? kTypeSize[type] : 1; }
} // namespace

const TiffReader::Entry* TiffReader::Ifd::find(quint16 tag) const
{
    for (const Entry& e : entries)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

TiffReader::TiffReader(const QByteArray& d) : m_d(d)
{
    m_valid = m_d.size() > 8 && (m_d.startsWith("II") || m_d.startsWith("MM"));
    m_le = m_d.startsWith("II");
}

quint16 TiffReader::u16(qint64 o) const
{
    if (o < 0 || o + 2 > m_d.size())
        return 0;
    auto p = reinterpret_cast<const uchar*>(m_d.constData() + o);
    return m_le ? quint16(p[0] | p[1] << 8) : quint16(p[0] << 8 | p[1]);
}

quint32 TiffReader::u32(qint64 o) const
{
    if (o < 0 || o + 4 > m_d.size())
        return 0;
    auto p = reinterpret_cast<const uchar*>(m_d.constData() + o);
    return m_le ? quint32(p[0] | p[1] << 8 | p[2] << 16 | quint32(p[3]) << 24)
                : quint32(quint32(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

QList<TiffReader::Ifd> TiffReader::ifds() const
{
    QList<Ifd> out;
    if (m_valid)
        walk(u32(4), out, 0);
    return out;
}

void TiffReader::walk(quint32 ifd, QList<Ifd>& out, int depth) const
{
    if (depth > 4 || ifd == 0 || ifd >= quint32(m_d.size()) || out.size() > 64)
        return;
    Ifd cur;
    int n = u16(ifd);
    for (int i = 0; i < n; ++i) {
        qint64 e = ifd + 2 + i * 12;
        Entry en;
        en.tag = u16(e);
        en.type = u16(e + 2);
        en.count = u32(e + 4);
        qint64 bytes = qint64(en.count) * typeSize(en.type);
        en.offset = bytes <= 4 ? e + 8 : u32(e + 8);
        cur.entries << en;
    }
    out << cur;
    if (const Entry* sub = cur.find(0x014A)) // SubIFDs
        for (quint32 k = 0; k < sub->count && k < 16; ++k)
            walk(uint(*sub, k), out, depth + 1);
}

quint32 TiffReader::uint(const Entry& e, quint32 i) const
{
    if (i >= e.count)
        return 0;
    switch (e.type) {
    case 1: case 7: return e.offset + i < m_d.size() ? uchar(m_d[e.offset + i]) : 0;
    case 3: return u16(e.offset + 2 * i);
    case 4: case 13: return u32(e.offset + 4 * i);
    default: return quint32(real(e, i));
    }
}

double TiffReader::real(const Entry& e, quint32 i) const
{
    if (i >= e.count)
        return 0;
    switch (e.type) {
    case 5: {
        quint32 d = u32(e.offset + 8 * i + 4);
        return d ? double(u32(e.offset + 8 * i)) / d : 0;
    }
    case 10: {
        qint32 d = qint32(u32(e.offset + 8 * i + 4));
        return d ? double(qint32(u32(e.offset + 8 * i))) / d : 0;
    }
    case 11: {
        quint32 b = u32(e.offset + 4 * i);
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    case 12: {
        quint64 b = quint64(u32(e.offset + 8 * i + (m_le ? 4 : 0))) << 32 | u32(e.offset + 8 * i + (m_le ? 0 : 4));
        double f;
        std::memcpy(&f, &b, 8);
        return f;
    }
    default: return uint(e, i);
    }
}

QByteArray TiffReader::blob(const Entry& e) const
{
    qint64 bytes = qint64(e.count) * typeSize(e.type);
    return e.offset >= 0 && e.offset + bytes <= m_d.size() ? m_d.mid(e.offset, bytes) : QByteArray();
}

quint32 TiffReader::uint(const Ifd& ifd, quint16 tag, quint32 def) const
{
    const Entry* e = ifd.find(tag);
    return e ? uint(*e) : def;
}

} // namespace silma
