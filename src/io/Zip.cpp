#include "io/Zip.h"

#include <QtEndian>
#include <algorithm>
#include <zlib.h>

namespace zip {

namespace {

constexpr quint32 kLocalHeader = 0x04034b50;
constexpr quint32 kCentralHeader = 0x02014b50;
constexpr quint32 kEnd = 0x06054b50;
constexpr quint32 kEnd64 = 0x06064b50;
constexpr quint32 kEnd64Locator = 0x07064b50;
constexpr quint16 kZip64Extra = 0x0001;
constexpr qint64 kSmall = 0xffffffffLL;   // what fits a 32-bit field; 0xffffffff itself means "see zip64"

void put16(QByteArray& b, quint16 v)
{
    char c[2];
    qToLittleEndian(v, c);
    b.append(c, 2);
}
void put32(QByteArray& b, quint32 v)
{
    char c[4];
    qToLittleEndian(v, c);
    b.append(c, 4);
}
void put64(QByteArray& b, quint64 v)
{
    char c[8];
    qToLittleEndian(v, c);
    b.append(c, 8);
}
quint16 get16(const char* p) { return qFromLittleEndian<quint16>(p); }
quint32 get32(const char* p) { return qFromLittleEndian<quint32>(p); }
quint64 get64(const char* p) { return qFromLittleEndian<quint64>(p); }

quint32 crcOf(const QByteArray& data)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    const auto* p = reinterpret_cast<const Bytef*>(data.constData());
    qint64 left = data.size();
    while (left > 0) {
        const uInt chunk = uInt(std::min<qint64>(left, 1 << 30));
        crc = crc32(crc, p, chunk);
        p += chunk;
        left -= chunk;
    }
    return quint32(crc);
}

// windowBits: negative for raw deflate, 15 for the zlib wrapper.
QByteArray deflateWith(const QByteArray& data, int level, int windowBits)
{
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    QByteArray out;
    out.resize(qsizetype(deflateBound(&z, uLong(std::min<qint64>(data.size(), 1 << 30)))) + 64);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    qint64 inLeft = data.size();
    qint64 written = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        const uInt feed = uInt(std::min<qint64>(inLeft, 1 << 30));
        z.avail_in = feed;
        if (out.size() - written < (1 << 16))
            out.resize(out.size() + std::max<qsizetype>(out.size() / 2, 1 << 20));
        z.next_out = reinterpret_cast<Bytef*>(out.data() + written);
        z.avail_out = uInt(std::min<qint64>(out.size() - written, 1 << 30));
        const uInt availOut = z.avail_out;
        status = deflate(&z, inLeft == qint64(feed) ? Z_FINISH : Z_NO_FLUSH);
        inLeft -= feed - z.avail_in;
        written += availOut - z.avail_out;
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            deflateEnd(&z);
            return {};
        }
    }
    deflateEnd(&z);
    out.resize(written);
    return out;
}

QByteArray inflateWith(const QByteArray& data, qint64 expectedSize, int windowBits)
{
    if (expectedSize < 0)
        return {};
    z_stream z{};
    if (inflateInit2(&z, windowBits) != Z_OK)
        return {};
    QByteArray out(expectedSize, Qt::Uninitialized);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    qint64 inLeft = data.size(), written = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        const uInt feed = uInt(std::min<qint64>(inLeft, 1 << 30));
        z.avail_in = feed;
        z.next_out = reinterpret_cast<Bytef*>(out.data() + written);
        z.avail_out = uInt(std::min<qint64>(expectedSize - written, 1 << 30));
        const uInt availOut = z.avail_out;
        status = inflate(&z, Z_NO_FLUSH);
        inLeft -= feed - z.avail_in;
        written += availOut - z.avail_out;
        if (status == Z_BUF_ERROR && (written == expectedSize || inLeft == 0))
            break;
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&z);
            return {};
        }
    }
    inflateEnd(&z);
    return written == expectedSize ? out : QByteArray();
}

} // namespace

QByteArray deflateRaw(const QByteArray& data, int level) { return deflateWith(data, level, -MAX_WBITS); }
QByteArray inflateRaw(const QByteArray& data, qint64 expectedSize) { return inflateWith(data, expectedSize, -MAX_WBITS); }
QByteArray deflateZlib(const QByteArray& data, int level) { return deflateWith(data, level, MAX_WBITS); }
QByteArray inflateZlib(const QByteArray& data, qint64 expectedSize) { return inflateWith(data, expectedSize, MAX_WBITS); }

// ------------------------------------------------------------------ writer

Writer::Writer(const QString& path) : m_file(path)
{
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        fail(m_file.errorString());
}

bool Writer::fail(const QString& why)
{
    m_ok = false;
    if (m_error.isEmpty())
        m_error = why;
    return false;
}

bool Writer::add(const QString& name, const QByteArray& data, bool compress)
{
    if (!m_ok)
        return false;
    Entry e;
    e.name = name.toUtf8();
    e.crc = crcOf(data);
    e.size = data.size();
    e.offset = m_file.pos();
    QByteArray packed;
    if (compress) {
        packed = deflateRaw(data, 6);
        // Not worth it, or it failed: keep it as it is.
        if (packed.isEmpty() || packed.size() >= data.size())
            compress = false;
    }
    e.method = compress ? 8 : 0;
    e.packed = compress ? packed.size() : data.size();
    const bool big = e.size >= kSmall || e.packed >= kSmall;

    QByteArray header;
    put32(header, kLocalHeader);
    put16(header, big ? 45 : 20);   // version needed
    put16(header, 0x0800);          // names in UTF-8
    put16(header, e.method);
    put16(header, 0);               // time
    put16(header, 0x21);            // date: 1980-01-01
    put32(header, e.crc);
    put32(header, big ? quint32(kSmall) : quint32(e.packed));
    put32(header, big ? quint32(kSmall) : quint32(e.size));
    put16(header, quint16(e.name.size()));
    put16(header, big ? 20 : 0);
    header.append(e.name);
    if (big) {
        put16(header, kZip64Extra);
        put16(header, 16);
        put64(header, quint64(e.size));
        put64(header, quint64(e.packed));
    }
    const QByteArray& body = compress ? packed : data;
    if (m_file.write(header) != header.size() || m_file.write(body) != body.size())
        return fail(m_file.errorString());
    m_entries.push_back(e);
    return true;
}

bool Writer::finish()
{
    if (!m_ok)
        return false;
    const qint64 directoryAt = m_file.pos();
    QByteArray dir;
    for (const Entry& e : m_entries) {
        const bool big = e.size >= kSmall || e.packed >= kSmall || e.offset >= kSmall;
        put32(dir, kCentralHeader);
        put16(dir, 0x031e);   // made by: Unix, 3.0
        put16(dir, big ? 45 : 20);
        put16(dir, 0x0800);
        put16(dir, e.method);
        put16(dir, 0);
        put16(dir, 0x21);
        put32(dir, e.crc);
        put32(dir, big ? quint32(kSmall) : quint32(e.packed));
        put32(dir, big ? quint32(kSmall) : quint32(e.size));
        put16(dir, quint16(e.name.size()));
        put16(dir, big ? 28 : 0);
        put16(dir, 0);   // comment
        put16(dir, 0);   // disk
        put16(dir, 0);   // internal attributes
        put32(dir, 0100644u << 16);
        put32(dir, big ? quint32(kSmall) : quint32(e.offset));
        dir.append(e.name);
        if (big) {
            put16(dir, kZip64Extra);
            put16(dir, 24);
            put64(dir, quint64(e.size));
            put64(dir, quint64(e.packed));
            put64(dir, quint64(e.offset));
        }
    }
    const qint64 directorySize = dir.size();
    const bool big = directoryAt >= kSmall || m_entries.size() >= 0xffff;
    QByteArray end;
    if (big) {
        const qint64 end64At = directoryAt + directorySize;
        put32(end, kEnd64);
        put64(end, 44);
        put16(end, 45);
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, m_entries.size());
        put64(end, m_entries.size());
        put64(end, quint64(directorySize));
        put64(end, quint64(directoryAt));
        put32(end, kEnd64Locator);
        put32(end, 0);
        put64(end, quint64(end64At));
        put32(end, 1);
    }
    put32(end, kEnd);
    put16(end, 0);
    put16(end, 0);
    put16(end, quint16(std::min<size_t>(m_entries.size(), 0xffff)));
    put16(end, quint16(std::min<size_t>(m_entries.size(), 0xffff)));
    put32(end, big ? quint32(kSmall) : quint32(directorySize));
    put32(end, big ? quint32(kSmall) : quint32(directoryAt));
    put16(end, 0);
    if (m_file.write(dir) != dir.size() || m_file.write(end) != end.size())
        return fail(m_file.errorString());
    m_file.close();
    if (m_file.error() != QFileDevice::NoError)
        return fail(m_file.errorString());
    m_ok = false;   // finished: nothing more goes in
    return true;
}

// ------------------------------------------------------------------ reader

bool Reader::fail(const QString& why)
{
    m_error = why;
    return false;
}

bool Reader::open(const QString& path)
{
    m_file.setFileName(path);
    m_entries.clear();
    if (!m_file.open(QIODevice::ReadOnly))
        return fail(m_file.errorString());
    const qint64 size = m_file.size();
    // The end record sits in the last 64 KB (it may carry a comment).
    const qint64 tailSize = std::min<qint64>(size, 65557);
    m_file.seek(size - tailSize);
    const QByteArray tail = m_file.read(tailSize);
    qint64 endAt = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i) {
        if (get32(tail.constData() + i) == kEnd) {
            endAt = i;
            break;
        }
    }
    if (endAt < 0)
        return fail(QStringLiteral("not a zip archive"));
    const char* end = tail.constData() + endAt;
    qint64 count = get16(end + 10);
    qint64 dirSize = get32(end + 12);
    qint64 dirAt = get32(end + 16);
    // Zip64: the real numbers are in a record of their own, found through a locator just before.
    if (endAt >= 20 && get32(tail.constData() + endAt - 20) == kEnd64Locator) {
        const qint64 end64At = qint64(get64(tail.constData() + endAt - 20 + 8));
        m_file.seek(end64At);
        const QByteArray e64 = m_file.read(56);
        if (e64.size() < 56 || get32(e64.constData()) != kEnd64)
            return fail(QStringLiteral("damaged zip64 directory"));
        count = qint64(get64(e64.constData() + 32));
        dirSize = qint64(get64(e64.constData() + 40));
        dirAt = qint64(get64(e64.constData() + 48));
    }
    if (dirAt < 0 || dirAt + dirSize > size)
        return fail(QStringLiteral("damaged zip directory"));
    m_file.seek(dirAt);
    const QByteArray dir = m_file.read(dirSize);
    qint64 at = 0;
    for (qint64 i = 0; i < count; ++i) {
        if (at + 46 > dir.size() || get32(dir.constData() + at) != kCentralHeader)
            return fail(QStringLiteral("damaged zip directory"));
        const char* h = dir.constData() + at;
        Entry e;
        e.method = get16(h + 10);
        e.crc = get32(h + 16);
        e.packed = get32(h + 20);
        e.size = get32(h + 24);
        const int nameLength = get16(h + 28), extraLength = get16(h + 30), commentLength = get16(h + 32);
        e.offset = get32(h + 42);
        e.name = QString::fromUtf8(h + 46, nameLength);
        // Zip64 extra field: whichever of the three were too large, in this order.
        const char* extra = h + 46 + nameLength;
        for (int k = 0; k + 4 <= extraLength;) {
            const int id = get16(extra + k), length = get16(extra + k + 2);
            if (id == kZip64Extra) {
                int f = k + 4;
                if (e.size == kSmall && f + 8 <= k + 4 + length) {
                    e.size = qint64(get64(extra + f));
                    f += 8;
                }
                if (e.packed == kSmall && f + 8 <= k + 4 + length) {
                    e.packed = qint64(get64(extra + f));
                    f += 8;
                }
                if (e.offset == kSmall && f + 8 <= k + 4 + length)
                    e.offset = qint64(get64(extra + f));
            }
            k += 4 + length;
        }
        m_entries.push_back(e);
        at += 46 + nameLength + extraLength + commentLength;
    }
    return true;
}

QStringList Reader::names() const
{
    QStringList out;
    for (const Entry& e : m_entries)
        out << e.name;
    return out;
}

bool Reader::contains(const QString& name) const
{
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.name == name; });
}

QByteArray Reader::read(const QString& name)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.name == name; });
    if (it == m_entries.end()) {
        fail(QStringLiteral("%1 is missing").arg(name));
        return {};
    }
    m_file.seek(it->offset);
    const QByteArray local = m_file.read(30);
    if (local.size() < 30 || get32(local.constData()) != kLocalHeader) {
        fail(QStringLiteral("damaged entry %1").arg(name));
        return {};
    }
    m_file.seek(it->offset + 30 + get16(local.constData() + 26) + get16(local.constData() + 28));
    const QByteArray packed = m_file.read(it->packed);
    if (packed.size() != it->packed) {
        fail(QStringLiteral("%1 is cut short").arg(name));
        return {};
    }
    QByteArray data;
    if (it->method == 0)
        data = packed;
    else if (it->method == 8)
        data = inflateRaw(packed, it->size);
    else {
        fail(QStringLiteral("%1 is packed in a way this cannot read").arg(name));
        return {};
    }
    if (data.size() != it->size || crcOf(data) != it->crc) {
        fail(QStringLiteral("%1 is damaged").arg(name));
        return {};
    }
    return data;
}

} // namespace zip
