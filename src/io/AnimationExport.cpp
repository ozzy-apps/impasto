#include "io/AnimationExport.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace animio {

namespace {

bool fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return false;
}

// ------------------------------------------------------------------ palette

struct Rgb {
    int r, g, b;
};

// Median cut: the box of colours that spreads furthest along one channel is
// cut at its median, until there are `count` boxes; each gives its mean.
std::vector<Rgb> medianCut(std::vector<Rgb> samples, int count)
{
    struct Box {
        size_t begin, end;
    };
    std::vector<Box> boxes{{0, samples.size()}};
    auto spread = [&](const Box& b, int* channel) {
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (size_t i = b.begin; i < b.end; ++i) {
            const int v[3] = {samples[i].r, samples[i].g, samples[i].b};
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], v[c]);
                hi[c] = std::max(hi[c], v[c]);
            }
        }
        // Green counts a little more and blue a little less, as the eye has it.
        const int w[3] = {(hi[0] - lo[0]) * 3, (hi[1] - lo[1]) * 4, (hi[2] - lo[2]) * 2};
        *channel = int(std::max_element(w, w + 3) - w);
        return w[*channel];
    };
    while (int(boxes.size()) < count) {
        int best = -1, bestSpread = 0, bestChannel = 0;
        for (size_t k = 0; k < boxes.size(); ++k) {
            int channel = 0;
            const int s = boxes[k].end - boxes[k].begin > 1 ? spread(boxes[k], &channel) : 0;
            if (s > bestSpread) {
                best = int(k);
                bestSpread = s;
                bestChannel = channel;
            }
        }
        if (best < 0)
            break;   // every box is a single colour
        Box& b = boxes[size_t(best)];
        auto key = [bestChannel](const Rgb& c) { return bestChannel == 0 ? c.r : bestChannel == 1 ? c.g : c.b; };
        const size_t mid = b.begin + (b.end - b.begin) / 2;
        std::nth_element(samples.begin() + std::ptrdiff_t(b.begin), samples.begin() + std::ptrdiff_t(mid), samples.begin() + std::ptrdiff_t(b.end),
                         [&](const Rgb& x, const Rgb& y) { return key(x) < key(y); });
        const Box upper{mid, b.end};
        b.end = mid;
        boxes.push_back(upper);
    }
    std::vector<Rgb> palette;
    for (const Box& b : boxes) {
        if (b.end <= b.begin)
            continue;
        long long sum[3] = {0, 0, 0};
        for (size_t i = b.begin; i < b.end; ++i) {
            sum[0] += samples[i].r;
            sum[1] += samples[i].g;
            sum[2] += samples[i].b;
        }
        const long long n = (long long)(b.end - b.begin);
        palette.push_back({int(sum[0] / n), int(sum[1] / n), int(sum[2] / n)});
    }
    return palette;
}

// The nearest palette colour, remembered for every colour at 5 bits a channel.
class Nearest {
public:
    Nearest(const std::vector<Rgb>& palette, int first) : m_palette(palette), m_first(first) { m_cache.fill(-1); }
    int operator()(int r, int g, int b)
    {
        const int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        int& slot = m_cache[size_t(key)];
        if (slot < 0) {
            const int cr = (r & ~7) | 4, cg = (g & ~7) | 4, cb = (b & ~7) | 4;
            int best = 0, bestD = INT32_MAX;
            for (size_t i = 0; i < m_palette.size(); ++i) {
                const int dr = m_palette[i].r - cr, dg = m_palette[i].g - cg, db = m_palette[i].b - cb;
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bestD) {
                    bestD = d;
                    best = int(i);
                }
            }
            slot = best + m_first;
        }
        return slot;
    }

private:
    const std::vector<Rgb>& m_palette;
    int m_first;
    std::array<int, 32768> m_cache;
};

// --------------------------------------------------------------------- LZW

// GIF's LZW, the way giflib writes it: codes grow from 9 bits as the table
// fills, and the table starts over at 4095 entries.
class LzwWriter {
public:
    explicit LzwWriter(QByteArray& out) : m_out(out), m_table(size_t(4096) * 256, -1) {}

    void encode(const uchar* pixels, size_t count)
    {
        m_out.append(char(8));   // the smallest code size: 8-bit pixels
        resetTable();
        put(kClear);
        int current = pixels[0];
        for (size_t i = 1; i < count; ++i) {
            const int pixel = pixels[i];
            const size_t key = size_t(current) * 256 + size_t(pixel);
            if (m_table[key] >= 0) {
                current = m_table[key];
                continue;
            }
            put(current);
            current = pixel;
            if (m_next >= 4095) {
                put(kClear);
                resetTable();
            } else {
                m_table[key] = m_next++;
                m_used.push_back(key);
            }
        }
        put(current);
        put(kEnd);
        if (m_bitCount > 0)
            m_block.append(char(m_bits & 0xff));
        // Out in sub-blocks of at most 255 bytes, then an empty one.
        for (qsizetype at = 0; at < m_block.size(); at += 255) {
            const qsizetype n = std::min<qsizetype>(255, m_block.size() - at);
            m_out.append(char(n));
            m_out.append(m_block.constData() + at, n);
        }
        m_out.append(char(0));
    }

private:
    static constexpr int kClear = 256, kEnd = 257;

    void resetTable()
    {
        for (size_t key : m_used)
            m_table[key] = -1;
        m_used.clear();
        m_next = kEnd + 1;
        m_codeBits = 9;
        m_limit = 512;
    }
    void put(int code)
    {
        m_bits |= quint32(code) << m_bitCount;
        m_bitCount += m_codeBits;
        while (m_bitCount >= 8) {
            m_block.append(char(m_bits & 0xff));
            m_bits >>= 8;
            m_bitCount -= 8;
        }
        if (m_next >= m_limit && m_codeBits < 12) {
            ++m_codeBits;
            m_limit = 1 << m_codeBits;
        }
    }

    QByteArray& m_out;
    QByteArray m_block;
    std::vector<int> m_table;
    std::vector<size_t> m_used;
    int m_next = kEnd + 1, m_codeBits = 9, m_limit = 512;
    quint32 m_bits = 0;
    int m_bitCount = 0;
};

void put16(QByteArray& out, int v)
{
    out.append(char(v & 0xff));
    out.append(char((v >> 8) & 0xff));
}

void put32be(QByteArray& out, quint32 v)
{
    out.append(char(v >> 24));
    out.append(char((v >> 16) & 0xff));
    out.append(char((v >> 8) & 0xff));
    out.append(char(v & 0xff));
}

// Hundredths of a second for each frame, rounded so that the whole keeps time.
QVector<int> centiseconds(const QVector<Frame>& frames, int fps)
{
    QVector<int> out;
    long long elapsed = 0;
    for (const Frame& f : frames) {
        const long long from = (elapsed * 100 + fps / 2) / fps;
        elapsed += std::max(1, f.hold);
        const long long to = (elapsed * 100 + fps / 2) / fps;
        out.push_back(int(std::max<long long>(2, to - from)));   // below 2 most viewers play it slower, not faster
    }
    return out;
}

bool writeFile(const QString& path, const QByteArray& data, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, file.errorString());
    file.write(data);
    if (!file.commit())
        return fail(error, file.errorString());
    return true;
}

} // namespace

// --------------------------------------------------------------------- GIF

bool writeGif(const QString& path, const QVector<Frame>& frames, int fps, bool loop, QString* error)
{
    if (frames.isEmpty() || frames.first().image.isNull())
        return fail(error, QObject::tr("There are no frames to write"));
    const QSize size = frames.first().image.size();
    if (size.width() > 65535 || size.height() > 65535)
        return fail(error, QObject::tr("A GIF can be at most 65,535 pixels a side"));
    QVector<QImage> images;
    bool transparent = false;
    for (const Frame& f : frames) {
        QImage img = f.image.convertToFormat(QImage::Format_ARGB32);
        if (img.size() != size)
            img = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < img.height() && !transparent; y += 2) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x += 2) {
                if (qAlpha(row[x]) < 128) {
                    transparent = true;
                    break;
                }
            }
        }
        images.push_back(img);
    }

    // One palette for every frame, so that nothing flickers from one to the next.
    std::vector<Rgb> samples;
    const qint64 total = qint64(size.width()) * size.height() * images.size();
    const int step = int(std::max<qint64>(1, total / 400000));
    qint64 at = 0;
    for (const QImage& img : images) {
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x, ++at) {
                if (at % step == 0 && qAlpha(row[x]) >= 128)
                    samples.push_back({qRed(row[x]), qGreen(row[x]), qBlue(row[x])});
            }
        }
    }
    if (samples.empty())
        samples.push_back({255, 255, 255});
    const int first = transparent ? 1 : 0;   // index 0 is "nothing" when the paper is hidden
    const std::vector<Rgb> palette = medianCut(samples, 256 - first);
    Nearest nearest(palette, first);

    QByteArray out("GIF89a");
    put16(out, size.width());
    put16(out, size.height());
    out.append(char(0xF7));   // a global table of 256 colours
    out.append(char(0));
    out.append(char(0));
    for (int i = 0; i < 256; ++i) {
        const int k = i - first;
        const Rgb c = (k >= 0 && k < int(palette.size())) ? palette[size_t(k)] : Rgb{0, 0, 0};
        out.append(char(c.r));
        out.append(char(c.g));
        out.append(char(c.b));
    }
    if (loop) {
        out.append("\x21\xFF\x0BNETSCAPE2.0\x03\x01", 16);
        put16(out, 0);   // forever
        out.append(char(0));
    }
    const QVector<int> delays = centiseconds(frames, fps);
    std::vector<uchar> indices(size_t(size.width()) * size.height());
    // Floyd-Steinberg, carrying the error of each pixel on to its neighbours.
    std::vector<float> errNow(size_t(size.width() + 2) * 3), errNext(size_t(size.width() + 2) * 3);
    for (int f = 0; f < images.size(); ++f) {
        const QImage& img = images[f];
        std::fill(errNow.begin(), errNow.end(), 0.0f);
        for (int y = 0; y < img.height(); ++y) {
            std::fill(errNext.begin(), errNext.end(), 0.0f);
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = row[x];
                uchar& slot = indices[size_t(y) * size.width() + size_t(x)];
                if (transparent && qAlpha(px) < 128) {
                    slot = 0;
                    continue;
                }
                float* e = &errNow[size_t(x + 1) * 3];
                const int r = std::clamp(int(std::lround(qRed(px) + e[0])), 0, 255);
                const int g = std::clamp(int(std::lround(qGreen(px) + e[1])), 0, 255);
                const int b = std::clamp(int(std::lround(qBlue(px) + e[2])), 0, 255);
                const int index = nearest(r, g, b);
                slot = uchar(index);
                const Rgb& c = palette[size_t(index - first)];
                const float d[3] = {float(r - c.r), float(g - c.g), float(b - c.b)};
                for (int k = 0; k < 3; ++k) {
                    errNow[size_t(x + 2) * 3 + size_t(k)] += d[k] * 7.0f / 16.0f;
                    errNext[size_t(x) * 3 + size_t(k)] += d[k] * 3.0f / 16.0f;
                    errNext[size_t(x + 1) * 3 + size_t(k)] += d[k] * 5.0f / 16.0f;
                    errNext[size_t(x + 2) * 3 + size_t(k)] += d[k] * 1.0f / 16.0f;
                }
            }
            std::swap(errNow, errNext);
        }
        // How long it shows, and (with see-through paper) that it is cleared before the next.
        out.append("\x21\xF9\x04", 3);
        out.append(char((transparent ? 2 : 1) << 2 | (transparent ? 1 : 0)));
        put16(out, delays[f]);
        out.append(char(0));
        out.append(char(0));
        out.append(char(0x2C));
        put16(out, 0);
        put16(out, 0);
        put16(out, size.width());
        put16(out, size.height());
        out.append(char(0));
        LzwWriter lzw(out);
        lzw.encode(indices.data(), indices.size());
    }
    out.append(char(0x3B));
    return writeFile(path, out, error);
}

// -------------------------------------------------------------------- APNG

bool writeApng(const QString& path, const QVector<Frame>& frames, int fps, bool loop, QString* error)
{
    if (frames.isEmpty() || frames.first().image.isNull())
        return fail(error, QObject::tr("There are no frames to write"));
    const QSize size = frames.first().image.size();
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    auto chunk = [&](const char* type, const QByteArray& data) {
        put32be(out, quint32(data.size()));
        const qsizetype start = out.size();
        out.append(type, 4);
        out.append(data);
        const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(out.constData() + start), uInt(out.size() - start));
        put32be(out, quint32(crc));
    };
    quint32 sequence = 0;
    for (int f = 0; f < frames.size(); ++f) {
        QImage img = frames[f].image.convertToFormat(QImage::Format_ARGB32);
        if (img.size() != size)
            img = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!img.save(&buffer, "PNG"))
            return fail(error, QObject::tr("Could not encode frame %1").arg(f + 1));
        // Take the PNG apart: its header once, its image data for every frame.
        QByteArray idat;
        QByteArray ihdr;
        for (qsizetype at = 8; at + 12 <= png.size();) {
            const quint32 length = quint32(uchar(png[at])) << 24 | quint32(uchar(png[at + 1])) << 16 | quint32(uchar(png[at + 2])) << 8 | quint32(uchar(png[at + 3]));
            const QByteArray type = png.mid(at + 4, 4);
            if (type == "IHDR")
                ihdr = png.mid(at + 8, length);
            else if (type == "IDAT")
                idat.append(png.mid(at + 8, length));
            at += 12 + qsizetype(length);
        }
        if (f == 0) {
            chunk("IHDR", ihdr);
            QByteArray actl;
            put32be(actl, quint32(frames.size()));
            put32be(actl, loop ? 0u : 1u);
            chunk("acTL", actl);
        }
        QByteArray fctl;
        put32be(fctl, sequence++);
        put32be(fctl, quint32(size.width()));
        put32be(fctl, quint32(size.height()));
        put32be(fctl, 0);
        put32be(fctl, 0);
        // The delay exactly: so many frames over the frame rate.
        fctl.append(char((std::max(1, frames[f].hold) >> 8) & 0xff));
        fctl.append(char(std::max(1, frames[f].hold) & 0xff));
        fctl.append(char((fps >> 8) & 0xff));
        fctl.append(char(fps & 0xff));
        fctl.append(char(1));   // cleared before the next frame
        fctl.append(char(0));   // replaces what is there, see-through parts included
        chunk("fcTL", fctl);
        if (f == 0) {
            chunk("IDAT", idat);
        } else {
            QByteArray fdat;
            put32be(fdat, sequence++);
            fdat.append(idat);
            chunk("fdAT", fdat);
        }
    }
    chunk("IEND", QByteArray());
    return writeFile(path, out, error);
}

// ------------------------------------------------------------------- video

QString ffmpegPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

bool writeVideo(const QString& path, const QVector<Frame>& frames, int fps, QString* error)
{
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty())
        return fail(error, QObject::tr("Video needs ffmpeg, which is not installed"));
    if (frames.isEmpty())
        return fail(error, QObject::tr("There are no frames to write"));
    // Each frame as a PNG down a pipe; a video has no see-through parts, so they become white.
    QVector<QByteArray> encoded;
    for (const Frame& f : frames) {
        QImage flat(f.image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        {
            QPainter p(&flat);
            p.drawImage(0, 0, f.image);
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        flat.save(&buffer, "PNG", 10);
        for (int k = 0; k < std::max(1, f.hold); ++k)
            encoded.push_back(png);
    }
    auto run = [&](const QStringList& codec, QString* message) {
        QProcess process;
        QStringList args{QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("image2pipe"),
                         QStringLiteral("-framerate"), QString::number(fps), QStringLiteral("-i"), QStringLiteral("-"),
                         QStringLiteral("-vf"), QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2")};
        args << codec << path;
        process.start(ffmpeg, args);
        if (!process.waitForStarted(5000)) {
            *message = process.errorString();
            return false;
        }
        for (const QByteArray& png : encoded) {
            process.write(png);
            process.waitForBytesWritten(-1);
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);
        *message = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    QString message;
    if (run({QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-crf"), QStringLiteral("18")}, &message))
        return true;
    // An ffmpeg built without x264 still has MPEG-4.
    if (run({QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-q:v"), QStringLiteral("2")}, &message))
        return true;
    return fail(error, message.isEmpty() ? QObject::tr("ffmpeg could not write the video") : message);
}

// ---------------------------------------------------------------- sequence

int writeSequence(const QString& path, const QVector<Frame>& frames, QString* error)
{
    const QFileInfo info(path);
    const QString base = info.completeBaseName().isEmpty() ? QStringLiteral("frame") : info.completeBaseName();
    const QDir dir = info.absoluteDir();
    int written = 0;
    for (const Frame& f : frames) {
        for (int k = 0; k < std::max(1, f.hold); ++k) {
            const QString name = dir.filePath(QStringLiteral("%1_%2.png").arg(base).arg(written + 1, 4, 10, QLatin1Char('0')));
            if (!f.image.save(name, "PNG")) {
                fail(error, QObject::tr("Could not write %1").arg(QFileInfo(name).fileName()));
                return written;
            }
            ++written;
        }
    }
    return written;
}

} // namespace animio
