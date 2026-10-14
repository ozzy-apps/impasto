// OpenRaster: a zip of PNG layers and an XML file that says how they stack.
// https://www.openraster.org/

#include "io/Interchange.h"
#include "io/Layered.h"
#include "io/Zip.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <functional>

namespace interchange {

namespace {

QByteArray png(const QImage& img)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    writer.setQuality(70);   // a little less squeezing than the most: big layers would take long
    writer.write(img);
    return out;
}

// Large layers are larger than Qt reads by default; lifted while a file is read.
struct ReadAnySize {
    int before = QImageReader::allocationLimit();
    ReadAnySize() { QImageReader::setAllocationLimit(0); }
    ~ReadAnySize() { QImageReader::setAllocationLimit(before); }
};

QString number(double v)
{
    return QString::number(v, 'f', 3);
}

} // namespace

bool exportOra(CanvasEngine& engine, const QString& path, QString* error, QStringList* notes)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    layered::Source src(engine);
    const QSize size = src.size();
    const QVector<LayerInfo>& layers = src.layers();
    const QString temporary = path + QStringLiteral(".part");
    zip::Writer zip(temporary);
    if (!zip.ok())
        return fail(zip.error());
    // The first entry, stored, so that the file says what it is in its first bytes.
    zip.add(QStringLiteral("mimetype"), QByteArrayLiteral("image/openraster"), false);

    QByteArray xml;
    QXmlStreamWriter out(&xml);
    out.setAutoFormatting(true);
    out.writeStartDocument();
    out.writeStartElement(QStringLiteral("image"));
    out.writeAttribute(QStringLiteral("version"), QStringLiteral("0.0.6"));
    out.writeAttribute(QStringLiteral("w"), QString::number(size.width()));
    out.writeAttribute(QStringLiteral("h"), QString::number(size.height()));
    if (src.dpi() > 0) {
        out.writeAttribute(QStringLiteral("xres"), QString::number(src.dpi()));
        out.writeAttribute(QStringLiteral("yres"), QString::number(src.dpi()));
    }
    out.writeStartElement(QStringLiteral("stack"));

    bool bakedMasks = false, bakedClipping = false, droppedClipping = false, flattenedText = false, mergedGroups = false;
    int written = 0;
    // A stack lists what is on top first; the layers are kept bottom first.
    std::function<void(int)> stack = [&](int parent) {
        std::vector<int> members;
        for (int i = 0; i < layers.size(); ++i) {
            if (src.parentIndex(i) == parent)
                members.push_back(i);
        }
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            const int i = *it;
            const LayerInfo& l = layers[i];
            if (l.isGroup && l.hasMask && l.maskEnabled) {
                // No masks in OpenRaster: a masked group goes as one layer,
                // what it shows put together with its mask taken out of it.
                QRect rect;
                QByteArray pixels = src.groupPixels(i, &rect);
                const LayerData g = src.read(i);
                if (!pixels.isEmpty())
                    layered::multiplyBy(pixels, rect, g.mask, size.width());
                const QRect inside = pixels.isEmpty() ? QRect() : layered::contentBounds(pixels, rect.size());
                QImage image;
                QPoint at;
                if (!inside.isEmpty()) {
                    image = layered::straight8(layered::crop(pixels, rect.size(), inside), inside.size());
                    at = inside.topLeft() + rect.topLeft();
                } else {
                    image = QImage(1, 1, QImage::Format_RGBA8888);
                    image.fill(Qt::transparent);
                }
                const QString name = QStringLiteral("data/layer%1.png").arg(++written);
                if (!zip.add(name, png(image), false))
                    return;
                out.writeEmptyElement(QStringLiteral("layer"));
                out.writeAttribute(QStringLiteral("name"), l.name);
                out.writeAttribute(QStringLiteral("src"), name);
                out.writeAttribute(QStringLiteral("x"), QString::number(at.x()));
                out.writeAttribute(QStringLiteral("y"), QString::number(at.y()));
                out.writeAttribute(QStringLiteral("opacity"), number(l.opacity));
                out.writeAttribute(QStringLiteral("visibility"), l.visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
                out.writeAttribute(QStringLiteral("composite-op"), layered::oraOp(l.blend));
                mergedGroups = true;
                continue;
            }
            if (l.isGroup) {
                out.writeStartElement(QStringLiteral("stack"));
                out.writeAttribute(QStringLiteral("name"), l.name);
                out.writeAttribute(QStringLiteral("opacity"), number(l.opacity));
                out.writeAttribute(QStringLiteral("visibility"), l.visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
                out.writeAttribute(QStringLiteral("composite-op"), layered::oraOp(l.blend));
                out.writeAttribute(QStringLiteral("isolation"), QStringLiteral("isolate"));
                stack(i);
                out.writeEndElement();
                continue;
            }
            LayerData d = src.read(i);
            float opacity = l.opacity;
            bool visible = l.visible;
            if (d.info.isText)
                flattenedText = true;
            // No masks in OpenRaster: what the mask hides is taken out of the pixels.
            if (!d.mask.isEmpty() && l.maskEnabled && !d.pixels.isEmpty()) {
                layered::multiplyBy(d.pixels, d.rect, d.mask, size.width());
                bakedMasks = true;
            }
            // Nor clipping: the layer keeps only what lies over the layer it was clipped to.
            const int base = src.clipBase(i);
            if (l.clipping && base < 0)
                droppedClipping = true;
            if (base >= 0 && layers[base].isGroup) {
                droppedClipping = true;
            } else if (base >= 0) {
                const LayerData under = src.read(base);
                if (!d.pixels.isEmpty())
                    layered::multiplyByAlpha(d.pixels, d.rect, under.pixels, under.rect);
                if (!under.mask.isEmpty() && layers[base].maskEnabled && !d.pixels.isEmpty())
                    layered::multiplyBy(d.pixels, d.rect, under.mask, size.width());
                opacity *= layers[base].opacity;
                visible = visible && layers[base].visible;
                bakedClipping = true;
            }
            QImage image = d.pixels.isEmpty() ? QImage() : layered::straight8(d.pixels, d.rect.size());
            QPoint at = d.rect.topLeft();
            if (image.isNull()) {
                image = QImage(1, 1, QImage::Format_RGBA8888);
                image.fill(Qt::transparent);
                at = QPoint(0, 0);
            }
            const QString name = QStringLiteral("data/layer%1.png").arg(++written);
            if (!zip.add(name, png(image), false))
                return;
            out.writeEmptyElement(QStringLiteral("layer"));
            out.writeAttribute(QStringLiteral("name"), l.name);
            out.writeAttribute(QStringLiteral("src"), name);
            out.writeAttribute(QStringLiteral("x"), QString::number(at.x()));
            out.writeAttribute(QStringLiteral("y"), QString::number(at.y()));
            out.writeAttribute(QStringLiteral("opacity"), number(opacity));
            out.writeAttribute(QStringLiteral("visibility"), visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
            out.writeAttribute(QStringLiteral("composite-op"), layered::oraOp(l.blend));
            if (l.id == engine.activeLayerId())
                out.writeAttribute(QStringLiteral("selected"), QStringLiteral("true"));
        }
    };
    stack(-1);
    // The paper, as the layer other programs expect at the bottom.
    if (src.backgroundVisible() && zip.ok()) {
        QImage whole(size, QImage::Format_RGBA8888);
        whole.fill(src.background());
        zip.add(QStringLiteral("data/background.png"), png(whole), false);
        out.writeEmptyElement(QStringLiteral("layer"));
        out.writeAttribute(QStringLiteral("name"), QStringLiteral("Background"));
        out.writeAttribute(QStringLiteral("src"), QStringLiteral("data/background.png"));
        out.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
        out.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
        out.writeAttribute(QStringLiteral("opacity"), number(1.0));
        out.writeAttribute(QStringLiteral("visibility"), QStringLiteral("visible"));
        out.writeAttribute(QStringLiteral("composite-op"), QStringLiteral("svg:src-over"));
    }
    out.writeEndElement();   // stack
    out.writeEndElement();   // image
    out.writeEndDocument();
    zip.add(QStringLiteral("stack.xml"), xml, true);

    // What the picture looks like, for programs that only show it.
    const QImage merged = layered::straight8(src.composite(), size);
    zip.add(QStringLiteral("mergedimage.png"), png(merged), false);
    zip.add(QStringLiteral("Thumbnails/thumbnail.png"), png(merged.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)), false);
    if (!zip.finish()) {
        QFile::remove(temporary);
        return fail(zip.error());
    }
    QFile::remove(path);
    if (!QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        return fail(QObject::tr("Could not write %1.").arg(QFileInfo(path).fileName()));
    }
    if (notes) {
        if (bakedMasks)
            *notes << QObject::tr("OpenRaster has no layer masks: what they hid is left out of the layers.");
        if (bakedClipping)
            *notes << QObject::tr("OpenRaster has no clipping: clipped layers keep only what lies over the layer below.");
        if (droppedClipping)
            *notes << QObject::tr("Layers clipped to a group are no longer clipped.");
        if (mergedGroups)
            *notes << QObject::tr("Each group with a mask went as one layer, with what the mask hid left out.");
        if (flattenedText)
            *notes << QObject::tr("Text goes as pixels.");
    }
    return true;
}

bool readOra(const QString& path, layered::Document* doc, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    zip::Reader zip;
    if (!zip.open(path))
        return fail(zip.error());
    if (!zip.contains(QStringLiteral("stack.xml")))
        return fail(QObject::tr("This is not an OpenRaster file."));
    const QByteArray xml = zip.read(QStringLiteral("stack.xml"));
    ReadAnySize anySize;
    QXmlStreamReader in(xml);
    bool unknownModes = false, skipped = false, sized = false;

    // The file as a tree first: each stack lists what is on top first.
    struct Node {
        bool group = false;
        LayerData data;
        std::vector<Node> children;
    };
    auto attr = [&](const char* name) { return in.attributes().value(QLatin1String(name)).toString(); };
    auto common = [&](LayerInfo& info) {
        info.name = attr("name");
        const QString opacity = attr("opacity");
        info.opacity = opacity.isEmpty() ? 1.0f : std::clamp(opacity.toFloat(), 0.0f, 1.0f);
        info.visible = attr("visibility") != QLatin1String("hidden");
        bool known = true;
        info.blend = layered::fromOraOp(attr("composite-op"), &known);
        unknownModes = unknownModes || !known;
    };
    std::function<void(Node&)> readStack = [&](Node& stack) {
        while (!in.atEnd()) {
            in.readNext();
            if (in.isEndElement() && in.name() == QLatin1String("stack"))
                return;
            if (!in.isStartElement())
                continue;
            if (in.name() == QLatin1String("stack")) {
                Node group;
                group.group = true;
                group.data.info.isGroup = true;
                common(group.data.info);
                readStack(group);
                stack.children.push_back(std::move(group));
            } else if (in.name() == QLatin1String("layer")) {
                Node layer;
                common(layer.data.info);
                QImage image;
                image.loadFromData(zip.read(attr("src")), "png");
                if (image.isNull()) {
                    skipped = true;   // kept, empty, so that the stack still makes sense
                } else {
                    layer.data.rect = QRect(QPoint(attr("x").toInt(), attr("y").toInt()), image.size());
                    layer.data.pixels = layered::premultipliedHalf(image);
                    if (layered::contentBounds(layer.data.pixels, image.size()).isEmpty()) {
                        layer.data.rect = QRect();
                        layer.data.pixels.clear();
                    }
                }
                stack.children.push_back(std::move(layer));
            } else if (in.name() == QLatin1String("text") || in.name() == QLatin1String("filter")) {
                skipped = true;
            }
        }
    };
    Node root;
    while (!in.atEnd()) {
        in.readNext();
        if (!in.isStartElement())
            continue;
        if (in.name() == QLatin1String("image")) {
            doc->size = QSize(attr("w").toInt(), attr("h").toInt());
            doc->dpi = qRound(attr("xres").toDouble());
            sized = true;
        } else if (in.name() == QLatin1String("stack")) {
            readStack(root);
            break;   // the picture is its one outermost stack
        }
    }
    if (in.hasError())
        return fail(QObject::tr("The layer list in this file is damaged: %1").arg(in.errorString()));
    if (!sized || doc->size.isEmpty())
        return fail(QObject::tr("This OpenRaster file does not say how large it is."));

    // Then as a list, bottom first, each group's header just above its members.
    std::function<int(const Node&)> entries = [&](const Node& n) {
        int count = n.group ? 1 : 0;
        for (const Node& c : n.children)
            count += c.group ? entries(c) : 1;
        return count;
    };
    std::function<void(const Node&, int)> flatten = [&](const Node& stack, int parent) {
        for (auto it = stack.children.rbegin(); it != stack.children.rend(); ++it) {
            if (it->group) {
                const int header = int(doc->layers.size()) + entries(*it) - 1;
                flatten(*it, header);
                LayerData h = it->data;
                h.parentIndex = parent;
                doc->layers.push_back(std::move(h));
            } else {
                LayerData d = it->data;
                d.parentIndex = parent;
                doc->layers.push_back(std::move(d));
            }
        }
    };
    flatten(root, -1);
    if (unknownModes)
        doc->notes << QObject::tr("Some blend modes are unknown here and became Normal.");
    if (skipped)
        doc->notes << QObject::tr("Some parts of the file (text, filters, or a picture that could not be read) were left out.");
    return true;
}

} // namespace interchange
