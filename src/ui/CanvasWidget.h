#pragma once

#include "brush/BrushPreset.h"
#include "brush/StrokeEngine.h"
#include "core/Types.h"
#include "engine/CanvasEngine.h"
#include "engine/Text.h"
#include "gfx/Gl.h"
#include "shape/Perspective.h"
#include "shape/QuickShape.h"

#include <QElapsedTimer>
#include <QFont>
#include <QOpenGLWidget>
#include <QPainterPath>
#include <QTimer>
#include <QTransform>

class CanvasEngine;
class QPlainTextEdit;
class ToolState;

// The viewport: shows the composited canvas, turns pen and mouse input into
// strokes, and handles navigation, colour picking and transform handles.
class CanvasWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    CanvasWidget(CanvasEngine* engine, ToolState* tools, QWidget* parent = nullptr);
    ~CanvasWidget() override;

    // ---- view (zoom 1.0 = one canvas pixel per screen pixel)
    double zoom() const { return m_zoom; }
    double rotation() const { return m_rotation; }
    void fitToView();
    // Room taken along the bottom (the animation timeline): fitting the canvas leaves it free.
    void setBottomInset(int px) { m_bottomInset = std::max(0, px); }
    void zoomTo(double zoom);
    void zoomStep(int direction);
    void rotateStep(double degrees);
    void resetRotation();
    void flipView();
    QPointF widgetToCanvas(const QPointF& logical) const;
    QPointF canvasToWidget(const QPointF& canvas) const;

    // ---- eyedropper as a one-shot tool (Alt and right-click always pick)
    void armEyedropper(bool armed);
    bool eyedropperArmed() const { return m_eyedropperArmed; }

    // ---- transform session for the active layer
    bool startTransform();
    void finishTransform(bool commit);
    bool transforming() const { return m_tf.active; }
    void transformFlip(bool horizontal);
    void transformRotate(double degrees);
    void transformFit();
    void transformReset();
    void setTransformUniform(bool uniform);
    bool transformUniform() const { return m_tf.uniform; }
    // Distort: each corner moves on its own, for putting things into perspective.
    void setTransformDistort(bool distort);
    bool transformDistort() const { return m_tf.distort; }
    QVector<QPointF> transformHandlePositions() const;   // widget coordinates, the eight handles
    // Warp: a sheet of 4 x 4 points over the layer; moving one bends the paint around it.
    void setTransformWarp(bool warp);
    bool transformWarp() const { return m_tf.warp; }
    QVector<QPointF> transformWarpPositions() const;     // widget coordinates, row by row

    // ---- QuickShape: rest the pen at the end of a stroke and it snaps to clean
    // geometry. Keep the pen down to resize it; once lifted it can be reopened
    // for editing with handles until something else happens to the canvas.
    enum class ShapeFamily { Other, Ellipse, Quad };
    bool shapeEditOffered() const { return m_shape.offered; }
    bool shapeEditing() const { return m_shape.editing; }
    bool startShapeEdit();
    void finishShapeEdit(bool commit);
    QString shapeName() const { return m_shape.shape.name(); }
    ShapeFamily shapeFamily() const;
    // "ellipse" / "circle", or "quad" / "rectangle" / "square"; empty otherwise.
    QString shapeVariant() const;
    void setShapeVariant(const QString& variant);
    QVector<QPointF> shapeHandlePositions() const;   // widget coordinates
    // The points of the shape being edited. Ctrl + click on the line adds one
    // (and it can be dragged on at once), Ctrl + click on a point takes it
    // out, Alt + click on a point changes it between a smooth point and a
    // sharp corner. A smooth point that has been chosen shows a handle either
    // side of it. The same from the edit bar, for a pen with no keyboard by it.
    int shapePoint() const { return m_shape.editing ? m_shape.node : -1; }   // the chosen one, or -1
    bool shapePointSmooth() const;
    void setShapePointSmooth(bool smooth);
    bool removeShapePoint();                         // the chosen one
    // Joins the ends of the shape being edited, or cuts a closed one open at
    // the chosen point (the first, when none is chosen). The outline does not
    // move when it is opened: the two ends lie together, to be pulled apart.
    bool setShapeClosed(bool closed);
    bool shapeClosed() const { return m_shape.editing && m_shape.shape.closed(); }
    bool shapeCanClose() const;
    bool shapeAddArmed() const { return m_shape.addArmed; }
    void armShapeAdd(bool armed);                    // the next tap on the line adds a point there
    QVector<QPointF> shapeArmPositions() const;      // widget coordinates: the chosen point's handles, in then out
    const QuickShape& editedShape() const { return m_shape.shape; }

    void setQuickShape(bool enabled, int holdMs);
    // Global pen response: raw pressure goes through this curve before any brush sees it.
    void setPressureCurve(const QVector<QPointF>& points);
    QVector<QPointF> pressureCurve() const { return m_pressureCurve; }
    // The drawing pad of the brush lab: the canvas fills the widget at actual
    // size and stays put, rather than being fitted between the bars.
    void setPadMode(bool on)
    {
        m_padMode = on;
        m_fitPending = true;
    }

    // ---- selection mode: drags outline an area instead of painting. The
    // selection itself lives in the engine and stays active after leaving the mode.
    // Automatic picks the area of similar colour under the pen; the others are drawn.
    enum class SelectShape { Automatic, Freehand, Rectangle, Ellipse };
    bool selectMode() const { return m_select.mode; }
    void setSelectMode(bool on);
    SelectShape selectShape() const { return m_select.shape; }
    void setSelectShape(SelectShape shape) { m_select.shape = shape; }
    bool selectSubtract() const { return m_select.subtract; }
    void setSelectSubtract(bool subtract) { m_select.subtract = subtract; }

    void setSelectTolerance(float tolerance) { m_selectTolerance = tolerance; }

    // ---- text: typed straight onto the canvas. A press says where; pressing
    // again drags it about. commitText turns it into pixels and announces them.
    bool textMode() const { return m_text.mode; }
    void setTextMode(bool on);
    bool textPlaced() const { return m_text.placed; }
    QFont textFont() const { return m_text.font; }
    void setTextFont(const QFont& font);
    Qt::Alignment textAlignment() const { return m_text.alignment; }
    void setTextAlignment(Qt::Alignment alignment);
    // Room between letters (a share of the size) and between lines (a multiple of the font's own).
    double textLetterSpacing() const { return m_text.letterSpacing; }
    double textLineSpacing() const { return m_text.lineSpacing; }
    void setTextSpacing(double letters, double lines);
    QString textContent() const;
    bool commitText();
    // Text that is already on a layer, opened to be retyped. Tapping it with
    // the text tool does the same.
    bool editTextLayer(int layerId);
    int textEditLayer() const { return m_text.editLayer; }
    // Everything about the text as it stands, for whoever makes the layer of it.
    TextInfo textInfo() const;

    // ---- liquify: a session on the active layer, ended with apply or cancel
    bool startLiquify();
    void finishLiquify(bool commit);
    bool liquifying() const { return m_liquify.active; }
    LiquifyMode liquifyMode() const { return m_liquify.mode; }
    void setLiquifyMode(LiquifyMode mode) { m_liquify.mode = mode; }
    float liquifySize() const { return m_liquify.size; }           // diameter, canvas px
    void setLiquifySize(float px);
    // The size as a slider position, 0..1: finer steps at the small end.
    static constexpr float kLiquifyMinSize = 20.0f, kLiquifyMaxSize = 5000.0f;
    float liquifySizeFraction() const;
    void setLiquifySizeFraction(float t);
    float liquifyStrength() const { return m_liquify.strength; }   // 0..1
    void setLiquifyStrength(float strength) { m_liquify.strength = strength; }

    // ---- gradient mode: a drag lays a gradient from where it starts to where it ends
    bool gradientMode() const { return m_gradient.mode; }
    void setGradientMode(bool on);
    GradientKind gradientKind() const { return m_gradient.kind; }
    void setGradientKind(GradientKind kind) { m_gradient.kind = kind; }
    // Fades out to nothing, or runs into a second colour.
    bool gradientToTransparent() const { return m_gradient.toTransparent; }
    void setGradientToTransparent(bool on) { m_gradient.toTransparent = on; }
    void setGradientSecondColor(const QColor& c) { m_gradient.second = c; }
    // A gradient of its own colours instead: as many stops as wanted, each where it is put.
    bool gradientCustom() const { return m_gradient.custom; }
    void setGradientCustom(bool on) { m_gradient.custom = on; }
    QVector<GradientStop> gradientStops() const { return m_gradient.stops; }
    void setGradientStops(const QVector<GradientStop>& stops) { m_gradient.stops = stops; }

    // How long since the pen or mouse last did anything here, in milliseconds.
    qint64 idleMs() const { return m_clock.elapsed() - m_lastPointerMs; }

    // ---- crop: a frame over the canvas. It may reach past the edge to add room.
    bool startCrop();
    void finishCrop(bool commit);
    bool cropping() const { return m_crop.active; }
    QRect cropRect() const { return m_crop.rect; }
    void setCropRect(const QRect& rect);
    QVector<QPointF> cropHandlePositions() const;   // widget coordinates

    // ---- guides: while they are being arranged, their points can be dragged
    void setGuideEditing(bool on);
    bool guideEditing() const { return m_guideEdit; }
    QVector<QPointF> guideHandlePositions() const;   // widget coordinates
    // Symmetry's own two points, while it is on: its centre, and the one that turns it.
    QVector<QPointF> symmetryHandlePositions() const;

    // ---- fill: armed for one click, or dropped from the colour chip. Either
    // way, keeping the pen down and sliding sideways changes how far the
    // colour spreads before it is let go.
    bool fillArmed() const { return m_fillArmed; }
    void armFill(bool armed);
    bool fillAt(const QPointF& widgetPos);
    void setFillTolerance(float tolerance) { m_fillTolerance = tolerance; }
    // The colour chip being dragged about: `allowed` is false over the bars.
    // Resting it on the canvas starts the fill there with the threshold live.
    void dropMove(const QPointF& widgetPos, bool allowed);
    bool dropRelease(const QPointF& widgetPos, bool allowed);
    // A fill or automatic selection whose threshold is still being adjusted.
    bool floodAdjusting() const;
    void endFloodDrag(bool commit);

signals:
    void viewChanged();
    void eyedropperChanged(bool armed);
    void selectModeChanged(bool on);
    void fillArmedChanged(bool armed);
    void fillToleranceChanged(float tolerance);
    void selectToleranceChanged(float tolerance);
    void thresholdShown();   // the threshold readout has taken the place a message would use
    void transformFinished();
    void toast(const QString& text);
    void cropChanged();
    void cropFinished();
    void gradientModeChanged(bool on);
    void liquifyFinished();
    void liquifySizeChanged();
    void textModeChanged(bool on);
    void textChanged();
    void textCommitted(const TextInfo& info, const QString& name);   // new text, to become a layer
    void textStyleChanged();   // another text was opened: its font, size and alignment are the current ones
    void shapeEditOfferChanged(bool offered);
    void shapeEditStarted();
    void shapeEditChanged();
    void shapeEditFinished();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void tabletEvent(QTabletEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    struct PointerInput {
        QPointF pos;   // logical widget coordinates
        float pressure = 1.0f;
        float tiltX = 0.0f, tiltY = 0.0f;
        Qt::MouseButton button = Qt::NoButton;
        Qt::KeyboardModifiers mods;
        bool eraserEnd = false;
    };
    enum class Drag { None, Stroke, Pan, Zoom, Rotate, Pick, Transform, Shape, Select, Flood, Size, Guide, Crop, Gradient, Liquify, Text };

    void pointerPress(const PointerInput& in);
    void pointerMove(const PointerInput& in);
    void pointerRelease(const PointerInput& in);
    StrokeSample makeSample(const PointerInput& in) const;
    void pickAt(const QPointF& logical);
    void strokeTick();
    void updateCursorShape();
    void destroyGL();

    QTransform viewTransform() const;   // canvas px -> device px
    QPointF toDevice(const QPointF& logical) const { return logical * devicePixelRatioF(); }
    void zoomAround(double newZoom, const QPointF& deviceAnchor);
    void rotateAround(double degrees, const QPointF& deviceAnchor);

    void selectPress(const PointerInput& in);
    void selectMove(const PointerInput& in);
    void selectRelease(const PointerInput& in);
    QPainterPath selectDragPath() const;
    void drawSelectionOverlay(QPainter& p);
    void drawGuides(QPainter& p);
    void cropPress(const PointerInput& in);
    void cropMove(const PointerInput& in);
    void drawCropOverlay(QPainter& p);
    int hitGuideHandle(const QPointF& logical) const;
    void dragGuideHandle(const QPointF& logical, Qt::KeyboardModifiers mods = Qt::NoModifier);
    bool assist(PointerInput& in, bool decideNow = false);
    bool beginFloodDrag(const QPointF& widgetPos, bool select, bool subtract, bool fromDrop);
    void moveFloodDrag(const QPointF& widgetPos);
    void drawThreshold(QPainter& p);

    void beginShapeCapture(const PointerInput& in, StrokeTool tool);
    void captureShapePoint(const PointerInput& in);
    void trySnapShape();
    void adjustHeldShape(const PointerInput& in);
    void renderShape();
    void buildShapeDynamics();
    void anchorShapeStart(const QPointF& start, double winding);
    std::vector<PathPoint> shapeStrokePath() const;
    void shapePress(const PointerInput& in);
    void shapeRelease();
    void replaceShape(const QuickShape& shape);
    int shapeNodeFor(int handle);
    int addShapePoint(const QPointF& logical);
    bool removeShapePoint(int handle);
    void toggleShapePoint(int handle);
    void dragShapeHandle(const PointerInput& in);
    int hitShapeHandle(const QPointF& logical) const;
    QPointF shapeKnobPos() const;
    void offerShapeEdit();
    void withdrawShapeOffer();
    void checkShapeOffer();
    void drawShapeOverlay(QPainter& p);

    QTransform transformMatrix() const;
    QPointF handleCanvasPos(int handle) const;
    QPointF rotateKnobPos() const;   // logical widget coordinates
    int hitHandle(const QPointF& logical) const;
    void applyTransform();
    void drawOverlay(QPainter& p);

    CanvasEngine* m_engine;
    ToolState* m_tools;
    gfx::Program m_present;
    gfx::FullscreenPass m_fs;
    bool m_glReady = false;

    double m_zoom = 1.0;       // device px per canvas px
    double m_rotation = 0.0;   // degrees
    QPointF m_pan;             // device px position of the canvas centre
    bool m_flip = false;
    bool m_fitPending = true;
    QSize m_lastSize;

    Drag m_drag = Drag::None;
    QPointF m_dragStart, m_panStart;   // device px
    double m_zoomStart = 1.0, m_rotStart = 0.0;
    bool m_space = false, m_alt = false, m_shift = false;
    bool m_eyedropperArmed = false;
    QPointF m_cursor;
    bool m_cursorInside = false;
    QColor m_pickPrev;

    QElapsedTimer m_clock;
    QTimer m_strokeTimer;
    PointerInput m_lastInput;
    qint64 m_lastInputMs = 0;
    qint64 m_lastPointerMs = 0;

    // Everything needed to draw the same stroke again along a different path.
    // How the pen behaved along a stroke, resampled at even steps of its
    // length. QuickShape replays it along the clean outline so the line keeps
    // the weight it was drawn with.
    struct StrokeDynamics {
        std::vector<float> pressure, tiltX, tiltY, speed;
        static float at(const std::vector<float>& values, double t);
    };

    struct StrokeRecipe {
        StrokeTool tool = StrokeTool::Brush;
        BrushPreset preset;
        float size = 1.0f, opacity = 1.0f;
        QColor color;
        quint32 seed = 0;
        StrokeDynamics dynamics;
        // Closed outlines: where along the shape's own path the stroke began
        // (0..1), and whether it ran the other way round.
        double startParam = 0.0;
        bool reversed = false;
    };

    struct ShapeState {
        // Gathered while an ordinary stroke is drawn.
        std::vector<QPointF> points;     // canvas px
        std::vector<float> pressures, tiltsX, tiltsY;   // pen state at each point
        std::vector<double> times;       // seconds
        double travelled = 0.0;          // widget px
        QPointF lastPos, holdAnchor;     // widget px
        qint64 holdSince = 0;
        bool holdTried = false;

        bool snapped = false;            // pen still down, stroke replaced by the shape
        bool adjusting = false;          // ...and the pen has moved far enough to resize it
        bool editing = false;            // edit mode with handles
        QuickShape shape;
        QuickShape atGrab;               // as recognised, before the pen moved again
        QuickShape original;             // what Cancel restores
        QuickShape lastFree;             // proportions before Circle/Square was pressed
        QPointF grab, grabPos;           // pen at the snap: canvas px, widget px
        StrokeRecipe recipe;

        int handle = -1;
        int node = -1;                   // the point last touched: the one the edit bar acts on
        bool addArmed = false;           // the next tap on the line adds a point
        QPointF startMouse;
        QPointF pressPos;                // widget px
        bool pressMoved = false;
        int toggleOnRelease = -1;        // Alt went down on this point; a tap changes it, a drag moves it
        QuickShape startShape;
        QString snapLabel;               // "Circle", "45°", ... while a snap is engaged

        bool offered = false;            // the committed shape can still be reopened
        quint64 offerSerial = 0;
        int offerLayer = 0;
    } m_shape;

    // A press on the shape just drawn: if it turns out to be a tap, the shape
    // is reopened for editing instead of getting a dot.
    struct ShapeTap {
        bool candidate = false, moved = false;
        QPointF at;          // widget px
        qint64 since = 0;
        ShapeState saved;    // the offer as it stood, before the new stroke began
    } m_shapeTap;
    bool nearOfferedShape(const QPointF& logical) const;

    struct SelectState {
        bool mode = false;
        SelectShape shape = SelectShape::Freehand;
        bool subtract = false;
        std::vector<QPointF> points;   // canvas px, freehand outline so far
        QPointF start, current;        // canvas px
        QPointF lastPos;               // widget px
    } m_select;
    QTimer m_antsTimer;
    int m_antsPhase = 0;

    struct CropSession {
        bool active = false;
        QRect rect;          // canvas px; may extend beyond the canvas
        int handle = -1;     // 0..7 clockwise from the top left, 8 = the whole frame
        QPointF startMouse;
        QRect startRect;
    } m_crop;
    struct GradientMode {
        bool mode = false;
        GradientKind kind = GradientKind::Linear;
        bool toTransparent = true;
        QColor second = Qt::white;
        bool custom = false;
        QVector<GradientStop> stops;
        QPointF from, to;   // canvas px, of the drag in progress
    } m_gradient;
    void applyGradient();
    struct TextSession {
        bool mode = false;
        bool placed = false;       // the pen has said where it goes
        QPlainTextEdit* editor = nullptr;
        QFont font;
        Qt::Alignment alignment = Qt::AlignLeft;
        double letterSpacing = 0.0, lineSpacing = 1.0;
        QPointF anchor;            // canvas px: top of the first line, at the side the alignment names
        QPointF grab;              // anchor minus pen, while it is dragged
        QTimer blink;
        bool caretOn = true;
        int editLayer = 0;         // the text layer being retyped, 0 for new text
        QTransform matrix;         // how that layer's text has been moved, sized and turned since it was typed
    } m_text;
    // The pen reports its place a couple of hundred times a second. A shape
    // that follows it is drawn again once the reports at hand have all been
    // read, from the newest of them, rather than once for each.
    void queueShapeUpdate(const PointerInput& in, bool handle);
    void flushShapeUpdate();
    QTimer m_shapeRedraw;
    PointerInput m_shapePending;
    bool m_shapePendingAdjust = false, m_shapePendingHandle = false;
    QVector<TextLine> textLines(QRectF* bounds, double* lineHeight) const;
    QPointF toTextSpace(const QPointF& widgetPos) const;
    void textPress(const PointerInput& in);
    void textMove(const PointerInput& in);
    void drawTextOverlay(QPainter& p);

    struct LiquifySession {
        bool active = false;
        LiquifyMode mode = LiquifyMode::Push;
        float size = 240.0f;
        float strength = 0.6f;
        QPointF last;        // canvas px
        float pressure = 1.0f;
    } m_liquify;
    QTimer m_liquifyTimer;
    void liquifyPress(const PointerInput& in);
    void liquifyMove(const PointerInput& in);
    void liquifyTouch(const QPointF& canvasPos);
    void drawLiquifyCursor(QPainter& p);
    bool m_guideEdit = false;
    int m_guideHandle = -1;
    // A stroke being held to a perspective line.
    struct Assist {
        bool active = false, locked = false;
        QPointF start, startWidget;
        perspective::Constraint path;
    } m_assist;
    // Shift + drag: the brush grows and shrinks around where the drag began.
    QPointF m_sizeAnchor;
    float m_sizeStart = 0.0f;
    bool m_sizingLiquify = false;   // the drag is sizing the liquify brush rather than the paint brush

    bool m_fillArmed = false;
    float m_fillTolerance = 0.12f;
    float m_selectTolerance = 0.12f;
    QPointF m_dropPos, m_dropAnchor;
    bool m_dropVisible = false;
    bool m_dropTracking = false;
    QTimer m_dropHold;

    struct FloodDrag {
        bool select = false;        // an automatic selection rather than a fill
        bool fromDrop = false;      // started by resting the colour chip on the canvas
        bool adjusting = false;     // has slid far enough sideways to move the threshold
        QPointF start;              // widget px
        float startTolerance = 0.0f;
    } m_flood;
    bool m_quickShapeEnabled = true;
    int m_holdMs = 600;
    QVector<QPointF> m_pressureCurve;   // empty = straight through
    bool m_padMode = false;
    float curved(float pressure) const;

    struct TransformSession {
        bool active = false;
        QRectF bounds;                 // layer content at the start, canvas px
        QPointF center;                // where that content's centre is now
        double sx = 1.0, sy = 1.0, rot = 0.0;
        bool uniform = true;
        bool distort = false;
        bool warp = false;
        std::vector<QPointF> warpPts, startWarp;   // canvas px, row by row
        QPointF corner[4], startCorner[4];   // top left, top right, bottom right, bottom left (canvas px)
        int handle = -1;               // 0..7 scale, 8 rotate, 9 move
        QPointF startMouse, startCenter;
        double startSx = 1.0, startSy = 1.0, startRot = 0.0;
    } m_tf;
    int m_bottomInset = 0;
};
