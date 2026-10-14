#include "io/DocumentIO.h"

#include "engine/CanvasEngine.h"

#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <cstring>
#include <future>

namespace docio {

namespace {

constexpr quint32 kMagic = 0x534b424b;   // "SKBK"
constexpr quint32 kVersion = 3;   // 2 added layer masks and groups, 3 the selection
// A document without a selection is written as version 2, which is what it
// is: a Impasto from before the selection was kept can still open it.
constexpr quint32 kVersionWithoutSelection = 2;

// Tightest rectangle holding every pixel with non-zero alpha (RGBA16F rows).
QRect contentBounds(const QByteArray& pixels, const QSize& size)
{
    const auto* data = reinterpret_cast<const quint16*>(pixels.constData());
    int x0 = size.width(), y0 = size.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < size.height(); ++y) {
        const quint16* row = data + qsizetype(y) * size.width() * 4;
        for (int x = 0; x < size.width(); ++x) {
            if (row[x * 4 + 3] & 0x7fff) {   // any alpha other than +/-0
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QByteArray crop(const QByteArray& pixels, const QSize& size, const QRect& r)
{
    QByteArray out(qsizetype(r.width()) * r.height() * 8, Qt::Uninitialized);
    for (int y = 0; y < r.height(); ++y) {
        const char* src = pixels.constData() + (qsizetype(r.y() + y) * size.width() + r.x()) * 8;
        memcpy(out.data() + qsizetype(y) * r.width() * 8, src, size_t(r.width()) * 8);
    }
    return out;
}

struct Packed {
    QRect rect;
    QByteArray data;
};

// `pixels` are the part of the layer that starts at `origin` on the canvas.
Packed pack(QByteArray pixels, QSize size, QPoint origin)
{
    Packed p;
    p.rect = contentBounds(pixels, size);
    if (!p.rect.isEmpty()) {
        p.data = qCompress(crop(pixels, size, p.rect), 1);
        p.rect.translate(origin);
    }
    return p;
}

QJsonArray rectToJson(const QRect& r)
{
    return {r.x(), r.y(), r.width(), r.height()};
}

BlendMode blendFromName(const QString& name)
{
    for (const BlendModeInfo& info : blendModes()) {
        if (name == QLatin1String(info.name))
            return info.mode;
    }
    return BlendMode::Normal;
}

} // namespace

bool save(CanvasEngine& engine, const QString& path, QString* error)
{
    const QVector<LayerInfo> layers = engine.layers();
    const QSize size = engine.canvasSize();

    // GPU readback has to happen here; compression runs on worker threads.
    // Only the part of the canvas a layer has anything on is read, and no
    // more is kept waiting to be packed than fits comfortably in memory: on
    // a large canvas one layer read whole is more than a gigabyte.
    constexpr qint64 kWaiting = qint64(1) << 30;
    std::vector<std::future<Packed>> jobs;
    std::vector<std::future<QByteArray>> maskJobs;
    std::vector<qint64> jobBytes;
    jobs.reserve(size_t(layers.size()));
    qint64 waiting = 0;
    size_t settled = 0;
    for (const LayerInfo& l : layers) {
        const QRect held = l.isGroup ? QRect() : engine.layerHeldBounds(l.id);
        const qint64 bytes = qint64(held.width()) * held.height() * 8;
        while (waiting > 0 && waiting + bytes > kWaiting && settled < jobs.size()) {
            jobs[settled].wait();
            waiting -= jobBytes[settled++];
        }
        if (held.isEmpty())
            jobs.push_back(std::async(std::launch::deferred, [] { return Packed(); }));   // groups hold no pixels, nor do empty layers
        else
            jobs.push_back(std::async(std::launch::async, pack, engine.layerPixels(l.id, held), held.size(), held.topLeft()));
        jobBytes.push_back(held.isEmpty() ? 0 : bytes);
        waiting += jobBytes.back();
        const QByteArray grey = l.hasMask ? engine.layerMaskGrey(l.id) : QByteArray();
        maskJobs.push_back(std::async(std::launch::async, [grey] { return grey.isEmpty() ? QByteArray() : qCompress(grey, 1); }));
    }

    std::vector<Packed> packed;
    std::vector<QByteArray> masks;
    packed.reserve(jobs.size());
    for (auto& j : jobs)
        packed.push_back(j.get());
    for (auto& j : maskJobs)
        masks.push_back(j.get());
    QHash<int, int> indexOfId;
    for (int i = 0; i < layers.size(); ++i)
        indexOfId.insert(layers[i].id, i);

    QJsonObject root;
    root[QStringLiteral("app")] = QStringLiteral("Impasto");
    root[QStringLiteral("width")] = size.width();
    root[QStringLiteral("height")] = size.height();
    root[QStringLiteral("background")] = engine.backgroundColor().name();
    root[QStringLiteral("backgroundVisible")] = engine.backgroundVisible();
    if (engine.resolution() > 0)
        root[QStringLiteral("dpi")] = engine.resolution();
    const Guides& guides = engine.guides();
    QJsonObject g;
    g[QStringLiteral("symmetry")] = int(guides.symmetry);
    g[QStringLiteral("segments")] = guides.segments;
    g[QStringLiteral("symmetryOffsetX")] = guides.symOffset.x();
    g[QStringLiteral("symmetryOffsetY")] = guides.symOffset.y();
    g[QStringLiteral("symmetryAngle")] = double(guides.symAngle);
    g[QStringLiteral("symmetryRotational")] = guides.symRotational;
    g[QStringLiteral("symmetryMirrored")] = guides.symMirrored;
    g[QStringLiteral("grid")] = guides.grid;
    g[QStringLiteral("gridSpacing")] = double(guides.gridSpacing);
    g[QStringLiteral("perspective")] = int(guides.perspective);
    QJsonArray points;
    for (const QPointF& vp : guides.vp) {
        points.append(vp.x());
        points.append(vp.y());
    }
    g[QStringLiteral("vanishingPoints")] = points;
    g[QStringLiteral("isoLeft")] = double(guides.isoLeft);
    g[QStringLiteral("isoRight")] = double(guides.isoRight);
    g[QStringLiteral("snap")] = guides.snap;
    root[QStringLiteral("guides")] = g;
    int active = 0;
    QJsonArray arr;
    for (int i = 0; i < layers.size(); ++i) {
        const LayerInfo& l = layers[i];
        if (l.id == engine.activeLayerId())
            active = i;
        QJsonObject o;
        o[QStringLiteral("name")] = l.name;
        o[QStringLiteral("visible")] = l.visible;
        o[QStringLiteral("opacity")] = double(l.opacity);
        o[QStringLiteral("blend")] = QString::fromLatin1(blendModeInfo(l.blend).name);
        o[QStringLiteral("alphaLock")] = l.alphaLock;
        o[QStringLiteral("clipping")] = l.clipping;
        o[QStringLiteral("group")] = l.isGroup;
        o[QStringLiteral("parent")] = indexOfId.value(l.parent, -1);   // position in this list
        o[QStringLiteral("collapsed")] = l.collapsed;
        o[QStringLiteral("maskEnabled")] = l.maskEnabled;
        if (l.reference)
            o[QStringLiteral("reference")] = true;
        if (l.isText) {
            // What was typed, so that it can still be retyped after the document comes back.
            const TextInfo t = engine.layerText(l.id);
            QJsonObject text;
            text[QStringLiteral("text")] = t.text;
            text[QStringLiteral("family")] = t.family;
            text[QStringLiteral("size")] = t.pixelSize;
            text[QStringLiteral("bold")] = t.bold;
            text[QStringLiteral("italic")] = t.italic;
            text[QStringLiteral("align")] = t.alignment;
            if (t.letterSpacing != 0.0)
                text[QStringLiteral("letterSpacing")] = t.letterSpacing;
            if (t.lineSpacing != 1.0)
                text[QStringLiteral("lineSpacing")] = t.lineSpacing;
            text[QStringLiteral("color")] = t.color.name();
            text[QStringLiteral("anchor")] = QJsonArray{t.anchor.x(), t.anchor.y()};
            const QTransform& m = t.matrix;
            text[QStringLiteral("matrix")] = QJsonArray{m.m11(), m.m12(), m.m13(), m.m21(), m.m22(), m.m23(), m.m31(), m.m32(), m.m33()};
            o[QStringLiteral("text")] = text;
        }
        o[QStringLiteral("rect")] = rectToJson(packed[size_t(i)].rect);
        arr.append(o);
    }
    root[QStringLiteral("layers")] = arr;
    root[QStringLiteral("active")] = active;

    QByteArray preview;
    {
        const QImage flat = engine.renderImage().scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QBuffer buf(&preview);
        buf.open(QIODevice::WriteOnly);
        flat.save(&buf, "PNG");
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_0);
    // The selection goes along: where it is, and how much of each pixel there is selected.
    const QRect selBounds = engine.selectionBounds();
    QByteArray selection;
    if (!selBounds.isEmpty()) {
        const QImage sel = engine.selectionPixels();
        selection.reserve(qsizetype(selBounds.width()) * selBounds.height());
        for (int y = 0; y < sel.height(); ++y)
            selection.append(reinterpret_cast<const char*>(sel.constScanLine(y)), sel.width());
        selection = qCompress(selection, 1);
    }
    out << kMagic << (selBounds.isEmpty() ? kVersionWithoutSelection : kVersion) << QJsonDocument(root).toJson(QJsonDocument::Compact) << preview;
    for (size_t i = 0; i < packed.size(); ++i)
        out << packed[i].data << masks[i];
    if (!selBounds.isEmpty())
        out << selBounds << selection;
    if (out.status() != QDataStream::Ok || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

Summary peek(const QString& path)
{
    Summary summary;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return summary;
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    QByteArray json, preview;
    in >> magic >> version;
    if (magic != kMagic || version > kVersion)
        return summary;
    in >> json >> preview;
    if (in.status() != QDataStream::Ok)
        return summary;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    summary.size = QSize(root[QStringLiteral("width")].toInt(), root[QStringLiteral("height")].toInt());
    summary.layers = int(root[QStringLiteral("layers")].toArray().size());
    summary.preview.loadFromData(preview, "PNG");
    summary.ok = !summary.size.isEmpty();
    return summary;
}

bool load(CanvasEngine& engine, const QString& path, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    QByteArray json, preview;
    in >> magic >> version;
    if (magic != kMagic)
        return fail(QObject::tr("This is not a Impasto document."));
    if (version > kVersion)
        return fail(QObject::tr("This document was made with a newer version of Impasto."));
    in >> json >> preview;

    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QSize size(root[QStringLiteral("width")].toInt(), root[QStringLiteral("height")].toInt());
    const int maxDim = CanvasEngine::maxCanvasDimension();
    if (size.width() < 1 || size.height() < 1 || size.width() > maxDim || size.height() > maxDim)
        return fail(QObject::tr("The document has an invalid canvas size."));

    const QJsonArray arr = root[QStringLiteral("layers")].toArray();
    std::vector<LayerData> layers(size_t(arr.size()));
    std::vector<std::future<QByteArray>> jobs, maskJobs;
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject o = arr[i].toObject();
        LayerData& d = layers[size_t(i)];
        d.info.name = o[QStringLiteral("name")].toString();
        d.info.visible = o[QStringLiteral("visible")].toBool(true);
        d.info.opacity = float(o[QStringLiteral("opacity")].toDouble(1.0));
        d.info.blend = blendFromName(o[QStringLiteral("blend")].toString());
        d.info.alphaLock = o[QStringLiteral("alphaLock")].toBool();
        d.info.clipping = o[QStringLiteral("clipping")].toBool();
        const QJsonArray r = o[QStringLiteral("rect")].toArray();
        if (r.size() == 4)
            d.rect = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        d.info.isGroup = o[QStringLiteral("group")].toBool();
        d.info.collapsed = o[QStringLiteral("collapsed")].toBool();
        d.info.maskEnabled = o[QStringLiteral("maskEnabled")].toBool(true);
        d.info.reference = o[QStringLiteral("reference")].toBool();
        d.parentIndex = o[QStringLiteral("parent")].toInt(-1);
        const QJsonObject text = o[QStringLiteral("text")].toObject();
        const QJsonArray anchor = text[QStringLiteral("anchor")].toArray(), matrix = text[QStringLiteral("matrix")].toArray();
        if (!text.isEmpty() && anchor.size() == 2 && matrix.size() == 9) {
            d.hasText = true;
            d.text.text = text[QStringLiteral("text")].toString();
            d.text.family = text[QStringLiteral("family")].toString();
            d.text.pixelSize = std::clamp(text[QStringLiteral("size")].toInt(48), 1, 4000);
            d.text.bold = text[QStringLiteral("bold")].toBool();
            d.text.italic = text[QStringLiteral("italic")].toBool();
            d.text.alignment = text[QStringLiteral("align")].toInt(Qt::AlignLeft);
            d.text.letterSpacing = text[QStringLiteral("letterSpacing")].toDouble(0.0);
            d.text.lineSpacing = text[QStringLiteral("lineSpacing")].toDouble(1.0);
            d.text.color = QColor(text[QStringLiteral("color")].toString());
            d.text.anchor = QPointF(anchor[0].toDouble(), anchor[1].toDouble());
            d.text.matrix = QTransform(matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(), matrix[3].toDouble(), matrix[4].toDouble(),
                                       matrix[5].toDouble(), matrix[6].toDouble(), matrix[7].toDouble(), matrix[8].toDouble());
        }
        QByteArray compressed, compressedMask;
        in >> compressed;
        if (version >= 2)
            in >> compressedMask;
        jobs.push_back(std::async(std::launch::async, [compressed] {
            return compressed.isEmpty() ? QByteArray() : qUncompress(compressed);
        }));
        maskJobs.push_back(std::async(std::launch::async, [compressedMask] {
            return compressedMask.isEmpty() ? QByteArray() : qUncompress(compressedMask);
        }));
    }
    QRect selBounds;
    QByteArray selection;
    if (version >= 3 && !in.atEnd())
        in >> selBounds >> selection;
    if (in.status() != QDataStream::Ok)
        return fail(QObject::tr("The document is damaged or incomplete."));

    for (size_t i = 0; i < layers.size(); ++i) {
        LayerData& d = layers[i];
        d.pixels = jobs[i].get();
        d.mask = maskJobs[i].get();
        const bool inside = QRect(QPoint(0, 0), size).contains(d.rect);
        if (d.rect.isEmpty() || !inside || d.pixels.size() != qsizetype(d.rect.width()) * d.rect.height() * 8) {
            d.rect = QRect();
            d.pixels.clear();
        }
    }
    QColor bg(root[QStringLiteral("background")].toString());
    if (!bg.isValid())
        bg = Qt::white;
    engine.loadDocument(size, bg, root[QStringLiteral("backgroundVisible")].toBool(true), layers,
                        root[QStringLiteral("active")].toInt());
    engine.setResolution(root[QStringLiteral("dpi")].toInt(0));
    const QJsonObject g = root[QStringLiteral("guides")].toObject();
    Guides guides;
    guides.symmetry = Guides::Symmetry(std::clamp(g[QStringLiteral("symmetry")].toInt(0), 0, 4));
    guides.segments = g[QStringLiteral("segments")].toInt(6);
    // Absent from documents saved before symmetry could be moved: then it is about the middle, upright.
    guides.symOffset = QPointF(g[QStringLiteral("symmetryOffsetX")].toDouble(0.0), g[QStringLiteral("symmetryOffsetY")].toDouble(0.0));
    guides.symAngle = float(g[QStringLiteral("symmetryAngle")].toDouble(0.0));
    guides.symRotational = g[QStringLiteral("symmetryRotational")].toBool(false);
    guides.symMirrored = g[QStringLiteral("symmetryMirrored")].toBool(false);
    guides.grid = g[QStringLiteral("grid")].toBool(false);
    guides.gridSpacing = float(g[QStringLiteral("gridSpacing")].toDouble(100.0));
    guides.perspective = Guides::Perspective(std::clamp(g[QStringLiteral("perspective")].toInt(0), 0, 5));
    const QJsonArray points = g[QStringLiteral("vanishingPoints")].toArray();
    for (int i = 0; i < 3 && i * 2 + 1 < points.size(); ++i)
        guides.vp[i] = QPointF(points[i * 2].toDouble(), points[i * 2 + 1].toDouble());
    guides.isoLeft = float(g[QStringLiteral("isoLeft")].toDouble(30.0));
    guides.isoRight = float(g[QStringLiteral("isoRight")].toDouble(30.0));
    guides.snap = g[QStringLiteral("snap")].toBool(true);
    engine.setGuides(guides);
    if (!selBounds.isEmpty()) {
        const QByteArray rows = qUncompress(selection);
        if (rows.size() == qsizetype(selBounds.width()) * selBounds.height()) {
            QImage sel(selBounds.size(), QImage::Format_Grayscale8);
            for (int y = 0; y < sel.height(); ++y)
                memcpy(sel.scanLine(y), rows.constData() + qsizetype(y) * selBounds.width(), size_t(selBounds.width()));
            engine.setSelectionPixels(selBounds, sel);
        }
    }
    return true;
}

bool exportImage(CanvasEngine& engine, const QString& path, QString* error)
{
    QImage img = engine.renderImage();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("bmp")) {
        // No alpha channel in these formats: flatten onto white.
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }
    if (engine.resolution() > 0) {
        // The size it is meant to be on paper travels with the picture.
        const int perMetre = qRound(engine.resolution() / 0.0254);
        img.setDotsPerMeterX(perMetre);
        img.setDotsPerMeterY(perMetre);
    }
    if (!img.save(path, nullptr, suffix == QLatin1String("png") ? 60 : 95)) {
        if (error)
            *error = QObject::tr("Could not write %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

QImage readImage(const QString& path, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull() && error)
        *error = reader.errorString();
    return img;
}

} // namespace docio
