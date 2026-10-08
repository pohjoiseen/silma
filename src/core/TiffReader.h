#pragma once
// Minimal TIFF/DNG directory reader for tags LibRaw does not expose.

#include <QByteArray>
#include <QList>

namespace silma {

class TiffReader {
public:
    struct Entry {
        quint16 tag = 0, type = 0;
        quint32 count = 0;
        qint64 offset = 0; // of the value (inline values point into the entry)
    };
    struct Ifd {
        quint32 offset = 0; // in the file
        QList<Entry> entries;
        const Entry* find(quint16 tag) const;
    };

    explicit TiffReader(const QByteArray& d);
    bool valid() const { return m_valid; }

    // IFD0 and, recursively, its SubIFDs (depth first, in file order).
    QList<Ifd> ifds() const;

    // Element i of an integer (BYTE/SHORT/LONG) or real (RATIONAL/SRATIONAL/FLOAT/DOUBLE) tag.
    quint32 uint(const Entry& e, quint32 i = 0) const;
    double real(const Entry& e, quint32 i = 0) const;
    // Tag value as raw bytes.
    QByteArray blob(const Entry& e) const;
    // Convenience: value of `tag` in `ifd`, or `def` when absent.
    quint32 uint(const Ifd& ifd, quint16 tag, quint32 def = 0) const;

    const QByteArray& data() const { return m_d; }

private:
    quint16 u16(qint64 o) const;
    quint32 u32(qint64 o) const;
    void walk(quint32 ifd, QList<Ifd>& out, int depth) const;

    const QByteArray& m_d;
    bool m_le = true;
    bool m_valid = false;
};

} // namespace silma
