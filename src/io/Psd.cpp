// Photoshop documents: PSD, and PSB for sides past 30,000 pixels. Layers
// keep their names, groups, masks, clipping, alpha lock, visibility, opacity
// and blend modes, at 8 or 16 bits a channel. The layout follows Adobe's
// description of the format; the parts used here are explained where they
// are written.

#include "io/Interchange.h"
#include "io/Layered.h"
#include "io/Zip.h"

#include <cmath>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <functional>

namespace interchange {

namespace {

constexpr int kPsdLargest = 30000;   // a side past this needs the large document format (PSB)

// ---------------------------------------------------------------- writing

class Out {
public:
    Out(QFileDevice& file, bool psb) : m_file(file), m_psb(psb) {}
    bool psb() const { return m_psb; }
    bool ok() const { return m_ok; }
    qint64 pos() const { return m_file.pos(); }
    void seek(qint64 at) { m_ok = m_file.seek(at) && m_ok; }
    void bytes(const char* data, qint64 n) { m_ok = m_file.write(data, n) == n && m_ok; }
    void bytes(const QByteArray& b) { bytes(b.constData(), b.size()); }
    void u8(quint8 v) { bytes(reinterpret_cast<const char*>(&v), 1); }
    void u16(quint16 v) { put(v); }
    void i16(qint16 v) { put(v); }
    void u32(quint32 v) { put(v); }
    void i32(qint32 v) { put(v); }
    void u64(quint64 v) { put(v); }
    // A length that takes eight bytes in a large document and four otherwise.
    void length(quint64 v) { m_psb ? u64(v) : u32(quint32(v)); }
    void zeros(qint64 n) { bytes(QByteArray(n, '\0')); }
    // Writes a length at `at` and comes back.
    void patch(qint64 at, quint64 v, bool wide)
    {
        const qint64 here = pos();
        seek(at);
        wide ? u64(v) : u32(quint32(v));
        seek(here);
    }

private:
    template <typename T>
    void put(T v)
    {
        char c[sizeof(T)];
        qToBigEndian(v, c);
        bytes(c, sizeof(T));
    }
    QFileDevice& m_file;
    bool m_psb;
    bool m_ok = true;
};

// PackBits, the run-length code Photoshop uses: a count byte, then either
// that many bytes as they are or one byte to repeat.
void packBits(const uchar* in, int n, QByteArray& out)
{
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && in[i + run] == in[i])
            ++run;
        if (run >= 3) {
            out.append(char(1 - run));
            out.append(char(in[i]));
            i += run;
            continue;
        }
        const int start = i;
        int length = 0;
        while (i < n && length < 128) {
            if (i + 2 < n && in[i] == in[i + 1] && in[i] == in[i + 2])
                break;
            ++i;
            ++length;
        }
        out.append(char(length - 1));
        out.append(reinterpret_cast<const char*>(in) + start, length);
    }
}

bool unpackBits(const uchar* in, qint64 n, uchar* out, qint64 wanted)
{
    qint64 i = 0, o = 0;
    while (o < wanted && i < n) {
        const int c = static_cast<signed char>(in[i++]);
        if (c >= 0) {
            const qint64 length = std::min<qint64>(c + 1, wanted - o);
            if (i + length > n)
                return false;
            std::memcpy(out + o, in + i, size_t(length));
            i += c + 1;
            o += length;
        } else if (c != -128) {
            const qint64 length = std::min<qint64>(1 - c, wanted - o);
            if (i >= n)
                return false;
            std::memset(out + o, in[i++], size_t(length));
            o += length;
        }
    }
    return o == wanted;
}

qint64 writeChannelPredicted(Out& out, int rows, int rowBytes, const std::function<void(int, uchar*)>& row);

// One channel, run-length coded: the compression, every row's length, the rows.
// `row` fills in one row of samples, big-endian. Returns what it took in the file.
qint64 writeChannelPacked(Out& out, int rows, int rowBytes, const std::function<void(int, uchar*)>& row)
{
    const qint64 start = out.pos();
    out.u16(1);
    const qint64 table = out.pos();
    out.zeros(qint64(rows) * (out.psb() ? 4 : 2));
    std::vector<uchar> raw(size_t(std::max(rowBytes, 1)));
    std::vector<quint32> lengths(static_cast<size_t>(rows));
    QByteArray packed;
    for (int y = 0; y < rows; ++y) {
        row(y, raw.data());
        packed.clear();
        packBits(raw.data(), rowBytes, packed);
        lengths[size_t(y)] = quint32(packed.size());
        out.bytes(packed);
    }
    const qint64 end = out.pos();
    out.seek(table);
    for (quint32 l : lengths)
        out.psb() ? out.u32(l) : out.u16(quint16(l));
    out.seek(end);
    return end - start;
}

// One channel of 16-bit samples, as Photoshop keeps them: each sample as its
// difference from the one before it in the row, then deflated. Run-length
// coding hardly shrinks 16-bit paint, whose low bytes are all different.
qint64 writeChannelPredicted(Out& out, int rows, int rowBytes, const std::function<void(int, uchar*)>& row)
{
    QByteArray plane(qint64(rows) * rowBytes, Qt::Uninitialized);
    for (int y = 0; y < rows; ++y) {
        uchar* r = reinterpret_cast<uchar*>(plane.data()) + qint64(y) * rowBytes;
        row(y, r);
        // From the end back, so that each difference is taken from the sample as it was.
        for (int x = rowBytes - 2; x >= 2; x -= 2) {
            const quint16 v = quint16((r[x] << 8) | r[x + 1]), before = quint16((r[x - 2] << 8) | r[x - 1]);
            const quint16 d = quint16(v - before);
            r[x] = uchar(d >> 8);
            r[x + 1] = uchar(d & 0xff);
        }
    }
    const QByteArray packed = zip::deflateZlib(plane, 6);
    out.u16(3);
    out.bytes(packed);
    return 2 + packed.size();
}

// 8-bit samples run-length coded, 16-bit ones deflated with prediction.
qint64 writeChannel(Out& out, int rows, int rowBytes, const std::function<void(int, uchar*)>& row, int sample)
{
    return sample == 2 ? writeChannelPredicted(out, rows, rowBytes, row) : writeChannelPacked(out, rows, rowBytes, row);
}

void putSample(uchar* at, int bytes, quint16 value16)
{
    if (bytes == 1) {
        *at = uchar((quint32(value16) * 255 + 32767) / 65535);
    } else {
        at[0] = uchar(value16 >> 8);
        at[1] = uchar(value16 & 0xff);
    }
}

// A tagged block of additional layer information.
void taggedBlock(QByteArray& into, const char* key, const QByteArray& data)
{
    QByteArray padded = data;
    while (padded.size() % 4)
        padded.append('\0');
    into.append("8BIM", 4);
    into.append(key, 4);
    char n[4];
    qToBigEndian(quint32(padded.size()), n);
    into.append(n, 4);
    into.append(padded);
}

QByteArray be16(quint16 v)
{
    char c[2];
    qToBigEndian(v, c);
    return QByteArray(c, 2);
}

QByteArray be32(quint32 v)
{
    char c[4];
    qToBigEndian(v, c);
    return QByteArray(c, 4);
}

QByteArray be32s(qint32 v)
{
    char c[4];
    qToBigEndian(v, c);
    return QByteArray(c, 4);
}

// An sRGB colour profile, ICC version 2, put together here. The one Qt makes
// declares version 2 but uses a curve that only version 4 has; Krita reads a
// PSD carrying it and then cannot save the picture.
QByteArray srgbProfile()
{
    auto u32 = [](QByteArray& b, quint32 v) { b.append(be32(v)); };
    auto fixed = [&](QByteArray& b, double v) { u32(b, quint32(qint32(std::lround(v * 65536.0)))); };
    auto pad4 = [](QByteArray& b) {
        while (b.size() % 4)
            b.append('\0');
    };
    auto xyz = [&](double x, double y, double z) {
        QByteArray t("XYZ \0\0\0\0", 8);
        fixed(t, x);
        fixed(t, y);
        fixed(t, z);
        return t;
    };
    QByteArray desc("desc\0\0\0\0", 8);
    const QByteArray name("sRGB");
    u32(desc, quint32(name.size() + 1));
    desc.append(name);
    desc.append('\0');
    desc.append(QByteArray(4 + 4 + 2 + 1 + 67, '\0'));   // no Unicode or ScriptCode versions
    QByteArray cprt("text\0\0\0\0", 8);
    cprt.append("No copyright, use freely");
    cprt.append('\0');
    // The sRGB tone curve, as a table.
    QByteArray curve("curv\0\0\0\0", 8);
    constexpr int kSteps = 1024;
    u32(curve, kSteps);
    for (int i = 0; i < kSteps; ++i) {
        const double v = double(i) / (kSteps - 1);
        const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        curve.append(be16(quint16(std::lround(linear * 65535.0))));
    }
    // The primaries, adapted to the D50 white the profile connection space uses.
    const std::pair<const char*, QByteArray> tags[] = {
        {"desc", desc},
        {"cprt", cprt},
        {"wtpt", xyz(0.9642, 1.0, 0.8249)},
        {"rXYZ", xyz(0.4360747, 0.2225045, 0.0139322)},
        {"gXYZ", xyz(0.3850649, 0.7168786, 0.0971045)},
        {"bXYZ", xyz(0.1430804, 0.0606169, 0.7141733)},
        {"rTRC", curve},
        {"gTRC", curve},
        {"bTRC", curve},
    };
    const int count = int(std::size(tags));
    QByteArray table, data;
    qint64 offset = 128 + 4 + qint64(count) * 12;
    qint64 curveAt = -1;
    for (const auto& [sig, body] : tags) {
        const bool sharedCurve = body.startsWith("curv");
        qint64 at = offset + data.size();
        if (sharedCurve && curveAt >= 0) {
            at = curveAt;   // the three tone curves share one table
        } else {
            if (sharedCurve)
                curveAt = at;
            data.append(body);
            pad4(data);
        }
        table.append(sig, 4);
        u32(table, quint32(at));
        u32(table, quint32(body.size()));
    }
    QByteArray header;
    const quint32 size = quint32(128 + 4 + table.size() + data.size());
    u32(header, size);
    u32(header, 0);                  // preferred CMM: none
    u32(header, 0x02100000);         // version 2.1
    header.append("mntrRGB XYZ ", 12);
    header.append(QByteArray(12, '\0'));   // date
    header.append("acsp", 4);
    header.append(QByteArray(24, '\0'));   // platform, flags, maker, model, attributes
    u32(header, 0);                  // perceptual
    fixed(header, 0.9642);           // the D50 illuminant
    fixed(header, 1.0);
    fixed(header, 0.8249);
    header.append(QByteArray(128 - header.size(), '\0'));
    QByteArray out = header;
    u32(out, quint32(count));
    out.append(table);
    out.append(data);
    return out;
}

// Where a mask is not white: the rest it covers with its default, white.
QRect maskBounds(const QByteArray& grey, const QSize& size)
{
    int x0 = size.width(), y0 = size.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < size.height(); ++y) {
        const uchar* row = reinterpret_cast<const uchar*>(grey.constData()) + qsizetype(y) * size.width();
        for (int x = 0; x < size.width(); ++x) {
            if (row[x] != 255) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

// One entry of the layer list as Photoshop has it. Groups take two: one
// where they end (below their members) and one where they begin (above).
struct Record {
    enum Kind { Plain, GroupOpen, GroupClosed, GroupEnd, Background } kind = Plain;
    int source = -1;           // the layer in the document
    QString name;
    QRect rect;
    QRect maskRect;
    bool hasMask = false;
    bool maskEnabled = true;
    float opacity = 1.0f;
    bool visible = true;
    bool clipping = false;
    bool alphaLock = false;
    BlendMode blend = BlendMode::Normal;
    std::vector<qint16> channels;
    std::vector<qint64> lengthAt;   // where each channel's length is, to be filled in once known
};

} // namespace

bool exportPsd(CanvasEngine& engine, const QString& path, int bits, QString* error, QStringList* notes)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    layered::Source src(engine);
    const QSize size = src.size();
    const QVector<LayerInfo>& layers = src.layers();
    const bool psb = std::max(size.width(), size.height()) > kPsdLargest;
    const int sample = bits == 16 ? 2 : 1;
    const bool transparent = !src.backgroundVisible();

    // The list, bottom first, with groups opened and closed around their members.
    std::vector<Record> records;
    if (!transparent) {
        Record r;
        r.kind = Record::Background;
        r.name = QStringLiteral("Background");
        r.rect = QRect(QPoint(0, 0), size);
        records.push_back(r);
    }
    bool flattenedText = false;
    std::function<void(int)> list = [&](int parent) {
        for (int i = 0; i < layers.size(); ++i) {
            if (src.parentIndex(i) != parent)
                continue;
            const LayerInfo& l = layers[i];
            Record r;
            r.source = i;
            r.name = l.name;
            r.opacity = l.opacity;
            r.visible = l.visible;
            r.blend = l.blend;
            r.clipping = l.clipping;
            if (l.isGroup) {
                // The entry where the group ends carries the group's opacity,
                // mode and visibility as well: Photoshop reads them from the
                // other one, Krita from this one.
                Record end = r;
                end.kind = Record::GroupEnd;
                end.name = QStringLiteral("</Layer group>");
                records.push_back(end);
                list(i);
                r.kind = l.collapsed ? Record::GroupClosed : Record::GroupOpen;
                // A group's mask goes with the entry where it begins.
                if (l.hasMask) {
                    const LayerData g = src.read(i);
                    r.hasMask = !g.mask.isEmpty();
                    r.maskEnabled = l.maskEnabled;
                    r.maskRect = r.hasMask ? maskBounds(g.mask, size) : QRect();
                }
                records.push_back(r);
                continue;
            }
            r.alphaLock = l.alphaLock;
            flattenedText = flattenedText || l.isText;
            // The extent of the pixels and of the mask go in the list, ahead of the pixels themselves.
            const LayerData d = src.read(i);
            r.rect = d.rect;
            if (!d.mask.isEmpty()) {
                r.hasMask = true;
                r.maskEnabled = l.maskEnabled;
                r.maskRect = maskBounds(d.mask, size);
            }
            records.push_back(r);
        }
    };
    list(-1);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());
    Out out(file, psb);

    // Header: signature, version (2 for the large format), the number of
    // channels in the picture, its size, depth and colour mode (3: RGB).
    out.bytes("8BPS", 4);
    out.u16(psb ? 2 : 1);
    out.zeros(6);
    out.u16(transparent ? 4 : 3);
    out.u32(quint32(size.height()));
    out.u32(quint32(size.width()));
    out.u16(quint16(bits));
    out.u16(3);
    out.u32(0);   // no colour table

    // Resources: the colours' meaning (sRGB, said outright: without it a
    // 16-bit file is taken to be linear and comes out pale), and the
    // resolution, so that the print size comes along.
    QByteArray resources;
    {
        const QByteArray icc = srgbProfile();
        if (!icc.isEmpty()) {
            resources.append("8BIM", 4);
            resources.append(be16(0x040F));
            resources.append(QByteArray(2, '\0'));
            resources.append(be32(quint32(icc.size())));
            resources.append(icc);
            if (icc.size() % 2)
                resources.append('\0');
        }
    }
    if (src.dpi() > 0) {
        resources.append("8BIM", 4);
        resources.append(be16(0x03ED));
        resources.append(QByteArray(2, '\0'));   // no name, padded to even
        resources.append(be32(16));
        for (int axis = 0; axis < 2; ++axis) {
            resources.append(be32(quint32(src.dpi()) << 16));   // 16.16 fixed point
            resources.append(be16(1));                         // pixels per inch
            resources.append(be16(1));                         // shown in inches
        }
    }
    out.u32(quint32(resources.size()));
    out.bytes(resources);

    // Layer and mask information.
    const qint64 sectionAt = out.pos();
    out.length(0);
    // Photoshop keeps the layers of a 16-bit picture in a tagged block at
    // the end of this section, and leaves the usual place empty.
    const bool tagged = bits == 16;
    qint64 infoAt = 0, infoStart = 0;
    if (tagged) {
        out.length(0);   // the usual layer info: empty
        out.u32(0);      // global mask information: none
        out.bytes("8BIM", 4);
        out.bytes("Lr16", 4);
        infoAt = out.pos();
        out.length(0);
    } else {
        infoAt = out.pos();
        out.length(0);
    }
    infoStart = out.pos();
    // A negative count says the first alpha channel of the picture is its transparency.
    out.i16(qint16(transparent ? -int(records.size()) : int(records.size())));
    for (Record& r : records) {
        const bool divider = r.kind == Record::GroupOpen || r.kind == Record::GroupClosed || r.kind == Record::GroupEnd;
        out.i32(r.rect.isEmpty() ? 0 : r.rect.top());
        out.i32(r.rect.isEmpty() ? 0 : r.rect.left());
        out.i32(r.rect.isEmpty() ? 0 : r.rect.bottom() + 1);
        out.i32(r.rect.isEmpty() ? 0 : r.rect.right() + 1);
        r.channels = {-1, 0, 1, 2};
        if (r.hasMask)
            r.channels.push_back(-2);
        out.u16(quint16(r.channels.size()));
        for (qint16 id : r.channels) {
            out.i16(id);
            r.lengthAt.push_back(out.pos());
            out.length(2);
        }
        out.bytes("8BIM", 4);
        out.bytes(layered::psdKey(r.blend));
        out.u8(quint8(std::lround(std::clamp(r.opacity, 0.0f, 1.0f) * 255.0f)));
        out.u8(r.clipping ? 1 : 0);
        // Flags: alpha lock, hidden, and (with bit 3 saying bit 4 means
        // something) that a group's entries carry no pixels of their own.
        out.u8(quint8((r.alphaLock ? 0x01 : 0) | (r.visible ? 0 : 0x02) | 0x08 | (divider ? 0x10 : 0)));
        out.u8(0);
        QByteArray extra;
        if (r.hasMask) {
            extra.append(be32(20));
            const QRect m = r.maskRect;
            extra.append(be32s(m.isEmpty() ? 0 : m.top()));
            extra.append(be32s(m.isEmpty() ? 0 : m.left()));
            extra.append(be32s(m.isEmpty() ? 0 : m.bottom() + 1));
            extra.append(be32s(m.isEmpty() ? 0 : m.right() + 1));
            extra.append(char(255));                     // outside its rectangle the mask shows the layer
            extra.append(char(r.maskEnabled ? 0 : 0x02));  // bit 1: switched off
            extra.append(QByteArray(2, '\0'));
        } else {
            extra.append(be32(0));
        }
        extra.append(be32(0));   // no blending ranges
        // The name twice: short and in the old encoding, then in full as UTF-16.
        QByteArray shortName = r.name.toLatin1().left(255);
        QByteArray pascal(1, char(shortName.size()));
        pascal.append(shortName);
        while (pascal.size() % 4)
            pascal.append('\0');
        extra.append(pascal);
        QByteArray unicode = be32(quint32(r.name.size()));
        for (QChar c : r.name)
            unicode.append(be16(c.unicode()));
        taggedBlock(extra, "luni", unicode);
        if (divider) {
            // 1: an open group, 2: a closed one, 3: where a group ends. A
            // group of Impasto's is blended as a whole, so it is given its
            // own blend mode rather than Photoshop's "pass through".
            QByteArray section = be32(r.kind == Record::GroupOpen ? 1 : r.kind == Record::GroupClosed ? 2 : 3);
            section.append("8BIM", 4);
            section.append(layered::psdKey(r.blend));
            taggedBlock(extra, "lsct", section);
        }
        out.u32(quint32(extra.size()));
        out.bytes(extra);
    }

    // The pixels, layer by layer, channel by channel: alpha, red, green, blue, mask.
    for (Record& r : records) {
        std::vector<qint64> lengths;
        if (r.kind == Record::Background) {
            const QColor c = src.background();
            const quint16 v[3] = {quint16(c.red() * 257), quint16(c.green() * 257), quint16(c.blue() * 257)};
            for (qint16 id : r.channels) {
                lengths.push_back(writeChannel(out, size.height(), size.width() * sample, [&](int, uchar* row) {
                    const quint16 value = id < 0 ? 65535 : v[id];
                    for (int x = 0; x < size.width(); ++x)
                        putSample(row + x * sample, sample, value);
                }, sample));
            }
        } else if (r.rect.isEmpty() && !r.hasMask) {
            for (size_t k = 0; k < r.channels.size(); ++k) {
                out.u16(0);   // stored as it is: nothing
                lengths.push_back(2);
            }
        } else {
            const LayerData d = src.read(r.source);
            const QImage image = r.rect.isEmpty() ? QImage() : bits == 16 ? layered::straight16(d.pixels, d.rect.size())
                                                                          : layered::straight8(d.pixels, d.rect.size());
            for (qint16 id : r.channels) {
                if (id == -2) {
                    const QRect m = r.maskRect;
                    if (m.isEmpty()) {
                        out.u16(0);
                        lengths.push_back(2);
                        continue;
                    }
                    lengths.push_back(writeChannel(out, m.height(), m.width() * sample, [&](int y, uchar* row) {
                        const uchar* grey = reinterpret_cast<const uchar*>(d.mask.constData()) + qsizetype(m.y() + y) * size.width() + m.x();
                        for (int x = 0; x < m.width(); ++x)
                            putSample(row + x * sample, sample, quint16(grey[x] * 257));
                    }, sample));
                    continue;
                }
                if (image.isNull()) {
                    out.u16(0);
                    lengths.push_back(2);
                    continue;
                }
                const int channel = id < 0 ? 3 : id;
                lengths.push_back(writeChannel(out, image.height(), image.width() * sample, [&](int y, uchar* row) {
                    if (bits == 16) {
                        const auto* px = reinterpret_cast<const quint16*>(image.constScanLine(y));
                        for (int x = 0; x < image.width(); ++x)
                            putSample(row + x * 2, 2, px[x * 4 + channel]);
                    } else {
                        const uchar* px = image.constScanLine(y);
                        for (int x = 0; x < image.width(); ++x)
                            row[x] = px[x * 4 + channel];
                    }
                }, sample));
            }
        }
        for (size_t k = 0; k < lengths.size(); ++k)
            out.patch(r.lengthAt[k], quint64(lengths[k]), psb);
        if (!out.ok())
            break;
    }
    // The layer information is padded to a multiple of four.
    while ((out.pos() - infoStart) % 4)
        out.u8(0);
    out.patch(infoAt, quint64(out.pos() - infoStart), psb);
    if (!tagged)
        out.u32(0);   // global mask information: none
    out.patch(sectionAt, quint64(out.pos() - sectionAt - (psb ? 8 : 4)), psb);

    // The finished picture, for programs (and previews) that read no layers.
    {
        const QByteArray composite = src.composite();
        const QImage merged = bits == 16 ? layered::straight16(composite, size) : layered::straight8(composite, size);
        const int channels = transparent ? 4 : 3;
        out.u16(1);
        const qint64 table = out.pos();
        out.zeros(qint64(channels) * size.height() * (psb ? 4 : 2));
        std::vector<quint32> rowLengths;
        rowLengths.reserve(size_t(channels) * size_t(size.height()));
        std::vector<uchar> raw(size_t(size.width()) * size_t(sample));
        QByteArray packed;
        for (int c = 0; c < channels; ++c) {
            for (int y = 0; y < size.height(); ++y) {
                if (bits == 16) {
                    const auto* px = reinterpret_cast<const quint16*>(merged.constScanLine(y));
                    for (int x = 0; x < size.width(); ++x)
                        putSample(raw.data() + x * 2, 2, px[x * 4 + c]);
                } else {
                    const uchar* px = merged.constScanLine(y);
                    for (int x = 0; x < size.width(); ++x)
                        raw[size_t(x)] = px[x * 4 + c];
                }
                packed.clear();
                packBits(raw.data(), int(raw.size()), packed);
                rowLengths.push_back(quint32(packed.size()));
                out.bytes(packed);
            }
        }
        const qint64 end = out.pos();
        out.seek(table);
        for (quint32 l : rowLengths)
            psb ? out.u32(l) : out.u16(quint16(l));
        out.seek(end);
    }
    if (!out.ok() || !file.commit())
        return fail(QObject::tr("Could not write %1.").arg(QFileInfo(path).fileName()));
    if (notes) {
        if (psb)
            *notes << QObject::tr("A side is longer than 30,000 pixels, so it was saved in Photoshop's large document format (PSB).");
        if (flattenedText)
            *notes << QObject::tr("Text goes as pixels.");
    }
    return true;
}

// ---------------------------------------------------------------- reading

namespace {

class In {
public:
    explicit In(const QString& path) : m_file(path) { m_ok = m_file.open(QIODevice::ReadOnly); }
    bool ok() const { return m_ok; }
    void setLarge(bool psb) { m_psb = psb; }
    bool psb() const { return m_psb; }
    qint64 pos() const { return m_file.pos(); }
    qint64 size() const { return m_file.size(); }
    void seek(qint64 at) { m_ok = at >= 0 && at <= m_file.size() && m_file.seek(at) && m_ok; }
    void skip(qint64 n) { seek(pos() + n); }
    QByteArray bytes(qint64 n)
    {
        if (n < 0 || pos() + n > m_file.size()) {
            m_ok = false;
            return {};
        }
        QByteArray b = m_file.read(n);
        m_ok = b.size() == n && m_ok;
        return b;
    }
    quint8 u8() { return quint8(get<quint8>()); }
    quint16 u16() { return get<quint16>(); }
    qint16 i16() { return get<qint16>(); }
    quint32 u32() { return get<quint32>(); }
    qint32 i32() { return get<qint32>(); }
    quint64 u64() { return get<quint64>(); }
    qint64 length() { return m_psb ? qint64(u64()) : qint64(u32()); }

private:
    template <typename T>
    T get()
    {
        char c[sizeof(T)];
        if (m_file.read(c, sizeof(T)) != sizeof(T)) {
            m_ok = false;
            return T(0);
        }
        return qFromBigEndian<T>(c);
    }
    QFile m_file;
    bool m_psb = false;
    bool m_ok = false;
};

// Tagged blocks whose length takes eight bytes in a large document.
bool wideKey(const QByteArray& key)
{
    static const char* const keys[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    for (const char* k : keys) {
        if (key == QByteArray(k, 4))
            return true;
    }
    return false;
}

struct Channel {
    qint16 id = 0;
    qint64 length = 0;
    QByteArray samples;   // big-endian rows, once read
};

struct Entry {
    QRect rect;
    std::vector<Channel> channels;
    QByteArray key;
    quint8 opacity = 255;
    quint8 clipping = 0;
    quint8 flags = 0;
    QRect maskRect;
    quint8 maskDefault = 255;
    quint8 maskFlags = 0;
    bool hasMask = false;
    QString name;
    int section = 0;          // 1, 2: a group begins; 3: one ends
    QByteArray sectionKey;
    bool adjustment = false;  // an adjustment or fill layer: no pixels to speak of
    bool text = false, smart = false, effects = false;
    // The layer as the canvas will take it, made as soon as its channels are
    // read so that the file's samples need not all be kept at once.
    QRect pixelsRect;
    QByteArray pixels;        // RGBA16F, premultiplied
    QByteArray mask;          // 8-bit grey, the whole canvas
};

// One channel's samples from the file, whatever the compression.
bool decode(In& in, Channel& ch, int rows, int rowBytes, int sample)
{
    if (ch.length < 2) {
        in.skip(ch.length);
        return true;
    }
    const qint64 end = in.pos() + ch.length;
    const quint16 compression = in.u16();
    const qint64 wanted = qint64(rows) * rowBytes;
    if (rows <= 0 || rowBytes <= 0) {
        in.seek(end);
        return true;
    }
    QByteArray out;
    if (compression == 0) {
        out = in.bytes(wanted);
    } else if (compression == 1) {
        std::vector<qint64> lengths(static_cast<size_t>(rows));
        for (int y = 0; y < rows; ++y)
            lengths[size_t(y)] = in.psb() ? qint64(in.u32()) : qint64(in.u16());
        out = QByteArray(wanted, '\0');
        for (int y = 0; y < rows; ++y) {
            const QByteArray packed = in.bytes(lengths[size_t(y)]);
            if (!unpackBits(reinterpret_cast<const uchar*>(packed.constData()), packed.size(), reinterpret_cast<uchar*>(out.data()) + qint64(y) * rowBytes, rowBytes))
                return false;
        }
    } else if (compression == 2 || compression == 3) {
        out = zip::inflateZlib(in.bytes(ch.length - 2), wanted);
        if (out.size() != wanted)
            return false;
        if (compression == 3) {
            // Each sample was stored as its difference from the one before it in the row.
            for (int y = 0; y < rows; ++y) {
                uchar* row = reinterpret_cast<uchar*>(out.data()) + qint64(y) * rowBytes;
                if (sample == 1) {
                    for (int x = 1; x < rowBytes; ++x)
                        row[x] = uchar(row[x] + row[x - 1]);
                } else {
                    for (int x = 2; x + 1 < rowBytes; x += 2) {
                        const quint16 v = quint16(((row[x] << 8) | row[x + 1]) + ((row[x - 2] << 8) | row[x - 1]));
                        row[x] = uchar(v >> 8);
                        row[x + 1] = uchar(v & 0xff);
                    }
                }
            }
        }
    } else {
        return false;
    }
    if (out.size() != wanted)
        return false;
    ch.samples = out;
    in.seek(end);
    return in.ok();
}

quint16 sampleAt(const QByteArray& samples, qint64 index, int sample)
{
    const auto* p = reinterpret_cast<const uchar*>(samples.constData());
    return sample == 1 ? quint16(p[index] * 257) : quint16((p[index * 2] << 8) | p[index * 2 + 1]);
}

} // namespace

bool readPsd(const QString& path, layered::Document* doc, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    In in(path);
    if (!in.ok())
        return fail(QObject::tr("Could not open the file."));
    if (in.bytes(4) != "8BPS")
        return fail(QObject::tr("This is not a Photoshop file."));
    const quint16 version = in.u16();
    if (version != 1 && version != 2)
        return fail(QObject::tr("This Photoshop file has a version this cannot read."));
    in.setLarge(version == 2);
    in.skip(6);
    const int channels = in.u16();
    const int height = int(in.u32()), width = int(in.u32());
    const int depth = in.u16(), mode = in.u16();
    if (mode != 3 && mode != 1)
        return fail(QObject::tr("Only RGB and greyscale Photoshop files can be opened (this one is CMYK, Lab, indexed or another kind)."));
    if (depth != 8 && depth != 16)
        return fail(depth == 32 ? QObject::tr("32-bit Photoshop files cannot be opened; save it at 16 bits a channel.")
                                : QObject::tr("Only 8- and 16-bit Photoshop files can be opened."));
    const int sample = depth / 8;
    const bool grey = mode == 1;
    doc->size = QSize(width, height);
    in.skip(in.u32());   // colour table

    // Resources: the resolution, if it is there.
    {
        const qint64 resourcesLength = in.u32();
        const qint64 end = in.pos() + resourcesLength;
        while (in.ok() && in.pos() + 12 <= end) {
            in.skip(4);   // signature
            const quint16 id = in.u16();
            const int nameLength = in.u8();
            in.skip(nameLength + ((nameLength + 1) % 2));
            const quint32 dataSize = in.u32();
            const qint64 dataAt = in.pos();
            if (id == 0x03ED && dataSize >= 4)
                doc->dpi = int(std::lround(in.u32() / 65536.0));
            in.seek(dataAt + dataSize + (dataSize % 2));
        }
        in.seek(end);
    }
    if (!in.ok())
        return fail(QObject::tr("The file is damaged."));
    if (std::max(width, height) > CanvasEngine::maxCanvasDimension() || qint64(width) * height > CanvasEngine::maxCanvasPixels())
        return fail(QObject::tr("The picture is %1 × %2 pixels, more than this graphics card can hold (%3 megapixels).")
                        .arg(width).arg(height).arg(CanvasEngine::maxCanvasPixels() / 1'000'000));

    // A layer's channels made into what the canvas takes: straight colour at
    // 8 or 16 bits, premultiplied, cropped to what is in it; and its mask.
    auto assemble = [&](Entry& e) {
        if (e.adjustment || e.section == 3)
            return true;
        if (!e.rect.isEmpty() && e.section == 0) {
            const Channel* by[4] = {nullptr, nullptr, nullptr, nullptr};
            for (const Channel& ch : e.channels) {
                if (ch.id == -1)
                    by[3] = &ch;
                else if (ch.id >= 0 && ch.id < 3)
                    by[ch.id] = &ch;
            }
            QImage image(e.rect.size(), QImage::Format_RGBA64);
            if (image.isNull())
                return false;
            const qint64 count = qint64(e.rect.width()) * e.rect.height();
            for (int y = 0; y < e.rect.height(); ++y) {
                auto* px = reinterpret_cast<quint16*>(image.scanLine(y));
                for (int x = 0; x < e.rect.width(); ++x) {
                    const qint64 at = qint64(y) * e.rect.width() + x;
                    for (int c = 0; c < 3; ++c) {
                        const Channel* ch = grey ? by[0] : by[c];
                        px[x * 4 + c] = ch && ch->samples.size() >= count * sample ? sampleAt(ch->samples, at, sample) : 0;
                    }
                    px[x * 4 + 3] = by[3] && by[3]->samples.size() >= count * sample ? sampleAt(by[3]->samples, at, sample) : 65535;
                }
            }
            QByteArray half = layered::premultipliedHalf(image);
            const QRect inside = layered::contentBounds(half, e.rect.size());
            if (!inside.isEmpty()) {
                e.pixelsRect = inside.translated(e.rect.topLeft());
                e.pixels = inside.size() != e.rect.size() ? layered::crop(half, e.rect.size(), inside) : half;
            }
        }
        if (e.hasMask) {
            const Channel* m = nullptr;
            for (const Channel& ch : e.channels) {
                if (ch.id == -2)
                    m = &ch;
            }
            QByteArray mask(qsizetype(width) * height, char(e.maskDefault));
            if (m && !e.maskRect.isEmpty() && m->samples.size() >= qint64(e.maskRect.width()) * e.maskRect.height() * sample) {
                const QRect inside = e.maskRect & QRect(0, 0, width, height);
                for (int y = inside.top(); y <= inside.bottom(); ++y) {
                    for (int x = inside.left(); x <= inside.right(); ++x) {
                        const qint64 at = qint64(y - e.maskRect.y()) * e.maskRect.width() + (x - e.maskRect.x());
                        mask[qsizetype(y) * width + x] = char(sampleAt(m->samples, at, sample) / 257);
                    }
                }
            }
            e.mask = mask;
        }
        return true;
    };

    std::vector<Entry> entries;
    bool layersRead = false;
    // The layer list and its pixels, from wherever they are kept.
    auto readLayers = [&](qint64 infoEnd) {
        const int count = std::abs(int(in.i16()));
        for (int i = 0; i < count && in.ok(); ++i) {
            Entry e;
            const int top = in.i32(), left = in.i32(), bottom = in.i32(), right = in.i32();
            e.rect = bottom > top && right > left ? QRect(left, top, right - left, bottom - top) : QRect();
            const int n = in.u16();
            for (int c = 0; c < n; ++c) {
                Channel ch;
                ch.id = in.i16();
                ch.length = in.length();
                e.channels.push_back(ch);
            }
            in.skip(4);   // 8BIM
            e.key = in.bytes(4);
            e.opacity = in.u8();
            e.clipping = in.u8();
            e.flags = in.u8();
            in.skip(1);
            const qint64 extraLength = in.u32();
            const qint64 extraEnd = in.pos() + extraLength;
            const qint64 maskLength = in.u32();
            const qint64 maskEnd = in.pos() + maskLength;
            if (maskLength >= 20) {
                const int mt = in.i32(), ml = in.i32(), mb = in.i32(), mr = in.i32();
                e.maskRect = mb > mt && mr > ml ? QRect(ml, mt, mr - ml, mb - mt) : QRect();
                e.maskDefault = in.u8();
                e.maskFlags = in.u8();
                e.hasMask = true;
            }
            in.seek(maskEnd);
            in.skip(in.u32());   // blending ranges
            const int nameLength = in.u8();
            e.name = QString::fromLatin1(in.bytes(nameLength));
            in.skip((4 - (1 + nameLength) % 4) % 4);
            while (in.ok() && in.pos() + 12 <= extraEnd) {
                const QByteArray signature = in.bytes(4);
                if (signature != "8BIM" && signature != "8B64")
                    break;
                const QByteArray key = in.bytes(4);
                const qint64 length = in.psb() && wideKey(key) ? qint64(in.u64()) : qint64(in.u32());
                const qint64 at = in.pos();
                if (key == "luni" && length >= 4) {
                    const quint32 chars = in.u32();
                    QString name;
                    for (quint32 k = 0; k < chars && in.pos() + 2 <= at + length; ++k)
                        name.append(QChar(in.u16()));
                    if (!name.isEmpty() && name.back() == QChar(0))
                        name.chop(1);
                    e.name = name;
                } else if ((key == "lsct" || key == "lsdk") && length >= 4) {
                    e.section = int(in.u32());
                    if (length >= 12) {
                        in.skip(4);
                        e.sectionKey = in.bytes(4);
                    }
                } else if (key == "TySh" || key == "tySh") {
                    e.text = true;
                } else if (key == "SoLd" || key == "PlLd" || key == "SoLE" || key == "plLd") {
                    e.smart = true;
                } else if (key == "lfx2" || key == "lrFX" || key == "lmfx") {
                    e.effects = true;
                } else {
                    static const char* const adjustments[] = {"levl", "curv", "brit", "hue2", "hue ", "blnc", "blwh", "phfl", "mixr", "clrL",
                                                              "nvrt", "post", "thrs", "grdm", "selc", "vibA", "expA", "SoCo", "GdFl", "PtFl"};
                    for (const char* a : adjustments) {
                        if (key == QByteArray(a, 4))
                            e.adjustment = true;
                    }
                }
                in.seek(at + length);
            }
            in.seek(extraEnd);
            entries.push_back(std::move(e));
        }
        // Then every layer's channels, in the same order.
        for (Entry& e : entries) {
            for (Channel& ch : e.channels) {
                const QRect r = ch.id <= -2 ? e.maskRect : e.rect;
                if (ch.id < -2 || (ch.id == -2 && !e.hasMask) || e.adjustment) {
                    in.skip(ch.length);   // a vector mask's pixels, a mask with no record, an adjustment's: not used
                    continue;
                }
                if (!decode(in, ch, r.height(), r.width() * sample, sample))
                    return false;
            }
            if (!assemble(e))
                return false;
            e.channels.clear();   // done with the file's samples
        }
        in.seek(infoEnd);
        layersRead = true;
        return in.ok();
    };

    const qint64 sectionLength = in.length();
    const qint64 sectionEnd = in.pos() + sectionLength;
    if (sectionLength > 0) {
        const qint64 infoLength = in.length();
        const qint64 infoEnd = in.pos() + infoLength;
        if (infoLength > 0 && !readLayers(infoEnd))
            return fail(QObject::tr("The layers in this file are damaged."));
        in.seek(infoEnd);
        // Global mask information, then tagged blocks, among them the layers of a 16-bit file.
        if (in.pos() + 4 <= sectionEnd)
            in.skip(in.u32());
        while (in.ok() && in.pos() + 12 <= sectionEnd) {
            const QByteArray signature = in.bytes(4);
            if (signature != "8BIM" && signature != "8B64")
                break;
            const QByteArray key = in.bytes(4);
            const qint64 length = in.psb() && wideKey(key) ? qint64(in.u64()) : qint64(in.u32());
            const qint64 at = in.pos();
            if ((key == "Lr16" || key == "Lr32") && !layersRead) {
                if (key == "Lr32")
                    return fail(QObject::tr("32-bit Photoshop files cannot be opened; save it at 16 bits a channel."));
                if (!readLayers(at + length))
                    return fail(QObject::tr("The layers in this file are damaged."));
            }
            in.seek(at + length + (length % 2));
        }
    }
    in.seek(sectionEnd);

    // The layers, bottom first, groups closed round their members.
    bool adjustments = false, text = false, smart = false, effects = false, passThrough = false, unknownModes = false;
    std::vector<std::vector<int>> open;
    auto addTo = [&](int index) {
        if (!open.empty())
            open.back().push_back(index);
    };
    for (Entry& e : entries) {
        if (e.section == 3) {
            open.emplace_back();
            continue;
        }
        LayerData d;
        d.info.name = e.name;
        d.info.opacity = e.opacity / 255.0f;
        d.info.visible = !(e.flags & 0x02);
        d.info.alphaLock = (e.flags & 0x01) != 0;
        d.info.clipping = e.clipping == 1;
        bool known = true;
        const QByteArray key = (e.section == 1 || e.section == 2) && !e.sectionKey.isEmpty() ? e.sectionKey : e.key;
        d.info.blend = layered::fromPsdKey(key, &known);
        passThrough = passThrough || key == "pass";
        unknownModes = unknownModes || (!known && key != "pass");
        if (e.section == 1 || e.section == 2) {
            d.info.isGroup = true;
            d.info.collapsed = e.section == 2;
            if (e.hasMask) {
                d.mask = std::move(e.mask);
                d.info.hasMask = true;
                d.info.maskEnabled = !(e.maskFlags & 0x02);
            }
            const int header = int(doc->layers.size());
            if (!open.empty()) {
                for (int member : open.back())
                    doc->layers[size_t(member)].parentIndex = header;
                open.pop_back();
            }
            doc->layers.push_back(d);
            addTo(header);
            continue;
        }
        if (e.adjustment) {
            adjustments = true;
            continue;
        }
        text = text || e.text;
        smart = smart || e.smart;
        effects = effects || e.effects;
        d.rect = e.pixelsRect;
        d.pixels = std::move(e.pixels);
        if (e.hasMask) {
            d.mask = std::move(e.mask);
            d.info.hasMask = true;
            d.info.maskEnabled = !(e.maskFlags & 0x02);
        }
        addTo(int(doc->layers.size()));
        doc->layers.push_back(std::move(d));
    }

    // No layers at all: the picture itself is the one layer.
    if (entries.empty()) {
        const quint16 compression = in.u16();
        const int rowBytes = width * sample;
        std::vector<QByteArray> planes;
        const int planesWanted = std::min(channels, grey ? 2 : 4);
        if (compression == 1) {
            std::vector<qint64> lengths(size_t(channels) * size_t(height));
            for (qint64& l : lengths)
                l = in.psb() ? qint64(in.u32()) : qint64(in.u16());
            for (int c = 0; c < channels && in.ok(); ++c) {
                QByteArray plane(qint64(height) * rowBytes, '\0');
                for (int y = 0; y < height; ++y) {
                    const QByteArray packed = in.bytes(lengths[size_t(c) * size_t(height) + size_t(y)]);
                    unpackBits(reinterpret_cast<const uchar*>(packed.constData()), packed.size(), reinterpret_cast<uchar*>(plane.data()) + qint64(y) * rowBytes, rowBytes);
                }
                if (c < planesWanted)
                    planes.push_back(plane);
            }
        } else if (compression == 0) {
            for (int c = 0; c < channels && c < planesWanted; ++c)
                planes.push_back(in.bytes(qint64(height) * rowBytes));
        } else {
            return fail(QObject::tr("The picture in this file is packed in a way this cannot read."));
        }
        if (!in.ok() || planes.empty())
            return fail(QObject::tr("The file is damaged."));
        QImage image(width, height, QImage::Format_RGBA64);
        for (int y = 0; y < height; ++y) {
            auto* px = reinterpret_cast<quint16*>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const qint64 at = qint64(y) * width + x;
                for (int c = 0; c < 3; ++c)
                    px[x * 4 + c] = sampleAt(planes[grey ? 0 : std::min<size_t>(size_t(c), planes.size() - 1)], at, sample);
                const size_t alpha = grey ? 1 : 3;
                px[x * 4 + 3] = planes.size() > alpha ? sampleAt(planes[alpha], at, sample) : 65535;
            }
        }
        LayerData d;
        d.info.name = QStringLiteral("Background");
        d.rect = QRect(0, 0, width, height);
        d.pixels = layered::premultipliedHalf(image);
        doc->layers.push_back(std::move(d));
    }

    if (adjustments)
        doc->notes << QObject::tr("Adjustment and fill layers were left out.");
    if (effects)
        doc->notes << QObject::tr("Layer effects (shadows, strokes, glows) were left out.");
    if (text)
        doc->notes << QObject::tr("Text came in as pixels.");
    if (smart)
        doc->notes << QObject::tr("Smart objects came in as plain pixels.");
    if (passThrough)
        doc->notes << QObject::tr("Groups set to Pass Through are blended as a whole, as Impasto's groups are.");
    if (unknownModes)
        doc->notes << QObject::tr("Some blend modes are unknown here and became Normal.");
    return true;
}

// ---------------------------------------------------------------- opening

bool canOpen(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("psd") || suffix == QLatin1String("psb") || suffix == QLatin1String("ora");
}

bool open(CanvasEngine& engine, const QString& path, QString* error, QStringList* notes)
{
    layered::Document doc;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool read = suffix == QLatin1String("ora") ? readOra(path, &doc, error) : readPsd(path, &doc, error);
    if (!read || !layered::load(engine, doc, error))
        return false;
    if (notes)
        *notes << doc.notes;
    return true;
}

} // namespace interchange
