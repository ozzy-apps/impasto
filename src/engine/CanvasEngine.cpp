#include "engine/CanvasEngine.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QRandomGenerator>
#include <cmath>
#include <cstring>

using gfx::gl;

namespace {
constexpr int kTile = 256;
constexpr int kBand = 2048;   // side of the buffers the canvas is composited in
// Graphics memory a canvas needs besides its layers, per pixel: the stroke
// buffer (10 bytes), the live copy of the layer being worked on (8), two
// compositing buffers (16), the picture shown with its mipmaps (11) and the
// selection (4).
constexpr qint64 kWorkingBytesPerPixel = 49;
constexpr qint64 kLayerBytesPerPixel = 8;
// Where layers and working buffers take memory only where there is paint,
// what stays the size of the canvas is the finished picture with its mipmaps
// (11 bytes a pixel) and the selection (2), rounded up.
constexpr qint64 kSparseFixedBytesPerPixel = 13;
// Canvases that leave less room than this many fully painted layers are not
// offered: the layer being painted on, its live copy, and one more.
constexpr qint64 kSparseLeastLayers = 3;
// The stroke buffer and the live copy of the layer, kept whole.
constexpr qint64 kStrokeBytesPerPixel = 18;
constexpr double kMemoryShare = 0.8;   // the desktop and other programs want some too
constexpr int kFloodUnderLine = 3;   // pixels a fill carries on beneath a reference line
} // namespace

// Makes the engine's context current for the duration of a public call and
// hands control back to whoever was current before (usually the viewport).
class CanvasEngine::Scope {
public:
    explicit Scope(const CanvasEngine* e)
    {
        m_prev = QOpenGLContext::currentContext();
        if (m_prev == e->m_ctx)
            return;
        m_prevSurface = m_prev ? m_prev->surface() : nullptr;
        e->m_ctx->makeCurrent(e->m_surface);
        m_switched = true;
    }
    ~Scope()
    {
        if (!m_switched)
            return;
        // Shared textures only become visible to the other context once flushed.
        gl()->glFlush();
        if (m_prev && m_prevSurface)
            m_prev->makeCurrent(m_prevSurface);
    }

private:
    QOpenGLContext* m_prev = nullptr;
    QSurface* m_prevSurface = nullptr;
    bool m_switched = false;
};

// ------------------------------------------------------------ undo commands

// Before/after pixels of a layer. Strokes store only the 256px tiles they
// touched (in a texture array, so undo never leaves the GPU); whole-layer
// operations keep a full copy and simply swap texture objects.
// Every state a layer's pixels are ever in has a number of its own. Undo and
// redo bring the number back with the pixels, which is how a layer knows that
// it is, once more, exactly the text that was typed.
static quint64 nextStamp()
{
    static quint64 last = 0;
    return ++last;
}

// What undo keeps in texture arrays, which gfx::textureBytes() does not see.
static qint64 s_arrayBytes = 0;

static void touch(Layer& layer)
{
    ++layer.revision;
    layer.stamp = nextStamp();
}

class PixelCommand : public Command {
public:
    // onMask: the pixels belong to the layer's mask rather than the layer.
    PixelCommand(CanvasEngine* e, LayerPtr layer, std::vector<QPoint> tiles, const QString& n, bool onMask = false)
        : m_e(e), m_layer(std::move(layer)), m_tiles(std::move(tiles)), m_mask(onMask)
    {
        name = n;
        m_stamp = m_layer->stamp;
        gfx::Texture& tex = m_layer->target(m_mask);
        const size_t total = size_t(e->m_tilesX) * size_t(e->m_tilesY);
        if (m_tiles.empty() || m_tiles.size() * 10 > total * 6 || m_tiles.size() > 2000) {
            m_tiles.clear();
            m_full.create(e->m_size.width(), e->m_size.height(), GL_RGBA16F, false, tex.sparse());
            gfx::copyTexture(tex, m_full);
            return;
        }
        gfx::GLF* f = gl();
        f->glGenTextures(1, &m_array);
        f->glBindTexture(GL_TEXTURE_2D_ARRAY, m_array);
        f->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA16F, kTile, kTile, GLsizei(m_tiles.size()));
        // A tile the layer has nothing in is remembered as just that.
        m_blank.resize(m_tiles.size());
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const QRect r = e->tileRect(m_tiles[i]);
            m_blank[i] = !tex.heldAt(r.topLeft());
            if (!m_blank[i])
                gfx::copyToLayer(tex, r, m_array, int(i));
        }
        m_bytes = qint64(m_tiles.size()) * kTile * kTile * 8;
        s_arrayBytes += m_bytes;
    }

    ~PixelCommand() override
    {
        if (m_array)
            s_arrayBytes -= m_bytes;
        if (m_array && gl())
            gl()->glDeleteTextures(1, &m_array);
    }

    void undo() override { swap(); }
    void redo() override { swap(); }
    // What a whole-layer copy holds changes as it trades places with the layer.
    qint64 bytes() const override { return m_full.valid() ? m_full.bytes() : m_bytes; }

private:
    void swap()
    {
        Layer& layer = *m_layer;
        if (m_mask && !layer.mask)
            return;   // the mask is gone; history order normally makes this impossible
        gfx::Texture& tex = layer.target(m_mask);
        if (m_full.valid()) {
            tex.swap(m_full);
            m_e->damageAll();
        } else {
            QRect bounds;
            for (size_t i = 0; i < m_tiles.size(); ++i) {
                const QRect r = m_e->tileRect(m_tiles[i]);
                // What the layer has there now goes into keeping, and what
                // was kept goes back: nothing for nothing, where either side
                // is empty, so that undoing a stroke also gives back the
                // memory the stroke took.
                const bool blankNow = !tex.heldAt(r.topLeft());
                if (!blankNow)
                    gfx::copyTexture(tex, m_e->m_tileTmp, r, {0, 0});
                if (m_blank[i])
                    tex.clearRect(r);
                else
                    gfx::copyFromLayer(m_array, int(i), tex, r);
                if (!blankNow)
                    gfx::copyToLayer(m_e->m_tileTmp, QRect(0, 0, r.width(), r.height()), m_array, int(i));
                m_blank[i] = blankNow;
                bounds |= r;
            }
            m_e->damage(bounds);
        }
        if (m_mask) {
            ++layer.maskRevision;
        } else {
            ++layer.revision;
            std::swap(layer.stamp, m_stamp);
        }
        emit m_e->layersChanged();
    }

    CanvasEngine* m_e;
    LayerPtr m_layer;
    std::vector<QPoint> m_tiles;
    std::vector<char> m_blank;   // per tile: nothing is kept for it, because there was nothing
    bool m_mask = false;
    quint64 m_stamp = 0;   // of the pixels kept here
    GLuint m_array = 0;
    gfx::Texture m_full;
    qint64 m_bytes = 0;
};

class PropsCommand : public Command {
public:
    PropsCommand(CanvasEngine* e, LayerPtr layer, LayerInfo before, LayerInfo after, const QString& n,
                 const QString& kind)
        : m_e(e), m_layer(std::move(layer)), m_before(std::move(before)), m_after(std::move(after)), m_kind(kind)
    {
        name = n;
    }
    void undo() override { m_e->applyProps(*m_layer, m_before); }
    void redo() override { m_e->applyProps(*m_layer, m_after); }
    bool mergeWith(const Command& next) override
    {
        const auto* o = dynamic_cast<const PropsCommand*>(&next);
        if (!o || m_kind.isEmpty() || o->m_kind != m_kind || o->m_layer != m_layer)
            return false;
        m_after = o->m_after;
        return true;
    }

private:
    CanvasEngine* m_e;
    LayerPtr m_layer;
    LayerInfo m_before, m_after;
    QString m_kind;
};

// ------------------------------------------------------------------ set-up

CanvasEngine::CanvasEngine(QObject* parent) : QObject(parent), m_history(this) {}

CanvasEngine::~CanvasEngine()
{
    if (!m_ctx)
        return;
    m_ctx->makeCurrent(m_surface);
    m_history.clear();
    m_layers.clear();
    m_stroke = {};
    m_edit = {};
    m_strokeBuf.destroy();
    m_live.destroy();
    m_result.destroy();
    m_scratch[0].destroy();
    m_scratch[1].destroy();
    m_tileTmp.destroy();
    m_compositeProg.destroy();
    m_mergeProg.destroy();
    m_thumbProg.destroy();
    m_occupancyProg.destroy();
    m_occupancy.destroy();
    m_heldMap.destroy();
    m_flipProg.destroy();
    m_transformProg.destroy();
    m_hsbProg.destroy();
    m_blurProg.destroy();
    m_maskMixProg.destroy();
    m_invertProg.destroy();
    m_floodDistProg.destroy();
    m_adjustProg.destroy();
    m_gradientProg.destroy();
    m_liquifyProg.destroy();
    m_liquifyApplyProg.destroy();
    m_liquify = {};
    m_warpProg.destroy();
    if (m_warpVbo)
        gl()->glDeleteBuffers(1, &m_warpVbo);
    if (m_warpVao)
        gl()->glDeleteVertexArrays(1, &m_warpVao);
    m_lut.destroy();
    m_bandBufs.clear();
    m_selMask.destroy();
    m_selTmp.destroy();
    m_floodDist.destroy();
    m_fs.destroy();
    m_dabs.destroy();
    m_ctx->doneCurrent();
    gfx::invalidateGlCache();
}

bool CanvasEngine::initialize(QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };

    m_ctx = new QOpenGLContext(this);
    m_ctx->setFormat(QSurfaceFormat::defaultFormat());
    m_ctx->setShareContext(QOpenGLContext::globalShareContext());
    if (!m_ctx->create())
        return fail(tr("Could not create an OpenGL context."));
    m_surface = new QOffscreenSurface(nullptr, this);
    m_surface->setFormat(m_ctx->format());
    m_surface->create();
    if (!m_ctx->makeCurrent(m_surface))
        return fail(tr("Could not activate the OpenGL context."));

    const QSurfaceFormat fmt = m_ctx->format();
    if (!gl() || fmt.version() < qMakePair(4, 3)) {
        return fail(tr("Impasto needs OpenGL 4.3 core or newer; this system offers %1.%2.")
                        .arg(fmt.majorVersion())
                        .arg(fmt.minorVersion()));
    }
    m_renderer = QString::fromLatin1(reinterpret_cast<const char*>(gl()->glGetString(GL_RENDERER)));

    // What this card can hold decides how big a canvas may be.
    GLint largest = 16384;
    gl()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    s_maxDimension = std::clamp(int(largest), 4096, 32768);
    // Only NVIDIA's own driver says truthfully how much memory the card has;
    // others that answer the question (software rendering does) answer something else.
    if (nvidiaMemoryInfo()) {
        GLint kilobytes = 0;
        gl()->glGetIntegerv(0x9047 /* GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX */, &kilobytes);
        if (kilobytes > 0)
            s_memoryBytes = qint64(kilobytes) * 1024;
    }
    s_sparse = gfx::sparseAllowed() && gfx::sparseSupported();
    // Room for the working buffers and two layers at the very least; or,
    // where only paint takes memory, for the picture itself and a few layers'
    // worth of paint.
    const qint64 perPixel = s_sparse ? kSparseFixedBytesPerPixel + kSparseLeastLayers * kLayerBytesPerPixel
                                     : kWorkingBytesPerPixel + 2 * kLayerBytesPerPixel;
    s_maxPixels = std::clamp(qint64(double(s_memoryBytes) * kMemoryShare) / perPixel, qint64(16'000'000), qint64(400'000'000));

    const QStringList fsVert{QStringLiteral(":/shaders/fullscreen.vert")};
    auto frag = [](const char* name) { return QStringList{QStringLiteral(":/shaders/%1.frag").arg(QLatin1String(name))}; };
    const QString blendChunk = QStringLiteral(":/shaders/blend.glsl");
    bool ok = m_compositeProg.build(fsVert, {blendChunk, QStringLiteral(":/shaders/composite.frag")});
    ok = m_mergeProg.build(fsVert, {blendChunk, QStringLiteral(":/shaders/merge.frag")}) && ok;
    ok = m_thumbProg.build(fsVert, frag("thumb")) && ok;
    ok = m_occupancyProg.build(fsVert, frag("occupancy")) && ok;
    ok = m_flipProg.build(fsVert, frag("flip")) && ok;
    ok = m_transformProg.build(fsVert, frag("transform")) && ok;
    ok = m_hsbProg.build(fsVert, frag("hsb")) && ok;
    ok = m_blurProg.build(fsVert, frag("blur")) && ok;
    ok = m_maskMixProg.build(fsVert, frag("maskmix")) && ok;
    ok = m_invertProg.build(fsVert, frag("invert")) && ok;
    ok = m_floodDistProg.build(fsVert, frag("flooddist")) && ok;
    ok = m_adjustProg.build(fsVert, frag("adjust")) && ok;
    ok = m_gradientProg.build(fsVert, frag("gradient")) && ok;
    ok = m_liquifyProg.build(fsVert, frag("liquify")) && ok;
    ok = m_liquifyApplyProg.build(fsVert, frag("liquifyapply")) && ok;
    ok = m_warpProg.build({QStringLiteral(":/shaders/warp.vert")}, frag("warp")) && ok;
    {
        // The warp mesh: two floats for where a corner goes, two for what it carries.
        gfx::GLF* f = gl();
        f->glGenVertexArrays(1, &m_warpVao);
        f->glGenBuffers(1, &m_warpVbo);
        f->glBindVertexArray(m_warpVao);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_warpVbo);
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        f->glEnableVertexAttribArray(1);
        f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(quintptr(2 * sizeof(float))));
        f->glBindVertexArray(0);
    }
    m_lut.create(256, 1, GL_RGBA8);
    m_fs.init();
    ok = m_dabs.init() && ok;
    m_tileTmp.create(kTile, kTile, GL_RGBA16F);
    if (!ok)
        return fail(tr("A shader failed to compile; see the terminal output for details."));
    return true;
}

int CanvasEngine::layerEstimate(const QSize& size)
{
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels <= 0)
        return 0;
    // Sparse: the picture, the selection, and what a stroke passes through:
    // kept whole on a small canvas, the size of the layer being worked on at most on a large one.
    const qint64 working = !s_sparse ? kWorkingBytesPerPixel
                                     : kSparseFixedBytesPerPixel + (leanBuffers(pixels) ? kLayerBytesPerPixel : kStrokeBytesPerPixel);
    const qint64 left = qint64(double(s_memoryBytes) * kMemoryShare) - pixels * working - (s_sparse ? qint64(2) * kBand * kBand * 8 : 0);
    return int(std::clamp(left / (pixels * kLayerBytesPerPixel), qint64(0), qint64(999)));
}

// Everything that is the size of the canvas but not part of the document:
// working buffers, the selection, the tile map.
void CanvasEngine::resizeBuffers(const QSize& size)
{
    m_size = size;
    const int w = size.width(), h = size.height();
    // Everything a stroke or an edit passes through only takes memory where
    // the stroke or the edit is. The finished picture is kept whole.
    const bool lean = leanBuffers(qint64(w) * h);
    m_strokeBuf.create(w, h, lean);
    m_live.create(w, h, GL_RGBA16F, false, lean);
    m_scratch[0].create(w, h, GL_RGBA16F, false, true);
    m_scratch[1].create(w, h, GL_RGBA16F, false, true);
    m_result.create(w, h, GL_RGBA16F, true);
    m_selMask.create(w, h, GL_R8);
    m_selTmp.create(w, h, GL_R8);
    m_occupancy.create(m_live.pagesX(), m_live.pagesY(), GL_R8);
    m_heldMap.create(m_live.pagesX(), m_live.pagesY(), GL_R8);
    m_heldMap.setFilter(GL_NEAREST, GL_NEAREST);
    m_floodDist.destroy();
    m_selMask.clear();
    m_selImage = QImage(size, QImage::Format_Grayscale8);
    m_selImage.fill(0);
    m_selBounds = QRect();
    m_feather = {};
    m_flood = {};
    m_gradient = {};
    m_liquify = {};
    m_bandBufs.clear();
    m_overlay = nullptr;
    m_overlayMask = false;
    m_live.clear();
    m_result.clear();
    m_tilesX = (w + kTile - 1) / kTile;
    m_tilesY = (h + kTile - 1) / kTile;
    m_dirtyTiles.assign(size_t(m_tilesX) * size_t(m_tilesY), 0);
    m_dirty = QRect();
    m_mipsStale = true;
}

void CanvasEngine::allocate(const QSize& size)
{
    resizeBuffers(size);
    m_referenceId = 0;
    m_resolution = 0;
    m_guides = {};
    m_anim = {};
    m_playFrame = -1;
    m_maskEditing = false;
    m_texts.clear();
    m_textEditId = 0;
}

// The layers as they were at another canvas size. Applying and undoing are
// the same act: what is held here changes places with what is in the document.
class CanvasSizeCommand : public Command {
public:
    struct Entry {
        LayerPtr layer;
        gfx::Texture tex;
        std::shared_ptr<gfx::Texture> mask;
        quint64 stamp = nextStamp();
    };
    CanvasSizeCommand(CanvasEngine* e, const QString& n) : m_e(e) { name = n; }
    std::vector<Entry> entries;
    QSize size;
    Guides guides;

    void undo() override { swap(); }
    void redo() override { swap(); }
    qint64 bytes() const override
    {
        qint64 total = 0;
        for (const Entry& e : entries)
            total += e.tex.bytes() + (e.mask ? e.mask->bytes() : 0);
        return total;
    }
    void swap()
    {
        for (Entry& e : entries) {
            if (!e.layer->isGroup)
                e.layer->tex.swap(e.tex);
            std::swap(e.layer->mask, e.mask);
            std::swap(e.layer->stamp, e.stamp);
            ++e.layer->revision;
            ++e.layer->maskRevision;
        }
        const QSize was = m_e->m_size;
        m_e->resizeBuffers(size);
        size = was;
        std::swap(m_e->m_guides, guides);
        m_e->damageAll();
        emit m_e->selectionChanged();
        emit m_e->guidesChanged();
        emit m_e->documentChanged();
        emit m_e->layersChanged();
    }

private:
    CanvasEngine* m_e;
};

// Redraws `src` into `dst` (which may be another size) through `placement`.
void CanvasEngine::resample(const gfx::Texture& src, gfx::Texture& dst, QTransform placement, bool fillWhite)
{
    // Shrinking to less than half in one step would skip pixels and shimmer:
    // go down by halves first, each of which averages four pixels into one.
    const gfx::Texture* from = &src;
    gfx::Texture steps[2];
    int next = 0;
    auto draw = [&](const gfx::Texture& in, gfx::Texture& out, const QTransform& t, bool smooth) {
        float m[9];
        gfx::toMat3(t.inverted(), m);
        // Only where what the source has can land (a mask is white beyond its edges: all of it).
        const QRect reach = fillWhite ? out.rect() : t.mapRect(QRectF(in.heldBounds())).adjusted(-3.0, -3.0, 3.0, 3.0).toAlignedRect() & out.rect();
        const QRect area = out.holdOnly(reach);
        if (area.isEmpty())
            return;
        fullscreenTo(out, &area);
        m_transformProg.use();
        in.bind(0);
        in.bind(1);
        m_transformProg.set("uSrc", 0);
        m_transformProg.set("uSelection", 1);
        m_transformProg.set("uHasSelection", 0);
        m_transformProg.setMat3("uInverse", m);
        m_transformProg.set("uCanvasSize", float(in.width()), float(in.height()));
        m_transformProg.set("uSmooth", smooth ? 1 : 0);
        m_transformProg.set("uFillWhite", fillWhite ? 1 : 0);
        m_fs.draw();
        gl()->glDisable(GL_SCISSOR_TEST);
    };
    while (std::min(std::abs(placement.m11()), std::abs(placement.m22())) < 0.5 && from->width() > 2 && from->height() > 2) {
        gfx::Texture& half = steps[next];
        half.create((from->width() + 1) / 2, (from->height() + 1) / 2, GL_RGBA16F);
        draw(*from, half, QTransform::fromScale(0.5, 0.5), true);
        from = &half;
        next = 1 - next;
        placement = QTransform::fromScale(2.0, 2.0) * placement;
    }
    // A plain shift by whole pixels copies them exactly.
    const bool shiftOnly = placement.type() <= QTransform::TxTranslate && placement.dx() == std::round(placement.dx())
                           && placement.dy() == std::round(placement.dy());
    draw(*from, dst, placement, !shiftOnly);
}

bool CanvasEngine::resizeCanvas(const QSize& newSize, const QTransform& placement, const QString& undoName)
{
    const int limit = maxCanvasDimension();
    if (!hasDocument() || busy() || newSize.width() < 1 || newSize.height() < 1 || newSize.width() > limit || newSize.height() > limit)
        return false;
    if (!placement.isInvertible() || (newSize == m_size && placement.isIdentity()))
        return false;
    Scope s(this);
    auto compound = std::make_unique<CompoundCommand>(undoName);
    if (hasSelection()) {
        // A selection drawn for the old canvas means nothing on the new one.
        const SelSnapPtr before = snapshotSelection();
        m_selImage.fill(0);
        selectionEdited();
        pushSelection(before, QString(), compound.get());
    }
    auto cmd = std::make_unique<CanvasSizeCommand>(this, QString());
    for (const LayerPtr& layer : m_layers) {
        CanvasSizeCommand::Entry e;
        e.layer = layer;
        if (!layer->isGroup) {
            e.tex.create(newSize.width(), newSize.height(), GL_RGBA16F, false, true);
            resample(layer->tex, e.tex, placement, false);
            // One layer at a time: all of them drawn whole and trimmed afterwards would not fit.
            trim(e.tex, e.tex.rect());
        }
        if (layer->mask) {
            // Room added round a mask shows the layer, as a new mask would.
            e.mask = std::make_shared<gfx::Texture>();
            e.mask->create(newSize.width(), newSize.height(), GL_RGBA16F);
            resample(*layer->mask, *e.mask, placement, true);
        }
        cmd->entries.push_back(std::move(e));
    }
    // Guides stay on the part of the picture they were set up for.
    cmd->size = newSize;
    cmd->guides = m_guides;
    for (QPointF& vp : cmd->guides.vp)
        vp = placement.map(vp);
    cmd->guides.symOffset = placement.map(symmetryCentre()) - QPointF(newSize.width() / 2.0, newSize.height() / 2.0);
    cmd->guides.gridSpacing = std::clamp(m_guides.gridSpacing * float(std::abs(placement.m11())), 8.0f, 2000.0f);
    cmd->swap();
    compound->add(std::move(cmd));
    m_history.push(std::move(compound));
    setModified(true);
    return true;
}

void CanvasEngine::newDocument(const QSize& size, const QColor& background, bool backgroundVisible,
                               const QImage& firstLayer)
{
    Scope s(this);
    m_stroke = {};
    m_edit = {};
    m_overlay = nullptr;
    m_history.clear();
    m_layers.clear();
    m_nameCounter = 0;
    allocate(size);
    m_background = background;
    m_backgroundVisible = backgroundVisible;
    LayerPtr layer = createLayer(nextLayerName());
    if (!firstLayer.isNull())
        layer->tex.upload(firstLayer.copy(QRect(QPoint(0, 0), size)));
    m_layers.push_back(layer);
    m_activeId = layer->id;
    damageAll();
    setModified(false);
    emit selectionChanged();
    emit documentChanged();
    emit layersChanged();
}

void CanvasEngine::loadDocument(const QSize& size, const QColor& background, bool backgroundVisible,
                                const std::vector<LayerData>& layers, int activeIndex)
{
    Scope s(this);
    m_stroke = {};
    m_edit = {};
    m_overlay = nullptr;
    m_history.clear();
    m_layers.clear();
    m_nameCounter = 0;
    allocate(size);
    m_background = background;
    m_backgroundVisible = backgroundVisible;
    const int w = size.width(), h = size.height();
    for (const LayerData& d : layers) {
        LayerPtr layer = d.info.isGroup ? createGroup(d.info.name) : createLayer(d.info.name);
        layer->visible = d.info.visible;
        layer->opacity = d.info.opacity;
        layer->blend = d.info.blend;
        layer->alphaLock = d.info.alphaLock;
        layer->clipping = d.info.clipping;
        layer->collapsed = d.info.collapsed;
        layer->hold = std::clamp(d.info.hold, 1, 99);
        const QRect r = d.rect & canvasRect();
        if (!layer->isGroup && !r.isEmpty() && r == d.rect) {
            layer->tex.uploadHalf(r, d.pixels);
            // The picture was saved as one rectangle; the empty stretches inside it need no memory.
            trim(layer->tex, r);
        }
        if (d.mask.size() == qsizetype(w) * h) {
            const QImage grey(reinterpret_cast<const uchar*>(d.mask.constData()), w, h, w, QImage::Format_Grayscale8);
            layer->mask = std::make_shared<gfx::Texture>();
            layer->mask->create(w, h, GL_RGBA16F);
            layer->mask->upload(grey);
            layer->maskEnabled = d.info.maskEnabled;
        }
        if (d.info.reference)
            m_referenceId = layer->id;
        if (d.hasText && !layer->isGroup)
            m_texts.insert(layer->stamp, d.text);
        m_layers.push_back(layer);
        ++m_nameCounter;
    }
    // Parents are stored as positions in the list. A group's members must sit
    // directly below its header; anything else is put at the top level.
    for (size_t i = 0; i < m_layers.size() && i < layers.size(); ++i) {
        const int pi = layers[i].parentIndex;
        if (pi > int(i) && pi < int(m_layers.size()) && m_layers[size_t(pi)]->isGroup)
            m_layers[i]->parent = m_layers[size_t(pi)]->id;
    }
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const int parent = m_layers[i]->parent;
        if (!parent)
            continue;
        bool contiguous = true;
        for (size_t k = i + 1; k < m_layers.size() && m_layers[k]->id != parent; ++k)
            contiguous = contiguous && isInside(*m_layers[k], parent);
        if (!contiguous)
            m_layers[i]->parent = 0;
    }
    bool anyPaintable = false;
    for (const LayerPtr& l : m_layers)
        anyPaintable = anyPaintable || !l->isGroup;
    if (!anyPaintable)
        m_layers.push_back(createLayer(nextLayerName()));
    const int idx = std::clamp(activeIndex, 0, int(m_layers.size()) - 1);
    m_activeId = m_layers[size_t(idx)]->id;
    damageAll();
    setModified(false);
    emit selectionChanged();
    emit documentChanged();
    emit layersChanged();
}

void CanvasEngine::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged(modified);
}

void CanvasEngine::setBackgroundColor(const QColor& c)
{
    if (c == m_background)
        return;
    const QColor before = m_background;
    auto apply = [this](const QColor& col) {
        m_background = col;
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    apply(c);
    auto cmd = std::make_unique<FnCommand>(tr("Background Colour"), [=] { apply(before); }, [=] { apply(c); });
    m_history.push(std::move(cmd));
    setModified(true);
}

void CanvasEngine::setBackgroundVisible(bool visible)
{
    if (visible == m_backgroundVisible)
        return;
    auto apply = [this](bool v) {
        m_backgroundVisible = v;
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    apply(visible);
    m_history.push(std::make_unique<FnCommand>(tr("Background Visibility"), [=] { apply(!visible); },
                                               [=] { apply(visible); }));
    setModified(true);
}

// ------------------------------------------------------------------ layers

namespace {
QColor greyOf(const QColor& c)
{
    const int g = qGray(c.rgb());
    return QColor(g, g, g);
}
} // namespace

LayerPtr CanvasEngine::find(int id) const
{
    for (const LayerPtr& l : m_layers) {
        if (l->id == id)
            return l;
    }
    return nullptr;
}

int CanvasEngine::indexOf(int id) const
{
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i]->id == id)
            return int(i);
    }
    return -1;
}

int CanvasEngine::depthOf(const Layer& l) const
{
    int depth = 0;
    for (int parent = l.parent; parent != 0 && depth < 64; ++depth) {
        const LayerPtr p = find(parent);
        if (!p)
            break;
        parent = p->parent;
    }
    return depth;
}

bool CanvasEngine::isInside(const Layer& l, int groupId) const
{
    int guard = 0;
    for (int parent = l.parent; parent != 0 && guard < 64; ++guard) {
        if (parent == groupId)
            return true;
        const LayerPtr p = find(parent);
        if (!p)
            break;
        parent = p->parent;
    }
    return false;
}

// A group's members sit directly below its header. Returns where the block
// that ends at `index` begins; for a plain layer that is the layer itself.
int CanvasEngine::blockStart(int index) const
{
    const Layer& head = *m_layers[size_t(index)];
    int start = index;
    if (head.isGroup) {
        while (start > 0 && isInside(*m_layers[size_t(start - 1)], head.id))
            --start;
    }
    return start;
}

LayerInfo CanvasEngine::infoOf(const Layer& l) const
{
    LayerInfo i;
    i.id = l.id;
    i.name = l.name;
    i.visible = l.visible;
    i.opacity = l.opacity;
    i.blend = l.blend;
    i.alphaLock = l.alphaLock;
    i.clipping = l.clipping;
    i.revision = l.revision;
    i.hasMask = bool(l.mask);
    i.maskEnabled = l.maskEnabled;
    i.maskRevision = l.maskRevision;
    i.isGroup = l.isGroup;
    i.parent = l.parent;
    i.depth = depthOf(l);
    i.collapsed = l.collapsed;
    i.reference = l.id == m_referenceId;
    i.isText = !l.isGroup && m_texts.contains(l.stamp);
    i.hold = l.hold;
    return i;
}

QVector<LayerInfo> CanvasEngine::layers() const
{
    QVector<LayerInfo> out;
    out.reserve(qsizetype(m_layers.size()));
    for (const LayerPtr& l : m_layers)
        out.push_back(infoOf(*l));
    return out;
}

LayerInfo CanvasEngine::layerInfo(int id) const
{
    const LayerPtr l = find(id);
    return l ? infoOf(*l) : LayerInfo{};
}

QString CanvasEngine::nextLayerName()
{
    return tr("Layer %1").arg(++m_nameCounter);
}

LayerPtr CanvasEngine::createLayer(const QString& name)
{
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextId++;
    layer->name = name;
    // A layer takes memory only where there is paint on it.
    layer->tex.create(m_size.width(), m_size.height(), GL_RGBA16F, false, true);
    layer->tex.clear();
    layer->stamp = nextStamp();
    return layer;
}

LayerPtr CanvasEngine::createGroup(const QString& name)
{
    auto group = std::make_shared<Layer>();
    group->id = m_nextId++;
    group->name = name;
    group->isGroup = true;
    return group;
}

LayerPtr CanvasEngine::cloneLayer(const Layer& src)
{
    LayerPtr copy = src.isGroup ? createGroup(src.name) : createLayer(src.name);
    copy->visible = src.visible;
    copy->opacity = src.opacity;
    copy->blend = src.blend;
    copy->alphaLock = src.alphaLock;
    copy->clipping = src.clipping;
    copy->collapsed = src.collapsed;
    copy->hold = src.hold;
    copy->maskEnabled = src.maskEnabled;
    copy->parent = src.parent;
    if (!src.isGroup)
        gfx::copyTexture(src.tex, copy->tex);
    // A copy of text is text as well.
    const auto text = m_texts.constFind(src.stamp);
    if (!src.isGroup && text != m_texts.constEnd())
        m_texts.insert(copy->stamp, text.value());
    if (src.mask) {
        copy->mask = std::make_shared<gfx::Texture>();
        copy->mask->create(m_size.width(), m_size.height(), GL_RGBA16F);
        gfx::copyTexture(*src.mask, *copy->mask);
    }
    return copy;
}

// Groups hold layers but no pixels of their own; their masks can be painted on, though.
bool CanvasEngine::paintable(const LayerPtr& layer, bool quiet)
{
    if (!layer)
        return false;
    if (layer->isGroup && m_maskEditing && layer->mask && layer->id == m_activeId)
        return true;
    if (layer->isGroup) {
        if (!quiet)
            emit message(tr("Pick a layer inside the group"));
        return false;
    }
    return true;
}

CanvasEngine::Structure CanvasEngine::captureStructure() const
{
    Structure s;
    s.order = m_layers;
    s.parents.reserve(m_layers.size());
    for (const LayerPtr& l : m_layers)
        s.parents.push_back(l->parent);
    s.active = m_activeId;
    return s;
}

void CanvasEngine::applyStructure(const Structure& s)
{
    m_layers = s.order;
    for (size_t i = 0; i < m_layers.size(); ++i)
        m_layers[i]->parent = s.parents[i];
    m_activeId = s.active;
    damageAll();
    emit layersChanged();
}

std::unique_ptr<Command> CanvasEngine::structureCommand(const QString& name, const Structure& before)
{
    const Structure after = captureStructure();
    return std::make_unique<FnCommand>(name, [=, this] { applyStructure(before); }, [=, this] { applyStructure(after); });
}

void CanvasEngine::pushStructure(const QString& name, const Structure& before)
{
    m_history.push(structureCommand(name, before));
    setModified(true);
    damageAll();
    emit layersChanged();
}

// Where a new layer goes: on top of the active layer, or as the top member of
// the active group.
int CanvasEngine::insertionPoint(int* parent) const
{
    const int i = indexOf(m_activeId);
    if (i < 0) {
        *parent = 0;
        return int(m_layers.size());
    }
    const Layer& active = *m_layers[size_t(i)];
    if (active.isGroup) {
        *parent = active.id;
        return i;
    }
    *parent = active.parent;
    return i + 1;
}

void CanvasEngine::setActiveLayer(int id)
{
    if (id == m_activeId || !find(id) || busy())
        return;
    const int frameWas = m_anim.enabled ? currentFrame() : -1;
    m_activeId = id;
    m_maskEditing = false;
    m_history.breakMerge();
    if (m_anim.enabled && currentFrame() != frameWas)
        damageAll();   // another frame comes into view
    emit layersChanged();
}

int CanvasEngine::addLayer()
{
    if (!hasDocument() || busy())
        return 0;
    Scope s(this);
    const Structure before = captureStructure();
    LayerPtr layer = createLayer(nextLayerName());
    const int index = insertionPoint(&layer->parent);
    m_layers.insert(m_layers.begin() + index, layer);
    m_activeId = layer->id;
    m_maskEditing = false;
    pushStructure(tr("Add Layer"), before);
    return layer->id;
}

int CanvasEngine::addGroup()
{
    if (!hasDocument() || busy())
        return 0;
    Scope s(this);
    const Structure before = captureStructure();
    int groups = 0;
    for (const LayerPtr& l : m_layers)
        groups += l->isGroup;
    LayerPtr group = createGroup(tr("Group %1").arg(groups + 1));
    const int index = insertionPoint(&group->parent);
    if (depthOf(*group) >= maxGroupDepth()) {
        emit message(tr("Groups can be nested %1 deep at most").arg(maxGroupDepth()));
        return 0;
    }
    m_layers.insert(m_layers.begin() + index, group);
    m_activeId = group->id;
    m_maskEditing = false;
    pushStructure(tr("New Group"), before);
    return group->id;
}

int CanvasEngine::addImageLayer(const QImage& image, const QString& name, const QPoint& topLeft, const QString& undoName)
{
    if (!hasDocument() || image.isNull() || busy())
        return 0;
    Scope s(this);
    QImage img = image;
    if (img.width() > m_size.width() || img.height() > m_size.height())
        img = img.scaled(m_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!makeRoom(qint64(img.width()) * img.height() * 8))
        return 0;
    const Structure before = captureStructure();
    LayerPtr layer = createLayer(name.isEmpty() ? nextLayerName() : name);
    QPoint at((m_size.width() - img.width()) / 2, (m_size.height() - img.height()) / 2);
    if (topLeft.x() != INT_MIN && img.size() == image.size()) {
        // Keep it where it came from, nudged back onto the canvas if need be.
        at.setX(std::clamp(topLeft.x(), 0, m_size.width() - img.width()));
        at.setY(std::clamp(topLeft.y(), 0, m_size.height() - img.height()));
    }
    layer->tex.upload(img, at);
    const int index = insertionPoint(&layer->parent);
    m_layers.insert(m_layers.begin() + index, layer);
    m_activeId = layer->id;
    m_maskEditing = false;
    pushStructure(undoName.isEmpty() ? tr("Insert Image") : undoName, before);
    return layer->id;
}

// ------------------------------------------------------------------- text

int CanvasEngine::addTextLayer(const TextInfo& info, const QString& name)
{
    QPoint at;
    const QImage image = text::render(info, m_size, &at);
    if (image.isNull())
        return 0;
    const int id = addImageLayer(image, name, at, tr("Add Text"));
    if (const LayerPtr layer = find(id)) {
        m_texts.insert(layer->stamp, info);
        emit layersChanged();
    }
    return id;
}

bool CanvasEngine::layerIsText(int id) const
{
    const LayerPtr layer = find(id);
    return layer && !layer->isGroup && m_texts.contains(layer->stamp);
}

TextInfo CanvasEngine::layerText(int id) const
{
    const LayerPtr layer = find(id);
    return layer ? m_texts.value(layer->stamp) : TextInfo();
}

bool CanvasEngine::setLayerText(int id, const TextInfo& info)
{
    const LayerPtr layer = find(id);
    if (!layer || layer->isGroup || busy())
        return false;
    QPoint at;
    const QImage image = text::render(info, m_size, &at);
    Scope s(this);
    auto cmd = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, tr("Edit Text"));
    layer->tex.clear();
    if (!image.isNull())
        layer->tex.upload(image, at);
    touch(*layer);
    m_texts.insert(layer->stamp, info);
    m_history.push(std::move(cmd));
    setModified(true);
    damageAll();
    emit layersChanged();
    return true;
}

int CanvasEngine::textLayerAt(const QPointF& canvasPos) const
{
    // What is on top is found first. A layer inside a hidden group is hidden too.
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        const Layer& l = **it;
        const auto text = m_texts.constFind(l.stamp);
        if (l.isGroup || text == m_texts.constEnd())
            continue;
        bool shown = l.visible;
        for (LayerPtr up = find(l.parent); up && shown; up = find(up->parent))
            shown = up->visible;
        if (shown && text::outline(text.value()).containsPoint(canvasPos, Qt::OddEvenFill))
            return l.id;
    }
    return 0;
}

void CanvasEngine::setTextEditLayer(int id)
{
    if (m_textEditId == id)
        return;
    m_textEditId = id;
    Scope s(this);
    damageAll();
}

void CanvasEngine::duplicateLayer(int id)
{
    const int i = indexOf(id);
    if (i < 0 || busy())
        return;
    Scope s(this);
    const Structure before = captureStructure();
    // A group is duplicated with everything in it; the copies point at the
    // copied group rather than the original.
    const int start = blockStart(i);
    std::vector<LayerPtr> copies;
    QHash<int, int> newIds;
    qint64 wanted = 0;
    for (int k = start; k <= i; ++k)
        wanted += m_layers[size_t(k)]->tex.bytes() + (m_layers[size_t(k)]->mask ? m_layers[size_t(k)]->mask->bytes() : 0);
    if (!makeRoom(wanted))
        return;
    for (int k = start; k <= i; ++k) {
        LayerPtr copy = cloneLayer(*m_layers[size_t(k)]);
        newIds.insert(m_layers[size_t(k)]->id, copy->id);
        copies.push_back(copy);
    }
    for (const LayerPtr& copy : copies)
        copy->parent = newIds.value(copy->parent, copy->parent);
    m_layers.insert(m_layers.begin() + i + 1, copies.begin(), copies.end());
    m_activeId = copies.back()->id;
    m_maskEditing = false;
    pushStructure(m_layers[size_t(i)]->isGroup ? tr("Duplicate Group") : tr("Duplicate Layer"), before);
}

void CanvasEngine::removeLayer(int id)
{
    const int i = indexOf(id);
    if (i < 0 || busy())
        return;
    const int start = blockStart(i);
    int remaining = 0;
    for (int k = 0; k < int(m_layers.size()); ++k)
        remaining += (k < start || k > i) && !m_layers[size_t(k)]->isGroup;
    if (remaining == 0) {
        emit message(tr("A canvas needs at least one layer"));
        return;
    }
    Scope s(this);
    const Structure before = captureStructure();
    const bool wasGroup = m_layers[size_t(i)]->isGroup;
    const bool activeGone = indexOf(m_activeId) >= start && indexOf(m_activeId) <= i;
    m_layers.erase(m_layers.begin() + start, m_layers.begin() + i + 1);
    if (activeGone)
        m_activeId = m_layers[size_t(std::clamp(start - 1, 0, int(m_layers.size()) - 1))]->id;
    m_maskEditing = false;
    pushStructure(wasGroup ? tr("Delete Group") : tr("Delete Layer"), before);
}

void CanvasEngine::moveLayer(int id, int parentId, int aboveId)
{
    const int i = indexOf(id);
    if (i < 0 || busy())
        return;
    const LayerPtr root = m_layers[size_t(i)];
    const LayerPtr parent = parentId ? find(parentId) : nullptr;
    const LayerPtr above = aboveId ? find(aboveId) : nullptr;
    // Not into itself, not next to something inside itself, and only into groups.
    if (parentId && (!parent || !parent->isGroup || parentId == id || isInside(*parent, id)))
        return;
    if (aboveId && (!above || aboveId == id || isInside(*above, id) || above->parent != parentId))
        return;
    const int start = blockStart(i);
    const int newDepth = parent ? depthOf(*parent) + 1 : 0;
    for (int k = start; k <= i; ++k) {
        const Layer& member = *m_layers[size_t(k)];
        if (member.isGroup && newDepth + depthOf(member) - depthOf(*root) >= maxGroupDepth()) {
            emit message(tr("Groups can be nested %1 deep at most").arg(maxGroupDepth()));
            return;
        }
    }

    Scope s(this);
    const Structure before = captureStructure();
    const std::vector<LayerPtr> block(m_layers.begin() + start, m_layers.begin() + i + 1);
    m_layers.erase(m_layers.begin() + start, m_layers.begin() + i + 1);
    root->parent = parentId;
    int at = int(m_layers.size());              // top of the whole stack
    if (above)
        at = blockStart(indexOf(aboveId));      // directly below that sibling and all it contains
    else if (parent)
        at = indexOf(parentId);                 // top member: directly below the group's header
    m_layers.insert(m_layers.begin() + at, block.begin(), block.end());

    if (m_layers == before.order && root->parent == before.parents[size_t(i)])
        return;   // dropped where it already was
    pushStructure(root->isGroup ? tr("Move Group") : tr("Move Layer"), before);
}

int CanvasEngine::groupLayer(int id)
{
    const int i = indexOf(id);
    if (i < 0 || busy())
        return 0;
    const LayerPtr root = m_layers[size_t(i)];
    for (int k = blockStart(i); k <= i; ++k) {
        const Layer& member = *m_layers[size_t(k)];
        if ((member.isGroup ? depthOf(member) + 1 : depthOf(*root)) >= maxGroupDepth()) {
            emit message(tr("Groups can be nested %1 deep at most").arg(maxGroupDepth()));
            return 0;
        }
    }
    Scope s(this);
    const Structure before = captureStructure();
    int groups = 0;
    for (const LayerPtr& l : m_layers)
        groups += l->isGroup;
    LayerPtr group = createGroup(tr("Group %1").arg(groups + 1));
    group->parent = root->parent;
    root->parent = group->id;
    m_layers.insert(m_layers.begin() + i + 1, group);
    pushStructure(tr("Group"), before);
    return group->id;
}

void CanvasEngine::ungroupLayer(int groupId)
{
    const int i = indexOf(groupId);
    if (i < 0 || !m_layers[size_t(i)]->isGroup || busy())
        return;
    Scope s(this);
    const Structure before = captureStructure();
    const LayerPtr group = m_layers[size_t(i)];
    for (const LayerPtr& l : m_layers) {
        if (l->parent == groupId)
            l->parent = group->parent;
    }
    m_layers.erase(m_layers.begin() + i);
    if (m_activeId == groupId && !m_layers.empty())
        m_activeId = m_layers[size_t(std::clamp(i - 1, 0, int(m_layers.size()) - 1))]->id;
    pushStructure(tr("Ungroup"), before);
}

void CanvasEngine::flattenGroup(int groupId)
{
    const int i = indexOf(groupId);
    if (i < 0 || !m_layers[size_t(i)]->isGroup || busy())
        return;
    Scope s(this);
    const Structure before = captureStructure();
    const LayerPtr group = m_layers[size_t(i)];
    LayerPtr flat = createLayer(group->name);
    flat->visible = group->visible;
    flat->opacity = group->opacity;
    flat->blend = group->blend;
    flat->parent = group->parent;
    // What the group looks like on its own; its opacity, blend mode and mask
    // carry over to the new layer instead of being baked in.
    trim(flat->tex, compositeGroupInto(groupId, flat->tex));
    flat->mask = group->mask;
    flat->maskEnabled = group->maskEnabled;
    const int start = blockStart(i);
    m_layers.erase(m_layers.begin() + start, m_layers.begin() + i + 1);
    m_layers.insert(m_layers.begin() + start, flat);
    m_activeId = flat->id;
    m_maskEditing = false;
    pushStructure(tr("Flatten Group"), before);
}

void CanvasEngine::setLayerCollapsed(int id, bool collapsed)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->isGroup || layer->collapsed == collapsed)
        return;
    layer->collapsed = collapsed;   // how the panel looks, not part of the artwork: no undo step
    emit layersChanged();
}

void CanvasEngine::applyProps(Layer& l, const LayerInfo& p)
{
    l.name = p.name;
    l.visible = p.visible;
    l.opacity = p.opacity;
    l.blend = p.blend;
    l.alphaLock = p.alphaLock;
    l.clipping = p.clipping;
    l.hold = p.hold;
    damageAll();
    emit layersChanged();
}

void CanvasEngine::changeProps(int id, const std::function<void(LayerInfo&)>& edit, const QString& undoName,
                               const QString& mergeKind)
{
    const LayerPtr layer = find(id);
    if (!layer || m_stroke.active)
        return;
    const LayerInfo before = infoOf(*layer);
    LayerInfo after = before;
    edit(after);
    Scope s(this);
    applyProps(*layer, after);
    m_history.push(std::make_unique<PropsCommand>(this, layer, before, after, undoName, mergeKind));
    setModified(true);
}

void CanvasEngine::setLayerName(int id, const QString& name)
{
    changeProps(id, [&](LayerInfo& i) { i.name = name; }, tr("Rename Layer"), {});
}

void CanvasEngine::setLayerVisible(int id, bool visible)
{
    changeProps(id, [&](LayerInfo& i) { i.visible = visible; }, tr("Layer Visibility"), {});
}

void CanvasEngine::setLayerOpacity(int id, float opacity)
{
    changeProps(id, [&](LayerInfo& i) { i.opacity = std::clamp(opacity, 0.0f, 1.0f); }, tr("Layer Opacity"),
                QStringLiteral("opacity"));
}

void CanvasEngine::setLayerBlend(int id, BlendMode mode)
{
    changeProps(id, [&](LayerInfo& i) { i.blend = mode; }, tr("Blend Mode"), QStringLiteral("blend"));
}

// ------------------------------------------------------------- animation

std::vector<std::vector<const Layer*>> CanvasEngine::frameParts() const
{
    std::vector<std::vector<const Layer*>> frames;
    for (const LayerPtr& lp : m_layers) {
        if (lp->parent != 0)
            continue;
        if (lp->clipping && !lp->isGroup && !frames.empty())
            frames.back().push_back(lp.get());
        else
            frames.push_back({lp.get()});
    }
    return frames;
}

QVector<int> CanvasEngine::frames() const
{
    QVector<int> ids;
    for (const auto& parts : frameParts())
        ids.push_back(parts.front()->id);
    return ids;
}

int CanvasEngine::frameOf(int layerId) const
{
    LayerPtr l = find(layerId);
    while (l && l->parent != 0)
        l = find(l->parent);
    if (!l)
        return -1;
    const auto frames = frameParts();
    for (size_t k = 0; k < frames.size(); ++k) {
        for (const Layer* part : frames[k]) {
            if (part == l.get())
                return int(k);
        }
    }
    return -1;
}

void CanvasEngine::animationEntries(const std::function<void(const Layer*, bool, float, const float*)>& add) const
{
    // Earlier frames go red, later ones green, as most animation software has it.
    static const float before[4] = {0.93f, 0.18f, 0.20f, 0.85f}, after[4] = {0.15f, 0.72f, 0.30f, 0.85f};
    const auto frames = frameParts();
    const int n = int(frames.size());
    if (n == 0)
        return;
    const int current = std::clamp(m_playFrame >= 0 ? m_playFrame : currentFrame(), 0, n - 1);
    auto frame = [&](int k, float times, const float* tint) {
        const Layer* base = frames[size_t(k)].front();
        if (!base->visible || base->opacity <= 0.0f || base->id == m_textEditId)
            return;
        add(base, false, base->opacity * times, tint);
        for (size_t j = 1; j < frames[size_t(k)].size(); ++j) {
            const Layer* c = frames[size_t(k)][j];
            if (c->visible && c->opacity > 0.0f && c->id != m_textEditId)
                add(c, true, c->opacity * base->opacity * times, tint);
        }
    };
    const bool background = m_anim.background && current != 0;
    const bool foreground = m_anim.foreground && n > 1 && current != n - 1;
    if (background)
        frame(0, 1.0f, nullptr);
    if (m_playFrame < 0) {
        const int reach = std::clamp(m_anim.onionFrames, 0, 12);
        for (int d = reach; d >= 1; --d) {
            // The nearest at the opacity chosen, each one further out fainter.
            const float times = std::clamp(m_anim.onionOpacity, 0.0f, 1.0f) * float(reach + 1 - d) / float(reach);
            for (const int k : {current - d, current + d}) {
                if (k < 0 || k >= n || (background && k == 0) || (foreground && k == n - 1))
                    continue;
                frame(k, times, m_anim.tintOnion ? (k < current ? before : after) : nullptr);
            }
        }
    }
    frame(current, 1.0f, nullptr);
    if (foreground)
        frame(n - 1, 1.0f, nullptr);
}

void CanvasEngine::setAnimation(const Animation& animation)
{
    Animation a = animation;
    a.fps = std::clamp(a.fps, 1, 60);
    a.onionFrames = std::clamp(a.onionFrames, 0, 12);
    a.onionOpacity = std::clamp(a.onionOpacity, 0.0f, 1.0f);
    if (a == m_anim)
        return;
    m_anim = a;
    if (!a.enabled)
        m_playFrame = -1;
    damageAll();
    setModified(true);
    emit animationChanged();
}

void CanvasEngine::showFrame(int index)
{
    const auto frames = frameParts();
    if (index < 0 || index >= int(frames.size()))
        return;
    // A group's frame is worked on in its topmost layer.
    const Layer* base = frames[size_t(index)].front();
    int id = base->id;
    if (base->isGroup) {
        for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
            const Layer& l = **it;
            if (l.isGroup)
                continue;
            LayerPtr up = find(l.parent);
            while (up && up->id != base->id)
                up = find(up->parent);
            if (up) {
                id = l.id;
                break;
            }
        }
    }
    setActiveLayer(id);
}

void CanvasEngine::setPlaybackFrame(int index)
{
    if (index == m_playFrame)
        return;
    m_playFrame = index;
    if (m_anim.enabled)
        damageAll();
}

void CanvasEngine::setFrameHold(int id, int frames)
{
    changeProps(id, [&](LayerInfo& i) { i.hold = std::clamp(frames, 1, 99); }, tr("Frame Hold"), {});
}

int CanvasEngine::addFrame()
{
    if (!hasDocument() || busy())
        return 0;
    Scope s(this);
    const Structure before = captureStructure();
    LayerPtr layer = createLayer(nextLayerName());
    layer->parent = 0;
    // Straight after the current frame and whatever is clipped to it, or on top when there is none.
    size_t at = m_layers.size();
    const int current = currentFrame();
    const auto frames = frameParts();
    if (current >= 0 && current + 1 < int(frames.size()))
        at = size_t(indexOf(frames[size_t(current) + 1].front()->id));
    m_layers.insert(m_layers.begin() + std::ptrdiff_t(at), layer);
    m_activeId = layer->id;
    m_maskEditing = false;
    pushStructure(tr("Add Frame"), before);
    if (m_anim.enabled)
        damageAll();
    return layer->id;
}

QImage CanvasEngine::renderFrame(int index)
{
    if (!hasDocument())
        return {};
    Scope s(this);
    const int was = m_playFrame;
    const bool wasEnabled = m_anim.enabled;
    m_anim.enabled = true;
    m_playFrame = std::max(0, index);
    damageAll();
    prepareFrame(false);
    const QImage image = m_result.readback(canvasRect());
    m_anim.enabled = wasEnabled;
    m_playFrame = was;
    damageAll();
    return image;
}

QImage CanvasEngine::frameThumbnail(int index, int maxDim)
{
    const auto frames = frameParts();
    if (index < 0 || index >= int(frames.size()))
        return {};
    const Layer* base = frames[size_t(index)].front();
    if (!base->isGroup && frames[size_t(index)].size() == 1)
        return layerThumbnail(base->id, maxDim);
    // A group, or a layer with others clipped to it: the frame as it plays, without the paper.
    const bool paper = m_backgroundVisible;
    const Animation keep = m_anim;
    m_backgroundVisible = false;
    m_anim.background = m_anim.foreground = false;
    QImage image = renderFrame(index);
    m_backgroundVisible = paper;
    m_anim = keep;
    damageAll();
    return image.scaled(QSize(maxDim, maxDim), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void CanvasEngine::setLayerAlphaLock(int id, bool locked)
{
    changeProps(id, [&](LayerInfo& i) { i.alphaLock = locked; }, tr("Alpha Lock"), {});
}

void CanvasEngine::setLayerClipping(int id, bool clipping)
{
    changeProps(id, [&](LayerInfo& i) { i.clipping = clipping; }, tr("Clipping Mask"), {});
}

void CanvasEngine::clearLayer(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || busy() || !paintable(layer))
        return;
    Scope s(this);
    const bool onMask = m_maskEditing && layer->mask && id == m_activeId;
    gfx::Texture& target = layer->target(onMask);
    const QRect chosen = hasSelection() ? m_selBounds : canvasRect();
    if (onMask) {
        // Clearing a mask reveals: paint it white, through the selection if there is one.
        coatLayer(layer, true, chosen, Qt::white, false, true, tr("Clear Mask"));
    } else if (hasSelection()) {
        // Erase through the selection mask instead of wiping the whole layer.
        coatLayer(layer, false, chosen & target.heldBounds(), Qt::black, true, false, tr("Clear Selection"));
    } else {
        auto cmd = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, tr("Clear Layer"));
        layer->tex.clear();
        touch(*layer);
        m_history.push(std::move(cmd));
        setModified(true);
    }
    damageAll();
    emit layersChanged();
}

void CanvasEngine::coatLayer(const LayerPtr& layer, bool onMask, const QRect& reach, const QColor& ink, bool erase, bool alphaLock,
                             const QString& undoName)
{
    gfx::Texture& target = layer->target(onMask);
    if (reach.isEmpty())
        return;
    // The same road a stroke takes: the live copy starts out as the layer,
    // takes the coat where it reaches, and the tiles it reached go back.
    gfx::copyTexture(target, m_live);
    std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), uint8_t(0));
    markTiles(reach);
    const float flat[4] = {float(ink.redF()), float(ink.greenF()), float(ink.blueF()), 1.0f};
    mergeStroke(&target, m_strokeBuf, m_live, reach, nullptr, 1.0f, erase, alphaLock, flat, true);
    commitLiveTiles(layer, m_live, dirtyTileList(), undoName, onMask);
    if (erase && !onMask)
        trim(layer->tex, reach);
}

void CanvasEngine::trim(gfx::Texture& tex, const QRect& within)
{
    const QRect r = within & tex.rect();
    if (!tex.sparse() || tex.heldPages() == 0 || r.isEmpty())
        return;
    // A texel per block; usually the canvas's own count of blocks, but a
    // canvas being given a new size has layers of that size before it does.
    if (tex.pagesX() != m_occupancy.width() || tex.pagesY() != m_occupancy.height()) {
        m_occupancy.create(tex.pagesX(), tex.pagesY(), GL_R8);
        m_heldMap.create(tex.pagesX(), tex.pagesY(), GL_R8);
        m_heldMap.setFilter(GL_NEAREST, GL_NEAREST);
    }
    const QRect blocks(QPoint(r.left() / gfx::kPage, r.top() / gfx::kPage), QPoint(r.right() / gfx::kPage, r.bottom() / gfx::kPage));
    // Looked at on the graphics card, a block to a texel; only the answers come back.
    m_heldMap.upload(QImage(tex.pages().data(), tex.pagesX(), tex.pagesY(), tex.pagesX(), QImage::Format_Grayscale8));
    fullscreenTo(m_occupancy, &blocks);
    m_occupancyProg.use();
    tex.bind(0);
    m_heldMap.bind(1);
    m_occupancyProg.set("uSrc", 0);
    m_occupancyProg.set("uHeld", 1);
    m_occupancyProg.set("uPage", gfx::kPage);
    m_fs.draw();
    gl()->glDisable(GL_SCISSOR_TEST);
    const QImage found = m_occupancy.readbackGrey(blocks);
    for (int y = 0; y < blocks.height(); ++y) {
        const uchar* row = found.constScanLine(y);
        for (int x = 0; x < blocks.width(); ++x) {
            const int px = blocks.x() + x, py = blocks.y() + y;
            if (!row[x] && tex.held(px, py))
                tex.release(tex.pageRect(px, py));
        }
    }
}

void CanvasEngine::takeFrom(gfx::Texture& source, gfx::Texture& tex, const QRect& reach)
{
    // Outside the reach the new version is empty, whatever the source holds
    // there: nothing is copied from it and the layer keeps no memory for it.
    // Inside it the copy brings memory along only where the source has some.
    const QRect kept = tex.keepOnly(reach);
    if (!kept.isEmpty())
        gfx::copyTexture(source, tex, kept, kept.topLeft());
    trim(tex, kept);
}

QRect CanvasEngine::compositeGroupInto(int groupId, gfx::Texture& target)
{
    // Only where one of its members has anything, a band at a time.
    QRect reach;
    for (const LayerPtr& l : m_layers) {
        if (!l->isGroup && isInside(*l, groupId))
            reach |= l->tex.heldBounds();
    }
    target.clearRect(target.rect(), !target.sparse());
    gl()->glDisable(GL_BLEND);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int y = reach.top(); y <= reach.bottom(); y += kBand) {
        for (int x = reach.left(); x <= reach.right(); x += kBand) {
            const QRect band(x, y, std::min(kBand, reach.right() + 1 - x), std::min(kBand, reach.bottom() + 1 - y));
            if (const gfx::Texture* result = compositeLevel(groupId, 1, band, clear))
                gfx::copyTexture(*result, target, QRect(QPoint(0, 0), band.size()), band.topLeft());
        }
    }
    gl()->glDisable(GL_SCISSOR_TEST);
    return reach;
}

QByteArray CanvasEngine::groupPixels(int groupId, QRect* rect)
{
    const LayerPtr group = find(groupId);
    if (!group || !group->isGroup)
        return {};
    Scope s(this);
    const QRect reach = compositeGroupInto(groupId, m_scratch[1]);
    QByteArray pixels = reach.isEmpty() ? QByteArray() : m_scratch[1].readbackHalf(reach);
    releaseTemps();
    if (rect)
        *rect = reach;
    return pixels;
}

void CanvasEngine::fillLayer(int id, const QColor& color)
{
    const LayerPtr layer = find(id);
    if (!layer || busy() || !paintable(layer))
        return;
    Scope s(this);
    const bool onMask = m_maskEditing && layer->mask && id == m_activeId;
    const QColor ink = onMask ? greyOf(color) : color;
    // Like a stroke of the one colour, so that alpha lock and the selection
    // are honoured. With alpha lock it can only land where there is paint.
    const bool locked = onMask || layer->alphaLock;
    QRect reach = hasSelection() ? m_selBounds : canvasRect();
    if (layer->alphaLock && !onMask)
        reach &= layer->tex.heldBounds();
    // The paint itself and its live copy.
    if (!makeRoom(qint64(reach.width()) * reach.height() * 16))
        return;
    coatLayer(layer, onMask, reach, ink, false, locked, onMask ? tr("Fill Mask") : tr("Fill Layer"));
    damageAll();
    emit layersChanged();
}

void CanvasEngine::flipTexture(gfx::Texture& tex, bool horizontal)
{
    // Where the picture ends up: the part of the canvas it is on, turned over.
    const QRect from = tex.heldBounds();
    if (from.isEmpty())
        return;
    const QRect reach = horizontal ? QRect(m_size.width() - from.right() - 1, from.y(), from.width(), from.height())
                                   : QRect(from.x(), m_size.height() - from.bottom() - 1, from.width(), from.height());
    const QRect area = m_live.holdOnly(reach);
    fullscreenTo(m_live, &area);
    m_flipProg.use();
    tex.bind(0);
    m_flipProg.set("uSrc", 0);
    m_flipProg.set("uHorizontal", horizontal ? 1 : 0);
    m_fs.draw();
    gl()->glDisable(GL_SCISSOR_TEST);
    takeFrom(m_live, tex, reach);
}

void CanvasEngine::flipLayer(int id, bool horizontal)
{
    const LayerPtr layer = find(id);
    if (!layer || busy() || !paintable(layer) || layer->isGroup)
        return;
    // Flipping is its own inverse, so undo costs no memory.
    const auto other = std::make_shared<quint64>(nextStamp());
    auto flip = [=, this] {
        flipTexture(layer->tex, horizontal);
        ++layer->revision;
        std::swap(layer->stamp, *other);
        if (layer->mask) {
            flipTexture(*layer->mask, horizontal);
            ++layer->maskRevision;
        }
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    flip();
    m_history.push(std::make_unique<FnCommand>(horizontal ? tr("Flip Horizontal") : tr("Flip Vertical"), flip, flip));
    setModified(true);
}

void CanvasEngine::flipCanvas(bool horizontal)
{
    if (!hasDocument() || busy())
        return;
    const std::vector<LayerPtr> all = m_layers;
    const auto others = std::make_shared<std::vector<quint64>>();
    for (size_t i = 0; i < all.size(); ++i)
        others->push_back(nextStamp());
    auto flip = [=, this] {
        for (size_t i = 0; i < all.size(); ++i) {
            const LayerPtr& l = all[i];
            if (!l->isGroup) {
                flipTexture(l->tex, horizontal);
                ++l->revision;
                std::swap(l->stamp, (*others)[i]);
            }
            if (l->mask) {
                flipTexture(*l->mask, horizontal);
                ++l->maskRevision;
            }
        }
        if (hasSelection()) {
            m_selImage = m_selImage.mirrored(horizontal, !horizontal);
            selectionEdited();
        }
        // The guides were set up for the picture, and turn over with it.
        for (QPointF& vp : m_guides.vp) {
            if (horizontal)
                vp.setX(m_size.width() - vp.x());
            else
                vp.setY(m_size.height() - vp.y());
        }
        if (horizontal)
            m_guides.symOffset.setX(-m_guides.symOffset.x());
        else
            m_guides.symOffset.setY(-m_guides.symOffset.y());
        m_guides.symAngle = -m_guides.symAngle;
        std::swap(m_guides.isoLeft, m_guides.isoRight);
        emit guidesChanged();
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    flip();
    m_history.push(std::make_unique<FnCommand>(horizontal ? tr("Flip Canvas Horizontally") : tr("Flip Canvas Vertically"),
                                               flip, flip));
    setModified(true);
}

// One layer blended onto a backdrop, written to whatever target is bound.
void CanvasEngine::drawCompositePass(const CompositePass& p)
{
    m_compositeProg.use();
    m_compositeProg.set("uBackdrop", 0);
    m_compositeProg.set("uSrc", 1);
    m_compositeProg.set("uClip", 2);
    m_compositeProg.set("uMask", 3);
    m_compositeProg.set("uClipMask", 4);
    // Unused samplers still need something harmless bound that is not the target.
    (p.backdrop ? p.backdrop : p.src)->bind(0);
    p.src->bind(1);
    (p.clip ? p.clip : p.src)->bind(2);
    (p.mask ? p.mask : p.src)->bind(3);
    (p.clipMask ? p.clipMask : p.src)->bind(4);
    m_compositeProg.set("uBackdropIsColor", p.backdrop ? 0 : 1);
    m_compositeProg.set("uBackdropColor", p.colour[0], p.colour[1], p.colour[2], p.colour[3]);
    m_compositeProg.set("uHasClip", p.clip ? 1 : 0);
    m_compositeProg.set("uHasMask", p.mask ? 1 : 0);
    m_compositeProg.set("uHasClipMask", p.clip && p.clipMask ? 1 : 0);
    m_compositeProg.set("uOpacity", p.opacity);
    m_compositeProg.set("uMode", p.mode);
    if (p.tint)
        m_compositeProg.set("uTint", p.tint[0], p.tint[1], p.tint[2], p.tint[3]);
    else
        m_compositeProg.set("uTint", 0.0f, 0.0f, 0.0f, 0.0f);
    // A fragment's place in the target, and from there in each texture.
    const QPoint target = p.intoBand ? m_bandOrigin : QPoint(0, 0);
    auto place = [&](const char* name, const gfx::Texture* tex) {
        const QPoint at = target - (isBand(tex) ? m_bandOrigin : QPoint(0, 0));
        m_compositeProg.setInts(name, at.x(), at.y());
    };
    place("uBackdropAt", p.backdrop);
    place("uSrcAt", p.src);
    place("uClipAt", p.clip);
    place("uMaskAt", p.mask);
    place("uClipMaskAt", p.clipMask);
    m_fs.draw();
}

bool CanvasEngine::isBand(const gfx::Texture* tex) const
{
    if (!tex)
        return false;
    for (const auto& b : m_bandBufs) {
        if (b.get() == tex)
            return true;
    }
    return false;
}

void CanvasEngine::mergeDown(int id)
{
    const int i = indexOf(id);
    if (i <= 0 || busy())
        return;
    const LayerPtr upper = m_layers[size_t(i)];
    const LayerPtr lower = m_layers[size_t(i - 1)];
    if (upper->isGroup || lower->isGroup || upper->parent != lower->parent) {
        emit message(tr("Merge Down needs a plain layer directly underneath"));
        return;
    }
    Scope s(this);
    if (!makeRoom(lower->tex.bytes() + upper->tex.bytes() * 2))
        return;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const gfx::Texture* lowerMask = lower->mask && lower->maskEnabled ? lower->mask.get() : nullptr;
    const gfx::Texture* upperMask = upper->mask && upper->maskEnabled ? upper->mask.get() : nullptr;

    // The merged layer has no mask, so the lower one's is baked in first.
    const gfx::Texture* backdrop = &lower->tex;
    // Nothing can come of it where neither layer has anything.
    const QRect reach = lower->tex.heldBounds() | upper->tex.heldBounds();
    const QRect area = m_live.holdOnly(reach);
    if (lowerMask) {
        m_scratch[0].holdOnly(reach);
        fullscreenTo(m_scratch[0], &area);
        drawCompositePass({nullptr, clear, &lower->tex, 1.0f, 0, lowerMask, nullptr, nullptr});
        backdrop = &m_scratch[0];
    }
    fullscreenTo(m_live, &area);
    drawCompositePass({backdrop, clear, &upper->tex, upper->opacity, int(upper->blend), upperMask,
                       upper->clipping ? backdrop : nullptr, nullptr});
    gl()->glDisable(GL_SCISSOR_TEST);

    auto compound = std::make_unique<CompoundCommand>(tr("Merge Down"));
    auto pixels = std::make_unique<PixelCommand>(this, lower, std::vector<QPoint>{}, QString());
    takeFrom(m_live, lower->tex, reach);
    releaseTemps();
    touch(*lower);
    compound->add(std::move(pixels));
    if (lowerMask)
        maskCommand(lower, lower->mask, nullptr, compound.get(), QString());
    const Structure before = captureStructure();
    m_layers.erase(m_layers.begin() + i);
    m_activeId = lower->id;
    m_maskEditing = false;
    compound->add(structureCommand(QString(), before));
    m_history.push(std::move(compound));
    setModified(true);
    damageAll();
    emit layersChanged();
}

QImage CanvasEngine::layerThumbnail(int id, int maxDim, bool mask)
{
    const LayerPtr layer = find(id);
    if (!layer || (mask ? !layer->mask : layer->isGroup))
        return {};
    Scope s(this);
    const bool live = m_overlay == layer.get() && m_overlayMask == mask;
    const gfx::Texture& src = live ? m_live : layer->target(mask);
    const QSize target = m_size.scaled(maxDim, maxDim, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    return downscale(src, canvasRect(), target);
}

std::array<QVector<float>, 4> CanvasEngine::layerHistogram(int id)
{
    std::array<QVector<float>, 4> out;
    for (QVector<float>& channel : out)
        channel.fill(0.0f, 256);
    const LayerPtr layer = find(id);
    if (!layer || layer->isGroup)
        return out;
    Scope s(this);
    const QSize target = m_size.scaled(640, 640, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const QImage small = downscale(layer->tex, canvasRect(), target).convertToFormat(QImage::Format_ARGB32);
    double counts[4][256] = {};
    for (int y = 0; y < small.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(small.constScanLine(y));
        for (int x = 0; x < small.width(); ++x) {
            const int a = qAlpha(row[x]);
            if (a < 8)
                continue;
            const double weight = a / 255.0;
            const int r = qRed(row[x]), g = qGreen(row[x]), b = qBlue(row[x]);
            counts[0][std::clamp(qRound(0.299 * r + 0.587 * g + 0.114 * b), 0, 255)] += weight;
            counts[1][r] += weight;
            counts[2][g] += weight;
            counts[3][b] += weight;
        }
    }
    for (int c = 0; c < 4; ++c) {
        double most = 0.0;
        for (double n : counts[c])
            most = std::max(most, n);
        for (int i = 0; i < 256 && most > 0.0; ++i)
            out[size_t(c)][i] = float(counts[c][i] / most);
    }
    return out;
}

QByteArray CanvasEngine::layerPixels(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || layer->isGroup)
        return {};
    Scope s(this);
    return layer->tex.readbackHalf(canvasRect());
}

QByteArray CanvasEngine::layerPixels(int id, const QRect& region)
{
    const LayerPtr layer = find(id);
    const QRect r = region & canvasRect();
    if (!layer || layer->isGroup || r.isEmpty())
        return {};
    Scope s(this);
    return layer->tex.readbackHalf(r);
}

QRect CanvasEngine::layerHeldBounds(int id) const
{
    const LayerPtr layer = find(id);
    return layer && !layer->isGroup ? layer->tex.heldBounds() : QRect();
}

int CanvasEngine::layerHeldBlocks(int id) const
{
    const LayerPtr layer = find(id);
    return layer && !layer->isGroup ? layer->tex.heldPages() : 0;
}

qint64 CanvasEngine::memoryInUse() const
{
    return gfx::textureBytes() + s_arrayBytes;
}

bool CanvasEngine::sparseLayers()
{
    return s_sparse;
}

bool CanvasEngine::leanBuffers(qint64 pixels)
{
    if (!s_sparse)
        return false;
    if (s_leanFrom >= 0)
        return pixels >= s_leanFrom;
    // From where keeping them whole would take an eighth of what there is to use.
    return pixels * kStrokeBytesPerPixel > qint64(double(s_memoryBytes) * kMemoryShare) / 8;
}

bool CanvasEngine::nvidiaMemoryInfo() const
{
    return m_ctx && m_renderer.contains(QLatin1String("NVIDIA")) && m_ctx->hasExtension(QByteArrayLiteral("GL_NVX_gpu_memory_info"));
}

qint64 CanvasEngine::memoryForSpeed() const
{
    if (!m_live.sparse())
        return m_scratch[0].bytes() + m_scratch[1].bytes();
    return m_live.bytes() + m_strokeBuf.color.bytes() + m_strokeBuf.coverage.bytes() + m_scratch[0].bytes() + m_scratch[1].bytes();
}

qint64 CanvasEngine::memoryBudget() const
{
    if (s_budgetOverride >= 0)
        return s_budgetOverride;
    qint64 budget = qint64(double(s_memoryBytes) * kMemoryShare);
    if (m_ctx && nvidiaMemoryInfo()) {
        // What is free right now, counting what this program already holds
        // as its own: another program may have taken a good part of the card.
        GLint kilobytes = 0;
        gl()->glGetIntegerv(0x9049 /* GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX */, &kilobytes);
        if (kilobytes > 0)
            budget = std::min(budget, memoryInUse() + qint64(kilobytes) * 1024 - (qint64(384) << 20));
    }
    return budget;
}

bool CanvasEngine::makeRoom(qint64 wanted)
{
    const qint64 budget = memoryBudget();
    // First what is only kept for speed: scratch space, and the live copy's
    // memory for places the next stroke may never go.
    if (memoryInUse() + wanted > budget) {
        releaseTemps();
        if (!busy() && m_live.sparse()) {
            m_live.releaseAll();
            m_liveCopy = {};
            m_strokeBuf.color.releaseAll();
            m_strokeBuf.coverage.releaseAll();
        }
    }
    bool forgot = false;
    while (memoryInUse() + wanted > budget && m_history.dropOldest())
        forgot = true;
    if (forgot)
        emit message(tr("Graphics memory is running short: the oldest undo steps were let go"));
    if (memoryInUse() + wanted <= budget)
        return true;
    emit message(tr("Graphics memory is full. Merging or deleting layers makes room."));
    return false;
}

void CanvasEngine::releaseTemps()
{
    m_scratch[0].releaseAll();
    m_scratch[1].releaseAll();
}

QByteArray CanvasEngine::layerMaskGrey(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->mask)
        return {};
    Scope s(this);
    const QImage img = layer->mask->readback(canvasRect());
    QByteArray grey(qsizetype(img.width()) * img.height(), Qt::Uninitialized);
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        char* out = grey.data() + qsizetype(y) * img.width();
        for (int x = 0; x < img.width(); ++x)
            out[x] = char(row[x * 4]);
    }
    return grey;
}

QRect CanvasEngine::layerContentBounds(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || layer->isGroup)
        return {};
    Scope s(this);
    // Only the part of the canvas the layer has anything on needs looking at.
    const QRect held = layer->tex.heldBounds();
    if (held.isEmpty())
        return {};
    const QImage img = layer->tex.readback(held);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            if (row[x * 4 + 3]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1)).translated(held.topLeft());
}

QRect CanvasEngine::editableBounds(int id)
{
    QRect bounds = layerContentBounds(id);
    if (hasSelection())
        bounds &= m_selBounds;
    return bounds & canvasRect();
}

// ------------------------------------------------------------------- masks

bool CanvasEngine::maskEditing() const
{
    const LayerPtr layer = activeLayer();
    return m_maskEditing && layer && layer->mask;
}

void CanvasEngine::setMaskEditing(bool on)
{
    if (busy())
        return;
    const LayerPtr layer = activeLayer();
    on = on && layer && layer->mask;
    if (m_maskEditing == on)
        return;
    m_maskEditing = on;
    if (on)
        emit message(tr("Painting on the mask: black hides, white reveals"));
    emit layersChanged();
}

// Swaps a layer's mask for another one, or for none, as an undoable step.
// The command keeps the mask it replaced alive.
void CanvasEngine::maskCommand(const LayerPtr& layer, std::shared_ptr<gfx::Texture> before,
                               std::shared_ptr<gfx::Texture> after, CompoundCommand* into, const QString& name)
{
    auto apply = [this, layer](const std::shared_ptr<gfx::Texture>& mask) {
        layer->mask = mask;
        ++layer->maskRevision;
        damageAll();
        emit layersChanged();
    };
    apply(after);
    auto cmd = std::make_unique<FnCommand>(name, [=] { apply(before); }, [=] { apply(after); });
    if (into) {
        into->add(std::move(cmd));
    } else {
        m_history.push(std::move(cmd));
        setModified(true);
    }
}

void CanvasEngine::addLayerMask(int id)
{
    const LayerPtr layer = find(id);
    // A group can have a mask too: it hides part of everything in the group.
    if (!layer || layer->mask || busy())
        return;
    Scope s(this);
    auto mask = std::make_shared<gfx::Texture>();
    mask->create(m_size.width(), m_size.height(), GL_RGBA16F);
    if (hasSelection()) {
        // Start from the selection: what is selected shows, the rest is
        // hidden. As a grey picture the selection is that mask already.
        mask->upload(m_selImage);
    } else {
        mask->clear(1.0f, 1.0f, 1.0f, 1.0f);
    }
    layer->maskEnabled = true;
    maskCommand(layer, nullptr, mask, nullptr, tr("Add Mask"));
    m_activeId = id;
    m_maskEditing = true;
    emit layersChanged();
}

void CanvasEngine::removeLayerMask(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->mask || busy())
        return;
    Scope s(this);
    maskCommand(layer, layer->mask, nullptr, nullptr, tr("Delete Mask"));
    m_maskEditing = false;
}

void CanvasEngine::applyLayerMask(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->mask || layer->isGroup || busy())
        return;   // a group has no pixels of its own to bake a mask into
    Scope s(this);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const QRect reach = layer->tex.heldBounds();
    const QRect area = m_live.holdOnly(reach);
    if (!area.isEmpty()) {
        fullscreenTo(m_live, &area);
        drawCompositePass({nullptr, clear, &layer->tex, 1.0f, 0, layer->mask.get(), nullptr, nullptr});
        gl()->glDisable(GL_SCISSOR_TEST);
    }
    auto compound = std::make_unique<CompoundCommand>(tr("Apply Mask"));
    auto pixels = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, QString());
    takeFrom(m_live, layer->tex, reach);
    touch(*layer);
    compound->add(std::move(pixels));
    maskCommand(layer, layer->mask, nullptr, compound.get(), QString());
    m_history.push(std::move(compound));
    m_maskEditing = false;
    setModified(true);
}

void CanvasEngine::invertLayerMask(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->mask || busy())
        return;
    // Inverting twice gives the original back, so undo costs no memory.
    auto invert = [=, this] {
        if (!layer->mask)
            return;
        fullscreenTo(m_live);
        m_invertProg.use();
        layer->mask->bind(0);
        m_invertProg.set("uSrc", 0);
        m_fs.draw();
        gfx::copyTexture(m_live, *layer->mask);
        m_live.releaseAll();   // a mask is the whole canvas; no need to go on holding that much
        ++layer->maskRevision;
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    invert();
    m_history.push(std::make_unique<FnCommand>(tr("Invert Mask"), invert, invert));
    setModified(true);
}

void CanvasEngine::setLayerMaskEnabled(int id, bool enabled)
{
    const LayerPtr layer = find(id);
    if (!layer || !layer->mask || layer->maskEnabled == enabled || m_stroke.active)
        return;
    auto apply = [this, layer](bool on) {
        layer->maskEnabled = on;
        ++layer->maskRevision;
        damageAll();
        emit layersChanged();
    };
    Scope s(this);
    apply(enabled);
    m_history.push(std::make_unique<FnCommand>(enabled ? tr("Enable Mask") : tr("Disable Mask"), [=] { apply(!enabled); },
                                               [=] { apply(enabled); }));
    setModified(true);
}

// --------------------------------------------------------------- selection

namespace {

// Smallest rectangle holding every non-zero byte of an 8-bit image.
QRect nonZeroBounds(const QImage& img)
{
    const int w = img.width(), h = img.height();
    int x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const uchar* row = img.constScanLine(y);
        int a = 0;
        // Empty rows are the common case: skip them eight bytes at a time.
        for (; a + 8 <= w; a += 8) {
            quint64 word;
            memcpy(&word, row + a, 8);
            if (word)
                break;
        }
        while (a < w && !row[a])
            ++a;
        if (a == w)
            continue;
        int b = w - 1;
        while (!row[b])
            --b;
        x0 = std::min(x0, a);
        x1 = std::max(x1, b);
        y0 = std::min(y0, y);
        y1 = y;
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

// `b` joined to, cut out of, or put in place of `a`; all 0..255 coverage.
inline uchar combineCoverage(int a, int b, SelectOp op)
{
    switch (op) {
    case SelectOp::Add:
        return uchar(a + b - (a * b + 127) / 255);
    case SelectOp::Subtract:
        return uchar((a * (255 - b) + 127) / 255);
    case SelectOp::Replace:
        break;
    }
    return uchar(b);
}

} // namespace

class SelectionCommand : public Command {
public:
    SelectionCommand(CanvasEngine* e, CanvasEngine::SelSnapPtr before, CanvasEngine::SelSnapPtr after, const QString& n)
        : m_e(e), m_before(std::move(before)), m_after(std::move(after))
    {
        name = n;
        m_bytes = m_before->pixels.sizeInBytes() + m_after->pixels.sizeInBytes();
    }
    void undo() override { m_e->restoreSelection(m_before); }
    void redo() override { m_e->restoreSelection(m_after); }
    qint64 bytes() const override { return m_bytes; }
    void setAfter(CanvasEngine::SelSnapPtr after) { m_after = std::move(after); }

private:
    CanvasEngine* m_e;
    CanvasEngine::SelSnapPtr m_before, m_after;
    qint64 m_bytes = 0;
};

float CanvasEngine::selectionAt(const QPoint& canvasPos) const
{
    if (!m_selBounds.contains(canvasPos))
        return 0.0f;
    return float(m_selImage.constScanLine(canvasPos.y())[canvasPos.x()]) / 255.0f;
}

CanvasEngine::SelSnapPtr CanvasEngine::snapshotSelection() const
{
    auto snap = std::make_shared<SelSnap>();
    snap->bounds = m_selBounds;
    if (!m_selBounds.isEmpty())
        snap->pixels = m_selImage.copy(m_selBounds);
    return snap;
}

QImage CanvasEngine::selectionPixels() const
{
    return m_selBounds.isEmpty() ? QImage() : m_selImage.copy(m_selBounds);
}

void CanvasEngine::setSelectionPixels(const QRect& bounds, const QImage& pixels)
{
    if (!hasDocument())
        return;
    Scope s(this);
    auto snap = std::make_shared<SelSnap>();
    if (!bounds.isEmpty() && canvasRect().contains(bounds) && pixels.size() == bounds.size()) {
        snap->bounds = bounds;
        snap->pixels = pixels.convertToFormat(QImage::Format_Grayscale8);
    }
    loadSelection(snap);
    selectionEdited();
}

bool CanvasEngine::sameSelection(const SelSnapPtr& snap) const
{
    return snap->bounds == m_selBounds && (m_selBounds.isEmpty() || snap->pixels == m_selImage.copy(m_selBounds));
}

void CanvasEngine::loadSelection(const SelSnapPtr& snap)
{
    m_selImage.fill(0);
    if (!snap || snap->bounds.isEmpty())
        return;
    const QRect& b = snap->bounds;
    for (int y = 0; y < b.height(); ++y)
        memcpy(m_selImage.scanLine(b.top() + y) + b.left(), snap->pixels.constScanLine(y), size_t(b.width()));
}

void CanvasEngine::restoreSelection(const SelSnapPtr& snap)
{
    loadSelection(snap);
    selectionEdited();
}

// m_selImage has been changed: bring everything that follows from it up to date.
void CanvasEngine::selectionEdited()
{
    m_selBounds = nonZeroBounds(m_selImage);
    m_selMask.upload(m_selImage);
    emit selectionChanged();
    emit damaged();   // nothing to recomposite, but the outline has to be redrawn
}

void CanvasEngine::pushSelection(const SelSnapPtr& before, const QString& name, CompoundCommand* into)
{
    auto cmd = std::make_unique<SelectionCommand>(this, before, snapshotSelection(), name);
    if (into)
        into->add(std::move(cmd));
    else
        m_history.push(std::move(cmd));
}

void CanvasEngine::applyPath(const QPainterPath& path, SelectOp op, const QString& undoName)
{
    if (!hasDocument() || busy())
        return;
    const QRectF extent = path.boundingRect();
    const QRect box = extent.toAlignedRect() & canvasRect();
    // Slivers thinner than a pixel are not worth keeping.
    const bool nothing = path.isEmpty() || extent.width() < 1.0 || extent.height() < 1.0 || box.isEmpty();
    if (nothing && (op != SelectOp::Replace || !hasSelection()))
        return;
    Scope s(this);
    const SelSnapPtr before = snapshotSelection();
    if (op == SelectOp::Replace)
        m_selImage.fill(0);
    if (!nothing) {
        QImage shape(box.size(), QImage::Format_Grayscale8);
        shape.fill(0);
        QPainter p(&shape);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-box.topLeft());
        p.fillPath(path, Qt::white);
        p.end();
        for (int y = 0; y < box.height(); ++y) {
            uchar* dst = m_selImage.scanLine(box.top() + y) + box.left();
            const uchar* src = shape.constScanLine(y);
            for (int x = 0; x < box.width(); ++x)
                dst[x] = combineCoverage(dst[x], src[x], op);
        }
    }
    selectionEdited();
    if (!sameSelection(before))
        pushSelection(before, undoName);
}

void CanvasEngine::setSelection(const QPainterPath& path, const QString& undoName)
{
    applyPath(path, SelectOp::Replace, undoName);
}

void CanvasEngine::selectPath(const QPainterPath& path, SelectOp op)
{
    applyPath(path, op,
              op == SelectOp::Subtract ? tr("Remove from Selection") : op == SelectOp::Add ? tr("Add to Selection") : tr("Select"));
}

void CanvasEngine::selectAll()
{
    if (!hasDocument() || busy() || m_selBounds == canvasRect())
        return;
    Scope s(this);
    const SelSnapPtr before = snapshotSelection();
    m_selImage.fill(255);
    selectionEdited();
    pushSelection(before, tr("Select All"));
}

void CanvasEngine::clearSelection()
{
    if (!hasSelection() || busy())
        return;
    Scope s(this);
    const SelSnapPtr before = snapshotSelection();
    m_selImage.fill(0);
    selectionEdited();
    pushSelection(before, tr("Deselect"));
}

void CanvasEngine::invertSelection()
{
    if (!hasSelection()) {
        selectAll();
        return;
    }
    if (busy())
        return;
    Scope s(this);
    const SelSnapPtr before = snapshotSelection();
    m_selImage.invertPixels();
    selectionEdited();
    pushSelection(before, tr("Invert Selection"));
}

void CanvasEngine::selectLayerContents(int id)
{
    const LayerPtr layer = find(id);
    if (!layer || busy() || !paintable(layer) || layer->isGroup)
        return;
    Scope s(this);
    const QImage pixels = layer->tex.readback(canvasRect());
    QImage hidden;
    if (layer->mask && layer->maskEnabled)
        hidden = layer->mask->readback(canvasRect());
    const SelSnapPtr before = snapshotSelection();
    for (int y = 0; y < m_size.height(); ++y) {
        uchar* dst = m_selImage.scanLine(y);
        const uchar* src = pixels.constScanLine(y);
        const uchar* mask = hidden.isNull() ? nullptr : hidden.constScanLine(y);
        for (int x = 0; x < m_size.width(); ++x) {
            const int a = src[x * 4 + 3];
            dst[x] = uchar(mask ? (a * mask[x * 4] + 127) / 255 : a);
        }
    }
    selectionEdited();
    if (!hasSelection())
        emit message(tr("There is nothing on that layer to select"));
    if (!sameSelection(before))
        pushSelection(before, tr("Select Layer Contents"));
}

// The blur runs on the GPU: the selection as it was goes up once more, is
// blurred along each axis, and the result is read back.
void CanvasEngine::featherSelection(float radius)
{
    if (!hasDocument() || busy())
        return;
    radius = std::clamp(radius, 0.0f, 250.0f);
    const bool continuing = m_feather.base && m_feather.command && m_feather.serial == m_history.serial();
    if (!continuing) {
        if (!hasSelection() || radius <= 0.0f)
            return;
        m_feather.base = snapshotSelection();
    }
    Scope s(this);
    loadSelection(m_feather.base);
    if (radius < 0.25f) {
        m_selMask.upload(m_selImage);
    } else {
        m_selTmp.upload(m_selImage);
        m_blurProg.use();
        m_blurProg.set("uSrc", 0);
        m_blurProg.set("uRadius", radius);
        // The soft edge reaches no further than the radius beyond the selection.
        const int margin = int(std::ceil(radius)) + 3;
        const QRect reach = m_selBounds.adjusted(-margin, -margin, margin, margin) & canvasRect();
        const QRect area = m_scratch[0].holdOnly(reach);
        fullscreenTo(m_scratch[0], &area);
        m_selTmp.bind(0);
        m_blurProg.set("uDir", 1.0f / float(m_size.width()), 0.0f);
        m_fs.draw();
        fullscreenTo(m_selMask);
        m_scratch[0].bind(0);
        m_blurProg.set("uDir", 0.0f, 1.0f / float(m_size.height()));
        m_fs.draw();
        m_selImage = m_selMask.readbackGrey(canvasRect());
    }
    m_selBounds = nonZeroBounds(m_selImage);
    emit selectionChanged();
    emit damaged();

    if (continuing) {
        m_feather.command->setAfter(snapshotSelection());
    } else {
        auto cmd = std::make_unique<SelectionCommand>(this, m_feather.base, snapshotSelection(), tr("Feather Selection"));
        m_feather.command = cmd.get();
        m_history.push(std::move(cmd));
        // As long as the history has not moved on, that command is still its
        // newest entry and later calls may keep adjusting it.
        m_feather.serial = m_history.serial();
    }
}

QImage CanvasEngine::copyPixels(QPoint* origin)
{
    const LayerPtr layer = activeLayer();
    if (!layer || busy() || !paintable(layer, true) || layer->isGroup)
        return {};
    const QRect bounds = editableBounds(layer->id);
    if (bounds.isEmpty())
        return {};
    Scope s(this);
    QImage img = layer->tex.readback(bounds).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (hasSelection()) {
        // Scale by how much of each pixel is selected.
        for (int y = 0; y < img.height(); ++y) {
            uchar* row = img.scanLine(y);
            const uchar* cover = m_selImage.constScanLine(bounds.top() + y) + bounds.left();
            for (int x = 0; x < img.width(); ++x) {
                for (int c = 0; c < 4; ++c)
                    row[x * 4 + c] = uchar((row[x * 4 + c] * cover[x] + 127) / 255);
            }
        }
    }
    if (origin)
        *origin = bounds.topLeft();
    return img;
}

// ------------------------------------------------------------------ flood

int CanvasEngine::referenceLayerId() const
{
    const LayerPtr layer = find(m_referenceId);
    return layer ? layer->id : 0;
}

void CanvasEngine::setReferenceLayer(int id)
{
    const LayerPtr layer = find(id);
    // A group may be the reference: then it is everything in it, put together, that fills stop at.
    if (busy() || (id != 0 && !layer) || referenceLayerId() == id)
        return;
    const int before = m_referenceId;
    auto apply = [this](int which) {
        m_referenceId = which;
        emit layersChanged();
    };
    apply(id);
    m_history.push(std::make_unique<FnCommand>(id ? tr("Set Reference Layer") : tr("Clear Reference Layer"),
                                               [=] { apply(before); }, [=] { apply(id); }));
    setModified(true);
    if (id)
        emit message(tr("Reference: fills on other layers now stop at this layer's lines"));
}

bool CanvasEngine::beginFlood(FloodKind kind, const QPoint& canvasPos, float tolerance, const QColor& color, SelectOp op)
{
    if (!hasDocument() || busy() || !canvasRect().contains(canvasPos))
        return false;
    const LayerPtr layer = activeLayer();
    if (!layer)
        return false;
    LayerPtr reference = find(referenceLayerId());
    if (reference == layer || (reference && !reference->visible))
        reference = nullptr;
    if (kind == FloodKind::Fill) {
        if (!paintable(layer))   // a group passes when its mask is being painted on
            return false;
        if (!layer->visible) {
            emit message(tr("The selected layer is hidden"));
            return false;
        }
    } else if (layer->isGroup && !reference) {
        emit message(tr("Pick a layer inside the group"));
        return false;
    }
    Scope s(this);
    // A fill can turn out to cover the canvas; at least a good piece of it has to fit.
    if (kind == FloodKind::Fill && !makeRoom(std::min(layerBytes() * 2, qint64(512) << 20)))
        return false;
    m_flood = {};
    m_flood.kind = kind;
    m_flood.layer = layer;
    m_flood.onMask = m_maskEditing && layer->mask;
    m_flood.seed = canvasPos;
    m_flood.color = m_flood.onMask ? greyOf(color) : color;
    m_flood.op = op;
    // The GPU measures every pixel against the seed; two bytes per pixel come back.
    if (!m_floodDist.valid())
        m_floodDist.create(m_size.width(), m_size.height(), GL_RG8);
    const gfx::Texture* own = layer->isGroup && !m_flood.onMask ? nullptr : &layer->target(m_flood.onMask);
    const gfx::Texture* ref = reference ? &reference->tex : nullptr;
    if (reference && reference->isGroup) {
        // A group as the reference: what it shows, put together, without
        // the layer being filled if that is one of its own.
        m_leftOutId = layer->id;
        compositeGroupInto(reference->id, m_scratch[1]);
        m_leftOutId = 0;
        ref = &m_scratch[1];
    }
    fullscreenTo(m_floodDist);
    m_floodDistProg.use();
    (own ? own : ref)->bind(0);
    (ref ? ref : own)->bind(1);
    m_floodDistProg.set("uOwn", 0);
    m_floodDistProg.set("uRef", 1);
    m_floodDistProg.set("uSeed", float(canvasPos.x()), float(canvasPos.y()));
    m_floodDistProg.set("uHasOwn", own ? 1 : 0);
    m_floodDistProg.set("uHasRef", ref ? 1 : 0);
    m_fs.draw();
    m_flood.apart = m_floodDist.readbackRG(canvasRect());
    m_flood.hasRef = ref != nullptr;
    releaseTemps();
    m_flood.cover.assign(size_t(m_size.width()) * size_t(m_size.height()), 0);
    m_flood.active = true;
    if (kind == FloodKind::Select) {
        m_flood.selBefore = snapshotSelection();
    } else {
        // The fill is shown through the live copy of the layer, like a stroke.
        gfx::copyTexture(layer->target(m_flood.onMask), m_live);
        m_overlay = layer.get();
        m_overlayMask = m_flood.onMask;
    }
    setFloodTolerance(tolerance);
    return true;
}

void CanvasEngine::setFloodTolerance(float tolerance)
{
    if (!m_flood.active)
        return;
    Scope s(this);
    m_flood.tolerance = std::clamp(tolerance, 0.0f, 1.0f);
    const QRect previous = m_flood.bounds;
    const int w = m_size.width();
    for (int y = previous.top(); y <= previous.bottom(); ++y)
        memset(m_flood.cover.data() + size_t(y) * w + previous.left(), 0, size_t(previous.width()));
    m_flood.bounds = floodRegion();
    showFlood(previous);
}

// Grows the region outwards from the seed. A pixel belongs while neither the
// layer nor the reference differs from what it is at the seed by more than the
// tolerance. Returns the bounds of what m_flood.cover now holds.
QRect CanvasEngine::floodRegion()
{
    const int w = m_size.width(), h = m_size.height();
    const bool hasRef = m_flood.hasRef;
    const auto* apart = reinterpret_cast<const uint8_t*>(m_flood.apart.constData());
    const int limit = std::clamp(int(std::lround(m_flood.tolerance * 255.0f)), 0, 254);
    uint8_t* mask = m_flood.cover.data();
    auto own = [&](size_t i) { return int(apart[i * 2]); };
    auto diff = [&](size_t i) { return int(std::max(apart[i * 2], apart[i * 2 + 1])); };
    auto open = [&](size_t i) { return !mask[i] && diff(i) <= limit; };

    // Scanline fill: take a whole run of a row at once, then look along the
    // rows above and below it for runs to continue with.
    std::vector<QPoint> stack{m_flood.seed};
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    while (!stack.empty()) {
        const QPoint p = stack.back();
        stack.pop_back();
        const int y = p.y();
        const size_t row = size_t(y) * w;
        if (!open(row + p.x()))
            continue;
        int left = p.x(), right = p.x();
        while (left > 0 && open(row + left - 1))
            --left;
        while (right + 1 < w && open(row + right + 1))
            ++right;
        memset(mask + row + left, 255, size_t(right - left + 1));
        x0 = std::min(x0, left);
        x1 = std::max(x1, right);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            const size_t other = size_t(ny) * w;
            bool inRun = false;
            for (int x = left; x <= right; ++x) {
                const bool free = open(other + x);
                if (free && !inRun)
                    stack.emplace_back(x, ny);
                inRun = free;
            }
        }
    }
    if (x1 < 0)
        return {};

    // The rim: pixels just outside that touch the region.
    //  - Stopped by the layer's own colours, they get partial cover the nearer
    //    they are to the limit, which closes the pale gap a hard edge leaves
    //    against antialiased line work.
    //  - Stopped only by the reference, they are the edge of a line drawn on
    //    another layer. The fill carries on underneath it for a few pixels, so
    //    no fringe of background shows between colour and line.
    const QRect core(QPoint(x0, y0), QPoint(x1, y1));
    const QRect ring = core.adjusted(-1, -1, 1, 1) & canvasRect();
    const float span = float(255 - limit);
    std::vector<std::pair<size_t, uint8_t>> rim;
    std::vector<QPoint> frontier;
    for (int y = ring.top(); y <= ring.bottom(); ++y) {
        const size_t row = size_t(y) * w;
        for (int x = ring.left(); x <= ring.right(); ++x) {
            const size_t i = row + x;
            if (mask[i] == 255)
                continue;
            const bool touches = (x > 0 && mask[i - 1] == 255) || (x + 1 < w && mask[i + 1] == 255)
                                 || (y > 0 && mask[i - w] == 255) || (y + 1 < h && mask[i + w] == 255);
            if (!touches)
                continue;
            if (hasRef && own(i) <= limit) {
                rim.emplace_back(i, uint8_t(255));
                frontier.emplace_back(x, y);
            } else {
                const float cover = std::clamp(1.0f - float(diff(i) - limit) / span, 0.0f, 1.0f);
                rim.emplace_back(i, uint8_t(cover * 254.0f));
            }
        }
    }
    for (const auto& [i, cover] : rim)
        mask[i] = cover;
    x0 = ring.left(), y0 = ring.top(), x1 = ring.right(), y1 = ring.bottom();
    for (int round = 0; round < kFloodUnderLine - 1 && !frontier.empty(); ++round) {
        std::vector<QPoint> next;
        for (const QPoint& p : frontier) {
            for (const QPoint& step : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                const int x = p.x() + step.x(), y = p.y() + step.y();
                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                const size_t i = size_t(y) * w + x;
                // Only further into the line: never out the other side of it.
                if (mask[i] || own(i) > limit || diff(i) <= limit)
                    continue;
                mask[i] = 255;
                next.emplace_back(x, y);
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
        frontier.swap(next);
    }
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

// Puts the region on screen: as paint on the live copy of the layer, or as
// the selection it would make.
void CanvasEngine::showFlood(const QRect& previous)
{
    const int w = m_size.width();
    const QRect bounds = m_flood.bounds;
    if (m_flood.kind == FloodKind::Select) {
        if (m_flood.op == SelectOp::Replace)
            m_selImage.fill(0);
        else
            loadSelection(m_flood.selBefore);
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
            uchar* dst = m_selImage.scanLine(y) + bounds.left();
            const uint8_t* cover = m_flood.cover.data() + size_t(y) * w + bounds.left();
            for (int x = 0; x < bounds.width(); ++x)
                dst[x] = combineCoverage(dst[x], cover[x], m_flood.op);
        }
        selectionEdited();
        return;
    }

    // Hand the region to the stroke merge, so alpha lock and the selection apply.
    Layer& layer = *m_flood.layer;
    gfx::Texture& target = layer.target(m_flood.onMask);
    if (!previous.isEmpty())
        gfx::copyTexture(target, m_live, previous, previous.topLeft());
    // A flat coat of the colour, let through where the region is.
    const QColor ink = m_flood.color;
    if (!bounds.isEmpty()) {
        const QImage cover(m_flood.cover.data() + size_t(bounds.top()) * w + bounds.left(), bounds.width(), bounds.height(), w,
                           QImage::Format_Grayscale8);
        m_strokeBuf.coverage.upload(cover, bounds.topLeft());
        const float flat[4] = {float(ink.redF()), float(ink.greenF()), float(ink.blueF()), 1.0f};
        mergeStroke(&target, m_strokeBuf, m_live, bounds, nullptr, 1.0f, false, m_flood.onMask || layer.alphaLock, flat, false);
    }
    m_flood.shown |= bounds;
    damage(previous | bounds);
}

void CanvasEngine::endFlood()
{
    if (!m_flood.active)
        return;
    Scope s(this);
    const FloodState done = std::move(m_flood);
    m_flood = {};
    if (done.kind == FloodKind::Select) {
        if (!sameSelection(done.selBefore))
            pushSelection(done.selBefore, done.op == SelectOp::Subtract ? tr("Remove from Selection") : tr("Automatic Selection"));
        return;
    }
    if (!done.bounds.isEmpty()) {
        std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), uint8_t(0));
        markTiles(done.bounds);
        commitLiveTiles(done.layer, m_live, dirtyTileList(), done.onMask ? tr("Fill Mask") : tr("Fill"), done.onMask);
        std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), uint8_t(0));
    }
    m_strokeBuf.clear();
    m_overlay = nullptr;
    m_overlayMask = false;
    damage(done.shown);
    emit layersChanged();
}

void CanvasEngine::cancelFlood()
{
    if (!m_flood.active)
        return;
    Scope s(this);
    const FloodState dropped = std::move(m_flood);
    m_flood = {};
    if (dropped.kind == FloodKind::Select) {
        restoreSelection(dropped.selBefore);
        return;
    }
    m_strokeBuf.clear();
    m_overlay = nullptr;
    m_overlayMask = false;
    damage(dropped.shown);
}

bool CanvasEngine::floodFill(const QPoint& canvasPos, const QColor& color, float tolerance)
{
    if (!beginFlood(FloodKind::Fill, canvasPos, tolerance, color))
        return false;
    const bool filled = !m_flood.bounds.isEmpty();
    endFlood();
    return filled;
}

bool CanvasEngine::floodSelect(const QPoint& canvasPos, float tolerance, SelectOp op)
{
    if (!beginFlood(FloodKind::Select, canvasPos, tolerance, QColor(), op))
        return false;
    endFlood();
    return true;
}

// ----------------------------------------------------------------- strokes

QRect CanvasEngine::tileRect(const QPoint& tile) const
{
    const int x = tile.x() * kTile, y = tile.y() * kTile;
    return QRect(x, y, std::min(kTile, m_size.width() - x), std::min(kTile, m_size.height() - y));
}

void CanvasEngine::markTiles(const QRect& rect)
{
    const QRect r = rect & canvasRect();
    if (r.isEmpty())
        return;
    for (int ty = r.top() / kTile; ty <= r.bottom() / kTile; ++ty) {
        for (int tx = r.left() / kTile; tx <= r.right() / kTile; ++tx)
            m_dirtyTiles[size_t(ty) * size_t(m_tilesX) + size_t(tx)] = 1;
    }
}

std::vector<QPoint> CanvasEngine::dirtyTileList() const
{
    std::vector<QPoint> tiles;
    for (int ty = 0; ty < m_tilesY; ++ty) {
        for (int tx = 0; tx < m_tilesX; ++tx) {
            if (m_dirtyTiles[size_t(ty) * size_t(m_tilesX) + size_t(tx)])
                tiles.emplace_back(tx, ty);
        }
    }
    return tiles;
}

void CanvasEngine::fullscreenTo(gfx::Texture& target, const QRect* scissor)
{
    gfx::GLF* f = gl();
    target.bindAsTarget();
    f->glDisable(GL_BLEND);
    if (scissor) {
        f->glEnable(GL_SCISSOR_TEST);
        f->glScissor(scissor->x(), scissor->y(), scissor->width(), scissor->height());
    } else {
        f->glDisable(GL_SCISSOR_TEST);
    }
}

void CanvasEngine::mergeStroke(const gfx::Texture* layerTex, StrokeBuffer& stroke, gfx::Texture& target,
                               const QRect& rect, const BrushPreset* preset, float opacity, bool erase,
                               bool alphaLock, const float* flat, bool fullCoverage)
{
    // The result is the layer with the stroke on it: there is something only
    // where one of the two has something. Memory for exactly that, not for
    // the whole rectangle, which for a large circle is mostly empty middle.
    const bool wasAuto = target.autoCommit();
    if (target.sparse()) {
        target.setAutoCommit(false);
        if (layerTex)
            target.ensureLike(*layerTex, rect);
        if (!erase && !alphaLock) {
            if (flat && fullCoverage)
                target.ensure(rect);
            else
                target.ensureLike(flat ? stroke.coverage : stroke.color, rect);
        }
    }
    fullscreenTo(target, &rect);
    m_mergeProg.use();
    m_mergeProg.set("uFlatColorOn", flat ? 1 : 0);
    if (flat)
        m_mergeProg.set("uFlatColor", flat[0], flat[1], flat[2], flat[3]);
    m_mergeProg.set("uFullCoverage", fullCoverage ? 1 : 0);
    (layerTex ? *layerTex : stroke.color).bind(0);
    stroke.color.bind(1);
    stroke.color.bind(2);
    stroke.coverage.bind(3);
    m_mergeProg.set("uLayer", 0);
    m_mergeProg.set("uStroke", 1);
    m_mergeProg.set("uCoverage", 3);
    // Previews (no layer) ignore the selection.
    const gfx::Texture* mask = layerTex ? selectionMask() : nullptr;
    (mask ? *mask : stroke.color).bind(4);
    m_mergeProg.set("uSelection", 4);
    m_mergeProg.set("uHasLayer", layerTex ? 1 : 0);
    m_mergeProg.set("uHasSelection", mask ? 1 : 0);
    m_mergeProg.set("uOpacity", opacity);
    m_mergeProg.set("uErase", erase ? 1 : 0);
    m_mergeProg.set("uAlphaLock", alphaLock ? 1 : 0);
    m_mergeProg.set("uWetEdges", preset ? std::clamp(preset->wetEdges, 0.0f, 1.0f) : 0.0f);
    m_mergeProg.set("uBlend", preset && !erase ? int(preset->blend) : 0);
    if (preset) {
        m_dabs.bindGrain(m_mergeProg, *preset, 2, false);
    } else {
        m_mergeProg.set("uGrain", 2);
        m_mergeProg.set("uGrainMode", 0);
    }
    m_fs.draw();
    gl()->glDisable(GL_SCISSOR_TEST);
    target.setAutoCommit(wasAuto);
}

bool CanvasEngine::beginStroke(StrokeTool tool, const BrushPreset& preset, float size, float opacity,
                               const QColor& color, const StrokeSample& sample, quint32 seed)
{
    if (!hasDocument() || busy())
        return false;
    const LayerPtr layer = activeLayer();
    if (!layer || !paintable(layer))
        return false;
    if (!layer->visible) {
        emit message(tr("The selected layer is hidden"));
        return false;
    }
    Scope s(this);
    // A stroke needs room for its buffer and the tiles it changes, twice over for undo.
    if (!makeRoom(qint64(32) << 20))
        return false;
    m_stroke = {};
    m_stroke.active = true;
    m_stroke.layer = layer;
    m_stroke.onMask = m_maskEditing && layer->mask;
    m_stroke.preset = preset;
    m_stroke.opacity = std::clamp(opacity, 0.0f, 1.0f);
    m_stroke.size = size;
    m_stroke.seed = seed ? seed : (QRandomGenerator::global()->generate() | 1u);
    QColor ink = color;
    if (m_stroke.onMask) {
        // A mask only has lightness, and erasing on it means revealing:
        // the eraser paints white.
        if (tool == StrokeTool::Eraser) {
            tool = StrokeTool::Brush;
            ink = Qt::white;
        } else {
            ink = greyOf(color);
        }
    }
    m_stroke.tool = tool;
    m_stroke.color = ink;
    m_stroke.smear = tool == StrokeTool::Smudge || (tool == StrokeTool::Brush && preset.wetMix > 0.0f);

    // On a large canvas copying the layer is most of what starting a stroke
    // costs; after a stroke on the same layer the copy is still there.
    // (Layers only: a stamp names one state of one layer's pixels and no
    // other, which a mask's count of changes does not.)
    const bool current = !m_stroke.onMask && !m_liveCopy.onMask && m_liveCopy.layer == layer.get() && m_liveCopy.version == layer->stamp
                         && m_liveCopy.written == m_live.written();
    // Going from layer to layer and back, the copy keeps the memory it has:
    // handing it out again block by block is what would take the time.
    if (!current)
        gfx::copyTexture(strokeTarget(), m_live, canvasRect(), QPoint(0, 0), true);
    m_liveCopy = {};
    releaseTemps();
    if (!m_stroke.smear)
        m_strokeBuf.clear();
    m_overlay = layer.get();
    m_overlayMask = m_stroke.onMask;
    std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), uint8_t(0));
    m_history.breakMerge();

    m_strokeEngine.setSpacingOverride(m_stroke.smear ? std::clamp(preset.spacing * 0.5f, 0.02f, 0.07f) : 0.0f);
    m_strokeEngine.setSeed(m_stroke.seed);
    m_strokeEngine.setTaperEnabled(tool != StrokeTool::Smudge);
    m_strokeEngine.begin(preset, size, ink, sample);
    processDabs();
    if (m_journalOn && !m_replaying)
        m_journalPending = {tool, size, m_stroke.opacity, color, m_stroke.seed, {sample}};
    return true;
}

void CanvasEngine::setStrokeJournal(bool on)
{
    m_journalOn = on;
    if (!on)
        m_journal.clear();
}

// Wipes the layer back to its ground and draws every recorded stroke again
// with `preset`. Nothing of this is undoable: the pad has no history to keep.
void CanvasEngine::redrawJournal(const BrushPreset& preset)
{
    const LayerPtr layer = activeLayer();
    if (!hasDocument() || busy() || !layer || layer->isGroup)
        return;
    {
        Scope s(this);
        layer->tex.clear();
        if (!m_journalGround.isNull())
            layer->tex.upload(m_journalGround.copy(QRect(QPoint(0, 0), m_journalGround.size().boundedTo(m_size))));
        touch(*layer);
        damageAll();
    }
    m_replaying = true;
    for (const JournalStroke& stroke : m_journal) {
        if (stroke.samples.empty())
            continue;
        const float size = std::clamp(stroke.size, preset.minSize, preset.maxSize);
        if (!beginStroke(stroke.tool, preset, size, stroke.opacity, stroke.color, stroke.samples.front(), stroke.seed))
            continue;
        for (size_t i = 1; i < stroke.samples.size(); ++i)
            strokeTo(stroke.samples[i]);
        endStroke();
    }
    m_replaying = false;
    m_history.clear();
    emit layersChanged();
}

// Forgets everything the stroke has drawn so far, ready for it to be drawn
// again. Returns the area that was covered.
QRect CanvasEngine::restartStrokeBuffer()
{
    const QRect previous = m_stroke.bounds;
    if (m_stroke.smear) {
        gfx::copyTexture(strokeTarget(), m_live, previous, previous.topLeft());
        m_stroke.hasSmudgePos.clear();
    } else {
        // Only where the stroke has been: on a large canvas, wiping all of it
        // for every redraw of a shape costs more than drawing the shape.
        m_strokeBuf.clearRect(previous.adjusted(-2, -2, 2, 2));
    }
    std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), uint8_t(0));
    m_stroke.bounds = QRect();
    return previous;
}

// Where the earlier version had paint and the new one has none, the live layer
// still shows the old pixels: merge that area again.
void CanvasEngine::finishStrokeRestart(const QRect& previous)
{
    if (previous.isEmpty())
        return;
    if (!m_stroke.smear) {
        mergeStroke(&strokeTarget(), m_strokeBuf, m_live, previous, &m_stroke.preset, m_stroke.opacity,
                    m_stroke.tool == StrokeTool::Eraser, strokeLocked());
    }
    damage(previous);
}

void CanvasEngine::setStrokePath(const std::vector<PathPoint>& path, bool closed)
{
    if (!m_stroke.active || path.empty())
        return;
    Scope s(this);
    const QRect previous = restartStrokeBuffer();

    StrokeSample first;
    first.pos = path.front().pos;
    first.pressure = path.front().pressure;
    first.tiltX = path.front().tiltX;
    first.tiltY = path.front().tiltY;
    m_strokeEngine.setSeed(m_stroke.seed);   // same jitter on every re-render
    m_strokeEngine.setTaperEnabled(!closed);
    m_strokeEngine.begin(m_stroke.preset, m_stroke.size, m_stroke.color, first);
    for (size_t i = 1; i < path.size(); ++i)
        m_strokeEngine.addDirect(path[i]);
    m_strokeEngine.end();
    processDabs();
    finishStrokeRestart(previous);
}

void CanvasEngine::cancelStroke()
{
    if (!m_stroke.active)
        return;
    Scope s(this);
    m_strokeEngine.dabs().clear();
    const QRect touched = m_stroke.bounds;
    m_overlay = nullptr;
    m_overlayMask = false;
    m_stroke = {};
    m_journalPending = {};
    damage(touched);
    emit layersChanged();
}

void CanvasEngine::strokeTo(const StrokeSample& sample)
{
    if (!m_stroke.active)
        return;
    Scope s(this);
    if (m_journalOn && !m_replaying)
        m_journalPending.samples.push_back(sample);
    m_strokeEngine.add(sample);
    processDabs();
}

// ----------------------------------------------------------------- liquify

bool CanvasEngine::beginLiquify()
{
    if (!hasDocument() || busy())
        return false;
    const LayerPtr layer = activeLayer();
    if (!layer || !paintable(layer) || layer->isGroup)
        return false;
    if (!layer->visible) {
        emit message(tr("The selected layer is hidden"));
        return false;
    }
    Scope s(this);
    // Its live copy, and the layer as it will be next to the layer as it was.
    if (!makeRoom(layer->tex.bytes() * 2 + (qint64(64) << 20)))
        return false;
    m_liquify = {};
    m_liquify.active = true;
    m_liquify.layer = layer;
    // Half floats hold offsets of a few hundred pixels to a small fraction of
    // one, at half the memory of full floats.
    for (gfx::Texture& t : m_liquify.field) {
        // Nothing moved is nothing stored: the field takes memory only where the brush has been.
        t.create(m_size.width(), m_size.height(), GL_RG16F, false, true);
        t.clear();
    }
    gfx::copyTexture(layer->tex, m_live);
    m_overlay = layer.get();
    m_overlayMask = false;
    return true;
}

void CanvasEngine::liquify(LiquifyMode mode, const QPointF& from, const QPointF& to, float radius, float strength)
{
    if (!m_liquify.active || radius < 1.0f)
        return;
    Scope s(this);
    strength = std::clamp(strength, 0.0f, 1.0f);
    // Short steps, so a fast drag still pushes smoothly: each one moves the
    // field by a fraction of the brush.
    const QPointF travel = to - from;
    const double length = std::hypot(travel.x(), travel.y());
    const int steps = std::clamp(int(std::ceil(length / (radius * 0.2))), 1, 64);
    float amount = strength;
    switch (mode) {
    case LiquifyMode::Push: amount = strength; break;
    case LiquifyMode::TwirlRight:
    case LiquifyMode::TwirlLeft: amount = strength * 0.16f; break;
    case LiquifyMode::Pinch:
    case LiquifyMode::Expand: amount = strength * 0.07f; break;
    case LiquifyMode::Restore: amount = strength * 0.35f; break;
    }
    QRect touched;
    m_liquifyProg.use();
    m_liquifyProg.set("uField", 0);
    m_liquifyProg.set("uSize", float(m_size.width()), float(m_size.height()));
    m_liquifyProg.set("uRadius", radius);
    m_liquifyProg.set("uAmount", amount);
    m_liquifyProg.set("uMode", int(mode));
    m_liquifyProg.set("uMove", float(travel.x() / steps), float(travel.y() / steps));
    for (int i = 1; i <= steps; ++i) {
        const QPointF c = from + travel * (double(i) / steps);
        const QRect rect = QRectF(c.x() - radius - 1, c.y() - radius - 1, 2 * radius + 2, 2 * radius + 2).toAlignedRect() & canvasRect();
        if (rect.isEmpty())
            continue;
        fullscreenTo(m_liquify.field[1], &rect);
        m_liquifyProg.use();
        m_liquify.field[0].bind(0);
        m_liquifyProg.set("uCentre", float(c.x()), float(c.y()));
        m_fs.draw();
        gl()->glDisable(GL_SCISSOR_TEST);
        gfx::copyTexture(m_liquify.field[1], m_liquify.field[0], rect, rect.topLeft());
        touched |= rect;
    }
    if (touched.isEmpty())
        return;
    m_liquify.changed = true;
    m_liquify.touched |= touched;
    showLiquify(touched);
}

// Redraws the live copy of the layer through the field, where it has changed.
void CanvasEngine::showLiquify(const QRect& rect)
{
    fullscreenTo(m_live, &rect);
    m_liquifyApplyProg.use();
    m_liquify.layer->tex.bind(0);
    m_liquify.field[0].bind(1);
    (hasSelection() ? m_selMask : m_liquify.layer->tex).bind(2);
    m_liquifyApplyProg.set("uSrc", 0);
    m_liquifyApplyProg.set("uField", 1);
    m_liquifyApplyProg.set("uSelection", 2);
    m_liquifyApplyProg.set("uHasSelection", hasSelection() ? 1 : 0);
    m_liquifyApplyProg.set("uSize", float(m_size.width()), float(m_size.height()));
    m_fs.draw();
    gl()->glDisable(GL_SCISSOR_TEST);
    damage(rect);
}

void CanvasEngine::resetLiquify()
{
    if (!m_liquify.active)
        return;
    Scope s(this);
    m_liquify.field[0].clear();
    m_liquify.field[1].clear();
    m_liquify.changed = false;
    gfx::copyTexture(m_liquify.layer->tex, m_live);
    damageAll();
}

void CanvasEngine::endLiquify(bool commit)
{
    if (!m_liquify.active)
        return;
    Scope s(this);
    const LayerPtr layer = m_liquify.layer;
    const bool changed = m_liquify.changed;
    // Paint ends up where it was or where the brush took it, nowhere else.
    const QRect reach = layer->tex.heldBounds() | m_liquify.touched;
    m_liquify = {};   // frees the field
    if (commit && changed)
        commitLiveFull(layer, m_live, tr("Liquify"), false, &reach);
    m_overlay = nullptr;
    m_overlayMask = false;
    damageAll();
    emit layersChanged();
}

// ---------------------------------------------------------------- gradient

bool CanvasEngine::beginGradient()
{
    if (!hasDocument() || busy())
        return false;
    const LayerPtr layer = activeLayer();
    if (!layer || !paintable(layer))
        return false;
    if (!layer->visible) {
        emit message(tr("The selected layer is hidden"));
        return false;
    }
    Scope s(this);
    {
        const QRect reach = hasSelection() ? m_selBounds : canvasRect();
        if (!makeRoom(qint64(reach.width()) * reach.height() * 16))
            return false;
    }
    m_gradient = {};
    m_gradient.active = true;
    m_gradient.layer = layer;
    m_gradient.onMask = m_maskEditing && layer->mask;
    gfx::copyTexture(layer->target(m_gradient.onMask), m_live);
    m_overlay = layer.get();
    m_overlayMask = m_gradient.onMask;
    return true;
}

void CanvasEngine::setGradient(const QPointF& from, const QPointF& to, const QColor& first, const QColor& second, GradientKind kind)
{
    setGradient(from, to, {GradientStop{0.0f, first}, GradientStop{1.0f, second}}, kind);
}

void CanvasEngine::setGradient(const QPointF& from, const QPointF& to, const QVector<GradientStop>& given, GradientKind kind)
{
    if (!m_gradient.active || given.size() < 2)
        return;
    Scope s(this);
    Layer& layer = *m_gradient.layer;
    const bool onMask = m_gradient.onMask;
    QVector<GradientStop> stops = given.mid(0, kMaxGradientStops);
    std::stable_sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.at < b.at; });
    // Drawn into the stroke buffer and merged like paint, so the selection and alpha lock apply.
    float at[kMaxGradientStops] = {}, colours[kMaxGradientStops * 4] = {};
    for (int i = 0; i < stops.size(); ++i) {
        const QColor& c = stops[i].color;
        const QColor ink = onMask ? greyOf(c) : c;
        const float a = onMask ? 1.0f : float(c.alphaF());
        at[i] = std::clamp(stops[i].at, 0.0f, 1.0f);
        colours[i * 4] = float(ink.redF()) * a;
        colours[i * 4 + 1] = float(ink.greenF()) * a;
        colours[i * 4 + 2] = float(ink.blueF()) * a;
        colours[i * 4 + 3] = a;
    }
    // It can only land inside the selection, and with alpha lock only where there is paint.
    QRect reach = hasSelection() ? m_selBounds : canvasRect();
    if (layer.alphaLock && !onMask)
        reach &= layer.tex.heldBounds();
    if (reach.isEmpty())
        return;
    m_strokeBuf.color.ensure(reach);
    fullscreenTo(m_strokeBuf.color, &reach);
    m_gradientProg.use();
    m_gradientProg.set("uFrom", float(from.x()), float(from.y()));
    m_gradientProg.set("uTo", float(to.x()), float(to.y()));
    m_gradientProg.set("uKind", int(kind));
    m_gradientProg.set("uCount", int(stops.size()));
    m_gradientProg.setFloats("uAt", at, kMaxGradientStops);
    m_gradientProg.setVec4s("uColor", colours, kMaxGradientStops);
    m_fs.draw();
    mergeStroke(&layer.target(onMask), m_strokeBuf, m_live, reach, nullptr, 1.0f, false, onMask || layer.alphaLock, nullptr, true);
    m_gradient.drawn = true;
    damage(reach);
}

void CanvasEngine::endGradient(bool commit)
{
    if (!m_gradient.active)
        return;
    Scope s(this);
    const GradientState done = m_gradient;
    m_gradient = {};
    if (commit && done.drawn) {
        // What the layer had, and wherever the gradient could reach.
        const QRect reach = done.layer->tex.heldBounds() | (hasSelection() ? m_selBounds : canvasRect());
        commitLiveFull(done.layer, m_live, done.onMask ? tr("Gradient on Mask") : tr("Gradient"), done.onMask, &reach);
    }
    m_strokeBuf.clear();
    m_overlay = nullptr;
    m_overlayMask = false;
    damageAll();
    emit layersChanged();
}

void CanvasEngine::setGuides(const Guides& guides)
{
    Guides g = guides;
    g.segments = std::clamp(g.segments, 2, 24);
    g.gridSpacing = std::clamp(g.gridSpacing, 8.0f, 2000.0f);
    g.isoLeft = std::clamp(g.isoLeft, 1.0f, 89.0f);
    g.isoRight = std::clamp(g.isoRight, 1.0f, 89.0f);
    if (g == m_guides || m_stroke.active)
        return;
    m_guides = g;
    emit guidesChanged();
    emit damaged();   // the viewport draws them
}

QPointF SymmetryOp::apply(const QPointF& p, const QPointF& centre) const
{
    const QPointF d = p - centre;
    if (mirror) {
        const double c = std::cos(2.0 * angle), s = std::sin(2.0 * angle);
        return centre + QPointF(d.x() * c + d.y() * s, d.x() * s - d.y() * c);
    }
    const double c = std::cos(angle), s = std::sin(angle);
    return centre + QPointF(d.x() * c - d.y() * s, d.x() * s + d.y() * c);
}

double symmetryAxis(const Guides& g)
{
    const double pi = 3.14159265358979323846;
    return (g.symmetry == Guides::Symmetry::Horizontal ? 0.0 : -pi / 2.0) + double(g.symAngle) * pi / 180.0;
}

QVector<SymmetryOp> symmetryOps(const Guides& g)
{
    const double pi = 3.14159265358979323846;
    const double turn = double(g.symAngle) * pi / 180.0;
    const double upright = -pi / 2.0 + turn, level = turn;
    auto mirror = [](double axis) { return SymmetryOp{true, axis}; };
    auto turned = [](double by) { return SymmetryOp{false, by}; };
    switch (g.symmetry) {
    case Guides::Symmetry::Off:
        break;
    case Guides::Symmetry::Vertical:
        return {g.symRotational ? turned(pi) : mirror(upright)};
    case Guides::Symmetry::Horizontal:
        return {g.symRotational ? turned(pi) : mirror(level)};
    case Guides::Symmetry::Quadrant:
        if (g.symRotational)
            return {turned(pi / 2.0), turned(pi), turned(pi * 1.5)};
        return {mirror(upright), mirror(level), turned(pi)};
    case Guides::Symmetry::Radial: {
        const int n = std::clamp(g.segments, 2, 24);
        QVector<SymmetryOp> ops;
        for (int k = 1; k < n; ++k)
            ops.push_back(turned(2.0 * pi * k / n));
        // A kaleidoscope: mirrored in every spoke and half way between each two.
        for (int k = 0; k < n && g.symMirrored; ++k)
            ops.push_back(mirror(upright + pi * k / n));
        return ops;
    }
    }
    return {};
}

QPointF CanvasEngine::symmetryCentre() const
{
    return QPointF(m_size.width() / 2.0, m_size.height() / 2.0) + m_guides.symOffset;
}

// Adds every repeat of every dab that symmetry asks for, one whole set after
// another. They share the original's randomness, so each copy is an exact
// image of it.
void CanvasEngine::repeatForSymmetry(std::vector<Dab>& dabs) const
{
    const QVector<SymmetryOp> ops = symmetryOps(m_guides);
    if (ops.isEmpty())
        return;
    const QPointF centre = symmetryCentre();
    const size_t n = dabs.size();
    dabs.reserve(n * size_t(ops.size() + 1));
    for (const SymmetryOp& op : ops) {
        for (size_t i = 0; i < n; ++i) {
            Dab d = dabs[i];
            const QPointF at = op.apply(QPointF(d.x, d.y), centre);
            d.x = float(at.x());
            d.y = float(at.y());
            d.angle = float(op.applyToAngle(d.angle));
            dabs.push_back(d);
        }
    }
}

void CanvasEngine::processDabs()
{
    std::vector<Dab>& dabs = m_strokeEngine.dabs();
    if (dabs.empty())
        return;
    const size_t drawn = dabs.size();
    repeatForSymmetry(dabs);
    QRect dirty;
    for (const Dab& d : dabs) {
        const QRect b = DabRenderer::dabBounds(d);
        markTiles(b);
        dirty |= b;
    }
    dirty &= canvasRect();

    if (m_stroke.smear) {
        // Each repeat is a smear of its own: it drags along what lies under
        // it, and remembers for itself where it last was. A wet brush is a
        // smear that brings paint of its own.
        const bool wet = m_stroke.tool == StrokeTool::Brush;
        const float charge = wet ? std::clamp(m_stroke.preset.wetCharge, 0.02f, 1.0f) : 0.0f;
        const float pull = wet ? std::clamp(m_stroke.preset.wetMix, 0.0f, 1.0f) : 1.0f;
        const size_t sets = dabs.size() / drawn;
        m_stroke.smudgePos.resize(sets);
        m_stroke.hasSmudgePos.resize(sets, 0);
        for (size_t k = 0; k < sets; ++k) {
            const std::vector<Dab> one(dabs.begin() + std::ptrdiff_t(k * drawn), dabs.begin() + std::ptrdiff_t((k + 1) * drawn));
            bool has = m_stroke.hasSmudgePos[k] != 0;
            m_dabs.smudge(m_live, m_strokeBuf.color, one, m_stroke.preset, m_stroke.opacity, m_stroke.smudgePos[k], has, selectionMask(),
                          strokeLocked() ? &strokeTarget() : nullptr, charge, pull);
            m_stroke.hasSmudgePos[k] = has ? 1 : 0;
        }
    } else {
        m_dabs.paint(m_strokeBuf, dabs, m_stroke.preset);
        if (!dirty.isEmpty()) {
            mergeStroke(&strokeTarget(), m_strokeBuf, m_live, dirty, &m_stroke.preset, m_stroke.opacity,
                        m_stroke.tool == StrokeTool::Eraser, strokeLocked());
        }
    }
    dabs.clear();
    m_stroke.bounds |= dirty;
    damage(dirty);
}

void CanvasEngine::commitLiveTiles(const LayerPtr& layer, gfx::Texture& source, const std::vector<QPoint>& tiles,
                                   const QString& undoName, bool onMask)
{
    if (tiles.empty())
        return;
    auto cmd = std::make_unique<PixelCommand>(this, layer, tiles, undoName, onMask);
    for (const QPoint& t : tiles) {
        const QRect r = tileRect(t);
        gfx::copyTexture(source, layer->target(onMask), r, r.topLeft());
    }
    if (onMask)
        ++layer->maskRevision;
    else
        touch(*layer);
    m_history.push(std::move(cmd));
    setModified(true);
}

void CanvasEngine::commitLiveFull(const LayerPtr& layer, gfx::Texture& source, const QString& undoName, bool onMask, const QRect* reach)
{
    auto cmd = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, undoName, onMask);
    // An edit may leave whole stretches empty that had paint before.
    takeFrom(source, layer->target(onMask), reach ? *reach : canvasRect());
    if (onMask)
        ++layer->maskRevision;
    else
        touch(*layer);
    m_history.push(std::move(cmd));
    setModified(true);
}

void CanvasEngine::endStroke(const QString& undoName)
{
    if (!m_stroke.active)
        return;
    Scope s(this);
    if (m_strokeEngine.end()) {
        // The stroke was regenerated with its end taper: draw it afresh.
        const QRect previous = restartStrokeBuffer();
        processDabs();
        finishStrokeRestart(previous);
    } else {
        processDabs();
    }

    const LayerPtr layer = m_stroke.layer;
    const QString name = !undoName.isEmpty()                    ? undoName
                         : m_stroke.onMask                      ? tr("Mask Stroke")
                         : m_stroke.tool == StrokeTool::Eraser  ? tr("Erase")
                         : m_stroke.tool == StrokeTool::Smudge ? tr("Smudge")
                                                               : tr("Brush Stroke");
    commitLiveTiles(layer, m_live, dirtyTileList(), name, m_stroke.onMask);
    // What was rubbed out altogether need not go on taking memory.
    if (m_stroke.tool == StrokeTool::Eraser && !m_stroke.onMask)
        trim(layer->tex, m_stroke.bounds);
    // The live copy is the layer, to the pixel: the next stroke can start from it.
    m_liveCopy = {layer.get(), m_stroke.onMask, m_stroke.onMask ? layer->maskRevision : layer->stamp, m_live.written()};
    m_overlay = nullptr;
    m_overlayMask = false;
    m_stroke = {};
    if (m_journalOn && !m_replaying && !m_journalPending.samples.empty()) {
        m_journal.push_back(std::move(m_journalPending));
        m_journalPending = {};
        // Old strokes go once there is more than can be redrawn in a moment.
        size_t samples = 0;
        for (const JournalStroke& stroke : m_journal)
            samples += stroke.samples.size();
        while (m_journal.size() > 1 && samples > 12000) {
            samples -= m_journal.front().samples.size();
            m_journal.erase(m_journal.begin());
        }
    }
    emit layersChanged();
}

// --------------------------------------------------------- transform/adjust

bool CanvasEngine::beginTransform()
{
    if (!hasDocument() || busy())
        return false;
    const LayerPtr layer = activeLayer();
    if (!paintable(layer) || layer->isGroup)
        return false;
    if (!layer->visible) {
        emit message(tr("Select a visible layer to transform"));
        return false;
    }
    Scope s(this);
    // Its live copy, and the layer as it will be next to the layer as it was.
    if (!makeRoom(layer->tex.bytes() * 2 + (qint64(64) << 20)))
        return false;
    m_edit = {};
    m_edit.kind = EditKind::Transform;
    m_edit.layer = layer;
    gfx::copyTexture(layer->tex, m_live);
    m_overlay = layer.get();
    m_overlayMask = false;
    return true;
}

void CanvasEngine::setTransform(const QTransform& canvasTransform, bool smooth)
{
    if (m_edit.kind != EditKind::Transform)
        return;
    m_edit.transform = canvasTransform;
    m_edit.smooth = smooth;
    m_edit.warp.clear();
    Scope s(this);
    renderEdit();
}

void CanvasEngine::setWarp(int cols, int rows, const std::vector<QPointF>& points, const QRectF& bounds)
{
    if (m_edit.kind != EditKind::Transform || cols < 2 || rows < 2 || int(points.size()) != cols * rows || bounds.isEmpty())
        return;
    m_edit.warp = points;
    m_edit.warpCols = cols;
    m_edit.warpRows = rows;
    m_edit.warpBounds = bounds;
    Scope s(this);
    renderEdit();
}

// A Catmull-Rom sheet: smooth, and it passes through every one of its points,
// so a handle is exactly where the paint under it ends up.
QPointF CanvasEngine::warpPoint(int cols, int rows, const std::vector<QPointF>& points, double u, double v)
{
    auto spline = [](const QPointF& a, const QPointF& b, const QPointF& c, const QPointF& d, double t) {
        const double t2 = t * t, t3 = t2 * t;
        return 0.5 * ((2.0 * b) + (c - a) * t + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 + (3.0 * b - a - 3.0 * c + d) * t3);
    };
    // Past the last point in a row or column the sheet is taken to run straight on.
    auto along = [&](const std::function<QPointF(int)>& at, int count, double s) {
        const int i = std::clamp(int(std::floor(s)), 0, count - 2);
        const QPointF b = at(i), c = at(i + 1);
        const QPointF a = i > 0 ? at(i - 1) : b * 2.0 - c;
        const QPointF d = i + 2 < count ? at(i + 2) : c * 2.0 - b;
        return spline(a, b, c, d, s - i);
    };
    return along([&](int r) { return along([&](int c) { return points[size_t(r * cols + c)]; }, cols, u); }, rows, v);
}

void CanvasEngine::drawWarp(const gfx::Texture& src, gfx::Texture& target, bool fillWhite, bool throughSelection, const QRect* reach)
{
    gfx::GLF* f = gl();
    const bool selected = throughSelection && hasSelection();
    // Underneath: what stays put. With a selection that is everything not
    // selected; otherwise nothing (or, for a mask, white: shown).
    if (selected) {
        m_scratch[1].clear(fillWhite ? 1.0f : 0.0f, fillWhite ? 1.0f : 0.0f, fillWhite ? 1.0f : 0.0f, fillWhite ? 1.0f : 0.0f);
        fullscreenTo(target, reach);
        m_maskMixProg.use();
        src.bind(0);
        m_scratch[1].bind(1);
        m_selMask.bind(2);
        m_maskMixProg.set("uA", 0);
        m_maskMixProg.set("uB", 1);
        m_maskMixProg.set("uMask", 2);
        m_fs.draw();
    } else if (reach && !fillWhite) {
        // Wiped where it is held, and held where the caller has decided.
        target.clearRect(*reach, true);
    } else {
        const float v = fillWhite ? 1.0f : 0.0f;
        target.clear(v, v, v, v);
    }

    // On top: the sheet, cut into small squares that each carry their piece of the layer.
    constexpr int kPerCell = 12;
    const int nx = (m_edit.warpCols - 1) * kPerCell, ny = (m_edit.warpRows - 1) * kPerCell;
    const QRectF b = m_edit.warpBounds;
    std::vector<float> corners(size_t(nx + 1) * size_t(ny + 1) * 4);
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double u = double(i) / kPerCell, v = double(j) / kPerCell;
            const QPointF at = warpPoint(m_edit.warpCols, m_edit.warpRows, m_edit.warp, u, v);
            float* c = corners.data() + (size_t(j) * size_t(nx + 1) + size_t(i)) * 4;
            c[0] = float(at.x());
            c[1] = float(at.y());
            c[2] = float(b.left() + b.width() * i / nx);
            c[3] = float(b.top() + b.height() * j / ny);
        }
    }
    std::vector<float> mesh;
    mesh.reserve(size_t(nx) * size_t(ny) * 24);
    auto push = [&](int i, int j) {
        const float* c = corners.data() + (size_t(j) * size_t(nx + 1) + size_t(i)) * 4;
        mesh.insert(mesh.end(), c, c + 4);
    };
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            push(i, j), push(i + 1, j), push(i + 1, j + 1);
            push(i, j), push(i + 1, j + 1), push(i, j + 1);
        }
    }
    target.bindAsTarget();
    if (reach) {
        f->glEnable(GL_SCISSOR_TEST);
        f->glScissor(reach->x(), reach->y(), reach->width(), reach->height());
    } else {
        f->glDisable(GL_SCISSOR_TEST);
    }
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_warpProg.use();
    src.bind(0);
    (selected ? m_selMask : src).bind(1);
    m_warpProg.set("uSrc", 0);
    m_warpProg.set("uSelection", 1);
    m_warpProg.set("uHasSelection", selected ? 1 : 0);
    m_warpProg.set("uCanvasSize", float(m_size.width()), float(m_size.height()));
    f->glBindVertexArray(m_warpVao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_warpVbo);
    f->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(float)), mesh.data(), GL_STREAM_DRAW);
    gfx::beforeDraw();
    f->glDrawArrays(GL_TRIANGLES, 0, GLsizei(mesh.size() / 4));
    f->glBindVertexArray(0);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
}

bool CanvasEngine::beginAdjustment(Adjustment type)
{
    if (!hasDocument() || busy() || type == Adjustment::None)
        return false;
    const LayerPtr layer = activeLayer();
    if (!paintable(layer) || layer->isGroup)
        return false;
    if (!layer->visible) {
        emit message(tr("Select a visible layer to adjust"));
        return false;
    }
    Scope s(this);
    // Its live copy, and the layer as it will be next to the layer as it was.
    if (!makeRoom(layer->tex.bytes() * 2 + (qint64(64) << 20)))
        return false;
    m_edit = {};
    m_edit.kind = EditKind::Adjust;
    m_edit.adjustment = type;
    m_edit.layer = layer;
    gfx::copyTexture(layer->tex, m_live);
    m_overlay = layer.get();
    m_overlayMask = false;
    return true;
}

QString CanvasEngine::adjustmentName(Adjustment type)
{
    switch (type) {
    case Adjustment::None: break;
    case Adjustment::HueSaturation: return tr("Hue, Saturation, Brightness");
    case Adjustment::GaussianBlur: return tr("Gaussian Blur");
    case Adjustment::Levels: return tr("Levels");
    case Adjustment::Curves: return tr("Curves");
    case Adjustment::BrightnessContrast: return tr("Brightness and Contrast");
    case Adjustment::ColorBalance: return tr("Colour Balance");
    case Adjustment::BlackAndWhite: return tr("Black and White");
    case Adjustment::Invert: return tr("Invert");
    case Adjustment::Posterize: return tr("Posterize");
    case Adjustment::Threshold: return tr("Threshold");
    case Adjustment::Sharpen: return tr("Sharpen");
    case Adjustment::Noise: return tr("Noise");
    case Adjustment::Mosaic: return tr("Mosaic");
    }
    return {};
}

void CanvasEngine::setAdjustment(const AdjustParams& params)
{
    if (m_edit.kind != EditKind::Adjust)
        return;
    m_edit.params = params;
    Scope s(this);
    renderEdit();
}

// Resamples `src` through the edit's transform into `target`. With a selection
// only what is selected moves. fillWhite is for masks, where whatever comes in
// from beyond the canvas must reveal rather than hide.
void CanvasEngine::drawTransform(const gfx::Texture& src, gfx::Texture& target, bool fillWhite, bool throughSelection, const QRect* reach)
{
    if (!m_edit.warp.empty()) {
        drawWarp(src, target, fillWhite, throughSelection, reach);
        return;
    }
    bool invertible = false;
    const QTransform inv = m_edit.transform.inverted(&invertible);
    if (!invertible)
        return;
    float m[9];
    gfx::toMat3(inv, m);
    fullscreenTo(target, reach);
    m_transformProg.use();
    src.bind(0);
    const bool selected = throughSelection && hasSelection();
    (selected ? m_selMask : src).bind(1);
    m_transformProg.set("uSrc", 0);
    m_transformProg.set("uSelection", 1);
    m_transformProg.set("uHasSelection", selected ? 1 : 0);
    m_transformProg.setMat3("uInverse", m);
    m_transformProg.set("uCanvasSize", float(m_size.width()), float(m_size.height()));
    m_transformProg.set("uSmooth", m_edit.smooth ? 1 : 0);
    m_transformProg.set("uFillWhite", fillWhite ? 1 : 0);
    m_fs.draw();
    gl()->glDisable(GL_SCISSOR_TEST);
}

// The part of the canvas an edit can leave paint on: what the layer has,
// carried to where the edit puts it. Outside it there is nothing to draw and
// nothing that need take memory.
QRect CanvasEngine::editReach() const
{
    const Layer& layer = *m_edit.layer;
    const QRect held = layer.tex.heldBounds();
    if (held.isEmpty())
        return {};
    if (m_edit.kind == EditKind::Adjust) {
        // A blur spreads as far as its radius, a mosaic by one of its squares.
        int margin = 0;
        if (m_edit.adjustment == Adjustment::GaussianBlur)
            margin = int(std::ceil(m_edit.params.radius)) + 3;
        else if (m_edit.adjustment == Adjustment::Mosaic)
            margin = int(std::ceil(m_edit.params.v[0])) + 2;
        return held.adjusted(-margin, -margin, margin, margin) & canvasRect();
    }
    // With a selection only what is selected moves; the rest stays where it is.
    const QRect moving = hasSelection() ? held & m_selBounds : held;
    QRectF to;
    if (!m_edit.warp.empty()) {
        // The sheet, followed along as finely as it is drawn.
        constexpr int kPerCell = 12;
        const int nx = (m_edit.warpCols - 1) * kPerCell, ny = (m_edit.warpRows - 1) * kPerCell;
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                const QPointF at = warpPoint(m_edit.warpCols, m_edit.warpRows, m_edit.warp, double(i) / kPerCell, double(j) / kPerCell);
                to |= QRectF(at, QSizeF(1.0, 1.0));
            }
        }
    } else if (!moving.isEmpty()) {
        const QTransform& t = m_edit.transform;
        // A corner carried to or past the horizon of a perspective lands anywhere.
        bool sound = true;
        const QRectF from(moving);
        for (const QPointF& c : {from.topLeft(), from.topRight(), from.bottomLeft(), from.bottomRight()})
            sound = sound && t.m13() * c.x() + t.m23() * c.y() + t.m33() > 1e-6;
        to = sound ? t.mapRect(from) : QRectF(canvasRect());
    }
    QRect reach = to.isEmpty() ? QRect() : to.adjusted(-3.0, -3.0, 3.0, 3.0).toAlignedRect();
    if (hasSelection())
        reach |= held;
    return reach & canvasRect();
}

void CanvasEngine::renderEdit()
{
    Layer& layer = *m_edit.layer;
    const QRect reach = editReach();
    if (reach.isEmpty()) {
        // Nothing on the layer, or all of it carried off the canvas.
        m_live.clear();
        damageAll();
        return;
    }
    // The live copy takes memory for that much and no more; every pass below
    // draws all of it (`area` is the reach, grown to the blocks memory comes in).
    const QRect area = m_live.holdOnly(reach);
    const QRect* within = &area;
    gfx::Texture& first = hasSelection() ? m_scratch[0] : m_live;
    if (&first != &m_live)
        first.holdOnly(reach);
    if (m_edit.kind == EditKind::Transform) {
        drawTransform(layer.tex, m_live, false, true, within);
    } else if (m_edit.adjustment == Adjustment::HueSaturation) {
        fullscreenTo(first, within);
        m_hsbProg.use();
        layer.tex.bind(0);
        m_hsbProg.set("uSrc", 0);
        m_hsbProg.set("uHue", m_edit.params.hue);
        m_hsbProg.set("uSaturation", m_edit.params.saturation);
        m_hsbProg.set("uBrightness", m_edit.params.brightness);
        m_fs.draw();
    } else if (m_edit.adjustment == Adjustment::GaussianBlur) {
        const float radius = m_edit.params.radius;
        if (radius < 0.05f) {
            gfx::copyTexture(layer.tex, m_live);
        } else {
            m_blurProg.use();
            m_blurProg.set("uSrc", 0);
            m_blurProg.set("uRadius", radius);
            m_scratch[1].holdOnly(reach);
            fullscreenTo(m_scratch[1], within);
            layer.tex.bind(0);
            m_blurProg.set("uDir", 1.0f / float(m_size.width()), 0.0f);
            m_fs.draw();
            fullscreenTo(first, within);
            m_scratch[1].bind(0);
            m_blurProg.set("uDir", 0.0f, 1.0f / float(m_size.height()));
            m_fs.draw();
        }
    } else {
        // Everything else is one pass of the same shader, told which sum to do.
        const float* v = m_edit.params.v;
        if (m_edit.adjustment == Adjustment::Curves) {
            QImage lut(256, 1, QImage::Format_RGBA8888);
            if (m_edit.params.lut.size() == 1024) {
                memcpy(lut.bits(), m_edit.params.lut.constData(), 1024);
            } else {
                for (int i = 0; i < 256; ++i)
                    lut.setPixel(i, 0, qRgb(i, i, i));   // nothing set yet: leave colours as they are
            }
            m_lut.upload(lut);
        }
        fullscreenTo(first, within);
        m_adjustProg.use();
        layer.tex.bind(0);
        m_lut.bind(1);
        m_adjustProg.set("uSrc", 0);
        m_adjustProg.set("uLut", 1);
        m_adjustProg.set("uOp", int(m_edit.adjustment) - int(Adjustment::Levels) + 1);
        m_adjustProg.set("uA", v[0], v[1], v[2], v[3]);
        m_adjustProg.set("uB", v[4], v[5], v[6], v[7]);
        m_fs.draw();
    }
    // Adjustments were rendered to a scratch target; keep them only inside the selection.
    const bool blurSkipped = m_edit.adjustment == Adjustment::GaussianBlur && m_edit.params.radius < 0.05f;
    if (m_edit.kind == EditKind::Adjust && hasSelection() && !blurSkipped) {
        fullscreenTo(m_live, within);
        m_maskMixProg.use();
        layer.tex.bind(0);
        m_scratch[0].bind(1);
        m_selMask.bind(2);
        m_maskMixProg.set("uA", 0);
        m_maskMixProg.set("uB", 1);
        m_maskMixProg.set("uMask", 2);
        m_fs.draw();
    }
    gl()->glDisable(GL_SCISSOR_TEST);
    damageAll();
}

void CanvasEngine::commitEdit()
{
    if (!editActive())
        return;
    Scope s(this);
    const QString name = m_edit.kind == EditKind::Transform ? tr("Transform") : adjustmentName(m_edit.adjustment);
    const LayerPtr layer = m_edit.layer;
    const QRect reach = editReach();
    // Text that is only moved, sized or turned as a whole stays text.
    bool stillText = false;
    TextInfo moved;
    if (m_edit.kind == EditKind::Transform && m_edit.warp.empty() && m_edit.transform.isAffine() && !hasSelection()) {
        const auto text = m_texts.constFind(layer->stamp);
        if (text != m_texts.constEnd()) {
            moved = text.value();
            moved.matrix = moved.matrix * m_edit.transform;
            stillText = true;
        }
    }
    if (m_edit.kind == EditKind::Transform && (hasSelection() || layer->mask)) {
        // Pixels, mask and selection travel together, as one undo step.
        auto compound = std::make_unique<CompoundCommand>(name);
        auto pixels = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, QString());
        takeFrom(m_live, layer->tex, reach);
        touch(*layer);
        compound->add(std::move(pixels));
        if (layer->mask) {
            drawTransform(*layer->mask, m_scratch[0], true);
            auto maskPixels = std::make_unique<PixelCommand>(this, layer, std::vector<QPoint>{}, QString(), true);
            gfx::copyTexture(m_scratch[0], *layer->mask);
            ++layer->maskRevision;
            compound->add(std::move(maskPixels));
        }
        if (hasSelection()) {
            // The selection moves the same way, resampled like the pixels were.
            const SelSnapPtr before = snapshotSelection();
            drawTransform(m_selMask, m_selTmp, false, false);
            m_selMask.swap(m_selTmp);
            m_selImage = m_selMask.readbackGrey(canvasRect());
            m_selBounds = nonZeroBounds(m_selImage);
            emit selectionChanged();
            pushSelection(before, QString(), compound.get());
        }
        m_history.push(std::move(compound));
        setModified(true);
    } else {
        commitLiveFull(layer, m_live, name, false, &reach);
    }
    if (stillText) {
        // Set again from the letters rather than left as stretched pixels:
        // however much it grew, it is as sharp as when it was typed.
        QPoint at;
        const QImage image = text::render(moved, m_size, &at);
        layer->tex.clear();
        if (!image.isNull())
            layer->tex.upload(image, at);
        m_texts.insert(layer->stamp, moved);
    }
    m_overlay = nullptr;
    m_edit = {};
    releaseTemps();
    damageAll();
    emit layersChanged();
}

void CanvasEngine::cancelEdit()
{
    if (!editActive())
        return;
    Scope s(this);
    m_overlay = nullptr;
    m_edit = {};
    releaseTemps();
    damageAll();
    emit layersChanged();
}

// ----------------------------------------------------------------- history

void CanvasEngine::undo(bool quiet)
{
    if (busy() || !m_history.canUndo())
        return;
    Scope s(this);
    const QString name = m_history.undo();
    setModified(true);
    if (!quiet)
        emit message(tr("Undo %1").arg(name));
}

void CanvasEngine::redo(bool quiet)
{
    if (busy() || !m_history.canRedo())
        return;
    Scope s(this);
    const QString name = m_history.redo();
    setModified(true);
    if (!quiet)
        emit message(tr("Redo %1").arg(name));
}

// ------------------------------------------------------------------ output

void CanvasEngine::damage(const QRect& r)
{
    const QRect clipped = r & canvasRect();
    if (clipped.isEmpty())
        return;
    m_dirty |= clipped;
    emit damaged();
}

gfx::Texture& CanvasEngine::bandBuffer(int level, int which)
{
    const size_t index = size_t(level) * 2 + size_t(which);
    while (m_bandBufs.size() <= index)
        m_bandBufs.emplace_back();
    if (!m_bandBufs[index]) {
        m_bandBufs[index] = std::make_unique<gfx::Texture>();
        m_bandBufs[index]->create(std::min(kBand, m_size.width()), std::min(kBand, m_size.height()), GL_RGBA16F);
    }
    return *m_bandBufs[index];
}

// Blends the direct members of `parentId` bottom to top and returns the texture
// holding the result (null when nothing in there is visible). Level 0 is the
// canvas itself and ends in m_result; deeper levels are groups, blended on
// their own over transparency so that the group as a whole can then take its
// opacity and blend mode. Each level ping-pongs between two buffers of its own,
// which is what lets a group be composited in the middle of its parent's run.
// Those buffers are the size of a band, not of the canvas: `band` is the part
// of the canvas being put together, and lies in them from their corner.
const gfx::Texture* CanvasEngine::compositeLevel(int parentId, int level, const QRect& band, const float background[4])
{
    m_bandOrigin = band.topLeft();
    struct Entry {
        const Layer* layer;
        bool clipped;
        float opacity;
        const float* tint = nullptr;
    };
    std::vector<Entry> entries;
    if (level == 0 && m_anim.enabled) {
        animationEntries([&](const Layer* l, bool clipped, float opacity, const float* tint) { entries.push_back({l, clipped, opacity, tint}); });
    } else {
        const Layer* base = nullptr;
        bool baseShown = false;
        for (const LayerPtr& lp : m_layers) {
            const Layer& l = *lp;
            if (l.parent != parentId)
                continue;
            const bool shown = l.visible && l.opacity > 0.0f && l.id != m_textEditId && l.id != m_leftOutId;
            // A layer clips to the nearest unclipped one below it in the same group.
            if (!l.clipping || l.isGroup || !base) {
                base = &l;
                baseShown = shown;
                if (shown)
                    entries.push_back({&l, false, l.opacity});
            } else if (baseShown && shown) {
                entries.push_back({&l, true, l.opacity * base->opacity});
            }
        }
    }

    gfx::GLF* f = gl();
    if (entries.empty()) {
        if (level > 0)
            return nullptr;
        m_result.bindAsTarget();
        f->glEnable(GL_SCISSOR_TEST);
        f->glScissor(band.x(), band.y(), band.width(), band.height());
        f->glClearColor(background[0], background[1], background[2], background[3]);
        f->glClear(GL_COLOR_BUFFER_BIT);
        return &m_result;
    }

    gfx::Texture* buffers[2] = {&bandBuffer(level, 0), &bandBuffer(level, 1)};
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const gfx::Texture* previous = nullptr;
    const gfx::Texture* baseTex = nullptr;
    const gfx::Texture* baseMask = nullptr;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Layer& l = *entries[i].layer;
        const gfx::Texture* src = nullptr;
        float opacity = entries[i].opacity;
        if (l.isGroup) {
            src = level < maxGroupDepth() ? compositeLevel(l.id, level + 1, band, clear) : nullptr;
            m_bandOrigin = band.topLeft();
        } else {
            src = (m_overlay == &l && !m_overlayMask) ? &m_live : &l.tex;
        }
        if (!src) {
            // An empty group still takes its turn, as a pass that adds nothing,
            // so the last pass of this level always lands in the right buffer.
            src = &m_live;
            opacity = 0.0f;
        }
        const gfx::Texture* mask = nullptr;
        if (l.mask && l.maskEnabled)
            mask = (m_overlay == &l && m_overlayMask) ? &m_live : l.mask.get();

        // The canvas level finishes in m_result so that pixels outside the
        // dirty rectangle stay valid; groups finish in whichever buffer is next.
        const bool last = i + 1 == entries.size();
        const bool intoBand = !(level == 0 && last);
        gfx::Texture& target = intoBand ? *buffers[i & 1] : m_result;
        target.bindAsTarget();
        f->glEnable(GL_SCISSOR_TEST);
        if (intoBand)
            f->glScissor(0, 0, band.width(), band.height());
        else
            f->glScissor(band.x(), band.y(), band.width(), band.height());
        drawCompositePass({previous, level == 0 ? background : clear, src, opacity, int(l.blend), mask,
                           entries[i].clipped ? baseTex : nullptr, entries[i].clipped ? baseMask : nullptr, intoBand, entries[i].tint});
        previous = &target;
        if (!entries[i].clipped) {
            baseTex = src;
            baseMask = mask;
        }
    }
    return previous;
}

void CanvasEngine::recomposite(const QRect& rect)
{
    const float bg[4] = {m_backgroundVisible ? float(m_background.redF()) : 0.0f,
                         m_backgroundVisible ? float(m_background.greenF()) : 0.0f,
                         m_backgroundVisible ? float(m_background.blueF()) : 0.0f,
                         m_backgroundVisible ? 1.0f : 0.0f};
    gl()->glDisable(GL_BLEND);
    for (int y = rect.top(); y <= rect.bottom(); y += kBand) {
        for (int x = rect.left(); x <= rect.right(); x += kBand)
            compositeLevel(0, 0, QRect(x, y, std::min(kBand, rect.right() + 1 - x), std::min(kBand, rect.bottom() + 1 - y)), bg);
    }
    gl()->glDisable(GL_SCISSOR_TEST);
}

void CanvasEngine::prepareFrame(bool wantMipmaps)
{
    if (!hasDocument())
        return;
    if (m_dirty.isEmpty() && !(wantMipmaps && m_mipsStale))
        return;
    Scope s(this);
    if (!m_dirty.isEmpty()) {
        recomposite(m_dirty);
        m_dirty = QRect();
        m_mipsStale = true;
    }
    if (wantMipmaps && m_mipsStale) {
        m_result.generateMipmaps();
        m_mipsStale = false;
    }
}

void CanvasEngine::finish()
{
    if (!m_ctx)
        return;
    Scope s(this);
    gl()->glFinish();
}

QImage CanvasEngine::downscale(const gfx::Texture& src, const QRect& srcRect, const QSize& target)
{
    gfx::Texture small;
    small.create(target.width(), target.height(), GL_RGBA8);
    fullscreenTo(small);
    m_thumbProg.use();
    src.bind(0);
    m_thumbProg.set("uSrc", 0);
    m_thumbProg.set("uSrcRect", float(srcRect.x()), float(srcRect.y()), float(srcRect.width()),
                    float(srcRect.height()));
    m_thumbProg.set("uTargetSize", float(target.width()), float(target.height()));
    m_fs.draw();
    return small.readback(small.rect());
}

QImage CanvasEngine::renderImage()
{
    if (!hasDocument())
        return {};
    // An animation's picture is the frame being worked on, as it plays: onion skins are only a guide.
    if (m_anim.enabled && m_playFrame < 0)
        return renderFrame(currentFrame());
    Scope s(this);
    prepareFrame(false);
    return m_result.readback(canvasRect());
}

QByteArray CanvasEngine::compositePixels()
{
    if (!hasDocument())
        return {};
    Scope s(this);
    const int was = m_playFrame;
    if (m_anim.enabled && m_playFrame < 0) {
        m_playFrame = std::max(0, currentFrame());
        damageAll();
    }
    prepareFrame(false);
    QByteArray pixels = m_result.readbackHalf(canvasRect());
    if (m_playFrame != was) {
        m_playFrame = was;
        damageAll();
    }
    return pixels;
}

QColor CanvasEngine::pickColor(const QPoint& canvasPos)
{
    if (!hasDocument() || !canvasRect().contains(canvasPos))
        return {};
    Scope s(this);
    prepareFrame(false);
    const QImage px = m_result.readback(QRect(canvasPos, QSize(1, 1)), QImage::Format_ARGB32);
    const QColor c = px.pixelColor(0, 0);
    if (c.alpha() == 0)
        return {};
    return QColor(c.red(), c.green(), c.blue());
}

QImage CanvasEngine::brushPreview(const BrushPreset& preset, const QSize& size, const QColor& color, float scale)
{
    Scope s(this);
    gfx::Texture tex;
    tex.create(size.width(), size.height(), GL_RGBA16F);
    tex.clear();

    const float w = float(size.width()), h = float(size.height());
    const float brushSize = std::clamp(preset.size * scale, 2.0f * scale, h * 0.42f);
    const float margin = brushSize * 0.5f + 8.0f * scale;
    StrokeEngine stroke;
    constexpr int n = 72;
    for (int i = 0; i <= n; ++i) {
        const float t = float(i) / float(n);
        StrokeSample smp;
        smp.pos = QPointF(margin + (w - 2.0f * margin) * t, h * 0.5f - std::sin(t * 6.2831853f) * h * 0.2f);
        smp.pressure = std::max(0.03f, std::pow(std::sin(t * 3.1415927f), 0.85f));
        smp.time = i * 0.012;
        if (i == 0)
            stroke.begin(preset, brushSize, color, smp);
        else
            stroke.add(smp);
    }
    stroke.end();
    // Same two steps as a real stroke: dabs into the stroke buffer, then the
    // merge pass that applies canvas-fixed grain.
    StrokeBuffer buffer;
    buffer.create(size.width(), size.height());
    m_dabs.paint(buffer, stroke.dabs(), preset);
    mergeStroke(nullptr, buffer, tex, tex.rect(), &preset, 1.0f, false, false);
    return tex.readback(tex.rect());
}
