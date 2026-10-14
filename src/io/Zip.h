#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <vector>

// Just enough of the zip format for OpenRaster files: entries stored as they
// are or deflated, written one after another to a file, and read back from
// one. Archives past 4 GB (zip64) are written and read as well, since a large
// canvas with many layers can make one.
namespace zip {

class Writer {
public:
    // Writes to `path`, replacing whatever is there.
    explicit Writer(const QString& path);
    bool ok() const { return m_ok; }
    QString error() const { return m_error; }
    // `compress`: deflate it. PNG data is compressed already and gains
    // nothing; the first entry of an OpenRaster file must be stored.
    bool add(const QString& name, const QByteArray& data, bool compress);
    bool finish();   // the directory at the end; nothing can be added after

private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint16 method = 0;
        qint64 size = 0, packed = 0, offset = 0;
    };
    bool fail(const QString& why);
    QFile m_file;
    std::vector<Entry> m_entries;
    bool m_ok = true;
    QString m_error;
};

class Reader {
public:
    bool open(const QString& path);
    QString error() const { return m_error; }
    QStringList names() const;
    bool contains(const QString& name) const;
    // The entry's contents, unpacked; empty with error() set when it cannot be read.
    QByteArray read(const QString& name);

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint32 crc = 0;
        qint64 size = 0, packed = 0, offset = 0;   // offset of its local header
    };
    bool fail(const QString& why);
    QFile m_file;
    std::vector<Entry> m_entries;
    QString m_error;
};

// Raw deflate and inflate (no zlib header), as zip and Photoshop's ZIP channels use.
QByteArray deflateRaw(const QByteArray& data, int level = 6);
QByteArray inflateRaw(const QByteArray& data, qint64 expectedSize);
// With the two-byte zlib header and checksum around it, as PSD's ZIP compression wants.
QByteArray deflateZlib(const QByteArray& data, int level = 6);
QByteArray inflateZlib(const QByteArray& data, qint64 expectedSize);

} // namespace zip
