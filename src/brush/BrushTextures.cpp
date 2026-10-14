#include "brush/BrushTextures.h"

#include "brush/TextureGen.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>
#include <algorithm>

namespace brushtex {

namespace {

const QLatin1String kPrefix("user:");
constexpr int kSize = texgen::kSize;
constexpr int kTipMargin = 16;   // a tip must fade out before the edge of its square

QString pathOf(const QString& name)
{
    return directory() + QLatin1Char('/') + name.mid(kPrefix.size()) + QStringLiteral(".png");
}

// Writes the map under the name its pixels give it.
QString store(const QImage& map, const QString& label, bool grain)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(grain ? "grain" : "tip");
    for (int y = 0; y < map.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(map.constScanLine(y)), map.width()));
    const QString name = kPrefix + QString::fromLatin1(hash.result().toHex().left(16));
    const QString path = pathOf(name);
    if (QFileInfo::exists(path))
        return name;
    QDir().mkpath(directory());
    QImage out = map;
    out.setText(QStringLiteral("label"), label);
    out.setText(QStringLiteral("kind"), grain ? QStringLiteral("grain") : QStringLiteral("tip"));
    return out.save(path, "PNG") ? name : QString();
}

void stretchLevels(QImage& map)
{
    int low = 255, high = 0;
    for (int y = 0; y < map.height(); ++y) {
        const uchar* row = map.constScanLine(y);
        for (int x = 0; x < map.width(); ++x) {
            low = std::min(low, int(row[x]));
            high = std::max(high, int(row[x]));
        }
    }
    if (high <= low || (low == 0 && high == 255))
        return;
    for (int y = 0; y < map.height(); ++y) {
        uchar* row = map.scanLine(y);
        for (int x = 0; x < map.width(); ++x)
            row[x] = uchar((int(row[x]) - low) * 255 / (high - low));
    }
}

QImage readImage(const QString& path, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull() && error)
        *error = QObject::tr("Could not read %1: %2").arg(QFileInfo(path).fileName(), reader.errorString());
    return img;
}

} // namespace

bool isUser(const QString& name)
{
    return name.startsWith(kPrefix);
}

QString directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/brush-textures");
}

QString tipFromImage(const QImage& source, const QString& label, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QString();
    };
    if (source.isNull())
        return fail(QObject::tr("There is no image"));
    QImage img = source.convertToFormat(QImage::Format_ARGB32);
    if (img.width() > 2048 || img.height() > 2048)
        img = img.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int w = img.width(), h = img.height();

    // What counts as paint? Transparency if the image has any; otherwise
    // whichever of light and dark is not the ground around the edge.
    bool hasAlpha = false;
    qint64 borderSum = 0, borderCount = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            hasAlpha = hasAlpha || qAlpha(row[x]) < 250;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                borderSum += qGray(row[x]);
                ++borderCount;
            }
        }
    }
    const bool darkOnLight = !hasAlpha && borderCount && borderSum / borderCount > 127;
    QImage cover(w, h, QImage::Format_Grayscale8);
    int x0 = w, y0 = h, x1 = -1, y1 = -1, peak = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        uchar* out = cover.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int v = hasAlpha ? qAlpha(row[x]) : darkOnLight ? 255 - qGray(row[x]) : qGray(row[x]);
            out[x] = uchar(v);
            peak = std::max(peak, v);
            if (v > 10) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0 || peak < 24)
        return fail(QObject::tr("That image is blank"));

    // The shape fills the square, centred, with room to fade out at the rim.
    const QImage shape = cover.copy(QRect(QPoint(x0, y0), QPoint(x1, y1)));
    const QSize fitted = shape.size().scaled(kSize - 2 * kTipMargin, kSize - 2 * kTipMargin, Qt::KeepAspectRatio);
    QImage map(kSize, kSize, QImage::Format_Grayscale8);
    map.fill(0);
    QPainter p(&map);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRect(QPoint((kSize - fitted.width()) / 2, (kSize - fitted.height()) / 2), fitted), shape);
    p.end();
    if (peak < 255) {
        for (int y = 0; y < kSize; ++y) {
            uchar* row = map.scanLine(y);
            for (int x = 0; x < kSize; ++x)
                row[x] = uchar(std::min(255, int(row[x]) * 255 / peak));
        }
    }
    const QString name = store(map, label, false);
    return name.isEmpty() ? fail(QObject::tr("Could not save the tip shape")) : name;
}

QString grainFromImage(const QImage& source, const QString& label, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QString();
    };
    if (source.isNull())
        return fail(QObject::tr("There is no image"));
    // Flatten onto white (bare paper), then stretch to the square a grain tiles in.
    QImage flat(source.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter p(&flat);
    p.drawImage(0, 0, source);
    p.end();
    QImage map = flat.scaled(kSize, kSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    stretchLevels(map);
    const QString name = store(map, label, true);
    return name.isEmpty() ? fail(QObject::tr("Could not save the grain")) : name;
}

QString importTip(const QString& imagePath, QString* error)
{
    const QImage img = readImage(imagePath, error);
    return img.isNull() ? QString() : tipFromImage(img, QFileInfo(imagePath).completeBaseName(), error);
}

QString importGrain(const QString& imagePath, QString* error)
{
    const QImage img = readImage(imagePath, error);
    return img.isNull() ? QString() : grainFromImage(img, QFileInfo(imagePath).completeBaseName(), error);
}

QString adopt(const QImage& map, const QString& label, bool grain)
{
    if (map.isNull())
        return {};
    QImage fixed = map.convertToFormat(QImage::Format_Grayscale8);
    if (fixed.size() != QSize(kSize, kSize))
        fixed = fixed.scaled(kSize, kSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return store(fixed, label, grain);
}

QString inverted(const QString& name)
{
    QImage map = load(name);
    if (map.isNull())
        return {};
    const QString label = map.text(QStringLiteral("label"));
    const bool grain = map.text(QStringLiteral("kind")) == QLatin1String("grain");
    map.invertPixels();
    return store(map, label, grain);
}

QImage load(const QString& name)
{
    if (!isUser(name))
        return {};
    QImage map(pathOf(name));
    if (map.isNull())
        return {};
    const QString label = map.text(QStringLiteral("label")), kind = map.text(QStringLiteral("kind"));
    map = map.convertToFormat(QImage::Format_Grayscale8);
    if (map.size() != QSize(kSize, kSize))
        map = map.scaled(kSize, kSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    map.setText(QStringLiteral("label"), label);
    map.setText(QStringLiteral("kind"), kind);
    return map;
}

QVector<Entry> list()
{
    QVector<Entry> out;
    const QFileInfoList files = QDir(directory()).entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time | QDir::Reversed);
    for (const QFileInfo& file : files) {
        QImageReader reader(file.absoluteFilePath());
        Entry e;
        e.name = kPrefix + file.completeBaseName();
        e.label = reader.text(QStringLiteral("label"));
        e.grain = reader.text(QStringLiteral("kind")) == QLatin1String("grain");
        if (e.label.isEmpty())
            e.label = file.completeBaseName().left(6);
        out.push_back(e);
    }
    return out;
}

} // namespace brushtex
