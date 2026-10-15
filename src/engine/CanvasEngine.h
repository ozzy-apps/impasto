#pragma once

#include "brush/BrushPreset.h"
#include "brush/DabRenderer.h"
#include "brush/StrokeEngine.h"
#include "core/Types.h"
#include "engine/History.h"
#include "engine/Text.h"
#include "gfx/Gl.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QTransform>
#include <QVector>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <vector>

class QOffscreenSurface;
class QOpenGLContext;

struct Layer {
    int id = 0;
    QString name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool alphaLock = false;
    bool clipping = false;   // clipped to the nearest unclipped layer below
    quint64 revision = 0;    // bumped whenever the pixels change
    quint64 stamp = 0;       // names what the pixels are: undo brings it back together with them
    gfx::Texture tex;        // RGBA16F, premultiplied, sRGB-encoded (groups have none)

    // Mask: an opaque greyscale image the size of the canvas. White shows the
    // layer, black hides it. It is stored like a layer so that brushes, undo
    // and fills work on it unchanged.
    std::shared_ptr<gfx::Texture> mask;
    bool maskEnabled = true;
    quint64 maskRevision = 0;

    // Groups: a group is a header entry whose members sit directly below it in
    // the stack and name it as their parent (0 = top level).
    bool isGroup = false;
    int parent = 0;
    bool collapsed = false;  // folded shut in the layer panel
    int hold = 1;            // as a frame of an animation: how many frames' time it stays up

    gfx::Texture& target(bool onMask) { return onMask ? *mask : tex; }
};
using LayerPtr = std::shared_ptr<Layer>;

// Plain-data view of a layer for the UI, which never touches GL objects.
struct LayerInfo {
    int id = 0;
    QString name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool alphaLock = false;
    bool clipping = false;
    quint64 revision = 0;
    bool hasMask = false;
    bool maskEnabled = true;
    quint64 maskRevision = 0;
    bool isGroup = false;
    int parent = 0;      // id of the enclosing group, 0 at the top level
    int depth = 0;       // how many groups enclose it
    bool collapsed = false;
    bool reference = false;   // fills on other layers follow this one
    bool isText = false;      // still the text that was typed: it can be edited as text
    int hold = 1;             // as a frame of an animation: how many frames' time it stays up
};

// An animation in the manner of Procreate's Animation Assist: every top-level
// layer or group is a frame, bottom to top, and a layer clipped to one goes
// with it. It is part of the document.
struct Animation {
    enum class Playback { Loop = 0, PingPong, OneShot };
    bool enabled = false;          // the document is an animation: the canvas shows a frame at a time
    Playback playback = Playback::Loop;
    int fps = 12;                  // 1..60
    int onionFrames = 2;           // shown faded on each side of the current frame, 0..12
    float onionOpacity = 0.5f;     // of the nearest; the farther ones fade further
    bool tintOnion = true;         // earlier frames red, later frames green
    bool background = false;       // the first frame stays under every other
    bool foreground = false;       // the last frame stays over every other
    bool operator==(const Animation&) const = default;
};

struct LayerData {
    LayerInfo info;
    QRect rect;          // region of the canvas that `pixels` covers
    QByteArray pixels;   // RGBA16F rows, top to bottom
    QByteArray mask;     // 8-bit grey, the whole canvas; empty = no mask
    int parentIndex = -1;   // position of the enclosing group in the list, -1 at the top level
    bool hasText = false;   // the pixels are text, and `text` says which
    TextInfo text;
};

enum class Adjustment {
    None,
    HueSaturation,
    GaussianBlur,
    // From here on they share one shader and take their numbers from AdjustParams::v.
    Levels,              // v: black point, white point, gamma, output black, output white
    Curves,              // lut
    BrightnessContrast,  // v: brightness -1..1, contrast -1..1
    ColorBalance,        // v: cyan-red, magenta-green, yellow-blue, each -1..1
    BlackAndWhite,       // v: amount 0..1, warmth 0..1
    Invert,              // v: amount 0..1
    Posterize,           // v: number of steps per channel
    Threshold,           // v: level 0..1
    Sharpen,             // v: amount
    Noise,               // v: amount, 1 = grey grain / 0 = coloured
    Mosaic,              // v: square size in canvas px
};
enum class SelectOp { Replace, Add, Subtract };

// Drawing guides: part of the document, not of the picture. With symmetry on,
// every brush and eraser stroke is repeated about the centre of the canvas.
struct Guides {
    enum class Symmetry { Off, Vertical, Horizontal, Quadrant, Radial };
    Symmetry symmetry = Symmetry::Off;   // Vertical: a vertical axis, left mirrors right
    int segments = 6;                    // Radial: how many times the stroke goes round
    // Symmetry need not be about the middle of the canvas, nor upright.
    QPointF symOffset;                   // its centre, measured from the centre of the canvas, in canvas pixels
    float symAngle = 0.0f;               // how far its axes are turned, in degrees (clockwise as seen)
    bool symRotational = false;          // Vertical, Horizontal, Quadrant: repeats are turned about the centre, not mirrored
    bool symMirrored = false;            // Radial: every repeat comes with its mirror image, as in a kaleidoscope
    bool grid = false;
    float gridSpacing = 100.0f;          // canvas pixels

    // Perspective: lines to draw along. With `snap` on, a stroke keeps to
    // whichever guide line it sets off along.
    enum class Perspective { Off, OnePoint, TwoPoint, ThreePoint, FivePoint, Isometric };
    Perspective perspective = Perspective::Off;
    QPointF vp[3];                       // vanishing points, canvas pixels (they may lie off the canvas).
                                         // FivePoint (fisheye): vp[0] is the centre, vp[1] a point on its rim.
    float isoLeft = 30.0f, isoRight = 30.0f;   // Isometric: how steeply the two receding directions rise, in degrees
    bool snap = true;
    bool operator==(const Guides&) const = default;
};

// One repeat of a stroke under symmetry: turned about the centre by `angle`,
// or mirrored in the line through the centre that runs at `angle` (radians,
// in canvas coordinates, where y points down).
struct SymmetryOp {
    bool mirror = false;
    double angle = 0.0;
    QPointF apply(const QPointF& p, const QPointF& centre) const;
    double applyToAngle(double a) const { return mirror ? 2.0 * angle - a : a + angle; }
};
// Every repeat the guides ask for, not counting the stroke as drawn.
QVector<SymmetryOp> symmetryOps(const Guides& g);
// The direction of the axis symmetry is turned by: the one upright when the angle is zero
// (level for Horizontal).
double symmetryAxis(const Guides& g);

enum class FloodKind { Fill, Select };
// Linear: along the line dragged. Radial: outwards from where the drag
// began. Angle: round that point, starting in the direction dragged.
// Reflected: along the line, both ways from where it began. Diamond: out
// from the start in a square, turned to the line.
enum class GradientKind { Linear, Radial, Angle, Reflected, Diamond };
// One colour of a gradient and where along it that colour is reached (0..1).
// The colour may be partly or wholly transparent.
struct GradientStop {
    float at = 0.0f;
    QColor color;
    bool operator==(const GradientStop&) const = default;
};
constexpr int kMaxGradientStops = 12;
enum class LiquifyMode { Push, TwirlRight, TwirlLeft, Pinch, Expand, Restore };

struct AdjustParams {
    float hue = 0.0f;         // -0.5..0.5 of a turn
    float saturation = 0.0f;  // -1..1
    float brightness = 0.0f;  // -1..1
    float radius = 0.0f;      // blur radius in canvas px
    float v[8] = {};          // the newer adjustments: see Adjustment
    QByteArray lut;           // Curves: 256 RGBA entries, what each channel value becomes
};

// Owns the document and everything on the GPU. It has its own OpenGL context
// (sharing textures with the viewport) so painting, undo, file I/O and preview
// rendering work regardless of what the widget tree is doing.
class CanvasEngine : public QObject {
    Q_OBJECT
public:
    explicit CanvasEngine(QObject* parent = nullptr);
    ~CanvasEngine() override;

    bool initialize(QString* error = nullptr);
    QString rendererName() const { return m_renderer; }

    // ---- document
    void newDocument(const QSize& size, const QColor& background, bool backgroundVisible = true,
                     const QImage& firstLayer = QImage());
    void loadDocument(const QSize& size, const QColor& background, bool backgroundVisible,
                      const std::vector<LayerData>& layers, int activeIndex);
    bool hasDocument() const { return !m_size.isEmpty(); }
    // How finely the picture is meant to be printed, in dots per inch; 0 when
    // nobody has said. It changes nothing on screen: it is kept with the
    // document and written into exported pictures, so that a canvas made as
    // 50 x 70 cm arrives at the printer as 50 x 70 cm.
    int resolution() const { return m_resolution; }
    void setResolution(int dpi) { m_resolution = std::clamp(dpi, 0, 2400); }
    QSize canvasSize() const { return m_size; }
    QRect canvasRect() const { return QRect(QPoint(0, 0), m_size); }
    QColor backgroundColor() const { return m_background; }
    bool backgroundVisible() const { return m_backgroundVisible; }
    void setBackgroundColor(const QColor& c);
    void setBackgroundVisible(bool visible);
    bool isModified() const { return m_modified; }
    void setModified(bool modified);
    // How large a canvas may be. Both follow from the graphics card: the
    // longest side from the largest texture it takes, the pixel count from
    // its memory, since every layer lives there at 16 bits per channel.
    static int maxCanvasDimension() { return s_maxDimension; }
    static qint64 maxCanvasPixels() { return s_maxPixels; }
    // Roughly how many layers of this size fit in graphics memory when each
    // is painted from edge to edge. Where layers are stored only where there
    // is paint (sparseLayers()), that is the worst case: a layer with less
    // on it takes less, and an empty one nothing.
    static int layerEstimate(const QSize& size);
    static qint64 graphicsMemoryBytes() { return s_memoryBytes; }
    // Gives the canvas a new size. `placement` says where what is on the old
    // canvas goes on the new one: a shift crops or adds room, a scale resamples
    // the picture. Every layer and mask follows; the selection is dropped. One
    // undo step.
    bool resizeCanvas(const QSize& newSize, const QTransform& placement, const QString& undoName);

    // ---- layers (index 0 is the bottom of the stack)
    QVector<LayerInfo> layers() const;
    LayerInfo layerInfo(int id) const;
    int layerCount() const { return int(m_layers.size()); }
    int activeLayerId() const { return m_activeId; }
    void setActiveLayer(int id);
    static int maxGroupDepth() { return 4; }
    int addLayer();
    int addGroup();
    // Wraps a layer (or a whole group) in a new group / dissolves a group,
    // keeping its members / replaces a group by one layer showing the same.
    int groupLayer(int id);
    void ungroupLayer(int groupId);
    void flattenGroup(int groupId);
    void setLayerCollapsed(int id, bool collapsed);
    // `topLeft` places the image; left at its default the image is centred.
    int addImageLayer(const QImage& image, const QString& name, const QPoint& topLeft = QPoint(INT_MIN, INT_MIN),
                      const QString& undoName = QString());
    void duplicateLayer(int id);
    void removeLayer(int id);
    // Places a layer, or a group with everything in it, inside `parentId`
    // (0 = top level) directly below the sibling `aboveId`; aboveId 0 puts it
    // on top of that parent's stack.
    void moveLayer(int id, int parentId, int aboveId);
    void mergeDown(int id);
    void clearLayer(int id);
    void fillLayer(int id, const QColor& color);
    void flipLayer(int id, bool horizontal);
    void flipCanvas(bool horizontal);
    void setLayerName(int id, const QString& name);
    void setLayerVisible(int id, bool visible);
    void setLayerOpacity(int id, float opacity);
    void setLayerBlend(int id, BlendMode mode);
    void setLayerAlphaLock(int id, bool locked);
    void setLayerClipping(int id, bool clipping);
    QImage layerThumbnail(int id, int maxDim, bool mask = false);
    // How the tones of a layer are spread: 256 steps each for lightness, red,
    // green and blue, with the fullest step of each counted as 1. What is
    // transparent does not count. It is the layer as it is kept, not as an
    // adjustment in progress shows it: what goes into the adjustment.
    std::array<QVector<float>, 4> layerHistogram(int id);

    // ---- text that stays text. A layer made from text remembers what was
    // typed for as long as its pixels are exactly that text: moving, sizing
    // and turning it keeps it so (and sets it afresh, so it stays sharp);
    // painting on it, erasing or filtering it makes it ordinary pixels, and
    // undoing that makes it text again.
    int addTextLayer(const TextInfo& info, const QString& name);
    bool layerIsText(int id) const;
    TextInfo layerText(int id) const;
    bool setLayerText(int id, const TextInfo& info);     // sets the layer afresh from the text; one undo step
    int textLayerAt(const QPointF& canvasPos) const;     // the topmost visible text under a point, or 0
    // The layer being retyped is left out of the picture meanwhile (0 = none).
    void setTextEditLayer(int id);
    QByteArray layerMaskGrey(int id);   // 8-bit, canvas-sized; empty without a mask

    // ---- layer masks
    void addLayerMask(int id);          // white, or the current selection if there is one
    void removeLayerMask(int id);
    void applyLayerMask(int id);        // bakes it into the layer's pixels
    void invertLayerMask(int id);
    void setLayerMaskEnabled(int id, bool enabled);
    // While on, painting, erasing, filling and clearing act on the active
    // layer's mask instead of its pixels.
    bool maskEditing() const;
    void setMaskEditing(bool on);
    QByteArray layerPixels(int id);
    // The same for one part of the canvas only. `holding`, when given,
    // receives the smallest rectangle around the blocks of the layer that
    // have memory: nothing of the layer lies outside it.
    QByteArray layerPixels(int id, const QRect& region);
    QRect layerHeldBounds(int id) const;
    // What a group shows on its own, put together (RGBA16F, premultiplied),
    // over `rect`: everything in it, but not its own opacity, mode or mask.
    QByteArray groupPixels(int groupId, QRect* rect);
    int layerHeldBlocks(int id) const;      // how many blocks of the layer have memory (gfx::kPage square each)
    // Graphics memory in use: the layers (and what goes with them), the
    // working buffers, and what undo is keeping.
    qint64 memoryInUse() const;
    // The part of that which is only kept for speed, and is given up first when memory runs short.
    qint64 memoryForSpeed() const;
    // What may be used in all: a share of the card's memory, less whatever
    // other programs have taken of it meanwhile.
    qint64 memoryBudget() const;
    bool nvidiaMemoryInfo() const;   // whether the card can be asked how much memory is free
    static bool sparseLayers();   // layers take memory only where there is paint
    // The buffers a stroke passes through are kept whole on canvases small
    // enough for that not to matter, since handing out memory block by block
    // as a stroke goes costs a little time each block; on larger ones they
    // are stored like layers. Tests set where "larger" begins (in pixels);
    // a negative number puts it back where the card's memory says.
    static bool leanBuffers(qint64 pixels);
    static void setLeanFrom(qint64 pixels) { s_leanFrom = pixels; }
    // For tests: pretend this much graphics memory may be used in all (a negative number: what the card has).
    static void setBudgetForTests(qint64 bytes) { s_budgetOverride = bytes; }
    QRect layerContentBounds(int id);
    // What a transform would move: the layer's content, or the part of it
    // inside the selection.
    QRect editableBounds(int id);

    // ---- selection: how much of every canvas pixel is selected, kept as an
    // 8-bit image so edges can be soft. While there is one, painting, erasing,
    // smudging, filling, clearing, adjustments and transforms only reach
    // inside it, in proportion where it is partial.
    bool hasSelection() const { return !m_selBounds.isEmpty(); }
    QRect selectionBounds() const { return m_selBounds; }
    float selectionAt(const QPoint& canvasPos) const;   // 0..1
    GLuint selectionTexture() const { return m_selMask.id(); }
    void setSelection(const QPainterPath& path, const QString& undoName);   // an empty path deselects
    void selectPath(const QPainterPath& path, SelectOp op);
    void selectAll();
    void clearSelection();
    void invertSelection();
    void selectLayerContents(int id);   // what the layer shows becomes the selection
    // The selection as a document keeps it: the 8-bit coverage inside its
    // bounds. Setting it is part of opening a document, not an undo step.
    QImage selectionPixels() const;
    void setSelectionPixels(const QRect& bounds, const QImage& pixels);
    // Softens the edge over `radius` canvas pixels. Calls that follow one
    // another with nothing else happening in between adjust the same feather
    // instead of piling up, and undo as one step.
    void featherSelection(float radius);
    // The selected pixels of the active layer (the whole layer without a
    // selection), cropped; `origin` receives where the crop sits on the canvas.
    QImage copyPixels(QPoint* origin);

    // ---- flood: the area of similar colour around a point, to fill it or to
    // select it. It is read from the active layer and, when another layer is
    // marked as the reference, from that one as well, so colour can be dropped
    // inside line art that lives on its own layer. `tolerance` is 0..1: how
    // different a pixel may be and still belong. Between begin and end the
    // tolerance can be changed and the result on screen follows.
    bool beginFlood(FloodKind kind, const QPoint& canvasPos, float tolerance, const QColor& color = QColor(),
                    SelectOp op = SelectOp::Add);
    void setFloodTolerance(float tolerance);
    void endFlood();      // one undo step
    void cancelFlood();
    bool floodActive() const { return m_flood.active; }
    float floodTolerance() const { return m_flood.tolerance; }
    bool floodFill(const QPoint& canvasPos, const QColor& color, float tolerance);
    bool floodSelect(const QPoint& canvasPos, float tolerance, SelectOp op);
    int referenceLayerId() const;       // 0 = none
    void setReferenceLayer(int id);

    // ---- strokes
    bool beginStroke(StrokeTool tool, const BrushPreset& preset, float size, float opacity,
                     const QColor& color, const StrokeSample& sample, quint32 seed = 0);
    void strokeTo(const StrokeSample& sample);
    // Throws away what the stroke has drawn so far and runs the brush along
    // `path` instead (QuickShape). The stroke stays open, so this can be called
    // again and again while a shape is being adjusted.
    void setStrokePath(const std::vector<PathPoint>& path, bool closed);
    void endStroke(const QString& undoName = QString());
    void cancelStroke();
    bool strokeActive() const { return m_stroke.active; }
    quint32 strokeSeed() const { return m_stroke.seed; }
    // Smoothing set-up: how many canvas pixels one screen pixel covers right
    // now, and the amount of stabilisation added to every brush.
    void setInputScale(float canvasPerScreenPx) { m_strokeEngine.setInputScale(canvasPerScreenPx); }

    // ---- a record of what is drawn, for the drawing pad of the brush lab.
    // Every stroke is kept as the pen made it, so that all of them can be
    // drawn again with a brush that has been changed since. `ground` is what
    // the layer holds before the first stroke: something to smudge or erase.
    void setStrokeJournal(bool on);
    int journalSize() const { return int(m_journal.size()); }
    void clearJournal() { m_journal.clear(); }
    void setJournalGround(const QImage& ground) { m_journalGround = ground; }
    void redrawJournal(const BrushPreset& preset);
    void setExtraStabilization(float amount) { m_strokeEngine.setExtraStabilization(amount); }
    QPointF strokeTip() const { return m_strokeEngine.tip(); }
    bool strokeHasString() const { return m_stroke.active && m_strokeEngine.stringActive(); }

    // ---- gradient: laid over the active layer (or its mask) from one colour to
    // another, either of which may be transparent. Between begin and end it can
    // be redrawn as often as its ends move; it respects the selection and
    // alpha lock, and ends as one undo step.
    bool beginGradient();
    void setGradient(const QPointF& from, const QPointF& to, const QColor& first, const QColor& second, GradientKind kind);
    // Through any number of colours (two to twelve), in whatever order they are given.
    void setGradient(const QPointF& from, const QPointF& to, const QVector<GradientStop>& stops, GradientKind kind);
    void endGradient(bool commit);
    bool gradientActive() const { return m_gradient.active; }

    // ---- liquify: the active layer's paint pushed about as if still wet.
    // Everything happens on the graphics card: a field records where each
    // pixel now takes its colour from, and each touch of the brush bends it.
    // The layer itself is untouched until the session ends.
    bool beginLiquify();
    // One movement of the brush from `from` to `to` (the same point for a
    // brush held still). `strength` is 0..1.
    void liquify(LiquifyMode mode, const QPointF& from, const QPointF& to, float radius, float strength);
    void resetLiquify();
    void endLiquify(bool commit);
    bool liquifyActive() const { return m_liquify.active; }
    bool liquifyChanged() const { return m_liquify.changed; }

    // ---- drawing guides
    const Guides& guides() const { return m_guides; }

    // ---- animation
    // While it is enabled the canvas shows the frame the active layer belongs
    // to, with the frames either side of it faded behind it as onion skins.
    const Animation& animation() const { return m_anim; }
    void setAnimation(const Animation& animation);   // marks the document changed; not an undo step
    QVector<int> frames() const;                     // the ids of the frames, bottom to top
    int frameOf(int layerId) const;                  // which frame a layer is part of; -1 for none
    int currentFrame() const { return frameOf(m_activeId); }
    // Makes a frame the current one: its layer (in a group, the topmost layer in it) becomes the active one.
    void showFrame(int index);
    // While playing: that one frame as it plays, without onion skins. -1 goes back to the active layer's.
    void setPlaybackFrame(int index);
    int playbackFrame() const { return m_playFrame; }
    void setFrameHold(int id, int frames);   // an undo step
    int addFrame();                          // an empty layer, as the frame after the current one
    // A frame as it plays, the whole canvas: with the background and
    // foreground frames if they are kept, and without onion skins.
    QImage renderFrame(int index);
    QImage frameThumbnail(int index, int maxDim);
    QPointF symmetryCentre() const;   // canvas pixels
    void setGuides(const Guides& guides);

    // ---- interactive edits: transform the active layer, or filter it
    bool beginTransform();
    void setTransform(const QTransform& canvasTransform, bool smooth = true);
    // Warp, as part of a transform: `bounds` (what the transform started from)
    // is carried by a smooth sheet that passes through `points`, a grid of
    // cols x rows given row by row. Moving a point bends the paint with it.
    void setWarp(int cols, int rows, const std::vector<QPointF>& points, const QRectF& bounds);
    // Where a point of the sheet lies: u runs 0..cols-1 across, v 0..rows-1 down.
    static QPointF warpPoint(int cols, int rows, const std::vector<QPointF>& points, double u, double v);
    bool beginAdjustment(Adjustment type);
    static QString adjustmentName(Adjustment type);
    void setAdjustment(const AdjustParams& params);
    void commitEdit();
    void cancelEdit();
    bool editActive() const { return m_edit.kind != EditKind::None; }

    // ---- history
    History& history() { return m_history; }
    void undo(bool quiet = false);   // quiet: no "Undo ..." message
    void redo(bool quiet = false);

    // ---- output
    void prepareFrame(bool wantMipmaps);   // recomposites whatever changed
    void finish();                         // blocks until the GPU has caught up (benchmarks)
    GLuint resultTexture() const { return m_result.id(); }
    QImage renderImage();
    QByteArray compositePixels();   // the same at full precision: RGBA16F, premultiplied, the whole canvas
    QColor pickColor(const QPoint& canvasPos);
    QImage brushPreview(const BrushPreset& preset, const QSize& size, const QColor& color, float scale);

signals:
    void damaged();
    void guidesChanged();
    void animationChanged();
    void layersChanged();
    void selectionChanged();
    void documentChanged();
    void modifiedChanged(bool modified);
    void message(const QString& text);

private:
    class Scope;
    friend class Scope;
    friend class PixelCommand;
    friend class PropsCommand;
    friend class SelectionCommand;
    friend class CanvasSizeCommand;
    enum class EditKind { None, Transform, Adjust };

    LayerPtr find(int id) const;
    int indexOf(int id) const;
    LayerPtr activeLayer() const { return find(m_activeId); }
    LayerPtr createLayer(const QString& name);
    LayerPtr createGroup(const QString& name);
    LayerPtr cloneLayer(const Layer& src);
    LayerInfo infoOf(const Layer& l) const;
    int depthOf(const Layer& l) const;
    bool isInside(const Layer& l, int groupId) const;
    int blockStart(int index) const;
    bool paintable(const LayerPtr& layer, bool quiet = false);

    // The order of the stack and who is inside which group. Structural edits
    // are undone by putting a whole snapshot back, which cannot go out of step
    // the way index arithmetic can once layers nest.
    struct Structure {
        std::vector<LayerPtr> order;
        std::vector<int> parents;
        int active = 0;
    };
    Structure captureStructure() const;
    void applyStructure(const Structure& s);
    std::unique_ptr<Command> structureCommand(const QString& name, const Structure& before);
    void pushStructure(const QString& name, const Structure& before);
    int insertionPoint(int* parent) const;

    // One layer blended onto a backdrop (a texture, or a flat colour when null).
    struct CompositePass {
        const gfx::Texture* backdrop;
        const float* colour;
        const gfx::Texture* src;
        float opacity;
        int mode;
        const gfx::Texture* mask;
        const gfx::Texture* clip;
        const gfx::Texture* clipMask;
        bool intoBand = false;   // the target is a band buffer, not something the size of the canvas
        const float* tint = nullptr;   // an onion skin's colour, and how far it goes (rgb, amount)
    };
    void drawCompositePass(const CompositePass& pass);
    // The top level of an animation, in the order it is drawn: the background
    // frame, the onion skins (farthest first), the current frame, the foreground frame.
    void animationEntries(const std::function<void(const Layer*, bool clipped, float opacity, const float* tint)>& add) const;
    std::vector<std::vector<const Layer*>> frameParts() const;   // each frame: its layer and the layers clipped to it
    bool isBand(const gfx::Texture* tex) const;
    // `reach`: the only part that needs drawing; everything else comes out as nothing.
    void drawTransform(const gfx::Texture& src, gfx::Texture& target, bool fillWhite, bool throughSelection = true,
                       const QRect* reach = nullptr);
    // `band`: the part of the canvas to put together, no larger than a band buffer.
    const gfx::Texture* compositeLevel(int parentId, int level, const QRect& band, const float background[4]);
    gfx::Texture& bandBuffer(int level, int which);
    gfx::Texture& strokeTarget() { return m_stroke.layer->target(m_stroke.onMask); }
    bool strokeLocked() const { return m_stroke.onMask || m_stroke.layer->alphaLock; }
    void maskCommand(const LayerPtr& layer, std::shared_ptr<gfx::Texture> before, std::shared_ptr<gfx::Texture> after,
                     CompoundCommand* into, const QString& name);
    void applyProps(Layer& l, const LayerInfo& p);
    void changeProps(int id, const std::function<void(LayerInfo&)>& edit, const QString& undoName,
                     const QString& mergeKind);
    QString nextLayerName();

    void allocate(const QSize& size);
    void resizeBuffers(const QSize& size);
    void resample(const gfx::Texture& src, gfx::Texture& dst, QTransform placement, bool fillWhite);
    void damage(const QRect& r);
    void damageAll() { damage(canvasRect()); }
    void recomposite(const QRect& rect);
    QRect tileRect(const QPoint& tile) const;
    void markTiles(const QRect& r);
    std::vector<QPoint> dirtyTileList() const;
    void processDabs();
    void repeatForSymmetry(std::vector<Dab>& dabs) const;
    QRect restartStrokeBuffer();
    void finishStrokeRestart(const QRect& previous);
    // `flat`: one colour (premultiplied RGBA) instead of what the stroke
    // buffer holds; `fullCoverage`: nothing of it held back by the buffer's outline.
    void mergeStroke(const gfx::Texture* layerTex, StrokeBuffer& stroke, gfx::Texture& target, const QRect& rect,
                     const BrushPreset* preset, float opacity, bool erase, bool alphaLock, const float* flat = nullptr,
                     bool fullCoverage = false);
    // A coat of one colour over `reach` of a layer or its mask (or an erasing
    // of it), through the selection: on the live copy first, then for good.
    void coatLayer(const LayerPtr& layer, bool onMask, const QRect& reach, const QColor& ink, bool erase, bool alphaLock,
                   const QString& undoName);
    // Gives back the memory of every block of a layer that turns out to have
    // nothing in it, inside `within`.
    void trim(gfx::Texture& tex, const QRect& within);
    // What a group shows on its own (not its opacity, mode or mask), drawn
    // into `target`, which is wiped first. Returns the part it can be in.
    QRect compositeGroupInto(int groupId, gfx::Texture& target);
    // Where the edit in progress can leave paint.
    QRect editReach() const;
    void releaseTemps();
    // Sees to it that `wanted` more bytes of graphics memory can be had,
    // forgetting the oldest undo steps if that is what it takes. False, with
    // a message for the artist, when even that is not enough.
    bool makeRoom(qint64 wanted);
    qint64 layerBytes() const { return qint64(m_size.width()) * m_size.height() * 8; }
    void fullscreenTo(gfx::Texture& target, const QRect* scissor = nullptr);
    void commitLiveTiles(const LayerPtr& layer, gfx::Texture& source, const std::vector<QPoint>& tiles,
                         const QString& undoName, bool onMask = false);
    // `reach`: the only part of the canvas the new version has anything on.
    void commitLiveFull(const LayerPtr& layer, gfx::Texture& source, const QString& undoName, bool onMask = false,
                        const QRect* reach = nullptr);
    // Makes a layer (or mask) what `source` holds inside `reach`, and nothing
    // outside it; then lets go of whatever blocks came out empty.
    void takeFrom(gfx::Texture& source, gfx::Texture& tex, const QRect& reach);
    QImage downscale(const gfx::Texture& src, const QRect& srcRect, const QSize& target);
    void flipTexture(gfx::Texture& tex, bool horizontal);
    bool busy() const { return m_stroke.active || editActive() || m_flood.active || m_gradient.active || m_liquify.active; }

    // A copy of the selection for undo: only the part that is not empty.
    struct SelSnap {
        QRect bounds;
        QImage pixels;
    };
    using SelSnapPtr = std::shared_ptr<const SelSnap>;
    SelSnapPtr snapshotSelection() const;
    bool sameSelection(const SelSnapPtr& snap) const;
    void loadSelection(const SelSnapPtr& snap);
    void restoreSelection(const SelSnapPtr& snap);
    void selectionEdited();
    void pushSelection(const SelSnapPtr& before, const QString& name, CompoundCommand* into = nullptr);
    void applyPath(const QPainterPath& path, SelectOp op, const QString& undoName);
    QRect floodRegion();
    void showFlood(const QRect& previous);
    const gfx::Texture* selectionMask() const { return hasSelection() ? &m_selMask : nullptr; }
    void renderEdit();

    static inline int s_maxDimension = 16384;
    static inline qint64 s_maxPixels = 100'000'000;
    int m_resolution = 0;
    static inline qint64 s_memoryBytes = qint64(4) << 30;
    static inline bool s_sparse = false;
    static inline qint64 s_leanFrom = -1;
    static inline qint64 s_budgetOverride = -1;

    QOpenGLContext* m_ctx = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    QString m_renderer;

    QSize m_size;
    QColor m_background = Qt::white;
    bool m_backgroundVisible = true;
    bool m_modified = false;
    std::vector<LayerPtr> m_layers;
    int m_activeId = 0;
    int m_nextId = 1;
    int m_nameCounter = 0;

    StrokeBuffer m_strokeBuf;   // dabs of the stroke in progress
    gfx::Texture m_live;        // active layer with the stroke/edit applied
    gfx::Texture m_result;      // composited canvas shown by the viewport
    gfx::Texture m_scratch[2];
    gfx::Texture m_tileTmp;
    gfx::Texture m_occupancy;   // one texel per block: is there anything in it
    gfx::Texture m_heldMap;     // one texel per block: has it memory at all
    gfx::Program m_occupancyProg;
    gfx::Program m_compositeProg, m_mergeProg, m_thumbProg, m_flipProg;
    gfx::Program m_transformProg, m_hsbProg, m_blurProg, m_maskMixProg, m_invertProg, m_floodDistProg, m_adjustProg, m_gradientProg, m_liquifyProg, m_liquifyApplyProg;
    gfx::Texture m_lut;         // 256 x 1, the curves of a Curves adjustment
    // The canvas is composited a band at a time, in buffers of that size:
    // two for the canvas itself and two per group nesting level, made on
    // demand. What they take does not grow with the canvas.
    std::vector<std::unique_ptr<gfx::Texture>> m_bandBufs;
    QPoint m_bandOrigin;        // where on the canvas the band being composited starts
    bool m_maskEditing = false;
    bool m_overlayMask = false;   // m_live stands in for the overlay layer's mask rather than its pixels
    QImage m_selImage;          // the selection: 8-bit coverage, canvas-sized
    QRect m_selBounds;          // where it is not zero; empty = nothing selected
    gfx::Texture m_selMask;     // the same on the GPU (R8)
    gfx::Texture m_selTmp;      // R8 scratch
    gfx::Texture m_floodDist;   // RG8, made when a flood first needs it
    int m_referenceId = 0;
    Guides m_guides;
    gfx::FullscreenPass m_fs;
    DabRenderer m_dabs;
    StrokeEngine m_strokeEngine;
    History m_history;

    QRect m_dirty;
    bool m_mipsStale = true;
    Layer* m_overlay = nullptr;   // layer currently displayed from m_live
    // What m_live is still an exact copy of, after a stroke went in: the next
    // stroke on the same layer need not copy the layer again.
    struct LiveCopy {
        const Layer* layer = nullptr;
        bool onMask = false;
        quint64 version = 0;      // the layer's stamp, or its mask's revision
        quint64 written = 0;      // m_live's own count of writes at that moment
    } m_liveCopy;
    int m_tilesX = 0, m_tilesY = 0;
    std::vector<uint8_t> m_dirtyTiles;

    struct StrokeState {
        bool active = false;
        StrokeTool tool = StrokeTool::Brush;
        LayerPtr layer;
        bool onMask = false;
        BrushPreset preset;
        float opacity = 1.0f;
        float size = 1.0f;
        QColor color;
        quint32 seed = 0;
        // Worked straight into the layer, one dab reading what the last one
        // wrote: the smudge tool, and a wet brush that drags paint along.
        bool smear = false;
        QRect bounds;   // everything the stroke has touched so far
        // Where the smear last was: one entry for the stroke as drawn, and
        // one for each of its repeats under symmetry.
        std::vector<QPointF> smudgePos;
        std::vector<char> hasSmudgePos;
    } m_stroke;

    QHash<quint64, TextInfo> m_texts;   // by the stamp of the pixels that are that text
    int m_textEditId = 0;
    int m_leftOutId = 0;   // a layer kept out of the picture while a reference group is put together
    Animation m_anim;
    int m_playFrame = -1;   // the frame shown while playing, or -1

    struct JournalStroke {
        StrokeTool tool = StrokeTool::Brush;
        float size = 1.0f, opacity = 1.0f;
        QColor color;
        quint32 seed = 0;
        std::vector<StrokeSample> samples;
    };
    bool m_journalOn = false;
    bool m_replaying = false;
    JournalStroke m_journalPending;
    std::vector<JournalStroke> m_journal;
    QImage m_journalGround;

    struct LiquifyState {
        bool active = false;
        bool changed = false;
        LayerPtr layer;
        gfx::Texture field[2];   // RG16F: the offsets, and a second copy to write each step into
        QRect touched;           // everywhere the brush has been
    } m_liquify;
    void showLiquify(const QRect& rect);

    struct GradientState {
        bool active = false;
        bool drawn = false;
        LayerPtr layer;
        bool onMask = false;
    } m_gradient;

    struct FeatherState {
        SelSnapPtr base;                      // the selection before it was softened
        class SelectionCommand* command = nullptr;
        quint64 serial = 0;                   // history serial right after that command went in
    } m_feather;

    struct FloodState {
        bool active = false;
        FloodKind kind = FloodKind::Fill;
        LayerPtr layer;
        bool onMask = false;
        QPoint seed;
        QColor color;
        SelectOp op = SelectOp::Add;
        float tolerance = 0.0f;
        QByteArray apart;              // per pixel: how far the layer, then the reference, is from the seed colour
        bool hasRef = false;
        std::vector<uint8_t> cover;    // the region found, one byte per canvas pixel
        QRect bounds;                  // where `cover` is not zero
        QRect shown;                   // everything the preview has touched
        SelSnapPtr selBefore;
    } m_flood;

    struct EditState {
        EditKind kind = EditKind::None;
        LayerPtr layer;
        Adjustment adjustment = Adjustment::None;
        AdjustParams params;
        QTransform transform;
        bool smooth = true;
        std::vector<QPointF> warp;   // empty unless the transform is a warp
        int warpCols = 0, warpRows = 0;
        QRectF warpBounds;
    } m_edit;
    void drawWarp(const gfx::Texture& src, gfx::Texture& target, bool fillWhite, bool throughSelection, const QRect* reach);
    gfx::Program m_warpProg;
    GLuint m_warpVao = 0, m_warpVbo = 0;
};
