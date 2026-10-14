#include "ui/CanvasWidget.h"

#include "engine/CanvasEngine.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPainterPath>
#include <QTabletEvent>
#include <QWheelEvent>
#include <cmath>

using gfx::gl;

namespace {
constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;
constexpr double kPi = 3.14159265358979323846;
// Sign of each scale handle relative to the box centre, clockwise from top-left.
constexpr int kHandleX[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
constexpr int kHandleY[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
constexpr int kRotateHandle = 8;
constexpr int kMoveHandle = 9;
constexpr int kWarpHandle = 100;   // first of the warp sheet's points
constexpr int kWarpSide = 4;       // points along each side of the sheet

// Corners that still make a proper four-sided shape, going round the same way
// as the layer does: anything else has no sensible picture to show.
bool properQuad(const QPointF c[4], bool clockwise)
{
    for (int i = 0; i < 4; ++i) {
        const QPointF a = c[(i + 1) % 4] - c[i], b = c[(i + 2) % 4] - c[(i + 1) % 4];
        const double turn = a.x() * b.y() - a.y() * b.x();
        if ((clockwise ? turn : -turn) < 4.0)
            return false;
    }
    return true;
}
double quadTurn(const QPointF c[4])
{
    const QPointF a = c[1] - c[0], b = c[2] - c[1];
    return a.x() * b.y() - a.y() * b.x();
}

QPointF rotated(const QPointF& p, double degrees)
{
    const double a = degrees * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {c * p.x() - s * p.y(), s * p.x() + c * p.y()};
}
} // namespace

CanvasWidget::CanvasWidget(CanvasEngine* engine, ToolState* tools, QWidget* parent)
    : QOpenGLWidget(parent), m_engine(engine), m_tools(tools)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setTabletTracking(true);
    setCursor(Qt::BlankCursor);
    m_clock.start();

    m_shapeRedraw.setSingleShot(true);
    m_shapeRedraw.setInterval(0);
    connect(&m_shapeRedraw, &QTimer::timeout, this, &CanvasWidget::flushShapeUpdate);
    m_strokeTimer.setInterval(8);
    connect(&m_strokeTimer, &QTimer::timeout, this, &CanvasWidget::strokeTick);
    connect(m_engine, &CanvasEngine::damaged, this, qOverload<>(&QWidget::update));
    connect(&m_engine->history(), &History::changed, this, &CanvasWidget::checkShapeOffer);
    connect(m_engine, &CanvasEngine::layersChanged, this, &CanvasWidget::checkShapeOffer);
    connect(m_engine, &CanvasEngine::documentChanged, this, [this] {
        withdrawShapeOffer();
        m_shape = {};
        m_tf.active = false;
        m_fitPending = true;
        update();
    });
    connect(m_tools, &ToolState::sizeChanged, this, qOverload<>(&QWidget::update));

    // Marching ants only need a timer while there is something to outline.
    m_antsTimer.setInterval(130);
    connect(&m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsPhase = (m_antsPhase + 1) % 8;
        update();
    });
    connect(m_engine, &CanvasEngine::selectionChanged, this, [this] {
        if (m_engine->hasSelection())
            m_antsTimer.start();
        else
            m_antsTimer.stop();
        update();
    });

    m_liquifyTimer.setInterval(33);
    connect(&m_liquifyTimer, &QTimer::timeout, this, [this] {
        if (m_drag == Drag::Liquify)
            liquifyTouch(m_liquify.last);
        else
            m_liquifyTimer.stop();
    });

    // The colour chip resting on the canvas: pour, and let the threshold be tuned.
    m_dropHold.setSingleShot(true);
    m_dropHold.setInterval(550);
    connect(&m_dropHold, &QTimer::timeout, this, [this] {
        if (!m_dropVisible || !beginFloodDrag(m_dropPos, false, false, true))
            return;
        m_dropVisible = false;
        m_flood.adjusting = true;
        emit thresholdShown();
        update();
    });
}

CanvasWidget::~CanvasWidget()
{
    destroyGL();
}

void CanvasWidget::destroyGL()
{
    if (!m_glReady)
        return;
    makeCurrent();
    m_present.destroy();
    m_fs.destroy();
    doneCurrent();
    m_glReady = false;
}

// -------------------------------------------------------------------- view

QTransform CanvasWidget::viewTransform() const
{
    const QSize cs = m_engine->canvasSize();
    QTransform t;
    t.translate(m_pan.x(), m_pan.y());
    t.rotate(m_rotation);
    t.scale(m_flip ? -m_zoom : m_zoom, m_zoom);
    t.translate(-cs.width() / 2.0, -cs.height() / 2.0);
    return t;
}

QPointF CanvasWidget::widgetToCanvas(const QPointF& logical) const
{
    return viewTransform().inverted().map(toDevice(logical));
}

QPointF CanvasWidget::canvasToWidget(const QPointF& canvas) const
{
    return viewTransform().map(canvas) / devicePixelRatioF();
}

void CanvasWidget::fitToView()
{
    const QSize cs = m_engine->canvasSize();
    if (cs.isEmpty() || width() <= 0 || height() <= 0)
        return;
    const double dpr = devicePixelRatioF();
    const double w = width() * dpr, h = height() * dpr;
    if (m_padMode) {
        m_zoom = 1.0;
        m_rotation = 0.0;
        m_flip = false;
        m_pan = QPointF(w / 2.0, h / 2.0);
        m_fitPending = false;
        emit viewChanged();
        update();
        return;
    }
    // Leave room for the top bar and sidebar so the canvas isn't hidden under them.
    const double availW = std::max(64.0, w - 150.0 * dpr), availH = std::max(64.0, h - 110.0 * dpr);
    m_zoom = std::clamp(std::min(availW / cs.width(), availH / cs.height()), kMinZoom, kMaxZoom);
    if (m_zoom > 1.0)
        m_zoom = 1.0;
    m_rotation = 0.0;
    m_flip = false;
    m_pan = QPointF(w / 2.0, h / 2.0 + 20.0 * dpr);
    m_fitPending = false;
    emit viewChanged();
    update();
}

void CanvasWidget::zoomAround(double newZoom, const QPointF& anchor)
{
    newZoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
    const double k = newZoom / m_zoom;
    m_pan = anchor + (m_pan - anchor) * k;
    m_zoom = newZoom;
    emit viewChanged();
    update();
}

void CanvasWidget::rotateAround(double degrees, const QPointF& anchor)
{
    m_pan = anchor + rotated(m_pan - anchor, degrees);
    m_rotation = std::fmod(m_rotation + degrees, 360.0);
    emit viewChanged();
    update();
}

void CanvasWidget::zoomTo(double zoom)
{
    zoomAround(zoom, toDevice(QPointF(width() / 2.0, height() / 2.0)));
}

void CanvasWidget::zoomStep(int direction)
{
    zoomTo(m_zoom * (direction > 0 ? 1.25 : 0.8));
}

void CanvasWidget::rotateStep(double degrees)
{
    rotateAround(degrees, toDevice(QPointF(width() / 2.0, height() / 2.0)));
}

void CanvasWidget::resetRotation()
{
    rotateStep(-m_rotation);
    m_rotation = 0.0;
}

void CanvasWidget::flipView()
{
    // Mirror the whole view about the vertical centre line of the viewport.
    const double cx = width() * devicePixelRatioF() / 2.0;
    m_flip = !m_flip;
    m_rotation = -m_rotation;
    m_pan.setX(2.0 * cx - m_pan.x());
    emit viewChanged();
    update();
}

// --------------------------------------------------------------- rendering

void CanvasWidget::initializeGL()
{
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &CanvasWidget::destroyGL, Qt::UniqueConnection);
    if (!gl()) {
        qCritical("CanvasWidget: no OpenGL 4.3 core context");
        return;
    }
    m_present.build({QStringLiteral(":/shaders/fullscreen.vert")}, {QStringLiteral(":/shaders/present.frag")});
    m_fs.init();
    m_glReady = true;
}

void CanvasWidget::resizeGL(int w, int h)
{
    const QSize now(w, h);
    if (m_fitPending || !m_lastSize.isValid()) {
        fitToView();
    } else {
        const double dpr = devicePixelRatioF();
        m_pan += QPointF((now.width() - m_lastSize.width()) * dpr / 2.0, (now.height() - m_lastSize.height()) * dpr / 2.0);
    }
    m_lastSize = now;
}

void CanvasWidget::paintGL()
{
    if (!m_glReady)
        return;
    if (m_fitPending)
        fitToView();

    // The engine recomposites in its own context, then gives ours back.
    m_engine->prepareFrame(m_zoom < 1.0);
    makeCurrent();

    gfx::GLF* f = gl();
    const double dpr = devicePixelRatioF();
    const int vw = int(std::lround(width() * dpr)), vh = int(std::lround(height() * dpr));
    f->glViewport(0, 0, vw, vh);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glDisable(GL_DEPTH_TEST);
    f->glDisable(GL_STENCIL_TEST);
    f->glDisable(GL_CULL_FACE);
    f->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const QColor ws = theme::workspace;
    f->glClearColor(ws.redF(), ws.greenF(), ws.blueF(), 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    if (m_engine->hasDocument()) {
        const QSize cs = m_engine->canvasSize();
        float m[9];
        gfx::toMat3(viewTransform().inverted(), m);
        m_present.use();
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, m_engine->resultTexture());
        m_present.set("uCanvas", 0);
        m_present.setMat3("uViewToCanvas", m);
        m_present.set("uCanvasSize", float(cs.width()), float(cs.height()));
        m_present.set("uViewport", float(vw), float(vh));
        m_present.set("uZoom", float(m_zoom));
        m_present.set("uWorkspace", ws.redF(), ws.greenF(), ws.blueF());
        m_present.set("uChecker", m_engine->backgroundVisible() ? 0 : 1);
        // The selection is outlined here rather than with a painter, so it
        // costs the same however intricate or soft it is. It stays out of
        // sight while a transform is moving what it surrounds.
        const bool outline = m_engine->hasSelection() && !m_engine->editActive();
        f->glActiveTexture(GL_TEXTURE1);
        f->glBindTexture(GL_TEXTURE_2D, outline ? m_engine->selectionTexture() : m_engine->resultTexture());
        f->glActiveTexture(GL_TEXTURE0);
        m_present.set("uSelection", 1);
        m_present.set("uHasSelection", outline ? 1 : 0);
        m_present.set("uAntsPhase", float(m_antsPhase));
        m_present.set("uSelDim", m_select.mode ? 0.16f : 0.0f);
        m_fs.draw();
        f->glBindVertexArray(0);
        f->glUseProgram(0);
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    drawOverlay(p);
}

void CanvasWidget::drawOverlay(QPainter& p)
{
    if (!m_engine->hasDocument())
        return;

    if (m_tf.active && m_tf.warp) {
        // The sheet's own lines, then its points.
        auto curve = [&](bool across, int index) {
            QPolygonF line;
            for (int k = 0; k <= (kWarpSide - 1) * 10; ++k) {
                const double s = k / 10.0;
                line << canvasToWidget(CanvasEngine::warpPoint(kWarpSide, kWarpSide, m_tf.warpPts, across ? s : index, across ? index : s));
            }
            return line;
        };
        p.setBrush(Qt::NoBrush);
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(pass == 0 ? QPen(QColor(0, 0, 0, 100), 2.6) : QPen(theme::accent, 1.2));
            for (int i = 0; i < kWarpSide; ++i) {
                p.drawPolyline(curve(true, i));
                p.drawPolyline(curve(false, i));
            }
        }
        for (int i = 0; i < int(m_tf.warpPts.size()); ++i) {
            p.setBrush(m_tf.handle == kWarpHandle + i ? QColor(Qt::white) : theme::accent);
            p.setPen(QPen(Qt::white, 1.5));
            p.drawEllipse(canvasToWidget(m_tf.warpPts[size_t(i)]), 5.5, 5.5);
        }
    } else if (m_tf.active) {
        QPolygonF box;
        for (int h : {0, 2, 4, 6})
            box << canvasToWidget(handleCanvasPos(h));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 110), 3));
        p.drawPolygon(box);
        p.setPen(QPen(theme::accent, 1.5));
        p.drawPolygon(box);

        if (!m_tf.distort) {
            const QPointF knob = rotateKnobPos();
            const QPointF topMid = canvasToWidget(handleCanvasPos(1));
            p.drawLine(topMid, knob);
            p.setPen(QPen(QColor(0, 0, 0, 120), 1));
            p.setBrush(QColor(0x34, 0xc7, 0x59));
            p.drawEllipse(knob, 6.5, 6.5);
        }
        for (int h = 0; h < 8; ++h) {
            p.setBrush(h == m_tf.handle ? QColor(Qt::white) : theme::accent);
            p.setPen(QPen(Qt::white, 1.5));
            p.drawEllipse(canvasToWidget(handleCanvasPos(h)), 5.5, 5.5);
        }
    }

    drawGuides(p);
    drawCropOverlay(p);
    drawLiquifyCursor(p);
    drawTextOverlay(p);
    drawSelectionOverlay(p);
    drawShapeOverlay(p);

    if (m_drag == Drag::Stroke && !m_shape.snapped && m_engine->strokeHasString()) {
        // The pull string: from where the line ends to where the pen is.
        const QPointF tip = canvasToWidget(m_engine->strokeTip());
        p.setPen(QPen(QColor(0, 0, 0, 110), 2.4));
        p.drawLine(tip, m_cursor);
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.0));
        p.drawLine(tip, m_cursor);
        p.setBrush(Qt::white);
        p.drawEllipse(tip, 2.5, 2.5);
    }

    if (m_drag == Drag::Pick) {
        // Procreate-style loupe ring: new colour on top, previous colour below.
        const QRectF ring(m_cursor.x() - 46, m_cursor.y() - 46, 92, 92);
        p.setPen(QPen(QColor(0, 0, 0, 90), 22, Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ring);
        p.setPen(QPen(m_tools->color(), 18, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, 0, 180 * 16);
        p.setPen(QPen(m_pickPrev, 18, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, 180 * 16, 180 * 16);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawEllipse(m_cursor, 3, 3);
    } else if (m_cursorInside && !m_tf.active && !m_shape.editing && !m_crop.active && !m_liquify.active && !m_text.mode && !m_select.mode && !m_gradient.mode && !m_space
               && !m_engine->editActive()
               && (m_drag == Drag::None || m_drag == Drag::Stroke || m_drag == Drag::Flood)) {
        if (m_fillArmed) {
            // A drop of the colour that is about to be poured.
            p.setPen(QPen(Qt::white, 2));
            p.setBrush(m_tools->color());
            p.drawEllipse(m_cursor + QPointF(10, -10), 8, 8);
            p.setPen(QPen(QColor(0, 0, 0, 170), 2.4));
            p.drawLine(m_cursor - QPointF(4, 0), m_cursor + QPointF(4, 0));
            p.drawLine(m_cursor - QPointF(0, 4), m_cursor + QPointF(0, 4));
        } else if (m_alt || m_eyedropperArmed) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 3));
            p.drawEllipse(m_cursor, 7, 7);
            p.setPen(QPen(Qt::white, 1.5));
            p.drawEllipse(m_cursor, 7, 7);
        } else {
            const double r = m_tools->size() * m_zoom / devicePixelRatioF() / 2.0;
            if (r >= 2.5) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0, 130), 2.2));
                p.drawEllipse(m_cursor, r, r);
                p.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
                p.drawEllipse(m_cursor, r, r);
            } else {
                p.setPen(QPen(QColor(0, 0, 0, 150), 2.4));
                p.drawLine(m_cursor - QPointF(5, 0), m_cursor + QPointF(5, 0));
                p.drawLine(m_cursor - QPointF(0, 5), m_cursor + QPointF(0, 5));
                p.setPen(QPen(Qt::white, 1.0));
                p.drawLine(m_cursor - QPointF(5, 0), m_cursor + QPointF(5, 0));
                p.drawLine(m_cursor - QPointF(0, 5), m_cursor + QPointF(0, 5));
            }
        }
    }

    if (m_drag == Drag::Gradient) {
        // The line the gradient runs along: full colour at the ring, the far colour at the dot.
        const QPointF a = canvasToWidget(m_gradient.from), b = canvasToWidget(m_gradient.to);
        p.setPen(QPen(QColor(0, 0, 0, 120), 3));
        p.drawLine(a, b);
        p.setPen(QPen(Qt::white, 1.3));
        p.drawLine(a, b);
        p.setBrush(m_tools->color());
        p.setPen(QPen(Qt::white, 2));
        p.drawEllipse(a, 7, 7);
        p.setBrush(m_gradient.toTransparent ? QColor(255, 255, 255, 60) : m_gradient.second);
        p.drawEllipse(b, 5, 5);
    }
    if (m_drag == Drag::Size) {
        // The brush as it will be, where the drag began, with its size beside it.
        const float diameter = m_sizingLiquify ? m_liquify.size : m_tools->size();
        const double r = std::max(1.5, diameter * m_zoom / devicePixelRatioF() / 2.0);
        p.setBrush(QColor(theme::accent.red(), theme::accent.green(), theme::accent.blue(), 46));
        p.setPen(QPen(QColor(0, 0, 0, 130), 2.4));
        p.drawEllipse(m_sizeAnchor, r, r);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawEllipse(m_sizeAnchor, r, r);
        const QString label = tr("%1 px").arg(diameter < 10.0f ? QString::number(diameter, 'f', 1) : QString::number(qRound(diameter)));
        p.setFont(theme::font(12, QFont::DemiBold));
        const QRectF tag(m_sizeAnchor.x() - 36, m_sizeAnchor.y() - r - 34, 72, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(28, 28, 30, 230));
        p.drawRoundedRect(tag, 12, 12);
        p.setPen(Qt::white);
        p.drawText(tag, Qt::AlignCenter, label);
    }
    if (m_dropVisible) {
        p.setPen(QPen(Qt::white, 2.5));
        p.setBrush(m_tools->color());
        p.drawEllipse(m_dropPos, 15, 15);
    }
    drawThreshold(p);

    // Zoom / rotation readout.
    QString status = QStringLiteral("%1%").arg(qRound(m_zoom * 100.0));
    if (std::abs(m_rotation) > 0.05)
        status += QStringLiteral("   %1°").arg(m_rotation, 0, 'f', 0);
    if (m_flip)
        status += tr("   mirrored");
    p.setFont(theme::font(11));
    p.setPen(QColor(255, 255, 255, 110));
    p.drawText(rect().adjusted(0, 0, -14, -10), Qt::AlignRight | Qt::AlignBottom, status);
}

// ------------------------------------------------------------------- input

void CanvasWidget::setPressureCurve(const QVector<QPointF>& points)
{
    m_pressureCurve = points;
}

float CanvasWidget::curved(float pressure) const
{
    return m_pressureCurve.isEmpty() ? pressure : CurveEditor::evaluate(m_pressureCurve, pressure);
}

StrokeSample CanvasWidget::makeSample(const PointerInput& in) const
{
    StrokeSample s;
    s.pos = widgetToCanvas(in.pos);
    s.pressure = curved(in.pressure);
    s.tiltX = in.tiltX;
    s.tiltY = in.tiltY;
    s.time = m_clock.nsecsElapsed() * 1e-9;
    return s;
}

void CanvasWidget::tabletEvent(QTabletEvent* e)
{
    PointerInput in;
    in.pos = e->position();
    in.pressure = float(e->pressure());
    in.tiltX = float(e->xTilt());
    in.tiltY = float(e->yTilt());
    in.button = e->button();
    in.mods = e->modifiers();
    in.eraserEnd = e->pointerType() == QPointingDevice::PointerType::Eraser;

    // Qt keeps sending pen events to whichever widget took the last press until
    // it sees the release. If that release went missing, a press over a panel
    // would arrive here: hand it back so it reaches what is under the pen.
    if (e->type() == QEvent::TabletPress && m_drag == Drag::None) {
        const QWidget* under = QApplication::widgetAt(e->globalPosition().toPoint());
        if (under && under != this) {
            e->ignore();
            return;
        }
    }
    // Hovering is left unaccepted on purpose. Qt then replays it as a mouse
    // move, which is what drives enter/leave and the cursor shape when the pen
    // travels between the canvas and the panels.
    if (e->type() == QEvent::TabletMove && m_drag == Drag::None && e->buttons() == Qt::NoButton) {
        pointerMove(in);
        e->ignore();
        return;
    }

    switch (e->type()) {
    case QEvent::TabletPress:
        pointerPress(in);
        break;
    case QEvent::TabletMove:
        pointerMove(in);
        break;
    case QEvent::TabletRelease:
        pointerRelease(in);
        break;
    default:
        break;
    }
    e->accept();
}

void CanvasWidget::mousePressEvent(QMouseEvent* e)
{
    PointerInput in;
    in.pos = e->position();
    in.button = e->button();
    in.mods = e->modifiers();
    pointerPress(in);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e)
{
    PointerInput in;
    in.pos = e->position();
    in.mods = e->modifiers();
    pointerMove(in);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e)
{
    PointerInput in;
    in.pos = e->position();
    in.button = e->button();
    in.mods = e->modifiers();
    pointerRelease(in);
}

void CanvasWidget::pointerPress(const PointerInput& in)
{
    m_lastPointerMs = m_clock.elapsed();
    setFocus();
    m_cursor = in.pos;
    m_cursorInside = true;
    if (m_drag != Drag::None || !m_engine->hasDocument())
        return;

    const QPointF dev = toDevice(in.pos);
    if (m_space || in.button == Qt::MiddleButton) {
        m_dragStart = dev;
        m_panStart = m_pan;
        m_zoomStart = m_zoom;
        m_rotStart = m_rotation;
        m_drag = (in.mods & Qt::ControlModifier) ? Drag::Zoom
                 : (in.mods & Qt::ShiftModifier) ? Drag::Rotate
                                                 : Drag::Pan;
        updateCursorShape();
        return;
    }

    if (m_tf.active) {
        if (in.button != Qt::LeftButton)
            return;
        const int hit = hitHandle(in.pos);
        m_tf.handle = hit >= 0 ? hit : kMoveHandle;
        m_tf.startMouse = widgetToCanvas(in.pos);
        m_tf.startCenter = m_tf.center;
        m_tf.startSx = m_tf.sx;
        m_tf.startSy = m_tf.sy;
        m_tf.startRot = m_tf.rot;
        for (int i = 0; i < 4; ++i)
            m_tf.startCorner[i] = m_tf.corner[i];
        m_tf.startWarp = m_tf.warpPts;
        m_drag = Drag::Transform;
        update();
        return;
    }
    if (m_shape.editing) {
        if (in.button == Qt::LeftButton)
            shapePress(in);
        return;
    }
    if (m_crop.active) {
        if (in.button == Qt::LeftButton)
            cropPress(in);
        return;
    }
    if (m_liquify.active) {
        if (in.button == Qt::LeftButton)
            liquifyPress(in);
        return;
    }
    if (m_text.mode && !m_engine->editActive()) {
        if (in.button == Qt::LeftButton)
            textPress(in);
        return;
    }
    if (m_gradient.mode && !m_engine->editActive()) {
        if (in.button == Qt::LeftButton && m_engine->beginGradient()) {
            withdrawShapeOffer();
            m_gradient.from = m_gradient.to = widgetToCanvas(in.pos);
            m_drag = Drag::Gradient;
            m_tools->noteColorUsed();
        }
        return;
    }
    if (m_engine->editActive())
        return;
    // Guides being arranged: a press on one of their points moves it.
    if (m_guideEdit && in.button == Qt::LeftButton && !m_select.mode) {
        const int hit = hitGuideHandle(in.pos);
        if (hit >= 0) {
            m_guideHandle = hit;
            m_drag = Drag::Guide;
            update();
            return;
        }
    }
    if (m_select.mode) {
        if (in.button != Qt::LeftButton)
            return;
        if (m_select.shape != SelectShape::Automatic) {
            selectPress(in);
        } else if (beginFloodDrag(in.pos, true, m_select.subtract != bool(m_alt || (in.mods & Qt::AltModifier)), false)) {
            m_drag = Drag::Flood;
        }
        return;
    }
    if (m_fillArmed && in.button == Qt::LeftButton) {
        // The fill stays armed until the pen lifts: sliding adjusts its threshold.
        if (beginFloodDrag(in.pos, false, false, false))
            m_drag = Drag::Flood;
        else
            armFill(false);
        return;
    }

    if (m_alt || m_eyedropperArmed || in.button == Qt::RightButton) {
        m_drag = Drag::Pick;
        m_pickPrev = m_tools->color();
        pickAt(in.pos);
        return;
    }
    if (in.button != Qt::LeftButton)
        return;
    if (m_shift || (in.mods & Qt::ShiftModifier)) {
        // Shift + drag sideways sizes the brush instead of painting.
        m_drag = Drag::Size;
        m_sizingLiquify = false;
        m_sizeAnchor = in.pos;
        m_sizeStart = m_tools->sizeFraction();
        update();
        return;
    }

    const StrokeTool tool = in.eraserEnd ? StrokeTool::Eraser : m_tools->tool();
    // A tap on the shape just drawn reopens it. Whether this is a tap is only
    // known when the pen lifts, so the stroke starts as usual and is taken back then.
    m_shapeTap = {};
    if (nearOfferedShape(in.pos)) {
        m_shapeTap.candidate = true;
        m_shapeTap.at = in.pos;
        m_shapeTap.since = m_clock.elapsed();
        m_shapeTap.saved = m_shape;
    }
    withdrawShapeOffer();
    m_engine->setInputScale(float(devicePixelRatioF() / m_zoom));
    if (m_engine->beginStroke(tool, m_tools->preset(tool), m_tools->size(tool), m_tools->opacity(tool),
                              m_tools->color(), makeSample(in))) {
        m_drag = Drag::Stroke;
        m_assist = {};
        if (m_engine->guides().perspective != Guides::Perspective::Off && m_engine->guides().snap) {
            m_assist.active = true;
            m_assist.startWidget = in.pos;
            m_assist.start = widgetToCanvas(in.pos);
        }
        beginShapeCapture(in, tool);
        m_lastInput = in;
        m_lastInputMs = m_clock.elapsed();
        m_strokeTimer.start();
        if (tool == StrokeTool::Brush)
            m_tools->noteColorUsed();
    }
}

void CanvasWidget::pointerMove(const PointerInput& in)
{
    m_lastPointerMs = m_clock.elapsed();
    m_cursor = in.pos;
    m_cursorInside = true;
    const QPointF dev = toDevice(in.pos);

    switch (m_drag) {
    case Drag::Stroke: {
        if (m_shapeTap.candidate && std::hypot(in.pos.x() - m_shapeTap.at.x(), in.pos.y() - m_shapeTap.at.y()) > 7.0)
            m_shapeTap.moved = true;   // a stroke after all
        PointerInput held = in;
        if (!assist(held))
            break;   // too soon to tell which guide line this stroke follows
        m_lastInput = held;
        if (m_shape.snapped) {
            queueShapeUpdate(held, false);   // the stroke is a shape now: the pen resizes it
        } else {
            m_engine->strokeTo(makeSample(held));
            captureShapePoint(held);
            m_lastInputMs = m_clock.elapsed();
        }
        break;
    }
    case Drag::Size: {
        const float t = std::clamp(m_sizeStart + float((in.pos.x() - m_sizeAnchor.x()) / 360.0), 0.0f, 1.0f);
        if (m_sizingLiquify)
            setLiquifySizeFraction(t);
        else
            m_tools->setSizeFraction(t);
        break;
    }
    case Drag::Guide:
        dragGuideHandle(in.pos, in.mods);
        break;
    case Drag::Crop:
        cropMove(in);
        break;
    case Drag::Liquify:
        liquifyMove(in);
        break;
    case Drag::Text:
        textMove(in);
        break;
    case Drag::Gradient:
        m_gradient.to = widgetToCanvas(in.pos);
        // Shift keeps it level, upright or on the diagonal.
        if (m_shift || (in.mods & Qt::ShiftModifier))
            QuickShape::snapLineEnd(m_gradient.from, m_gradient.to, true);
        applyGradient();
        break;
    case Drag::Shape:
        queueShapeUpdate(in, true);
        break;
    case Drag::Select:
        selectMove(in);
        break;
    case Drag::Flood:
        moveFloodDrag(in.pos);
        break;
    case Drag::Pan:
        m_pan = m_panStart + (dev - m_dragStart);
        emit viewChanged();
        break;
    case Drag::Zoom: {
        // Drag right/up to zoom in, anchored where the drag began.
        const double delta = (dev.x() - m_dragStart.x()) - (dev.y() - m_dragStart.y());
        const double z = std::clamp(m_zoomStart * std::exp(delta * 0.006), kMinZoom, kMaxZoom);
        m_pan = m_dragStart + (m_panStart - m_dragStart) * (z / m_zoomStart);
        m_zoom = z;
        emit viewChanged();
        break;
    }
    case Drag::Rotate: {
        const QPointF c = toDevice(QPointF(width() / 2.0, height() / 2.0));
        const double a0 = std::atan2(m_dragStart.y() - c.y(), m_dragStart.x() - c.x());
        const double a1 = std::atan2(dev.y() - c.y(), dev.x() - c.x());
        double target = m_rotStart + (a1 - a0) * 180.0 / kPi;
        // Gently snap to the quarter turns.
        const double nearest = std::round(target / 90.0) * 90.0;
        if (std::abs(target - nearest) < 3.0)
            target = nearest;
        m_pan = c + rotated(m_panStart - c, target - m_rotStart);
        m_rotation = std::fmod(target, 360.0);
        emit viewChanged();
        break;
    }
    case Drag::Pick:
        pickAt(in.pos);
        break;
    case Drag::Transform: {
        const QPointF m = widgetToCanvas(in.pos);
        TransformSession& t = m_tf;
        if (t.warp) {
            // One point of the sheet, or with a press anywhere else the whole of it.
            const QPointF d = m - t.startMouse;
            if (t.handle >= kWarpHandle && t.handle - kWarpHandle < int(t.warpPts.size())) {
                t.warpPts[size_t(t.handle - kWarpHandle)] = t.startWarp[size_t(t.handle - kWarpHandle)] + d;
            } else {
                for (size_t i = 0; i < t.warpPts.size(); ++i)
                    t.warpPts[i] = t.startWarp[i] + d;
            }
            applyTransform();
            break;
        }
        if (t.distort) {
            // A corner moves alone, a side takes its two corners, anywhere else takes all four.
            const QPointF d = m - t.startMouse;
            QPointF c[4] = {t.startCorner[0], t.startCorner[1], t.startCorner[2], t.startCorner[3]};
            if (t.handle == kMoveHandle) {
                for (QPointF& q : c)
                    q += d;
            } else if (t.handle % 2 == 0) {
                c[t.handle / 2] += d;
            } else if (t.handle < 8) {
                c[(t.handle - 1) / 2] += d;
                c[((t.handle - 1) / 2 + 1) % 4] += d;
            }
            if (properQuad(c, quadTurn(t.startCorner) > 0)) {
                for (int i = 0; i < 4; ++i)
                    t.corner[i] = c[i];
            }
            applyTransform();
            break;
        }
        if (t.handle == kMoveHandle) {
            t.center = t.startCenter + (m - t.startMouse);
        } else if (t.handle == kRotateHandle) {
            const double a0 = std::atan2(t.startMouse.y() - t.startCenter.y(), t.startMouse.x() - t.startCenter.x());
            const double a1 = std::atan2(m.y() - t.startCenter.y(), m.x() - t.startCenter.x());
            double rot = t.startRot + (a1 - a0) * 180.0 / kPi;
            if (in.mods & Qt::ShiftModifier)
                rot = std::round(rot / 15.0) * 15.0;
            t.rot = rot;
        } else {
            // Scale about the opposite handle, working in the box's own frame.
            const double hx = kHandleX[t.handle], hy = kHandleY[t.handle];
            const double hw = t.bounds.width() / 2.0, hh = t.bounds.height() / 2.0;
            const QPointF anchor = t.startCenter + rotated(QPointF(-hx * hw * t.startSx, -hy * hh * t.startSy), t.startRot);
            const QPointF v = rotated(m - anchor, -t.startRot);
            const QPointF d0(2.0 * hx * hw * t.startSx, 2.0 * hy * hh * t.startSy);
            const bool uniform = t.uniform != bool(in.mods & Qt::ShiftModifier);
            double sx = t.startSx, sy = t.startSy;
            if (uniform) {
                const double len2 = d0.x() * d0.x() + d0.y() * d0.y();
                const double k = len2 > 1e-9 ? (v.x() * d0.x() + v.y() * d0.y()) / len2 : 1.0;
                sx = t.startSx * k;
                sy = t.startSy * k;
            } else {
                if (hx != 0.0)
                    sx = v.x() / (2.0 * hx * hw);
                if (hy != 0.0)
                    sy = v.y() / (2.0 * hy * hh);
            }
            auto guard = [](double s) { return std::abs(s) < 0.01 ? (s < 0 ? -0.01 : 0.01) : s; };
            t.sx = guard(sx);
            t.sy = guard(sy);
            t.center = anchor + rotated(QPointF(hx * hw * t.sx, hy * hh * t.sy), t.startRot);
        }
        applyTransform();
        break;
    }
    case Drag::None:
        break;
    }
    update();
}

void CanvasWidget::pointerRelease(const PointerInput& in)
{
    m_lastPointerMs = m_clock.elapsed();
    m_cursor = in.pos;
    flushShapeUpdate();   // the shape ends up where the pen last was, not one report behind
    switch (m_drag) {
    case Drag::Stroke:
        if (in.button != Qt::LeftButton && in.button != Qt::NoButton)
            return;
        m_strokeTimer.stop();
        if (m_shapeTap.candidate && !m_shapeTap.moved && !m_shape.snapped && m_clock.elapsed() - m_shapeTap.since < 450) {
            // It was a tap on the shape: no dot, the shape opens for editing.
            m_engine->cancelStroke();
            m_assist = {};
            m_drag = Drag::None;
            m_shape = m_shapeTap.saved;
            m_shapeTap = {};
            emit shapeEditOfferChanged(true);
            startShapeEdit();
            updateCursorShape();
            update();
            return;
        }
        m_shapeTap = {};
        if (m_shape.snapped) {
            m_shape.snapped = false;
            m_shape.snapLabel.clear();
            m_engine->endStroke(tr("Shape"));
            offerShapeEdit();
        } else {
            PointerInput held = in;
            if (!assist(held, true))
                held.pos = m_assist.startWidget;   // lifted before going anywhere: a dot where it landed
            m_engine->strokeTo(makeSample(held));
            m_engine->endStroke();
        }
        m_assist = {};
        break;
    case Drag::Guide:
        m_guideHandle = -1;
        break;
    case Drag::Crop:
        m_crop.handle = -1;
        break;
    case Drag::Liquify:
        m_liquifyTimer.stop();
        break;
    case Drag::Gradient: {
        // A tap lays nothing down: a gradient needs somewhere to go.
        const QPointF a = canvasToWidget(m_gradient.from), b = canvasToWidget(m_gradient.to);
        m_engine->endGradient(std::hypot(a.x() - b.x(), a.y() - b.y()) > 4.0);
        break;
    }
    case Drag::Shape:
        shapeRelease();
        break;
    case Drag::Select:
        selectRelease(in);
        break;
    case Drag::Flood:
        endFloodDrag(true);
        break;
    case Drag::Pick:
        if (m_eyedropperArmed)
            armEyedropper(false);
        break;
    case Drag::Transform:
        m_tf.handle = -1;
        break;
    default:
        break;
    }
    m_drag = Drag::None;
    updateCursorShape();
    update();
}

void CanvasWidget::strokeTick()
{
    // While the pen rests on the surface no events arrive, but a stabilised
    // line should still catch up with it.
    if (m_drag != Drag::Stroke || m_shape.snapped)
        return;
    trySnapShape();
    if (m_shape.snapped || m_clock.elapsed() - m_lastInputMs < 12)
        return;
    m_engine->strokeTo(makeSample(m_lastInput));
}

void CanvasWidget::pickAt(const QPointF& logical)
{
    const QPointF c = widgetToCanvas(logical);
    const QColor picked = m_engine->pickColor(QPoint(int(std::floor(c.x())), int(std::floor(c.y()))));
    if (picked.isValid())
        m_tools->setColor(picked);
    update();
}

void CanvasWidget::armEyedropper(bool armed)
{
    if (m_eyedropperArmed == armed)
        return;
    m_eyedropperArmed = armed;
    emit eyedropperChanged(armed);
    update();
}

void CanvasWidget::wheelEvent(QWheelEvent* e)
{
    const QPoint d = e->angleDelta();
    const int steps = d.y() != 0 ? d.y() : d.x();
    if (steps == 0 || !m_engine->hasDocument() || m_padMode)
        return;
    if (e->modifiers() & Qt::ShiftModifier) {
        rotateAround(steps / 120.0 * 5.0, toDevice(QPointF(width() / 2.0, height() / 2.0)));
    } else if (e->modifiers() & Qt::AltModifier) {
        m_tools->setSizeFraction(m_tools->sizeFraction() + float(steps) / 120.0f * 0.02f);
    } else {
        zoomAround(m_zoom * std::pow(1.0015, steps), toDevice(e->position()));
    }
    e->accept();
}

void CanvasWidget::updateCursorShape()
{
    if (m_drag == Drag::Pan)
        setCursor(Qt::ClosedHandCursor);
    else if (m_drag == Drag::Zoom)
        setCursor(Qt::SizeVerCursor);
    else if (m_drag == Drag::Rotate)
        setCursor(Qt::CrossCursor);
    else if (m_space)
        setCursor(Qt::OpenHandCursor);
    else if (m_liquify.active)
        setCursor(Qt::BlankCursor);   // the brush outline is the cursor
    else if (m_tf.active || m_shape.editing || m_crop.active || m_engine->editActive())
        setCursor(Qt::ArrowCursor);
    else if (m_text.mode)
        setCursor(m_text.placed ? Qt::SizeAllCursor : Qt::IBeamCursor);
    else if (m_select.mode || m_gradient.mode)
        setCursor(Qt::CrossCursor);
    else
        setCursor(Qt::BlankCursor);
}

void CanvasWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Space:
        m_space = true;
        updateCursorShape();
        update();
        return;
    case Qt::Key_Alt:
        m_alt = true;
        update();
        return;
    case Qt::Key_Shift:
        // Shift makes a held shape perfect, like a second finger in Procreate.
        m_shift = true;
        if (m_drag == Drag::Stroke && m_shape.snapped)
            adjustHeldShape(m_lastInput);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_tf.active) {
            finishTransform(true);
            return;
        }
        if (m_shape.editing) {
            finishShapeEdit(true);
            return;
        }
        break;
    case Qt::Key_Escape:
        if (m_tf.active) {
            finishTransform(false);
            return;
        }
        if (m_shape.editing) {
            finishShapeEdit(false);
            return;
        }
        if (m_eyedropperArmed) {
            armEyedropper(false);
            return;
        }
        break;
    default:
        break;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) {
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        m_space = false;
        updateCursorShape();
        update();
        return;
    }
    if (e->key() == Qt::Key_Alt) {
        m_alt = false;
        update();
        return;
    }
    if (e->key() == Qt::Key_Shift) {
        m_shift = false;
        if (m_drag == Drag::Stroke && m_shape.snapped)
            adjustHeldShape(m_lastInput);
    }
    QOpenGLWidget::keyReleaseEvent(e);
}

void CanvasWidget::focusOutEvent(QFocusEvent* e)
{
    m_space = false;
    m_alt = false;
    m_shift = false;
    updateCursorShape();
    update();
    QOpenGLWidget::focusOutEvent(e);
}

void CanvasWidget::enterEvent(QEnterEvent* e)
{
    m_cursorInside = true;
    m_cursor = e->position();
    updateCursorShape();
    update();
}

void CanvasWidget::leaveEvent(QEvent*)
{
    m_cursorInside = false;
    update();
}

// --------------------------------------------------------------- transform

QTransform CanvasWidget::transformMatrix() const
{
    if (m_tf.distort) {
        const QRectF b = m_tf.bounds;
        QPolygonF from, to;
        from << b.topLeft() << b.topRight() << b.bottomRight() << b.bottomLeft();
        for (const QPointF& c : m_tf.corner)
            to << c;
        QTransform t;
        if (QTransform::quadToQuad(from, to, t))
            return t;
    }
    const QPointF c0 = m_tf.bounds.center();
    QTransform t;
    t.translate(m_tf.center.x(), m_tf.center.y());
    t.rotate(m_tf.rot);
    t.scale(m_tf.sx, m_tf.sy);
    t.translate(-c0.x(), -c0.y());
    return t;
}

QPointF CanvasWidget::handleCanvasPos(int handle) const
{
    if (m_tf.distort) {
        // Even handles are the corners themselves; odd ones sit half way along a side.
        if (handle % 2 == 0)
            return m_tf.corner[handle / 2];
        return (m_tf.corner[(handle - 1) / 2] + m_tf.corner[((handle - 1) / 2 + 1) % 4]) * 0.5;
    }
    const double hw = m_tf.bounds.width() / 2.0, hh = m_tf.bounds.height() / 2.0;
    return m_tf.center + rotated(QPointF(kHandleX[handle] * hw * m_tf.sx, kHandleY[handle] * hh * m_tf.sy), m_tf.rot);
}

QPointF CanvasWidget::rotateKnobPos() const
{
    const QPointF topMid = canvasToWidget(handleCanvasPos(1));
    const QPointF centre = canvasToWidget(m_tf.center);
    QPointF dir = topMid - centre;
    const double len = std::hypot(dir.x(), dir.y());
    dir = len > 1e-6 ? dir / len : QPointF(0, -1);
    return topMid + dir * 30.0;
}

int CanvasWidget::hitHandle(const QPointF& logical) const
{
    auto near = [&](const QPointF& p, double r) { return std::hypot(p.x() - logical.x(), p.y() - logical.y()) <= r; };
    if (m_tf.warp) {
        for (int i = 0; i < int(m_tf.warpPts.size()); ++i) {
            if (near(canvasToWidget(m_tf.warpPts[size_t(i)]), 13.0))
                return kWarpHandle + i;
        }
        return -1;
    }
    if (!m_tf.distort && near(rotateKnobPos(), 13.0))
        return kRotateHandle;
    for (int h = 0; h < 8; ++h) {
        if (near(canvasToWidget(handleCanvasPos(h)), 12.0))
            return h;
    }
    return -1;
}

void CanvasWidget::applyTransform()
{
    if (m_tf.warp)
        m_engine->setWarp(kWarpSide, kWarpSide, m_tf.warpPts, m_tf.bounds);
    else
        m_engine->setTransform(transformMatrix());
    update();
}

void CanvasWidget::setTransformWarp(bool warp)
{
    if (!m_tf.active || m_tf.warp == warp)
        return;
    if (warp) {
        // The sheet starts out lying on the layer as it has been placed so far.
        const QTransform placed = transformMatrix();
        const QRectF b = m_tf.bounds;
        m_tf.warpPts.clear();
        for (int r = 0; r < kWarpSide; ++r) {
            for (int c = 0; c < kWarpSide; ++c)
                m_tf.warpPts.push_back(placed.map(QPointF(b.left() + b.width() * c / (kWarpSide - 1), b.top() + b.height() * r / (kWarpSide - 1))));
        }
    }
    m_tf.warp = warp;   // switching it off goes back to how the box stood before
    applyTransform();
}

QVector<QPointF> CanvasWidget::transformWarpPositions() const
{
    QVector<QPointF> out;
    if (m_tf.active && m_tf.warp) {
        for (const QPointF& q : m_tf.warpPts)
            out.push_back(canvasToWidget(q));
    }
    return out;
}

bool CanvasWidget::startTransform()
{
    if (m_tf.active || m_drag != Drag::None)
        return false;
    const QRect bounds = m_engine->editableBounds(m_engine->activeLayerId());
    if (bounds.isEmpty()) {
        emit toast(m_engine->hasSelection() ? tr("Nothing inside the selection on this layer")
                                            : tr("Nothing to transform on this layer"));
        return false;
    }
    if (!m_engine->beginTransform())
        return false;
    const bool uniform = m_tf.uniform;
    m_tf = {};
    m_tf.active = true;
    m_tf.uniform = uniform;
    m_tf.bounds = QRectF(bounds);
    m_tf.center = m_tf.bounds.center();
    updateCursorShape();
    update();
    return true;
}

void CanvasWidget::finishTransform(bool commit)
{
    if (!m_tf.active)
        return;
    const bool changed = m_tf.warp || !transformMatrix().isIdentity();
    if (commit && changed)
        m_engine->commitEdit();
    else
        m_engine->cancelEdit();
    m_tf.active = false;
    m_tf.handle = -1;
    m_drag = Drag::None;
    updateCursorShape();
    update();
    emit transformFinished();
}

void CanvasWidget::setTransformDistort(bool distort)
{
    if (!m_tf.active || m_tf.distort == distort)
        return;
    if (distort) {
        // Start from the box as it stands.
        for (int i = 0; i < 4; ++i)
            m_tf.corner[i] = handleCanvasPos(i * 2);
    }
    m_tf.distort = distort;   // switching it off goes back to the box as it was before
    applyTransform();
}

QVector<QPointF> CanvasWidget::transformHandlePositions() const
{
    QVector<QPointF> out;
    if (m_tf.active) {
        for (int h = 0; h < 8; ++h)
            out.push_back(canvasToWidget(handleCanvasPos(h)));
    }
    return out;
}

void CanvasWidget::transformFlip(bool horizontal)
{
    if (!m_tf.active || m_tf.warp)
        return;
    if (m_tf.distort) {
        // The picture turns over inside the shape it has been given.
        if (horizontal) {
            std::swap(m_tf.corner[0], m_tf.corner[1]);
            std::swap(m_tf.corner[3], m_tf.corner[2]);
        } else {
            std::swap(m_tf.corner[0], m_tf.corner[3]);
            std::swap(m_tf.corner[1], m_tf.corner[2]);
        }
        applyTransform();
        return;
    }
    (horizontal ? m_tf.sx : m_tf.sy) *= -1.0;
    applyTransform();
}

void CanvasWidget::transformRotate(double degrees)
{
    if (!m_tf.active || m_tf.warp)
        return;
    if (m_tf.distort) {
        const QPointF middle = (m_tf.corner[0] + m_tf.corner[1] + m_tf.corner[2] + m_tf.corner[3]) * 0.25;
        for (QPointF& c : m_tf.corner)
            c = middle + rotated(c - middle, degrees);
        applyTransform();
        return;
    }
    m_tf.rot += degrees;
    applyTransform();
}

void CanvasWidget::transformFit()
{
    if (!m_tf.active)
        return;
    m_tf.distort = false;
    m_tf.warp = false;
    const QSize cs = m_engine->canvasSize();
    const double k = std::min(cs.width() / m_tf.bounds.width(), cs.height() / m_tf.bounds.height());
    m_tf.sx = m_tf.sx < 0 ? -k : k;
    m_tf.sy = m_tf.sy < 0 ? -k : k;
    m_tf.rot = 0.0;
    m_tf.center = QPointF(cs.width() / 2.0, cs.height() / 2.0);
    applyTransform();
}

void CanvasWidget::transformReset()
{
    if (!m_tf.active)
        return;
    m_tf.distort = false;
    m_tf.warp = false;
    m_tf.sx = m_tf.sy = 1.0;
    m_tf.rot = 0.0;
    m_tf.center = m_tf.bounds.center();
    applyTransform();
}

void CanvasWidget::setTransformUniform(bool uniform)
{
    m_tf.uniform = uniform;
}
