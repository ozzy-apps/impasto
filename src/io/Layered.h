#pragma once

#include "engine/CanvasEngine.h"

#include <QImage>
#include <QSize>
#include <QStringList>
#include <vector>

// What Photoshop and OpenRaster files carry in and out of Impasto: a
// document as a list of layers with their pixels. Export reads the engine one
// layer at a time, so that a large canvas with many layers never has to be in
// memory all at once; import gathers everything and hands it over in one go.
namespace layered {

// A document coming in.
struct Document {
    QSize size;
    QColor background = Qt::white;
    bool backgroundVisible = true;   // false: the canvas is transparent
    int dpi = 0;
    std::vector<LayerData> layers;   // bottom to top; members name their group through parentIndex
    QStringList notes;               // what did not come across, in words for the artist
};

// The open document, going out.
class Source {
public:
    explicit Source(CanvasEngine& engine);
    QSize size() const { return m_size; }
    QColor background() const { return m_engine.backgroundColor(); }
    bool backgroundVisible() const { return m_engine.backgroundVisible(); }
    int dpi() const { return m_engine.resolution(); }
    const QVector<LayerInfo>& layers() const { return m_layers; }   // bottom to top
    int parentIndex(int i) const;   // position of the enclosing group, -1 at the top level
    // Index of the layer that layer `i` is clipped to, or -1.
    int clipBase(int i) const;
    // A layer's pixels cropped to what is on it (RGBA16F, premultiplied),
    // and its mask (8-bit grey, the whole canvas) if it has one.
    LayerData read(int i);
    // What a group shows, put together: everything in it but its own
    // opacity, mode and mask (RGBA16F, premultiplied, over `rect`).
    QByteArray groupPixels(int i, QRect* rect);
    // The finished picture, with the paper under it when the paper shows.
    QByteArray composite();   // RGBA16F, premultiplied, the whole canvas

private:
    CanvasEngine& m_engine;
    QSize m_size;
    QVector<LayerInfo> m_layers;
};

// ---- pixels
// RGBA16F premultiplied, as the engine keeps layers, to and from what files
// keep: straight (unpremultiplied) colour at 8 or 16 bits a channel.
QImage straight8(const QByteArray& half, const QSize& size);
QImage straight16(const QByteArray& half, const QSize& size);
QByteArray premultipliedHalf(const QImage& image);
QRect contentBounds(const QByteArray& half, const QSize& size);   // where alpha is not zero
QByteArray crop(const QByteArray& half, const QSize& size, const QRect& r);
// Multiplies every pixel by an 8-bit coverage given for the whole canvas
// (`grey`, one byte a pixel, `canvasWidth` to a row); `rect` is where `half` sits.
void multiplyBy(QByteArray& half, const QRect& rect, const QByteArray& grey, int canvasWidth);
// The same by the alpha of another layer's pixels (`other` sitting at `otherRect`).
void multiplyByAlpha(QByteArray& half, const QRect& rect, const QByteArray& other, const QRect& otherRect);
// A flat colour over the whole canvas, as a layer would hold it.
QByteArray flat(const QColor& colour, const QSize& size);
// Whether pixels are one opaque colour from edge to edge; `colour` receives it.
bool uniform(const QByteArray& half, QColor* colour);

// Puts the document into the engine in place of the open one. The bottom
// layer becomes the paper again when it is a plain opaque "Background", as
// both formats store the paper that way.
bool load(CanvasEngine& engine, Document& doc, QString* error);

// Names for layer blend modes in the two formats. Unknown names come back as Normal.
QByteArray psdKey(BlendMode mode);
BlendMode fromPsdKey(const QByteArray& key, bool* known = nullptr);
QString oraOp(BlendMode mode);
BlendMode fromOraOp(const QString& op, bool* known = nullptr);

} // namespace layered
